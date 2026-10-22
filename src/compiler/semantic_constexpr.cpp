#include "semantic_analyzer_impl.h"

#include "gti/failure_metadata.h"
#include "gti/hir.h"
#include "gti/mir_constant_evaluator.h"
#include "gti/mir_lowering.h"

#include <algorithm>
#include <functional>

namespace lang {
namespace {
// Read-only traits do not require a fork. Allocate the isolated analysis
// snapshot only if ordinary HIR discovery actually requests an instance.
class StagedInstanceProvider final : public SemanticInstanceProvider {
public:
  StagedInstanceProvider(const SemanticInstanceProvider &facts,
                         std::function<SemanticInstanceProvider &()> acquire)
      : facts(facts), acquire(std::move(acquire)) {}
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
    return acquire().analyzeFunctionInstance(id, ownerTypes, ownerValues, types,
                                             values);
  }
  SemanticInstanceAnalysis analyzeConstructorInstance(
      ConstructorId id, const std::vector<SemanticType> &ownerTypes,
      const std::vector<CompileTimeValue> &ownerValues,
      const std::vector<SemanticType> &types,
      const std::vector<CompileTimeValue> &values) override {
    return acquire().analyzeConstructorInstance(id, ownerTypes, ownerValues,
                                                types, values);
  }
  SemanticInstanceAnalysis analyzeDestructorInstance(
      ClassId id, const std::vector<SemanticType> &types,
      const std::vector<CompileTimeValue> &values) override {
    return acquire().analyzeDestructorInstance(id, types, values);
  }
  SemanticInstanceAnalysis analyzeClassFieldInitializers(
      ClassId id, const std::vector<SemanticType> &types,
      const std::vector<CompileTimeValue> &values) override {
    return acquire().analyzeClassFieldInitializers(id, types, values);
  }

private:
  const SemanticInstanceProvider &facts;
  std::function<SemanticInstanceProvider &()> acquire;
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
  for (const auto &declaration : callableDeclarations) {
    (void)copy.registerCallable(*declaration);
  }
  copy.constructorGenericParameters = constructorGenericParameters;
  copy.rootNativeStorageSymbols = rootNativeStorageSymbols;
  copy.globalBorrowStorage = globalBorrowStorage;
  copy.genericConstraints = genericConstraints;
  copy.genericConstraintConcepts = genericConstraintConcepts;
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
    worker.impl = std::make_unique<Impl>(target, sourceGraph, false,
                                         workMetrics, sources, rawPointerAudit);
    worker.impl->catalog = catalog.snapshotForInstanceAnalysis();
    worker.impl->semanticModel = stagingModel;
    worker.impl->deferredLookup = deferredLookup;
    worker.impl->deferredLookupBase = deferredLookupBase;
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
  // Reanalysis may reset all body-local scopes and flow. Never run it on this
  // in-progress visitor. A request-local snapshot reuses the ordinary semantic
  // provider and exact declaration identities without lending it mutable caller
  // state. It lives until HIR has consumed every concrete instance delta.
  StagedInstanceProvider provider(*this, [&]() -> SemanticInstanceProvider & {
    return stagedWorkerFor(stagingModel, context.work);
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
  for (const Diagnostic &warning : hir.diagnostics) {
    if (std::none_of(diagnostics.begin(), diagnostics.end(),
                     [&](const Diagnostic &existing) {
                       return existing.code == warning.code &&
                              existing.primary == warning.primary &&
                              existing.message == warning.message;
                     })) {
      diagnostics.push_back(warning);
    }
  }
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
  if (result)
    return ConstexprEvaluation{.value = std::move(result.value),
                               .location = callSite};
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
