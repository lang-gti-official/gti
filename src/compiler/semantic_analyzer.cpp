#include "semantic_analyzer_impl.h"
#include <llvm/ADT/ScopeExit.h>

namespace lang {

SemanticVisitor::SemanticVisitor(
    TargetInfo target, const SourceGraph *sourceGraph, bool toolingOccurrences,
    FrontendWorkMetrics *workMetrics, const SourceManager *sources,
    RawPointerAuditMode rawPointerAudit, bool requireFunctionDefinitions,
    TrustedBodyAnalysis trustedBodies, SourceDebugProfile sourceDebugProfile,
    CompileLimits compileLimits)
    : impl(std::make_unique<Impl>(
          std::move(target), sourceGraph, toolingOccurrences, workMetrics,
          sources, rawPointerAudit, requireFunctionDefinitions, trustedBodies,
          sourceDebugProfile, compileLimits)) {}

SemanticVisitor::~SemanticVisitor() = default;
SemanticVisitor::SemanticVisitor(SemanticVisitor &&) noexcept = default;
SemanticVisitor &
SemanticVisitor::operator=(SemanticVisitor &&) noexcept = default;

auto SemanticVisitor::Impl::freshRequest() const -> std::unique_ptr<Impl> {
  auto fresh = std::make_unique<Impl>(target, sourceGraph, toolingOccurrences,
                                      workMetrics, sources, rawPointerAudit,
                                      requireFunctionDefinitions, trustedBodies,
                                      sourceDebugProfile, compileLimits);
  fresh->suggestEnumSwitchCases = suggestEnumSwitchCases;
  fresh->cancellation = cancellation;
  fresh->memberCompletionQuery = memberCompletionQuery;
  fresh->reuseConstantEvaluations = reuseConstantEvaluations;
  fresh->deferConstexprBodies = deferConstexprBodies;
  fresh->requiredFunctionBodies = requiredFunctionBodies;
  return fresh;
}

void SemanticVisitor::Impl::finishLimitedAnalysis() {
  for (auto &held : heldDiagnostics)
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(held.begin()),
                       std::make_move_iterator(held.end()));
  heldDiagnostics.clear();
  enforceErrorLimit = false;
}

bool SemanticVisitor::check(const Program &program,
                            const EntryDiagnosticObserver &observer,
                            const std::function<void()> &onRegistered) {
  // The lexical pass starts only after a preamble has passed admission.
  if (observer)
    impl->entryDiagnosticObserver = &observer;
  if (onRegistered)
    impl->registrationObserver = &onRegistered;
  llvm::scope_exit clear([this] {
    impl->entryDiagnosticObserver = nullptr;
    impl->registrationObserver = nullptr;
  });
  return check(program);
}

void SemanticVisitor::setMemberCompletionQuery(bool enabled) {
  impl->memberCompletionQuery = enabled;
  impl->concreteMemberCapture.reset();
}

bool SemanticVisitor::check(const Program &program) {
  impl->enforceErrorLimit = true;
  try {
    const bool resuming = impl->adoptedPreamble != nullptr;
    const bool valid = impl->check(program);
    if (!valid && resuming && impl->adoptedPreamble == nullptr) {
      // A resumed declaration invalidated an already analyzed prefix global.
      // Discard every partial semantic/value/effect fact and request journal;
      // the immutable cached prefix itself remains available to later edits.
      impl = impl->freshRequest();
      impl->enforceErrorLimit = true;
      const bool valid = impl->check(program);
      impl->enforceErrorLimit = false;
      return valid;
    }
    impl->enforceErrorLimit = false;
    return valid;
  } catch (const Impl::ErrorLimitReached &) {
    impl->finishLimitedAnalysis();
    return false;
  } catch (const Impl::AnalysisCancelled &) {
    impl->enforceErrorLimit = false;
    impl->analysisCancelled = true;
    return false;
  }
}

void SemanticVisitor::setEnumSwitchCaseSuggestions(bool enabled) {
  impl->suggestEnumSwitchCases = enabled;
}

void SemanticVisitor::setCancellation(const std::atomic<bool> *cancellation) {
  impl->cancellation = cancellation;
}

void SemanticVisitor::setConstantEvaluationReuse(bool reuse) {
  impl->reuseConstantEvaluations = reuse;
}

void SemanticVisitor::setConstexprBodyDeferral(bool defer) {
  impl->deferConstexprBodies = defer;
}

bool SemanticVisitor::cancelled() const { return impl->analysisCancelled; }

void SemanticVisitor::requireFunctionBodies(
    const std::unordered_set<const FunctionDecl *> &bodies) {
  impl->requiredFunctionBodies = bodies;
}

bool SemanticVisitor::check(const Expr &expr) { return impl->check(expr); }

bool SemanticVisitor::analyzeDemandedPrimaryBody(const FunctionDecl &body) {
  const FunctionDecl *roots[] = {&body};
  return impl->analyzeLateDemand(roots);
}

bool SemanticVisitor::lateDemandFailed() const {
  return impl->lateDemandFailed;
}

std::vector<const FunctionDecl *> SemanticVisitor::unmetBodyDemands() const {
  return impl->unmetBodyDemandList();
}

std::vector<const FunctionDecl *> SemanticVisitor::lateDemandedBodies() const {
  return impl->lateDemandedBodies;
}

bool SemanticVisitor::applyProgramRequest(const Program &program,
                                          const ProgramRequest &request) {
  return impl->applyProgramRequest(program, request);
}

bool SemanticVisitor::hadError() const { return impl->hadError(); }

const std::vector<SemanticDiagnostic> &SemanticVisitor::errors() const {
  return impl->errors();
}

SemanticType SemanticVisitor::expressionType() const {
  return impl->expressionType();
}

const SemanticModel &SemanticVisitor::model() const { return impl->model(); }

SemanticModel SemanticVisitor::takeModel() { return impl->takeModel(); }

void SemanticVisitor::detachBodyQueryContext() {
  impl->detachFromCaller();
  impl->analyzedProgram = nullptr;
}

std::optional<std::vector<Diagnostic>> SemanticVisitor::diagnoseEditedBody(
    const EditedFunctionBody &input,
    const std::atomic<bool> *cancellation) const {
  return impl->diagnoseEditedBody(input, cancellation);
}

std::optional<std::vector<Diagnostic>>
SemanticVisitor::diagnoseRegisteredLookup(
    const Program &program, const SourceManager &sources, const Token &token,
    const FunctionDecl *function, const std::atomic<bool> *cancellation) const {
  return impl->diagnoseRegisteredLookup(program, sources, token, function,
                                        cancellation);
}

std::optional<std::vector<Diagnostic>>
SemanticVisitor::Impl::diagnoseRegisteredLookup(
    const Program &program, const SourceManager &sources, const Token &token,
    const FunctionDecl *function, const std::atomic<bool> *cancellation) const {
  if (function) {
    if (function->isConstexpr() || function->isMacro() ||
        function->isProceduralMacro())
      return std::nullopt;
    EditedFunctionBody input{program, model(),   *sourceGraph,
                             sources, *function, *function->body(),
                             {}};
    return diagnoseEditedBody(input, cancellation, &token);
  }
  for (const auto &check : conformanceTypeChecks) {
    const auto &extent = check.declaration->extent();
    if (check.sourceUnit != sourceGraph->entryUnit() || !extent ||
        token.position < extent->start || token.position >= extent->end)
      continue;
    // The ordinary validator establishes this block's binders, enclosing
    // namespace, requirements and associated types in the isolated worker.
    auto worker = unevaluatedExpressionWorker(&model());
    worker->conformanceTypeChecks = {check};
    worker->cancellation = cancellation;
    DeclarationOrderScope order(*worker, *check.declaration);
    try {
      worker->validateConformanceTypes(*check.declaration);
    } catch (const AnalysisCancelled &) {
      return std::nullopt;
    } catch (const ErrorLimitReached &) {
      return std::nullopt;
    }
    return worker->lookupDiagnosticAt(token, check.sourceUnit);
  }
  return std::nullopt;
}

std::optional<std::vector<Diagnostic>>
SemanticVisitor::Impl::lookupDiagnosticAt(const Token &token,
                                          SourceUnitId unit) {
  if (!unmetBodyDemandList().empty())
    return std::nullopt;
  const auto site = tokenSpan(token);
  const auto first = std::ranges::find(diagnostics, DiagnosticSeverity::Error,
                                       &Diagnostic::severity);
  // Recovery from an earlier error can change which expression is checked.
  if (first != diagnostics.end() && first->primary == site &&
      (first->localNameRepair ||
       ((first->missingIncludeName == token.lexeme ||
         first->missingEnumerator) &&
        first->related.empty() && first->fixes.empty())))
    return std::vector<Diagnostic>{*first};
  // A resolved reference can withdraw only a missing-name finding for this
  // occurrence. Later errors, including pending consteval work, belong to
  // their own expressions and the ordinary full check.
  semanticModel.finalizeOccurrences();
  const auto *bound = model().database().findOccurrence(unit, token.position);
  if (bound && bound->span == site && bound->symbol &&
      hasRole(bound->roles, OccurrenceRole::Reference))
    return std::vector<Diagnostic>{};
  return std::nullopt;
}

std::optional<std::vector<Diagnostic>>
SemanticVisitor::Impl::diagnoseEditedBody(const EditedFunctionBody &input,
                                          const std::atomic<bool> *cancellation,
                                          const Token *lookup) const {
  const auto *function = input.model.findFunction(input.declaration);
  if (!function || function->declaration != &input.declaration ||
      function->bodyDeclaration() != &input.declaration)
    return std::nullopt;
  const bool generic =
      !function->genericParameters.empty() ||
      (function->ownerClass &&
       !classInfo(function->ownerClass).genericParameters.empty());
  if (!generic &&
      (!function->requirements.empty() ||
       function->returnType.kind == SemanticType::Reference ||
       function->returnBorrowOrigin != BorrowOriginKind::None ||
       !function->returnBorrowFields.empty() || function->dependentTextReturn))
    return std::nullopt;
  auto worker = unevaluatedExpressionWorker(&input.model);
  worker->sourceGraph = &input.graph;
  worker->sources = &input.sources;
  worker->suggestEnumSwitchCases = suggestEnumSwitchCases && !lookup;
  worker->analyzedProgram = &input.program;
  worker->cancellation = cancellation;
  const std::optional<ClassId> owner =
      function->ownerClass ? std::optional<ClassId>(function->ownerClass)
                           : std::nullopt;
  worker->enterDeclarationContext(function->namespaceScope,
                                  function->sourceUnit, owner);
  Impl::DeclarationOrderScope order(*worker, input.declaration);
  try {
    if (generic) {
      worker->beginTypeParameterScope(
          definitionOwnerParametersFor(input.declaration));
      worker->beginTypeParameterScope(genericParametersFor(input.declaration));
      if (!worker->beginRequirementScope(function))
        return std::nullopt;
    }
    worker->analyzeFunctionBody(input.declaration, function, false, true, false,
                                &input.body);
  } catch (const Impl::AnalysisCancelled &) {
    return std::nullopt;
  } catch (const Impl::ErrorLimitReached &) {
    return std::nullopt;
  }
  if (lookup)
    return worker->lookupDiagnosticAt(*lookup, function->sourceUnit);
  const auto &checked = *input.body.extent();
  // This worker reparsed the body and reused an unchanged header. Accept a
  // lexical spelling repair only if the recorded binding still spells the
  // replacement inside that function in the edited text.
  const auto localEvidence = [&](const Diagnostic &d) {
    if (d.related.empty() && d.fixes.empty())
      return true;
    if (!d.localNameRepair || d.related.size() != 1 || d.fixes.size() != 1 ||
        d.fixes.front().span != d.primary)
      return false;
    const auto &declaration = d.related.front().span;
    const auto *text = input.sources.find(checked.source);
    return text && input.declaration.extent() &&
           declaration.source == checked.source &&
           declaration.start >= input.declaration.extent()->start &&
           declaration.start <= declaration.end &&
           declaration.end <= checked.end && declaration.end <= text->size() &&
           std::string_view(*text).substr(
               declaration.start, declaration.end - declaration.start) ==
               d.fixes.front().replacement;
  };
  if (generic) {
    // An open generic can diagnose failed lexical lookup. Only a concrete
    // instance can establish that its instantiated body passes verification.
    std::vector<Diagnostic> missing;
    for (const auto &d : worker->diagnostics) {
      if ((d.code == "GTI-S2001" || d.missingEnumerator) &&
          d.severity == DiagnosticSeverity::Error &&
          d.primary.source == checked.source &&
          d.primary.start >= checked.start && d.primary.end <= checked.end &&
          localEvidence(d))
        missing.push_back(d);
    }
    if (missing.empty())
      return std::nullopt;
    return missing;
  }
  // Some body checks point at the function name (for example a path
  // that falls through). The declaration header has identical source.
  const auto &name = input.declaration.operatorName()
                         ? input.declaration.operatorName()->keyword
                         : input.declaration.name();
  for (const auto &diagnostic : worker->diagnostics) {
    // Diagnostics about other bodies, contracts or generated inputs belong
    // to the authoritative whole-snapshot run.
    const bool inBody = diagnostic.primary.start >= checked.start &&
                        diagnostic.primary.end <= checked.end;
    // The scalar switch hint is proved by this reparse. Its only edit must
    // insert inside the same body; it cannot stand in for borrow verification.
    if (diagnostic.code == "GTI-S2130" && inBody &&
        diagnostic.primary.source == checked.source &&
        diagnostic.related.empty() && diagnostic.fixes.size() == 1) {
      const auto &edit = diagnostic.fixes.front().span;
      if (edit.source == checked.source && edit.start == edit.end &&
          edit.start >= checked.start && edit.end <= checked.end)
        continue;
    }
    if (diagnostic.severity != DiagnosticSeverity::Error ||
        diagnostic.primary.source != checked.source ||
        (!inBody && diagnostic.primary != tokenSpan(name)) ||
        !localEvidence(diagnostic))
      return std::nullopt;
  }
  if (!hasErrorDiagnostics(worker->diagnostics)) {
    if (input.verify) {
      auto verified = input.verify(worker->model(), *worker, function->id);
      // Match full publication order: semantic findings precede MIR findings.
      if (verified)
        verified->insert(verified->begin(), worker->diagnostics.begin(),
                         worker->diagnostics.end());
      return verified;
    }
    if (!worker->diagnostics.empty())
      return std::nullopt;
  }
  return worker->diagnostics;
}

SemanticTypeTraits SemanticVisitor::traitsFor(const SemanticType &type) const {
  return impl->traitsFor(type);
}

bool SemanticVisitor::requiresActiveCleanupFor(const SemanticType &type) const {
  return impl->requiresActiveCleanupFor(type);
}

bool SemanticVisitor::lifecycleBoundBorrowCarrierFor(
    const SemanticType &type) const {
  return impl->lifecycleBoundBorrowCarrierFor(type);
}

std::optional<EnumTypeInfo>
SemanticVisitor::enumTypeFor(const SemanticType &type) const {
  return impl->enumTypeFor(type);
}

std::optional<ScopedThreadTaskShape>
SemanticVisitor::scopedThreadTaskShapeFor(const SemanticType &type) {
  return impl->scopedThreadTaskShapeFor(type);
}

void SemanticVisitor::useInstanceAnalysisCache(
    std::shared_ptr<InstanceAnalysisCache> cache, std::string context) {
  impl->instanceCache = std::move(cache);
  impl->instanceCacheContext = std::move(context);
}

void SemanticVisitor::publishInstanceAnalyses() {
  impl->publishInstanceAnalyses();
}

SemanticInstanceAnalysis SemanticVisitor::analyzeFunctionInstance(
    FunctionId functionId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &functionTypeArguments,
    const std::vector<CompileTimeValue> &functionValueArguments) {
  InstanceAnalysisKey key{.kind = InstanceAnalysisKind::Function,
                          .declaration = functionId,
                          .classTypes = classTypeArguments,
                          .classValues = classValueArguments,
                          .types = functionTypeArguments,
                          .values = functionValueArguments};
  return impl->analyzeReusableInstance(std::move(key), [&] {
    return impl->analyzeFunctionInstance(
        functionId, classTypeArguments, classValueArguments,
        functionTypeArguments, functionValueArguments);
  });
}

SemanticInstanceAnalysis SemanticVisitor::analyzeConstructorInstance(
    ConstructorId constructorId,
    const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &constructorTypeArguments,
    const std::vector<CompileTimeValue> &constructorValueArguments) {
  InstanceAnalysisKey key{.kind = InstanceAnalysisKind::Constructor,
                          .declaration = constructorId,
                          .classTypes = classTypeArguments,
                          .classValues = classValueArguments,
                          .types = constructorTypeArguments,
                          .values = constructorValueArguments};
  return impl->analyzeReusableInstance(std::move(key), [&] {
    return impl->analyzeConstructorInstance(
        constructorId, classTypeArguments, classValueArguments,
        constructorTypeArguments, constructorValueArguments);
  });
}

SemanticInstanceAnalysis SemanticVisitor::analyzeDestructorInstance(
    ClassId classId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments) {
  InstanceAnalysisKey key{.kind = InstanceAnalysisKind::Destructor,
                          .declaration = classId,
                          .classTypes = classTypeArguments,
                          .classValues = classValueArguments};
  return impl->analyzeReusableInstance(std::move(key), [&] {
    return impl->analyzeDestructorInstance(classId, classTypeArguments,
                                           classValueArguments);
  });
}

SemanticInstanceAnalysis SemanticVisitor::analyzeClassFieldInitializers(
    ClassId classId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments) {
  InstanceAnalysisKey key{.kind = InstanceAnalysisKind::ClassFieldInitializers,
                          .declaration = classId,
                          .classTypes = classTypeArguments,
                          .classValues = classValueArguments};
  return impl->analyzeReusableInstance(std::move(key), [&] {
    return impl->analyzeClassFieldInitializers(classId, classTypeArguments,
                                               classValueArguments);
  });
}

} // namespace lang
