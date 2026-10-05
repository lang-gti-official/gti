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
  // caller's partially analyzed instance delta. Each worker still owns its
  // scopes and semantic delta; only the bounded work account is shared.
  const SemanticModel &stagingModel =
      instanceBaseModel ? *instanceBaseModel : semanticModel;
  const auto *function = std::get_if<const FunctionDecl *>(&selected);
  const auto *construction = std::get_if<ResolvedConstructionInfo>(&selected);
  const auto *info =
      function && *function ? stagingModel.findFunction(**function) : nullptr;
  if ((function && (info == nullptr ||
                    !constexprDefinitionsAnalyzed.contains(*function))) ||
      (construction &&
       !constexprConstructorsAnalyzed.contains(construction->declaration))) {
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
  std::unique_ptr<Impl> worker;
  StagedInstanceProvider provider(*this, [&]() -> SemanticInstanceProvider & {
    if (!worker) {
      worker =
          std::make_unique<Impl>(target, sourceGraph, false, nullptr, sources);
      worker->catalog = catalog.snapshotForInstanceAnalysis();
      worker->semanticModel = stagingModel;
      worker->valueFlow = valueFlow;
      worker->constexprDefinitionsAnalyzed = constexprDefinitionsAnalyzed;
      worker->constexprConstructorsAnalyzed = constexprConstructorsAnalyzed;
      worker->requestSession.constexprWork = context.work;
      // Concrete constant bodies also demand source access summaries. Share
      // their request-local cycle/depth accounting, not mutable body state.
      worker->requestSession.rawResultQueries = requestSession.rawResultQueries;
      worker->genericDefaultConstants = genericDefaultConstants;
      worker->defaultArgumentSyntax = defaultArgumentSyntax;
      worker->defaultArgumentDependencies = defaultArgumentDependencies;
      worker->nextLambdaId = nextLambdaId;
    }
    return *worker;
  });
  HirLowerer lowerer(target);
  auto hir = construction
                 ? lowerer.lowerConstantConstructor(stagingModel, provider,
                                                    *construction, completed,
                                                    constructors)
                 : lowerer.lowerConstantFunction(stagingModel, provider,
                                                 info->id, completed,
                                                 substitution, constructors);
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
  auto metadata = FailureMetadataBuilder().build(*sourceGraph, *sources,
                                                 hir.program, entry->path);
  if (!metadata.valid()) {
    return std::nullopt;
  }
  auto mir = MirLowerer().lower(hir.program, metadata.metadata);
  if (!mir.valid()) {
    return std::nullopt;
  }
  MirBodyAddress root;
  if (construction) {
    for (const auto &candidate : hir.program.constructorInstances()) {
      if (candidate.declaration == construction->constructor) {
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
  auto result = construction
                    ? evaluateMirConstantConstructor(mir.program, root.owner,
                                                     arguments, limits)
                    : evaluateMirConstantFunction(mir.program, root.owner,
                                                  arguments, limits, receiver);
  context.work->steps += result.steps;
  // Until the ordinary MIR profile covers aggregates/references and every
  // existing statement, retain the legacy driver's admitted subset. Work
  // already executed still consumes the shared budget; arithmetic and resource
  // failures are authoritative and must never be retried in the AST driver.
  if (result.failure == ConstantEvaluationFailure::UnsupportedExpression ||
      result.failure == ConstantEvaluationFailure::UnsupportedType) {
    return std::nullopt;
  }
  if (workMetrics != nullptr) {
    ++workMetrics->constexprMirExecutions;
  }
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
  return result ? ConstexprEvaluation{.value = std::move(result.value),
                                      .location = callSite}
                : constexprFailure(location, result.failure,
                                   std::move(result.detail));
}

} // namespace lang
