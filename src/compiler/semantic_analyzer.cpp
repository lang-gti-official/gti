#include "semantic_analyzer_impl.h"

namespace lang {

SemanticVisitor::SemanticVisitor(
    TargetInfo target, const SourceGraph *sourceGraph, bool toolingOccurrences,
    FrontendWorkMetrics *workMetrics, const SourceManager *sources,
    RawPointerAuditMode rawPointerAudit, bool requireFunctionDefinitions,
    TrustedBodyAnalysis trustedBodies)
    : impl(std::make_unique<Impl>(std::move(target), sourceGraph,
                                  toolingOccurrences, workMetrics, sources,
                                  rawPointerAudit, requireFunctionDefinitions,
                                  trustedBodies)) {}

SemanticVisitor::~SemanticVisitor() = default;
SemanticVisitor::SemanticVisitor(SemanticVisitor &&) noexcept = default;
SemanticVisitor &
SemanticVisitor::operator=(SemanticVisitor &&) noexcept = default;

bool SemanticVisitor::check(const Program &program) {
  return impl->check(program);
}

void SemanticVisitor::requireFunctionBodies(
    const std::unordered_set<const FunctionDecl *> &bodies) {
  impl->requiredFunctionBodies = bodies;
}

bool SemanticVisitor::check(const Expr &expr) { return impl->check(expr); }

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

std::optional<EnumTypeInfo>
SemanticVisitor::enumTypeFor(const SemanticType &type) const {
  return impl->enumTypeFor(type);
}

std::optional<ScopedThreadTaskShape>
SemanticVisitor::scopedThreadTaskShapeFor(const SemanticType &type) {
  return impl->scopedThreadTaskShapeFor(type);
}

SemanticInstanceAnalysis SemanticVisitor::analyzeFunctionInstance(
    FunctionId functionId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &functionTypeArguments,
    const std::vector<CompileTimeValue> &functionValueArguments) {
  return impl->analyzeFunctionInstance(
      functionId, classTypeArguments, classValueArguments,
      functionTypeArguments, functionValueArguments);
}

SemanticInstanceAnalysis SemanticVisitor::analyzeConstructorInstance(
    ConstructorId constructorId,
    const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &constructorTypeArguments,
    const std::vector<CompileTimeValue> &constructorValueArguments) {
  return impl->analyzeConstructorInstance(
      constructorId, classTypeArguments, classValueArguments,
      constructorTypeArguments, constructorValueArguments);
}

SemanticInstanceAnalysis SemanticVisitor::analyzeDestructorInstance(
    ClassId classId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments) {
  return impl->analyzeDestructorInstance(classId, classTypeArguments,
                                         classValueArguments);
}

SemanticInstanceAnalysis SemanticVisitor::analyzeClassFieldInitializers(
    ClassId classId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments) {
  return impl->analyzeClassFieldInitializers(classId, classTypeArguments,
                                             classValueArguments);
}

} // namespace lang
