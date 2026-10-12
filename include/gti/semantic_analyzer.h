#pragma once

#include "gti/semantic_model.h"

#include <memory>

namespace lang {

class SourceManager;

// Narrow mutable capability used only by HIR's concrete-instance discovery.
// Consumers receive the immutable base SemanticModel separately and cannot
// initiate a fresh program/expression analysis through this interface.
class SemanticInstanceProvider {
public:
  virtual ~SemanticInstanceProvider() = default;

  [[nodiscard]] virtual SemanticTypeTraits
  traitsFor(const SemanticType &type) const = 0;
  [[nodiscard]] virtual bool
  requiresActiveCleanupFor(const SemanticType &type) const = 0;
  // Concrete enum descriptions are produced by semantic generic substitution;
  // HIR must not reinterpret the declaration's dependent payload syntax.
  [[nodiscard]] virtual std::optional<EnumTypeInfo>
  enumTypeFor(const SemanticType &) const {
    return std::nullopt;
  }

  // A borrowed nominal value with executable cleanup must retain its external
  // dependencies until that cleanup completes, not merely its last source use.
  // Query concrete semantic facts so generic field substitutions participate.
  [[nodiscard]] bool
  lifecycleBoundBorrowCarrierFor(const SemanticType &type) const {
    return type.kind == SemanticType::Class &&
           traitsFor(type).containsBorrowedState &&
           requiresActiveCleanupFor(type);
  }

  [[nodiscard]] virtual SemanticInstanceAnalysis analyzeFunctionInstance(
      FunctionId functionId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &functionTypeArguments,
      const std::vector<CompileTimeValue> &functionValueArguments) = 0;
  [[nodiscard]] virtual SemanticInstanceAnalysis analyzeConstructorInstance(
      ConstructorId constructorId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &constructorTypeArguments,
      const std::vector<CompileTimeValue> &constructorValueArguments) = 0;
  [[nodiscard]] virtual SemanticInstanceAnalysis analyzeDestructorInstance(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) = 0;
  [[nodiscard]] virtual SemanticInstanceAnalysis analyzeClassFieldInitializers(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) = 0;

  // Concrete discovery selected a primary body that bounded (Reachable)
  // analysis deferred. A provider that can complete that body's primary
  // analysis in the base model now (late demand) does so and returns true;
  // otherwise the caller reports the body as a semantic request so a fresh
  // analysis includes it.
  [[nodiscard]] virtual bool analyzeDemandedPrimaryBody(const FunctionDecl &) {
    return false;
  }
};

class SemanticVisitor final : public SemanticInstanceProvider {
public:
  explicit SemanticVisitor(
      TargetInfo target = TargetInfo::host(),
      const SourceGraph *sourceGraph = nullptr, bool toolingOccurrences = true,
      FrontendWorkMetrics *workMetrics = nullptr,
      const SourceManager *sources = nullptr,
      RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off,
      bool requireFunctionDefinitions = false,
      TrustedBodyAnalysis trustedBodies = TrustedBodyAnalysis::Complete);
  ~SemanticVisitor();

  SemanticVisitor(const SemanticVisitor &) = delete;
  SemanticVisitor &operator=(const SemanticVisitor &) = delete;
  SemanticVisitor(SemanticVisitor &&) noexcept;
  SemanticVisitor &operator=(SemanticVisitor &&) noexcept;

  bool check(const Program &program);
  // Select additional primary bodies for the next check, after HIR discovery.
  // AST identities must belong to that same parsed program.
  void
  requireFunctionBodies(const std::unordered_set<const FunctionDecl *> &bodies);
  bool check(const Expr &expr);
  // Applies artifact-level entry/export policy after ordinary language
  // semantics have resolved every declaration.
  bool applyProgramRequest(const Program &program,
                           const ProgramRequest &request);

  [[nodiscard]] bool hadError() const;
  [[nodiscard]] const std::vector<SemanticDiagnostic> &errors() const;
  [[nodiscard]] SemanticType expressionType() const;
  [[nodiscard]] const SemanticModel &model() const;
  [[nodiscard]] SemanticModel takeModel();
  [[nodiscard]] SemanticTypeTraits
  traitsFor(const SemanticType &type) const override;
  [[nodiscard]] bool
  requiresActiveCleanupFor(const SemanticType &type) const override;
  [[nodiscard]] std::optional<EnumTypeInfo>
  enumTypeFor(const SemanticType &type) const override;

  // Semantic type/entry selection only; callers still need a represented
  // invocation loan and join-all lifetime proof before executable admission.
  [[nodiscard]] std::optional<ScopedThreadTaskShape>
  scopedThreadTaskShapeFor(const SemanticType &type);

  [[nodiscard]] SemanticInstanceAnalysis analyzeFunctionInstance(
      FunctionId functionId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &functionTypeArguments,
      const std::vector<CompileTimeValue> &functionValueArguments) override;
  [[nodiscard]] SemanticInstanceAnalysis analyzeConstructorInstance(
      ConstructorId constructorId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &constructorTypeArguments,
      const std::vector<CompileTimeValue> &constructorValueArguments) override;
  [[nodiscard]] SemanticInstanceAnalysis analyzeDestructorInstance(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) override;
  [[nodiscard]] SemanticInstanceAnalysis analyzeClassFieldInitializers(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) override;
  [[nodiscard]] bool
  analyzeDemandedPrimaryBody(const FunctionDecl &body) override;

  // Late demand (see analyzeDemandedPrimaryBody) that could not keep the
  // analysis equivalent to analysing these bodies in the pass. The caller
  // must discard this analysis and require the returned bodies instead.
  [[nodiscard]] bool lateDemandFailed() const;
  [[nodiscard]] std::vector<const FunctionDecl *> lateDemandedBodies() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
