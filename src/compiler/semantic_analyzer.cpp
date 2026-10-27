#include "semantic_analyzer_impl.h"

namespace lang {

SemanticVisitor::SemanticVisitor(
    TargetInfo target, const SourceGraph *sourceGraph, bool toolingOccurrences,
    FrontendWorkMetrics *workMetrics, const SourceManager *sources,
    RawPointerAuditMode rawPointerAudit, bool requireFunctionDefinitions,
    TrustedBodyAnalysis trustedBodies, SourceDebugProfile sourceDebugProfile)
    : impl(std::make_unique<Impl>(std::move(target), sourceGraph,
                                  toolingOccurrences, workMetrics, sources,
                                  rawPointerAudit, requireFunctionDefinitions,
                                  trustedBodies, sourceDebugProfile)) {}

SemanticVisitor::~SemanticVisitor() = default;
SemanticVisitor::SemanticVisitor(SemanticVisitor &&) noexcept = default;
SemanticVisitor &
SemanticVisitor::operator=(SemanticVisitor &&) noexcept = default;

bool SemanticVisitor::check(const Program &program) {
  try {
    return impl->check(program);
  } catch (const Impl::AnalysisCancelled &) {
    impl->analysisCancelled = true;
    return false;
  }
}

void SemanticVisitor::setCancellation(const std::atomic<bool> *cancellation) {
  impl->cancellation = cancellation;
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
