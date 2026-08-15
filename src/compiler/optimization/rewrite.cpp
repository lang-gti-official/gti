#include "gti/optimization/rewrite.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] std::size_t bodyGroup(MirBodyKind kind) {
  switch (kind) {
  case MirBodyKind::Module:
    return 0;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    return 1;
  case MirBodyKind::Function:
    return 2;
  case MirBodyKind::Constructor:
    return 3;
  case MirBodyKind::Destructor:
    return 4;
  case MirBodyKind::Lambda:
    return 5;
  case MirBodyKind::HostedStartup:
    return 6;
  }
  return 7;
}

[[nodiscard]] auto bodyKey(MirBodyAddress address) {
  return std::tuple{bodyGroup(address.kind), address.owner,
                    static_cast<std::size_t>(address.kind)};
}

[[nodiscard]] bool addressLess(const MirInstructionAddress &left,
                               const MirInstructionAddress &right) {
  if (bodyKey(left.body) != bodyKey(right.body)) {
    return bodyKey(left.body) < bodyKey(right.body);
  }
  return std::tie(left.block, left.index) < std::tie(right.block, right.index);
}

void addError(MirEditResult &result, MirInstructionAddress address,
              MirInstructionId instruction, std::string message) {
  result.verification.errors.push_back({.bodyKind = address.body.kind,
                                        .owner = address.body.owner,
                                        .block = address.block,
                                        .instruction = instruction,
                                        .message = std::move(message)});
}

} // namespace

void MirProgramEditor::queueLiteralReplacement(
    MirInstructionAddress address, MirInstructionId expectedInstruction,
    MirOperation expectedOperation, Literal literal) {
  patches.push_back({.address = address,
                     .expectedInstruction = expectedInstruction,
                     .expectedOperation = expectedOperation,
                     .literal = std::move(literal)});
}

MirEditResult MirProgramEditor::apply() {
  MirEditResult result;
  std::vector<LiteralReplacement> queued = std::move(patches);
  patches.clear();
  if (queued.empty()) {
    return result;
  }

  std::sort(
      queued.begin(), queued.end(),
      [](const LiteralReplacement &left, const LiteralReplacement &right) {
        return addressLess(left.address, right.address);
      });

  for (std::size_t index = 0; index < queued.size(); ++index) {
    const LiteralReplacement &patch = queued[index];
    if (index != 0 && patch.address == queued[index - 1].address) {
      addError(result, patch.address, patch.expectedInstruction,
               "duplicate MIR instruction replacement");
      continue;
    }
    const MirBody *target = findMirBody(program, patch.address.body);
    if (target == nullptr) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement body does not exist");
      continue;
    }
    const MirBlock *block = target->findBlock(patch.address.block);
    if (block == nullptr || patch.address.index >= block->instructions.size()) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement address is outside the body");
      continue;
    }
    const MirInstruction &instruction =
        block->instructions[patch.address.index];
    if (patch.expectedInstruction == 0 ||
        instruction.id != patch.expectedInstruction) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement target is stale");
      continue;
    }
    if (instruction.kind != MirInstructionKind::Compute ||
        instruction.operation != patch.expectedOperation) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR replacement target is not the expected computation");
      continue;
    }
    if (patch.expectedOperation != MirOperation::Identity ||
        instruction.operands.size() != 1 ||
        instruction.operands.front().kind != MirOperandKind::Value ||
        instruction.operands.front().value == 0) {
      addError(result, patch.address, patch.expectedInstruction,
               "MIR literal replacement is missing its exact identity "
               "source");
      continue;
    }
    if (const auto *integer = std::get_if<std::uint64_t>(&patch.literal)) {
      const std::optional<CheckedIntegerDomain> domain =
          constantIntegerDomain(instruction.info.type);
      if (!domain ||
          !evaluateConstantLiteral(Literal{*integer}, *domain).value) {
        addError(result, patch.address, patch.expectedInstruction,
                 "MIR integer literal replacement is outside its result "
                 "domain");
      }
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  MirProgram candidate = program;
  std::vector<MirBodyAddress> touched;
  for (const LiteralReplacement &patch : queued) {
    MirBody *target = findMirBody(candidate, patch.address.body);
    MirInstruction &instruction = target->blocks[patch.address.block - 1]
                                      .instructions[patch.address.index];
    const MirValueId sourceValue = instruction.operands.front().value;
    MirInstruction replacement = instruction;
    replacement.operation = MirOperation::Literal;
    replacement.operands.clear();
    replacement.literal = patch.literal;
    replacement.literalProvenance =
        MirLiteralProvenance{.kind = MirLiteralProvenanceKind::IdentityFold,
                             .sourceValue = sourceValue};
    instruction = std::move(replacement);
    if (std::find(touched.begin(), touched.end(), patch.address.body) ==
        touched.end()) {
      touched.push_back(patch.address.body);
    }
  }

  for (MirBodyAddress address : touched) {
    MirBody *target = findMirBody(candidate, address);
    if (target == nullptr || !rebuildMirValueUses(*target)) {
      addError(result, {.body = address}, 0,
               "MIR replacement produced invalid value uses");
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  result.verification = verifyMirProgram(candidate);
  if (!result.verification.valid()) {
    return result;
  }

  program = std::move(candidate);
  result.changed = true;
  result.appliedPatches = queued.size();
  result.valueUsesRebuilt = true;
  result.invalidation.instructionFacts = true;
  result.invalidation.valueUses = true;
  return result;
}

} // namespace lang
