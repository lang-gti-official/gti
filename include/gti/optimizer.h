#pragma once

#include "gti/constant_evaluator.h"
#include "gti/hir.h"
#include "gti/mir.h"
#include "gti/optimization/report.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace lang {

class OptimizationResult;

enum class OptimizationLevel {
  O0,
  O1,
  O2,
  O3,
};

struct OptimizationOptions {
  bool verifyMir = true;
};

struct OptimizationRequest {
  const HirProgram &hir;
  MirProgram mir;
  OptimizationLevel level = OptimizationLevel::O0;
  TargetInfo target = TargetInfo::host();
  OptimizationOptions options;
  const OptimizationResult *compatibility = nullptr;
};

struct OptimizedProgram {
  // Immutable authority snapshot presented to the optimizer. Backends compare
  // the optimized program against this source MIR and admit only rewrites
  // authorized by the controlled MIR editor.
  MirProgram sourceMir;
  MirProgram mir;
  OptimizationReport report;

  [[nodiscard]] bool valid() const { return report.valid(); }
};

using IntegerConstant = ConstantInteger;

class OptimizationResult {
public:
  [[nodiscard]] const ConstantValue *replacement(HirValueId value) const;
  [[nodiscard]] const ConstantValue *replacement(const HirProgram &program,
                                                 const Expr &expression) const;
  [[nodiscard]] std::size_t foldedExpressionCount() const;
  void setReplacement(HirValueId value, ConstantValue replacement);

private:
  std::unordered_map<HirValueId, ConstantValue> constants;
};

struct OptimizationContext {
  const HirProgram &program;
  OptimizationLevel level;
  TargetInfo target;
};

class OptimizationPass {
public:
  OptimizationPass() = default;
  OptimizationPass(const OptimizationPass &) = delete;
  OptimizationPass &operator=(const OptimizationPass &) = delete;
  virtual ~OptimizationPass() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  virtual void run(const OptimizationContext &context,
                   OptimizationResult &result) = 0;
};

class ConstantFoldingPass final : public OptimizationPass {
public:
  [[nodiscard]] std::string_view name() const override;
  void run(const OptimizationContext &context,
           OptimizationResult &output) override;

private:
  [[nodiscard]] std::optional<ConstantValue> operand(const HirValue &value,
                                                     std::size_t index) const;
  void analyze(const HirBody &body);
  [[nodiscard]] std::optional<ConstantValue>
  evaluate(const HirValue &value) const;
  [[nodiscard]] static std::optional<ConstantValue>
  literal(const HirValue &value);
  [[nodiscard]] std::optional<ConstantValue>
  logical(const HirValue &value) const;
  [[nodiscard]] std::optional<ConstantValue> unary(const HirValue &value) const;
  [[nodiscard]] std::optional<ConstantValue>
  foldBinary(const HirValue &value) const;
  [[nodiscard]] std::optional<ConstantValue>
  conditional(const HirValue &value) const;
  [[nodiscard]] std::optional<ConstantValue>
  conversion(const HirValue &value) const;

  OptimizationResult *result = nullptr;
  std::unordered_map<HirValueId, ConstantValue> constants;
};

class OptimizationPipeline {
public:
  [[nodiscard]] OptimizedProgram run(OptimizationRequest request) const;

  [[nodiscard]] OptimizationResult
  run(const HirProgram &program, OptimizationLevel level,
      TargetInfo target = TargetInfo::host()) const;
};

} // namespace lang
