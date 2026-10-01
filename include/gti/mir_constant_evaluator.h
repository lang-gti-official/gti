#pragma once

#include "gti/mir.h"

#include <span>

namespace lang {

struct MirConstantLimits {
  std::size_t steps = 4096;
  std::size_t callDepth = 64;
};

struct MirConstantEvaluation {
  std::optional<ConstantValue> value;
  ConstantEvaluationFailure failure = ConstantEvaluationFailure::None;
  // Retained typed-body provenance. The staging caller maps these identities
  // to source spans; execution never needs an AST pointer.
  HirFunctionInstanceId function = 0;
  MirBodyAddress body;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::size_t steps = 0;
  std::string detail;

  [[nodiscard]] explicit operator bool() const { return value.has_value(); }
};

// Development foundation for staged constexpr lowering, not a post-semantics
// replacement for early type-forming evaluation. Executes canonical MIR only;
// ordinary verification precedes the additional bounded const profile.
[[nodiscard]] MirConstantEvaluation evaluateMirConstantFunction(
    const MirProgram &program, HirFunctionInstanceId function,
    std::span<const ConstantValue> arguments, MirConstantLimits limits = {},
    const std::optional<ConstantValue> &receiver = {});

[[nodiscard]] MirConstantEvaluation evaluateMirConstantConstructor(
    const MirProgram &program, HirConstructorInstanceId constructor,
    std::span<const ConstantValue> arguments, MirConstantLimits limits = {});

} // namespace lang
