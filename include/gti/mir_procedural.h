#pragma once

#include "gti/mir_constant_evaluator.h"
#include "gti/procedural_syntax.h"

#include <array>

namespace lang {

// A separate admission/publication profile of the same MIR executor. Neither
// arguments nor results are ordinary persistent constants. The caller owns the
// arena for the duration of execution; only the sealed result may outlive it.
struct MirProceduralRequest {
  SyntaxArena &arena;
  SyntaxTokens input;
  // Trusted declaration identities in SyntaxCategory order.
  std::array<SemanticType, 4> types;
  std::function<bool()> cancelled;
};

struct MirProceduralEvaluation {
  MirConstantEvaluation execution;
  std::optional<SealedSyntaxExpression> expression;
  std::optional<Diagnostic> diagnostic;
};

[[nodiscard]] MirProceduralEvaluation evaluateMirProceduralFunction(
    const MirProgram &program, HirFunctionInstanceId function,
    MirProceduralRequest &request, MirConstantLimits limits = {});

} // namespace lang
