#include "gti/optimizer.h"

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] const MirInstruction *definingInstruction(const MirBody &body,
                                                        MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  if (value == nullptr) {
    return nullptr;
  }
  const MirBlock *block = body.findBlock(value->definitionBlock);
  if (block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [value](const MirInstruction &instruction) {
                     return instruction.id == value->definition;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

[[nodiscard]] bool foldableScalarLiteral(const Literal &literal) {
  return std::holds_alternative<std::nullptr_t>(literal) ||
         std::holds_alternative<std::uint64_t>(literal) ||
         std::holds_alternative<BinaryFloat>(literal) ||
         std::holds_alternative<CharacterLiteral>(literal) ||
         std::holds_alternative<bool>(literal);
}

[[nodiscard]] std::optional<Literal>
identityLiteral(const MirBody &body, const MirInstruction &identity) {
  if (identity.kind != MirInstructionKind::Compute ||
      identity.operation != MirOperation::Identity ||
      identity.operands.size() != 1 ||
      identity.operands.front().kind != MirOperandKind::Value) {
    return std::nullopt;
  }

  MirValueId value = identity.operands.front().value;
  std::unordered_set<MirValueId> visited;
  while (value != 0 && visited.size() <= body.values.size() &&
         visited.insert(value).second) {
    const MirInstruction *definition = definingInstruction(body, value);
    if (definition == nullptr ||
        definition->kind != MirInstructionKind::Compute) {
      return std::nullopt;
    }
    if (definition->operation == MirOperation::Literal && definition->literal &&
        foldableScalarLiteral(*definition->literal)) {
      return definition->literal;
    }
    if (definition->operation != MirOperation::Identity ||
        definition->operands.size() != 1 ||
        definition->operands.front().kind != MirOperandKind::Value) {
      return std::nullopt;
    }
    value = definition->operands.front().value;
  }
  return std::nullopt;
}

struct LiteralIdentityFoldResult {
  OptimizationPassReport report;
  MirVerificationResult verification;
};

[[nodiscard]] LiteralIdentityFoldResult
foldLiteralIdentities(MirProgram &program,
                      const OptimizationResult &compatibility) {
  LiteralIdentityFoldResult result{
      .report = {.name = "fold-literal-identities"}};
  MirProgramEditor editor(program);
  for (const MirBodyAddress bodyAddress : editor.bodies()) {
    const MirBody *body = editor.body(bodyAddress);
    if (body == nullptr) {
      continue;
    }
    for (const MirBlock &block : body->blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const MirInstruction &instruction = block.instructions[index];
        if (instruction.hirValue == 0) {
          continue;
        }
        const std::optional<Literal> literal =
            identityLiteral(*body, instruction);
        if (!literal) {
          continue;
        }

        ++result.report.shadowComparisons;
        const ConstantEvaluation evaluated = evaluateConstantLiteral(
            *literal, constantIntegerDomain(instruction.info.type));
        const ConstantValue *expected =
            compatibility.replacement(instruction.hirValue);
        if (!evaluated.value || expected == nullptr ||
            *evaluated.value != *expected) {
          ++result.report.shadowMismatches;
          continue;
        }

        editor.queueLiteralReplacement(
            {.body = bodyAddress, .block = block.id, .index = index},
            instruction.id, MirOperation::Identity, *literal);
      }
    }
  }

  const MirEditResult edited = editor.apply();
  result.report.changed = edited.changed;
  result.report.appliedEdits = edited.appliedPatches;
  result.report.valueUsesRebuilt = edited.valueUsesRebuilt;
  result.report.invalidation = edited.invalidation;
  result.verification = edited.verification;
  return result;
}

} // namespace

OptimizedProgram OptimizationPipeline::run(OptimizationRequest request) const {
  OptimizedProgram result{.mir = std::move(request.mir)};
  result.report.verificationEnabled = request.options.verifyMir;

  if (request.options.verifyMir) {
    result.report.inputVerification = verifyMirProgram(result.mir);
    if (!result.report.inputVerification.valid()) {
      return result;
    }
  }

  if (request.level != OptimizationLevel::O0) {
    OptimizationResult localCompatibility;
    const OptimizationResult *compatibility = request.compatibility;
    if (compatibility == nullptr) {
      localCompatibility = run(request.hir, request.level, request.target);
      compatibility = &localCompatibility;
    }
    LiteralIdentityFoldResult pass =
        foldLiteralIdentities(result.mir, *compatibility);
    result.report.passes.push_back(std::move(pass.report));
    if (!pass.verification.valid()) {
      result.report.outputVerification = std::move(pass.verification);
      return result;
    }
  }

  if (request.options.verifyMir) {
    result.report.outputVerification = verifyMirProgram(result.mir);
  }
  return result;
}

} // namespace lang
