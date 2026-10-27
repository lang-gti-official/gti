#include "semantic_analyzer_impl.h"

#include "gti/failure_metadata.h"
#include "gti/hir.h"
#include "gti/mir_constant_evaluator.h"
#include "gti/mir_lowering.h"

#include <algorithm>
#include <functional>
#include <span>

namespace lang {
namespace {
// A staging request borrows its already-owned arguments. Failed effect
// journals are uncommon; do not allocate an owning key merely to check an
// empty journal on every ordinary constant-evaluation call.
struct StagedInstanceKeyView {
  InstanceAnalysisKind kind = InstanceAnalysisKind::Function;
  std::size_t declaration = 0;
  std::span<const SemanticType> ownerTypes;
  std::span<const CompileTimeValue> ownerValues;
  std::span<const SemanticType> types;
  std::span<const CompileTimeValue> values;
};

struct StagedHash {
  std::size_t value = 0;
  void add(std::size_t next) {
    value ^= next + std::size_t{0x9e3779b9} + (value << 6) + (value >> 2);
  }
  void type(const SemanticType &type) {
    add(type.kind);
    add(type.classId);
    add(type.enumId);
    add(type.genericParameterId);
    add(type.functionId);
    add(type.arrayLength);
    add(type.arguments.size());
    for (const auto &argument : type.arguments)
      this->type(argument);
    add(type.valueArguments.size());
    for (const auto &argument : type.valueArguments)
      add(argument.value);
  }
  void constant(const ConstantValue &value) {
    add(value.index());
    if (const auto *integer = std::get_if<ConstantInteger>(&value)) {
      add(integer->magnitude);
      add(integer->negative);
    } else if (const auto *text = std::get_if<std::string>(&value)) {
      add(std::hash<std::string>{}(*text));
    } else if (const auto *flag = std::get_if<bool>(&value)) {
      add(*flag);
    } else if (const auto *enumerator = std::get_if<ConstantEnum>(&value)) {
      add(enumerator->enumId);
      add(enumerator->value.magnitude);
    } else if (const auto *aggregate =
                   std::get_if<ConstantAggregateValue>(&value);
               aggregate != nullptr && aggregate->value != nullptr) {
      add(static_cast<std::size_t>(aggregate->value->kind));
      add(aggregate->value->identity);
      add(aggregate->value->variant);
      for (const auto &element : aggregate->value->elements)
        constant(element);
      for (const auto &field : aggregate->value->fields) {
        add(field.symbol);
        constant(field.value);
      }
    }
  }
};

// Read-only traits do not require a fork. Allocate the isolated analysis
// snapshot only if ordinary HIR discovery actually requests an instance.
class StagedInstanceProvider final : public SemanticInstanceProvider {
public:
  using ValidateResult = std::function<void(const StagedInstanceKeyView &,
                                            SemanticInstanceAnalysis &)>;
  StagedInstanceProvider(const SemanticInstanceProvider &facts,
                         std::function<SemanticInstanceProvider &()> acquire,
                         ValidateResult validate)
      : facts(facts), acquire(std::move(acquire)),
        validate(std::move(validate)) {}
  SemanticTypeTraits traitsFor(const SemanticType &type) const override {
    return facts.traitsFor(type);
  }
  bool requiresActiveCleanupFor(const SemanticType &type) const override {
    return facts.requiresActiveCleanupFor(type);
  }
  std::optional<EnumTypeInfo>
  enumTypeFor(const SemanticType &type) const override {
    return facts.enumTypeFor(type);
  }
  SemanticInstanceAnalysis analyzeFunctionInstance(
      FunctionId id, const std::vector<SemanticType> &ownerTypes,
      const std::vector<CompileTimeValue> &ownerValues,
      const std::vector<SemanticType> &types,
      const std::vector<CompileTimeValue> &values) override {
    auto result = acquire().analyzeFunctionInstance(id, ownerTypes, ownerValues,
                                                    types, values);
    validate({.kind = InstanceAnalysisKind::Function,
              .declaration = id,
              .ownerTypes = ownerTypes,
              .ownerValues = ownerValues,
              .types = types,
              .values = values},
             result);
    return result;
  }
  SemanticInstanceAnalysis analyzeConstructorInstance(
      ConstructorId id, const std::vector<SemanticType> &ownerTypes,
      const std::vector<CompileTimeValue> &ownerValues,
      const std::vector<SemanticType> &types,
      const std::vector<CompileTimeValue> &values) override {
    auto result = acquire().analyzeConstructorInstance(
        id, ownerTypes, ownerValues, types, values);
    validate({.kind = InstanceAnalysisKind::Constructor,
              .declaration = id,
              .ownerTypes = ownerTypes,
              .ownerValues = ownerValues,
              .types = types,
              .values = values},
             result);
    return result;
  }
  SemanticInstanceAnalysis analyzeDestructorInstance(
      ClassId id, const std::vector<SemanticType> &types,
      const std::vector<CompileTimeValue> &values) override {
    auto result = acquire().analyzeDestructorInstance(id, types, values);
    validate({.kind = InstanceAnalysisKind::Destructor,
              .declaration = id,
              .ownerTypes = types,
              .ownerValues = values},
             result);
    return result;
  }
  SemanticInstanceAnalysis analyzeClassFieldInitializers(
      ClassId id, const std::vector<SemanticType> &types,
      const std::vector<CompileTimeValue> &values) override {
    auto result = acquire().analyzeClassFieldInitializers(id, types, values);
    validate({.kind = InstanceAnalysisKind::ClassFieldInitializers,
              .declaration = id,
              .ownerTypes = types,
              .ownerValues = values},
             result);
    return result;
  }

private:
  const SemanticInstanceProvider &facts;
  std::function<SemanticInstanceProvider &()> acquire;
  ValidateResult validate;
};
} // namespace

SemanticVisitor::Impl::SemanticCatalog::SemanticCatalog(
    const SemanticCatalog &other) {
  *this = other;
}

auto SemanticVisitor::Impl::SemanticCatalog::operator=(
    const SemanticCatalog &other) -> SemanticCatalog & {
  if (this == &other)
    return *this;
  SemanticCatalog copy;
  copy.compilerTypeClasses = other.compilerTypeClasses;
  copy.namespaces = other.namespaces;
  copy.namespaceToolingSymbols = other.namespaceToolingSymbols;
  copy.namespaceAliases = other.namespaceAliases;
  copy.compilerPrivateNamespaceSymbol = other.compilerPrivateNamespaceSymbol;
  copy.typeAliasIds = other.typeAliasIds;
  copy.typeAliases = other.typeAliases;
  copy.conceptIds = other.conceptIds;
  copy.concepts = other.concepts;
  copy.namespaceSymbols = other.namespaceSymbols;
  copy.internalNamespaceSymbols = other.internalNamespaceSymbols;
  copy.classIds = other.classIds;
  copy.enumIds = other.enumIds;
  copy.visibleNamespaces = other.visibleNamespaces;
  copy.visibleNamespaceAliases = other.visibleNamespaceAliases;
  copy.visibleTypeAliasIds = other.visibleTypeAliasIds;
  copy.visibleConceptIds = other.visibleConceptIds;
  copy.visibleNamespaceSymbols = other.visibleNamespaceSymbols;
  copy.visibleClassIds = other.visibleClassIds;
  copy.visibleEnumIds = other.visibleEnumIds;
  copy.classDeclIds = other.classDeclIds;
  copy.conceptDeclIds = other.conceptDeclIds;
  copy.functionGenericParameters = other.functionGenericParameters;
  copy.constraintPredicateDeclarations = other.constraintPredicateDeclarations;
  for (const auto &declaration : other.callableDeclarations) {
    auto owned = std::make_unique<CallableDeclaration>(*declaration);
    copy.callableDeclarationsBySyntax.insert_or_assign(owned->syntax,
                                                       owned.get());
    copy.callableDeclarations.emplace_back(std::move(owned));
  }
  copy.constructorGenericParameters = other.constructorGenericParameters;
  copy.rootNativeStorageSymbols = other.rootNativeStorageSymbols;
  copy.globalBorrowStorage = other.globalBorrowStorage;
  copy.genericConstraints = other.genericConstraints;
  copy.genericConstraintConcepts = other.genericConstraintConcepts;
  copy.compilerTypeClasses = other.compilerTypeClasses;
  copy.classes = other.classes;
  copy.exactClassSpecializations = other.exactClassSpecializations;
  copy.conformances = other.conformances;
  copy.enums = other.enums;
  copy.predeclaredVariables = other.predeclaredVariables;
  copy.mutableFieldGroupOrigins = other.mutableFieldGroupOrigins;
  copy.recursiveLayoutFields = other.recursiveLayoutFields;
  copy.nextGenericParameterId = other.nextGenericParameterId;
  copy.nextConstructorId = other.nextConstructorId;
  copy.nextDefaultArgumentEffectOwner = other.nextDefaultArgumentEffectOwner;
  copy.nextFunctionId = other.nextFunctionId;
  copy.sealed = other.sealed;
  copy.sealedShape = other.sealedShape;
  *this = std::move(copy);
  return *this;
}

auto SemanticVisitor::Impl::SemanticCatalog::snapshotForInstanceAnalysis() const
    -> SemanticCatalog {
  assertUnchangedSinceSeal();
  SemanticCatalog copy;
  copy.compilerTypeClasses = compilerTypeClasses;
  copy.namespaces = namespaces;
  copy.namespaceToolingSymbols = namespaceToolingSymbols;
  copy.namespaceAliases = namespaceAliases;
  copy.compilerPrivateNamespaceSymbol = compilerPrivateNamespaceSymbol;
  copy.typeAliasIds = typeAliasIds;
  copy.typeAliases = typeAliases;
  copy.conceptIds = conceptIds;
  copy.concepts = concepts;
  copy.namespaceSymbols = namespaceSymbols;
  copy.internalNamespaceSymbols = internalNamespaceSymbols;
  copy.classIds = classIds;
  copy.enumIds = enumIds;
  copy.visibleNamespaces = visibleNamespaces;
  copy.visibleNamespaceAliases = visibleNamespaceAliases;
  copy.visibleTypeAliasIds = visibleTypeAliasIds;
  copy.visibleConceptIds = visibleConceptIds;
  copy.visibleNamespaceSymbols = visibleNamespaceSymbols;
  copy.visibleClassIds = visibleClassIds;
  copy.visibleEnumIds = visibleEnumIds;
  copy.classDeclIds = classDeclIds;
  copy.conceptDeclIds = conceptDeclIds;
  copy.functionGenericParameters = functionGenericParameters;
  copy.constraintPredicateDeclarations = constraintPredicateDeclarations;
  for (const auto &declaration : callableDeclarations) {
    (void)copy.registerCallable(*declaration);
  }
  copy.constructorGenericParameters = constructorGenericParameters;
  copy.rootNativeStorageSymbols = rootNativeStorageSymbols;
  copy.globalBorrowStorage = globalBorrowStorage;
  copy.genericConstraints = genericConstraints;
  copy.genericConstraintConcepts = genericConstraintConcepts;
  copy.compilerTypeClasses = compilerTypeClasses;
  copy.classes = classes;
  copy.exactClassSpecializations = exactClassSpecializations;
  copy.conformances = conformances;
  copy.enums = enums;
  copy.predeclaredVariables = predeclaredVariables;
  copy.mutableFieldGroupOrigins = mutableFieldGroupOrigins;
  copy.recursiveLayoutFields = recursiveLayoutFields;
  copy.nextGenericParameterId = nextGenericParameterId;
  copy.nextConstructorId = nextConstructorId;
  copy.nextDefaultArgumentEffectOwner = nextDefaultArgumentEffectOwner;
  copy.nextFunctionId = nextFunctionId;
  copy.seal();
  assert(copy.sealedShape == sealedShape);
  return copy;
}

auto SemanticVisitor::Impl::StagedConstantResults::environment(
    std::vector<std::uintptr_t> identity) -> std::size_t {
  return environments.try_emplace(std::move(identity), environments.size() + 1)
      .first->second;
}

auto SemanticVisitor::Impl::StagedConstantResults::hash(const Key &key)
    -> std::size_t {
  StagedHash hash;
  hash.add(key.function);
  hash.add(key.constructor);
  hash.add(std::hash<const void *>{}(key.declaration));
  hash.type(key.constructedType);
  for (const auto &type : key.typeArguments)
    hash.type(type);
  for (const auto &value : key.valueArguments)
    hash.add(value.value);
  for (const auto &type : key.parameterTypes)
    hash.type(type);
  hash.add(key.defaultArguments.size());
  hash.add(key.designatedArguments.size());
  hash.add(static_cast<std::size_t>(key.kind));
  hash.add(key.generatedDefault);
  hash.add(key.directInitialization);
  // The substitution maps are unordered; combine their entries commutatively.
  std::size_t substitution = 0;
  for (const auto &[parameter, type] : key.substitution.types) {
    StagedHash entry;
    entry.add(parameter);
    entry.type(type);
    substitution += entry.value;
  }
  for (const auto &[parameter, value] : key.substitution.values) {
    StagedHash entry;
    entry.add(parameter);
    entry.add(value.value);
    substitution += entry.value;
  }
  hash.add(substitution);
  for (const auto &argument : key.arguments)
    hash.constant(argument);
  if (key.receiver)
    hash.constant(*key.receiver);
  hash.add(key.environment);
  return hash.value;
}

auto SemanticVisitor::Impl::StagedConstantResults::same(const Key &left,
                                                        const Key &right)
    -> bool {
  return left.function == right.function &&
         left.constructor == right.constructor &&
         left.declaration == right.declaration &&
         left.environment == right.environment && left.kind == right.kind &&
         left.generatedDefault == right.generatedDefault &&
         left.directInitialization == right.directInitialization &&
         left.constructedType == right.constructedType &&
         left.typeArguments == right.typeArguments &&
         left.valueArguments == right.valueArguments &&
         left.parameterTypes == right.parameterTypes &&
         left.defaultArguments == right.defaultArguments &&
         left.designatedArguments == right.designatedArguments &&
         left.substitution.types == right.substitution.types &&
         left.substitution.values == right.substitution.values &&
         left.arguments == right.arguments && left.receiver == right.receiver;
}

auto SemanticVisitor::Impl::StagedConstantResults::find(const Key &key) const
    -> const Result * {
  const auto bucket = results.find(hash(key));
  if (bucket == results.end())
    return nullptr;
  const auto found =
      std::find_if(bucket->second.begin(), bucket->second.end(),
                   [&](const Result &result) { return same(result.key, key); });
  return found == bucket->second.end() ? nullptr : &*found;
}

auto SemanticVisitor::Impl::StagedConstantResults::record(Result result)
    -> void {
  auto &bucket = results[hash(result.key)];
  const auto found =
      std::find_if(bucket.begin(), bucket.end(), [&](const Result &existing) {
        return same(existing.key, result.key);
      });
  if (found != bucket.end()) {
    // A deeper success proves the shallower ones too.
    found->depth = std::max(found->depth, result.depth);
    return;
  }
  if (size >= capacity)
    return;
  bucket.push_back(std::move(result));
  ++size;
}

auto SemanticVisitor::Impl::StagedConstantResults::reusable(
    const ConstantValue &value) -> bool {
  const auto *aggregate = std::get_if<ConstantAggregateValue>(&value);
  if (aggregate == nullptr || aggregate->value == nullptr)
    return true;
  const ConstantAggregate &node = *aggregate->value;
  return node.kind != ConstantAggregateKind::TransientOwner &&
         std::all_of(node.elements.begin(), node.elements.end(), reusable) &&
         std::all_of(node.fields.begin(), node.fields.end(),
                     [](const auto &field) { return reusable(field.value); });
}

auto SemanticVisitor::Impl::stagedConstantKey(
    const std::variant<const FunctionDecl *, ResolvedConstructionInfo>
        &selected,
    const FunctionInfo *function, const std::vector<ConstantValue> &arguments,
    const GenericSubstitution &substitution,
    const std::optional<ConstantValue> &receiver,
    std::vector<FunctionId> completed, std::vector<ConstructorId> constructors)
    -> std::optional<StagedConstantResults::Key> {
  if (!std::all_of(arguments.begin(), arguments.end(),
                   StagedConstantResults::reusable) ||
      (receiver && !StagedConstantResults::reusable(*receiver)))
    return std::nullopt;
  // What the evaluation may execute: the definitions and constructors its
  // worker copies (those declared before this point) and those HIR lowers.
  std::vector<std::uintptr_t> identity{concreteReanalysis() ? 1U : 0U};
  const auto append = [&](std::vector<std::uintptr_t> values) {
    std::sort(values.begin(), values.end());
    identity.push_back(values.size());
    identity.insert(identity.end(), values.begin(), values.end());
  };
  std::vector<std::uintptr_t> values;
  for (const FunctionDecl *definition : constexprDefinitionsAnalyzed)
    if (declaredBeforeCurrent(definition))
      values.push_back(reinterpret_cast<std::uintptr_t>(definition));
  append(std::move(values));
  values = {};
  for (const auto *definition : constexprConstructorsAnalyzed)
    if (declaredBeforeCurrent(definition))
      values.push_back(reinterpret_cast<std::uintptr_t>(definition));
  append(std::move(values));
  append({completed.begin(), completed.end()});
  append({constructors.begin(), constructors.end()});
  StagedConstantResults::Key key{
      .substitution = substitution,
      .arguments = arguments,
      .receiver = receiver,
      .environment =
          requestSession.stagedConstants->environment(std::move(identity))};
  if (const auto *construction =
          std::get_if<ResolvedConstructionInfo>(&selected)) {
    key.constructor = construction->constructor;
    key.declaration = construction->declaration;
    key.constructedType = construction->constructedType;
    key.typeArguments = construction->typeArguments;
    key.valueArguments = construction->valueArguments;
    key.parameterTypes = construction->parameterTypes;
    key.defaultArguments = construction->defaultArguments;
    key.designatedArguments = construction->designatedArguments;
    key.kind = construction->kind;
    key.generatedDefault = construction->generatedDefault;
    key.directInitialization = construction->directInitialization;
  } else {
    key.function = function->id;
  }
  return key;
}

auto SemanticVisitor::Impl::appendStagedWarnings(
    const std::vector<Diagnostic> &warnings) -> void {
  for (const Diagnostic &warning : warnings) {
    if (std::none_of(diagnostics.begin(), diagnostics.end(),
                     [&](const Diagnostic &existing) {
                       return existing.code == warning.code &&
                              existing.primary == warning.primary &&
                              existing.message == warning.message;
                     })) {
      diagnostics.push_back(warning);
    }
  }
}

auto SemanticVisitor::Impl::stagedWorkerFor(
    const SemanticModel &stagingModel,
    const std::shared_ptr<ConstexprWorkBudget> &work) -> Impl & {
  std::size_t definitions = 0;
  for (const FunctionDecl *definition : constexprDefinitionsAnalyzed)
    definitions += declaredBeforeCurrent(definition) ? 1 : 0;
  std::size_t constructors = 0;
  for (const auto *definition : constexprConstructorsAnalyzed)
    constructors += declaredBeforeCurrent(definition) ? 1 : 0;
  const bool reanalysis = concreteReanalysis();
  StagedWorker &worker = stagedWorker;
  if (worker.impl == nullptr || worker.reanalysis != reanalysis ||
      worker.declarationOrder != currentDeclarationOrder ||
      worker.definitions != definitions ||
      worker.constructors != constructors) {
    const PhaseTimeScope workerScope("constexpr-staged-worker");
    worker.impl = std::make_unique<Impl>(
        target, sourceGraph, false, workMetrics, sources, rawPointerAudit,
        false, TrustedBodyAnalysis::Complete, sourceDebugProfile);
    worker.impl->catalog = catalog.snapshotForInstanceAnalysis();
    worker.impl->semanticModel = stagingModel;
    worker.impl->macroLookup = macroLookup;
    worker.impl->macroLookupBase = macroLookupBase;
    worker.impl->nextExpansion = nextExpansion;
    // The copy is a snapshot that outlives this evaluation. It must not keep
    // the side channels (a late-demand probe's sink, a batch's call journal)
    // an enclosing analysis lent the canonical model for its own duration.
    worker.impl->semanticModel.setFunctionDemandSink(nullptr);
    worker.impl->semanticModel.setCallJournal(nullptr);
    worker.impl->valueFlow = valueFlow;
    for (const FunctionDecl *definition : constexprDefinitionsAnalyzed) {
      if (declaredBeforeCurrent(definition))
        worker.impl->constexprDefinitionsAnalyzed.insert(definition);
    }
    for (const auto *definition : constexprConstructorsAnalyzed) {
      if (declaredBeforeCurrent(definition))
        worker.impl->constexprConstructorsAnalyzed.insert(definition);
    }
    worker.impl->genericDefaultConstants = genericDefaultConstants;
    worker.impl->defaultArgumentSyntax = defaultArgumentSyntax;
    worker.impl->defaultArgumentDependencies = defaultArgumentDependencies;
    worker.reanalysis = reanalysis;
    worker.declarationOrder = currentDeclarationOrder;
    worker.definitions = definitions;
    worker.constructors = constructors;
  }
  worker.impl->requestSession.constexprWork = work;
  worker.impl->requestSession.stagedConstants = requestSession.stagedConstants;
  // Concrete constant bodies also demand source access summaries. Share
  // their request-local cycle/depth accounting, not mutable body state.
  worker.impl->requestSession.rawResultQueries =
      requestSession.rawResultQueries;
  // This worker can reanalyse an instance whose reanalysis is open here or
  // further up; it must see that one owns the instance's assertions.
  worker.impl->requestSession.openReanalyses = openReanalyses();
  worker.impl->nextLambdaId = nextLambdaId;
  return *worker.impl;
}

auto SemanticVisitor::Impl::executeStagedConstexpr(
    const std::variant<const FunctionDecl *, ResolvedConstructionInfo>
        &selected,
    const std::vector<ConstantValue> &arguments,
    const GenericSubstitution &substitution,
    const std::optional<ConstantValue> &receiver, const Token &callSite,
    ConstexprInterpreter &context) -> std::optional<ConstexprEvaluation> {
  if (sourceGraph == nullptr || sources == nullptr) {
    return std::nullopt;
  }
  // Nested required constants use the canonical completed catalog, never the
  // caller's partially analyzed instance delta, whatever kind of instance is
  // being reanalysed. Each worker still owns its scopes and semantic delta;
  // only the bounded work account is shared.
  const SemanticModel &stagingModel = constexprStagingModel();
  const auto *function = std::get_if<const FunctionDecl *>(&selected);
  const auto *construction = std::get_if<ResolvedConstructionInfo>(&selected);
  const auto *info =
      function && *function ? stagingModel.findFunction(**function) : nullptr;
  if ((function &&
       (info == nullptr || !constexprDefinitionsAnalyzed.contains(*function) ||
        !declaredBeforeCurrent(*function))) ||
      (construction && !construction->generatedDefault &&
       (!constexprConstructorsAnalyzed.contains(construction->declaration) ||
        !declaredBeforeCurrent(construction->declaration)))) {
    return std::nullopt;
  }
  const std::size_t enclosingDepth = context.work->stagingDepth;
  if (enclosingDepth + context.callDepth >= constexprCallDepthLimit) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::ResourceLimit,
        "Nested constexpr staging exceeded the compiler call-depth limit.");
  }
  struct StagingDepthGuard {
    ConstexprWorkBudget &work;
    std::size_t saved;
    ~StagingDepthGuard() { work.stagingDepth = saved; }
  } depthGuard{*context.work, enclosingDepth};
  context.work->stagingDepth += context.callDepth + 1;
  std::vector<FunctionId> completed;
  std::vector<ConstructorId> constructors;
  for (const auto *declaration : constexprConstructorsAnalyzed) {
    if (const auto *candidate = stagingModel.findConstructor(*declaration))
      constructors.push_back(candidate->id);
  }
  for (const auto *declaration : constexprDefinitionsAnalyzed) {
    if (const auto *candidate = stagingModel.findFunction(*declaration)) {
      completed.push_back(candidate->id);
    }
  }
  // A success this request already proved is replayed (see
  // StagedConstantResults), unless the outer authority has since closed a
  // failed concrete query that a fresh evaluation would observe.
  if (!requestSession.stagedConstants)
    requestSession.stagedConstants = std::make_shared<StagedConstantResults>();
  const auto stagedConstants = requestSession.stagedConstants;
  const auto &concreteEffects = requestSession.programConcreteEffects;
  const auto reuseKey =
      !concreteEffects || concreteEffects->invalidInstances.empty()
          ? stagedConstantKey(selected, info, arguments, substitution, receiver,
                              completed, constructors)
          : std::nullopt;
  const std::size_t depth = enclosingDepth + context.callDepth;
  if (reuseKey) {
    const auto *reused = stagedConstants->find(*reuseKey);
    if (reused != nullptr && reused->depth >= depth &&
        context.work->steps + reused->steps <= constexprExpressionStepLimit) {
      context.work->steps += reused->steps;
      appendStagedWarnings(reused->warnings);
      if (workMetrics != nullptr)
        ++workMetrics->constexprStagedReuses;
      return ConstexprEvaluation{.value = reused->value, .location = callSite};
    }
  }
  const std::size_t stepsBefore = context.work->steps;
  // Reanalysis may reset all body-local scopes and flow. Never run it on this
  // in-progress visitor. A request-local snapshot reuses the ordinary semantic
  // provider and exact declaration identities without lending it mutable caller
  // state. It lives until HIR has consumed every concrete instance delta.
  StagedInstanceProvider provider(
      *this,
      [&]() -> SemanticInstanceProvider & {
        return stagedWorkerFor(stagingModel, context.work);
      },
      [&](const StagedInstanceKeyView &request,
          SemanticInstanceAnalysis &analysis) {
        const auto &state = requestSession.programConcreteEffects;
        if (!analysis.valid() || !state || state->invalidInstances.empty())
          return;
        // The isolated staged worker deliberately does not share mutable
        // effect graphs. Validate its result against the current outer
        // authority, which may already have closed failed concrete queries.
        preserveInvalidProgramEffectDiagnostics(
            {.kind = request.kind,
             .declaration = request.declaration,
             .classTypes = std::vector<SemanticType>(request.ownerTypes.begin(),
                                                     request.ownerTypes.end()),
             .classValues = std::vector<CompileTimeValue>(
                 request.ownerValues.begin(), request.ownerValues.end()),
             .types = std::vector<SemanticType>(request.types.begin(),
                                                request.types.end()),
             .values = std::vector<CompileTimeValue>(request.values.begin(),
                                                     request.values.end())},
            analysis);
      });
  std::string stagedDetail =
      function ? info->qualifiedName : std::string("constructor");
  for (const auto &[parameter, type] : substitution.types)
    stagedDetail += " " + typeSpelling(type);
  stagedDetail += " args=" + std::to_string(arguments.size());
  const PhaseTimeScope stagedScope("constexpr-staged-mir", stagedDetail);
  HirLowerer lowerer(target);
  auto hir = [&] {
    const PhaseTimeScope lowerScope("constexpr-staged-hir");
    return construction
               ? lowerer.lowerConstantConstructor(stagingModel, provider,
                                                  *construction, completed,
                                                  constructors)
               : lowerer.lowerConstantFunction(stagingModel, provider, info->id,
                                               completed, substitution,
                                               constructors);
  }();
  if (!hir.valid()) {
    if (hasErrorDiagnostics(hir.diagnostics)) {
      diagnostics.insert(diagnostics.end(), hir.diagnostics.begin(),
                         hir.diagnostics.end());
      return constexprFailure(
          callSite, ConstantEvaluationFailure::UnsupportedExpression,
          "The concrete constexpr instance failed semantic analysis.");
    }
    return std::nullopt;
  }
  appendStagedWarnings(hir.diagnostics);
  const auto *entry = sourceGraph->findUnit(sourceGraph->entryUnit());
  if (entry == nullptr) {
    return std::nullopt;
  }
  auto metadata = [&] {
    const PhaseTimeScope metadataScope("constexpr-staged-failure-metadata");
    return FailureMetadataBuilder().build(*sourceGraph, *sources, hir.program,
                                          entry->path);
  }();
  if (!metadata.valid()) {
    return std::nullopt;
  }
  auto mir = [&] {
    const PhaseTimeScope mirScope("constexpr-staged-mir-lower");
    return MirLowerer().lower(hir.program, metadata.metadata);
  }();
  if (!mir.valid()) {
    return std::nullopt;
  }
  MirBodyAddress root;
  if (construction) {
    for (const auto &candidate : hir.program.constructorInstances()) {
      const auto *owner =
          candidate.owner &&
                  candidate.owner <= hir.program.classInstances().size()
              ? &hir.program.classInstances()[candidate.owner - 1]
              : nullptr;
      if (candidate.declaration == construction->constructor && owner &&
          owner->type == construction->constructedType &&
          candidate.typeArguments == construction->typeArguments &&
          candidate.valueArguments == construction->valueArguments) {
        root = {MirBodyKind::Constructor, candidate.id};
        break;
      }
    }
  } else {
    for (const auto &candidate : hir.program.functionInstances()) {
      if (candidate.declaration == info->id) {
        root = {MirBodyKind::Function, candidate.id};
        break;
      }
    }
  }
  const MirConstantLimits limits{
      .steps = constexprExpressionStepLimit -
               std::min(context.work->steps, constexprExpressionStepLimit),
      .callDepth = constexprCallDepthLimit - context.work->stagingDepth + 1};
  auto result = [&] {
    const PhaseTimeScope evaluateScope("constexpr-staged-evaluate");
    return construction
               ? evaluateMirConstantConstructor(mir.program, root.owner,
                                                arguments, limits)
               : evaluateMirConstantFunction(mir.program, root.owner, arguments,
                                             limits, receiver);
  }();
  context.work->steps += result.steps;
  const auto mutableLoans = [](const MirBody &body) {
    return std::any_of(
        body.loans.begin(), body.loans.end(),
        [](const auto &loan) { return loan.access == AccessMode::Mutable; });
  };
  const auto resultValues = [](const MirBody &body) {
    const auto resultType = [](const auto &self,
                               const SemanticType &type) -> bool {
      return type.kind == SemanticType::UniqueOwner ||
             type.kind == SemanticType::TypePack ||
             ((type.kind == SemanticType::Array ||
               type.kind == SemanticType::Reference) &&
              type.arguments.size() == 1 && self(self, type.arguments[0]));
    };
    return resultType(resultType, body.returnType) ||
           std::any_of(body.places.begin(), body.places.end(),
                       [&](const auto &place) {
                         return resultType(resultType, place.type);
                       }) ||
           std::any_of(body.values.begin(), body.values.end(),
                       [&](const auto &value) {
                         return resultType(resultType, value.info.type);
                       });
  };
  // The compatibility walker does not model a payload enum's active
  // alternative, so a body holding one requires the MIR profile. A type that
  // only names an enum, such as a reflected payload-field metaobject, does not.
  const auto payloadValues = [&](const MirBody &body) {
    const auto payload = [&](const SemanticType &type) {
      return type.kind == SemanticType::Enum &&
             mir.program.findPayloadEnum(type) != nullptr;
    };
    return payload(body.returnType) ||
           std::any_of(
               body.places.begin(), body.places.end(),
               [&](const auto &place) { return payload(place.type); }) ||
           std::any_of(
               body.values.begin(), body.values.end(),
               [&](const auto &value) { return payload(value.info.type); });
  };
  const bool requiresMir =
      std::any_of(
          mir.program.functionInstances().begin(),
          mir.program.functionInstances().end(),
          [&](const auto &function) { return payloadValues(function.body); }) ||
      std::any_of(mir.program.constructorInstances().begin(),
                  mir.program.constructorInstances().end(),
                  [&](const auto &constructor) {
                    return payloadValues(constructor.body);
                  }) ||
      !mir.program.destructorInstances().empty() ||
      std::any_of(
          mir.program.functionInstances().begin(),
          mir.program.functionInstances().end(),
          [&](const auto &function) {
            const auto mutableReference = [](const SemanticType &type) {
              return type.kind == SemanticType::Reference &&
                     type.referenceAccess == AccessMode::Mutable;
            };
            return function.constexprFunction &&
                   function.definitionKind == MirDefinitionKind::Source &&
                   (function.receiverMutability !=
                        ReceiverMutability::ReadOnly ||
                    function.returnType == SemanticType::Void ||
                    mutableReference(function.returnType) ||
                    std::any_of(function.parameterTypes.begin(),
                                function.parameterTypes.end(),
                                mutableReference) ||
                    mutableLoans(function.body) ||
                    resultValues(function.body) ||
                    std::any_of(function.body.dropObligations.begin(),
                                function.body.dropObligations.end(),
                                [](const auto &drop) {
                                  return drop.dropType.requiresActiveCleanup;
                                }));
          }) ||
      std::any_of(
          mir.program.constructorInstances().begin(),
          mir.program.constructorInstances().end(),
          [&](const auto &constructor) {
            if (mutableLoans(constructor.body) ||
                resultValues(constructor.body) ||
                std::any_of(constructor.parameterTypes.begin(),
                            constructor.parameterTypes.end(),
                            [](const auto &type) {
                              return type.kind == SemanticType::Reference;
                            }))
              return true;
            for (const auto &block : constructor.body.blocks) {
              for (const auto &instruction : block.instructions) {
                if ((instruction.kind == MirInstructionKind::Assign ||
                     instruction.kind == MirInstructionKind::Modify) &&
                    instruction.destination) {
                  const auto *place =
                      constructor.body.findPlace(*instruction.destination);
                  if (place && place->root == MirPlaceRootKind::This)
                    return true;
                }
              }
            }
            return false;
          });
  // Until the ordinary MIR profile covers aggregates/references and every
  // existing statement, retain the legacy driver's admitted subset. Work
  // already executed still consumes the shared budget; arithmetic and resource
  // failures are authoritative and must never be retried in the AST driver.
  if (result.failure == ConstantEvaluationFailure::UnsupportedExpression ||
      result.failure == ConstantEvaluationFailure::UnsupportedType) {
    if (!requiresMir)
      return std::nullopt;
    // The compatibility walker does not own reference or receiver mutation.
    // Retrying a value-copy interpretation could silently lose a caller write.
    result.detail =
        "This constexpr call requires the verified MIR execution profile. " +
        result.detail;
  }
  if (workMetrics != nullptr) {
    ++workMetrics->constexprMirExecutions;
  }
  if (result.voidResult)
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedType,
        "A void constexpr call does not produce a constant value.");
  Token location = callSite;
  const auto *mirBody = findMirBody(mir.program, result.body);
  const HirBody *hirBody = nullptr;
  if (result.body.kind == MirBodyKind::Function) {
    if (const auto *function =
            hir.program.findFunctionInstance(result.body.owner))
      hirBody = &function->body;
  } else if (result.body.kind == MirBodyKind::Constructor) {
    if (const auto *constructor =
            hir.program.findConstructorInstance(result.body.owner))
      hirBody = &constructor->body;
  } else if (result.body.kind == MirBodyKind::Destructor) {
    if (const auto *destructor =
            hir.program.findDestructorInstance(result.body.owner))
      hirBody = &destructor->body;
  }
  if (mirBody != nullptr && hirBody != nullptr) {
    if (const auto *block = mirBody->findBlock(result.block)) {
      for (const auto &instruction : block->instructions) {
        if (instruction.id == result.instruction) {
          const auto *value = hirBody->findValue(instruction.hirValue);
          if (value != nullptr && value->source != nullptr) {
            location = expressionToken(*value->source);
          }
          break;
        }
      }
    }
  }
  if (result) {
    if (reuseKey && StagedConstantResults::reusable(*result.value))
      stagedConstants->record({.key = *reuseKey,
                               .value = *result.value,
                               .steps = context.work->steps - stepsBefore,
                               .depth = depth,
                               .warnings = hir.diagnostics});
    return ConstexprEvaluation{.value = std::move(result.value),
                               .location = callSite};
  }
  // Library internals are not the user's to fix: a failure executed inside a
  // compiler-trusted unit is reported at the evaluation site that required
  // it, with the library location kept as a note (the same policy as
  // concrete-instance diagnostics).
  auto failure =
      constexprFailure(location, result.failure, std::move(result.detail));
  if (!location.lexeme.empty() && isCompilerTrustedRequester(location) &&
      !isCompilerTrustedRequester(callSite)) {
    failure.libraryLocation = location;
    failure.location = callSite;
  }
  return failure;
}

} // namespace lang
