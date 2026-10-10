#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/devirtualization.h"
#include "gti/optimization/pass.h"
#include "gti/optimization/safety.h"
#include "gti/optimization/sccp.h"
#include "mir_verification_support.h"

#include <algorithm>
#include <iterator>
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

[[nodiscard]] bool keyLess(const MirInstructionKey &left,
                           const MirInstructionKey &right) {
  if (bodyKey(left.body) != bodyKey(right.body)) {
    return bodyKey(left.body) < bodyKey(right.body);
  }
  return left.instruction < right.instruction;
}

struct ConstInstructionLocation {
  const MirBlock *block = nullptr;
  const MirInstruction *instruction = nullptr;
};

struct InstructionLocation {
  MirBlock *block = nullptr;
  MirInstruction *instruction = nullptr;
};

[[nodiscard]] ConstInstructionLocation findInstruction(const MirBody &body,
                                                       MirInstructionId id) {
  for (const MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    if (found != block.instructions.end()) {
      return {.block = &block, .instruction = &*found};
    }
  }
  return {};
}

[[nodiscard]] InstructionLocation findInstruction(MirBody &body,
                                                  MirInstructionId id) {
  for (MirBlock &block : body.blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    if (found != block.instructions.end()) {
      return {.block = &block, .instruction = &*found};
    }
  }
  return {};
}

void addError(MirEditResult &result, MirBodyAddress body, MirBlockId block,
              MirInstructionId instruction, std::string message) {
  result.verification.errors.push_back({.bodyKind = body.kind,
                                        .owner = body.owner,
                                        .block = block,
                                        .instruction = instruction,
                                        .message = std::move(message)});
}

} // namespace

void MirEditor::queueLiteralReplacement(MirInstructionKey target,
                                        MirOperation expectedOperation,
                                        Literal literal) {
  patches.push_back({.target = target,
                     .expectedOperation = expectedOperation,
                     .literal = std::move(literal)});
}

void MirEditor::queueComputeFoldReplacement(MirInstructionKey target,
                                            MirOperation expectedOperation,
                                            Literal literal) {
  patches.push_back({.target = target,
                     .expectedOperation = expectedOperation,
                     .literal = std::move(literal),
                     .computeFold = true});
}

void MirEditor::queueSparseConstantReplacement(
    MirInstructionKey target, MirOperation expectedOperation, Literal literal,
    std::vector<MirBlockId> relatedBlocks,
    std::vector<MirInstructionId> relatedInstructions,
    std::vector<MirValueId> relatedValues) {
  patches.push_back({.target = target,
                     .expectedOperation = expectedOperation,
                     .literal = std::move(literal),
                     .sparseConstant = true,
                     .relatedBlocks = std::move(relatedBlocks),
                     .relatedInstructions = std::move(relatedInstructions),
                     .relatedValues = std::move(relatedValues)});
}

void MirEditor::queueBranchFold(MirBodyAddress body, MirBlockId block,
                                MirValueId expectedCondition, bool taken) {
  branchFolds.push_back({.body = body,
                         .block = block,
                         .expectedCondition = expectedCondition,
                         .taken = taken});
}

void MirEditor::queueScalarPromotion(MirBodyAddress body, MirPlaceId place) {
  scalarPromotions.push_back({.body = body, .place = place});
}

void MirEditor::queueDeadInstructionRemoval(MirInstructionKey target) {
  deadInstructions.push_back(target);
}

void MirEditor::queueUnreachableBlockRemoval(MirBodyAddress body,
                                             MirBlockId block) {
  unreachableBlocks.push_back({.body = body, .block = block});
}

void MirEditor::queueSafetyDischarge(MirInstructionKey target,
                                     MirSafetyDischarge discharge) {
  safetyDischarges.push_back(
      {.target = target, .discharge = std::move(discharge)});
}

MirEditResult MirEditor::apply() { return applyWithSafetySnapshot(nullptr); }

MirEditResult MirEditor::apply(MirBodyAnalysisManager &analyses) {
  const MirClosedCallSafetyAnalysis *sourceSafety =
      safetyDischarges.empty() ? nullptr : analyses.closedCallSafety(program);
  MirEditResult result = applyWithSafetySnapshot(sourceSafety);
  if (!result.changed) {
    return result;
  }

  const bool metadataOnly = std::all_of(
      result.transformations.begin(), result.transformations.end(),
      [](const MirTransformRecord &record) {
        return record.kind == MirTransformKind::DischargeSafetyCheck;
      });
  std::vector<MirBodyAddress> touched;
  for (const MirTransformRecord &record : result.transformations) {
    if (std::find(touched.begin(), touched.end(), record.body) ==
        touched.end()) {
      touched.push_back(record.body);
    }
  }
  std::sort(
      touched.begin(), touched.end(),
      [](MirBodyAddress left, MirBodyAddress right) {
        return std::pair{static_cast<std::size_t>(left.kind), left.owner} <
               std::pair{static_cast<std::size_t>(right.kind), right.owner};
      });
  const MirPreservedAnalyses preserved =
      metadataOnly ? MirPreservedAnalyses::all() : MirPreservedAnalyses::none();
  for (const MirBodyAddress address : touched) {
    std::vector<std::string> invalidated =
        analyses.invalidate(address, preserved);
    result.invalidatedAnalyses.insert(
        result.invalidatedAnalyses.end(),
        std::make_move_iterator(invalidated.begin()),
        std::make_move_iterator(invalidated.end()));
  }
  std::sort(result.invalidatedAnalyses.begin(),
            result.invalidatedAnalyses.end());
  result.invalidatedAnalyses.erase(
      std::unique(result.invalidatedAnalyses.begin(),
                  result.invalidatedAnalyses.end()),
      result.invalidatedAnalyses.end());
  result.analysesInvalidated = true;
  return result;
}

MirEditResult MirEditor::applyWithSafetySnapshot(
    const MirClosedCallSafetyAnalysis *cachedSourceSafety) {
  MirEditResult result;
  std::vector<LiteralReplacement> queued = std::move(patches);
  patches.clear();
  std::vector<BranchFold> queuedBranchFolds = std::move(branchFolds);
  branchFolds.clear();
  std::vector<ScalarPromotion> queuedScalarPromotions =
      std::move(scalarPromotions);
  scalarPromotions.clear();
  std::vector<MirInstructionKey> queuedDeadInstructions =
      std::move(deadInstructions);
  deadInstructions.clear();
  std::vector<UnreachableBlock> queuedUnreachableBlocks =
      std::move(unreachableBlocks);
  unreachableBlocks.clear();
  std::vector<SafetyDischarge> queuedSafetyDischarges =
      std::move(safetyDischarges);
  safetyDischarges.clear();
  auto queuedExactReceivers = std::move(exactReceivers);
  exactReceivers.clear();
  if (queued.empty() && queuedBranchFolds.empty() &&
      queuedScalarPromotions.empty() && queuedDeadInstructions.empty() &&
      queuedUnreachableBlocks.empty() && queuedSafetyDischarges.empty() &&
      queuedExactReceivers.empty()) {
    return result;
  }

  std::sort(
      queued.begin(), queued.end(),
      [](const LiteralReplacement &left, const LiteralReplacement &right) {
        return keyLess(left.target, right.target);
      });
  std::sort(queuedBranchFolds.begin(), queuedBranchFolds.end(),
            [](const BranchFold &left, const BranchFold &right) {
              return std::tuple{bodyKey(left.body), left.block} <
                     std::tuple{bodyKey(right.body), right.block};
            });
  std::sort(queuedScalarPromotions.begin(), queuedScalarPromotions.end(),
            [](const ScalarPromotion &left, const ScalarPromotion &right) {
              return std::tuple{bodyKey(left.body), left.place} <
                     std::tuple{bodyKey(right.body), right.place};
            });
  std::sort(queuedDeadInstructions.begin(), queuedDeadInstructions.end(),
            keyLess);
  std::sort(queuedUnreachableBlocks.begin(), queuedUnreachableBlocks.end(),
            [](const UnreachableBlock &left, const UnreachableBlock &right) {
              return std::tuple{bodyKey(left.body), left.block} <
                     std::tuple{bodyKey(right.body), right.block};
            });
  std::sort(
      queuedSafetyDischarges.begin(), queuedSafetyDischarges.end(),
      [](const SafetyDischarge &left, const SafetyDischarge &right) {
        return std::tuple{
                   bodyKey(left.target.body),
                   left.target.instruction,
                   static_cast<std::size_t>(left.discharge.family),
                   left.discharge.origin,
                   static_cast<std::size_t>(left.discharge.outcome.code),
                   static_cast<std::size_t>(left.discharge.outcome.detail)} <
               std::tuple{
                   bodyKey(right.target.body),
                   right.target.instruction,
                   static_cast<std::size_t>(right.discharge.family),
                   right.discharge.origin,
                   static_cast<std::size_t>(right.discharge.outcome.code),
                   static_cast<std::size_t>(right.discharge.outcome.detail)};
      });

  for (std::size_t index = 0; index < queued.size(); ++index) {
    const LiteralReplacement &patch = queued[index];
    if (index != 0 && patch.target == queued[index - 1].target) {
      addError(result, patch.target.body, 0, patch.target.instruction,
               "duplicate MIR instruction replacement");
      continue;
    }
    const MirBody *target = findMirBody(program, patch.target.body);
    if (target == nullptr) {
      addError(result, patch.target.body, 0, patch.target.instruction,
               "MIR replacement body does not exist");
      continue;
    }
    const ConstInstructionLocation location =
        findInstruction(*target, patch.target.instruction);
    if (patch.target.instruction == 0 || location.instruction == nullptr) {
      addError(result, patch.target.body, 0, patch.target.instruction,
               "MIR replacement instruction does not exist");
      continue;
    }
    const MirInstruction &instruction = *location.instruction;
    if (instruction.kind != MirInstructionKind::Compute ||
        instruction.operation != patch.expectedOperation) {
      addError(result, patch.target.body, location.block->id,
               patch.target.instruction,
               "MIR replacement target is not the expected computation");
      continue;
    }
    if (patch.sparseConstant) {
      const MirSccpAnalysis analysis =
          analyzeMirSccp(program, patch.target.body);
      const MirSccpValueFact *fact =
          instruction.result ? analysis.find(*instruction.result) : nullptr;
      if (!analysis.complete || fact == nullptr ||
          fact->kind != MirSccpLatticeKind::Constant ||
          fact->literal != std::optional<Literal>{patch.literal} ||
          patch.expectedOperation == MirOperation::Literal ||
          fact->evidence.blocks != patch.relatedBlocks ||
          fact->evidence.instructions != patch.relatedInstructions ||
          fact->evidence.values != patch.relatedValues) {
        addError(result, patch.target.body, location.block->id,
                 patch.target.instruction,
                 "MIR sparse-constant replacement does not replay from "
                 "canonical SCCP facts");
        continue;
      }
    } else if (patch.computeFold) {
      const bool valueOperands =
          !instruction.operands.empty() && instruction.operands.size() <= 2 &&
          std::all_of(instruction.operands.begin(), instruction.operands.end(),
                      [](const MirOperand &operand) {
                        return operand.kind == MirOperandKind::Value &&
                               operand.value != 0;
                      });
      if (patch.expectedOperation == MirOperation::Identity ||
          patch.expectedOperation == MirOperation::Literal || !valueOperands) {
        addError(result, patch.target.body, location.block->id,
                 patch.target.instruction,
                 "MIR compute-fold replacement is missing its exact value "
                 "operands");
        continue;
      }
    } else if (patch.expectedOperation != MirOperation::Identity ||
               instruction.operands.size() != 1 ||
               instruction.operands.front().kind != MirOperandKind::Value ||
               instruction.operands.front().value == 0) {
      addError(result, patch.target.body, location.block->id,
               patch.target.instruction,
               "MIR literal replacement is missing its exact identity "
               "source");
      continue;
    }
    if (const auto *integer = std::get_if<std::uint64_t>(&patch.literal)) {
      const std::optional<CheckedIntegerDomain> domain =
          constantIntegerDomain(instruction.info.type);
      if (!domain ||
          !evaluateConstantLiteral(Literal{*integer}, *domain).value) {
        addError(result, patch.target.body, location.block->id,
                 patch.target.instruction,
                 "MIR integer literal replacement is outside its result "
                 "domain");
      }
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  for (std::size_t index = 0; index < queuedBranchFolds.size(); ++index) {
    const BranchFold &fold = queuedBranchFolds[index];
    if (index != 0 && fold.body == queuedBranchFolds[index - 1].body &&
        fold.block == queuedBranchFolds[index - 1].block) {
      addError(result, fold.body, fold.block, 0,
               "duplicate MIR branch replacement");
      continue;
    }
    const MirBody *target = findMirBody(program, fold.body);
    const MirBlock *block =
        target == nullptr ? nullptr : target->findBlock(fold.block);
    if (block == nullptr ||
        block->terminator.kind != MirTerminatorKind::Branch ||
        !block->terminator.value ||
        block->terminator.value->kind != MirOperandKind::Value ||
        block->terminator.value->value != fold.expectedCondition ||
        (fold.taken ? block->terminator.target
                    : block->terminator.elseTarget) == 0) {
      addError(result, fold.body, fold.block, 0,
               "MIR branch fold target is not the expected literal branch");
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  for (std::size_t index = 0; index < queuedScalarPromotions.size(); ++index) {
    const ScalarPromotion &promotion = queuedScalarPromotions[index];
    if (index != 0 &&
        promotion.body == queuedScalarPromotions[index - 1].body &&
        promotion.place == queuedScalarPromotions[index - 1].place) {
      addError(result, promotion.body, 0, 0, "duplicate MIR scalar promotion");
      continue;
    }
    const MirBody *source = findMirBody(program, promotion.body);
    MirBody replay = source == nullptr ? MirBody{} : *source;
    if (source == nullptr || promotion.place == 0 ||
        !rewriteMirScalarPlace(replay, promotion.place)) {
      addError(result, promotion.body, 0, 0,
               "MIR scalar promotion claim is not proven by its source body");
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  for (std::size_t index = 0; index < queuedUnreachableBlocks.size(); ++index) {
    const UnreachableBlock &cleanup = queuedUnreachableBlocks[index];
    if (index != 0 && cleanup.body == queuedUnreachableBlocks[index - 1].body &&
        cleanup.block == queuedUnreachableBlocks[index - 1].block) {
      addError(result, cleanup.body, cleanup.block, 0,
               "duplicate MIR unreachable-block removal");
      continue;
    }
    const MirBody *source = findMirBody(program, cleanup.body);
    MirBody replay = source == nullptr ? MirBody{} : *source;
    if (source == nullptr || cleanup.block == 0 ||
        !rewriteMirUnreachableBlock(replay, cleanup.block)) {
      addError(result, cleanup.body, cleanup.block, 0,
               "MIR unreachable-block removal is not proven by reachability "
               "and schedule facts");
    }
  }
  // Each dead-instruction claim is proven against the unchanged source
  // program, which the canonical rewrite would otherwise verify per claim.
  std::optional<bool> sourceVerified;
  for (std::size_t index = 0; index < queuedDeadInstructions.size(); ++index) {
    const MirInstructionKey &cleanup = queuedDeadInstructions[index];
    if (index != 0 && cleanup == queuedDeadInstructions[index - 1]) {
      addError(result, cleanup.body, 0, cleanup.instruction,
               "duplicate MIR dead-instruction removal");
      continue;
    }
    if (!sourceVerified) {
      sourceVerified = verifyMirProgram(program).valid();
    }
    MirProgram replay = program;
    if (!*sourceVerified || !mir_detail::rewriteVerifiedMirDeadInstruction(
                                replay, cleanup.body, cleanup.instruction)) {
      addError(result, cleanup.body, 0, cleanup.instruction,
               "MIR dead-instruction removal is not proven by exact uses and "
               "effects");
    }
  }
  std::optional<MirClosedCallSafetyAnalysis> ownedSourceSafety;
  const MirClosedCallSafetyAnalysis *sourceSafety = cachedSourceSafety;
  if (!queuedSafetyDischarges.empty() && sourceSafety == nullptr) {
    ownedSourceSafety = analyzeMirClosedCallSafetyChecks(program);
    sourceSafety = &*ownedSourceSafety;
  }
  for (std::size_t index = 0; index < queuedSafetyDischarges.size(); ++index) {
    const SafetyDischarge &claim = queuedSafetyDischarges[index];
    if (index != 0 &&
        claim.target == queuedSafetyDischarges[index - 1].target &&
        claim.discharge == queuedSafetyDischarges[index - 1].discharge) {
      addError(result, claim.target.body, 0, claim.target.instruction,
               "duplicate MIR safety-check discharge");
      continue;
    }
    const MirBody *body = findMirBody(program, claim.target.body);
    const ConstInstructionLocation location =
        body == nullptr ? ConstInstructionLocation{}
                        : findInstruction(*body, claim.target.instruction);
    const MirClosedCallBodyAnalysis *analysis =
        sourceSafety == nullptr ? nullptr
                                : sourceSafety->find(claim.target.body);
    if (body == nullptr || location.block == nullptr ||
        location.instruction == nullptr ||
        hasMirSafetyDischarge(*location.instruction, claim.discharge) ||
        analysis == nullptr ||
        analysis->safety.find(claim.target.instruction, claim.discharge) ==
            nullptr) {
      addError(result, claim.target.body,
               location.block == nullptr ? 0 : location.block->id,
               claim.target.instruction,
               "MIR safety-check discharge is not proven by canonical MIR "
               "analysis");
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  MirProgram candidate = program;
  std::vector<MirBodyAddress> touched;
  std::sort(queuedExactReceivers.begin(), queuedExactReceivers.end(), keyLess);
  // Closed-world facts are re-derived from the pre-edit program rather than
  // trusted from the pass. Receiver refinement never changes a direct call's
  // arguments or a function's openness, so one snapshot serves every claim.
  const std::optional<MirClosedWorldFacts> closedWorld =
      queuedExactReceivers.empty()
          ? std::nullopt
          : std::optional(analyzeMirClosedWorldFacts(program));
  for (const auto &claim : queuedExactReceivers) {
    auto record =
        rewriteMirExactReceiver(candidate, claim.body, claim.instruction, pass,
                                closedWorld ? &*closedWorld : nullptr);
    if (!record) {
      addError(result, claim.body, 0, claim.instruction,
               "MIR exact receiver refinement lacks an independent proof");
      return result;
    }
    result.transformations.push_back(std::move(*record));
    if (std::find(touched.begin(), touched.end(), claim.body) == touched.end())
      touched.push_back(claim.body);
  }
  for (const LiteralReplacement &patch : queued) {
    MirBody *target = findMirBody(candidate, patch.target.body);
    InstructionLocation location =
        target == nullptr ? InstructionLocation{}
                          : findInstruction(*target, patch.target.instruction);
    if (location.instruction == nullptr || location.block == nullptr) {
      addError(result, patch.target.body, 0, patch.target.instruction,
               "MIR replacement instruction became invalid before mutation");
      return result;
    }
    MirInstruction &instruction = *location.instruction;
    MirInstruction replacement = instruction;
    MirTransformRecord record{
        .pass = pass,
        .kind = MirTransformKind::ReplaceInstruction,
        .proof = patch.sparseConstant ? MirTransformProofKind::SparseConstant
                 : patch.computeFold  ? MirTransformProofKind::ComputeFold
                                      : MirTransformProofKind::IdentityFold,
        .body = patch.target.body,
        .block = location.block->id,
        .instruction = instruction.id,
        .sourceValue = instruction.hirValue,
        .sourceStatement = instruction.hirStatement,
        .replacementLiteral = patch.literal,
        .relatedBlocks = patch.relatedBlocks,
        .relatedInstructions = patch.relatedInstructions,
        .relatedValues = patch.relatedValues,
        .requiredAnalyses = {.valueUses = true,
                             .reachability = patch.sparseConstant,
                             .dominance = true,
                             .sparseConstants = patch.sparseConstant}};
    if (patch.sparseConstant) {
      replacement.literalProvenance =
          MirLiteralProvenance{.kind = MirLiteralProvenanceKind::SparseConstant,
                               .sourceOperation = instruction.operation,
                               .sourceValues = patch.relatedValues};
    } else if (patch.computeFold) {
      MirLiteralProvenance provenance{.kind =
                                          MirLiteralProvenanceKind::ComputeFold,
                                      .sourceOperation = instruction.operation};
      for (const MirOperand &operand : instruction.operands) {
        provenance.sourceValues.push_back(operand.value);
        record.relatedValues.push_back(operand.value);
      }
      replacement.literalProvenance = std::move(provenance);
    } else {
      replacement.literalProvenance = MirLiteralProvenance{
          .kind = MirLiteralProvenanceKind::IdentityFold,
          .sourceValue = instruction.operands.front().value};
      record.relatedValues.push_back(instruction.operands.front().value);
    }
    replacement.operation = MirOperation::Literal;
    replacement.operands.clear();
    replacement.literal = patch.literal;
    instruction = std::move(replacement);
    result.transformations.push_back(std::move(record));
    if (std::find(touched.begin(), touched.end(), patch.target.body) ==
        touched.end()) {
      touched.push_back(patch.target.body);
    }
  }

  for (const BranchFold &fold : queuedBranchFolds) {
    MirBody *target = findMirBody(candidate, fold.body);
    MirBlock *block = nullptr;
    for (MirBlock &candidateBlock : target->blocks) {
      if (candidateBlock.id == fold.block) {
        block = &candidateBlock;
      }
    }
    MirTerminator &terminator = block->terminator;
    const MirTerminator sourceTerminator = terminator;
    // The folded condition must be a literal bool in the candidate (the
    // literal patches above may be what folded it) agreeing with the
    // taken direction.
    const bool *condition = nullptr;
    for (const MirBlock &candidateBlock : target->blocks) {
      for (const MirInstruction &instruction : candidateBlock.instructions) {
        if (instruction.result &&
            *instruction.result == fold.expectedCondition &&
            instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::Literal &&
            instruction.literal) {
          condition = std::get_if<bool>(&*instruction.literal);
        }
      }
    }
    if (condition == nullptr || *condition != fold.taken) {
      addError(result, fold.body, fold.block, 0,
               "MIR branch fold condition is not the agreed literal");
      continue;
    }
    terminator.kind = MirTerminatorKind::Goto;
    terminator.targetArguments =
        fold.taken ? terminator.targetArguments : terminator.elseArguments;
    terminator.target = fold.taken ? terminator.target : terminator.elseTarget;
    terminator.elseTarget = 0;
    terminator.elseArguments.clear();
    terminator.value.reset();
    terminator.provenance =
        MirTerminatorProvenance{.kind = MirTerminatorProvenanceKind::BranchFold,
                                .foldSourceValue = fold.expectedCondition};
    result.transformations.push_back(
        {.pass = pass,
         .kind = MirTransformKind::ReplaceTerminator,
         .proof = MirTransformProofKind::BranchFold,
         .body = fold.body,
         .block = fold.block,
         .sourceValue = sourceTerminator.hirValue,
         .sourceStatement = sourceTerminator.hirStatement,
         .relatedBlocks = {sourceTerminator.target,
                           sourceTerminator.elseTarget},
         .relatedValues = {fold.expectedCondition},
         .requiredAnalyses = {
             .valueUses = true, .reachability = true, .dominance = true}});
    if (std::find(touched.begin(), touched.end(), fold.body) == touched.end()) {
      touched.push_back(fold.body);
    }
  }
  for (const ScalarPromotion &promotion : queuedScalarPromotions) {
    const MirBody *source = findMirBody(program, promotion.body);
    MirBody *target = findMirBody(candidate, promotion.body);
    if (source == nullptr || target == nullptr) {
      addError(result, promotion.body, 0, 0,
               "MIR scalar promotion body became invalid before mutation");
      continue;
    }
    const MirPlace *sourcePlace = source->findPlace(promotion.place);
    const std::optional<MirScalarPromotionRewrite> rewrite =
        rewriteMirScalarPlace(*target, promotion.place);
    if (sourcePlace == nullptr || !rewrite) {
      addError(result, promotion.body, 0, 0,
               "MIR scalar promotion failed during atomic mutation");
      continue;
    }

    MirTransformRecord placeRecord{
        .pass = pass,
        .kind = MirTransformKind::PromotePlace,
        .proof = MirTransformProofKind::ScalarPromotion,
        .body = promotion.body,
        .place = promotion.place,
        .sourceValue = sourcePlace->sourceValue,
        .relatedBlocks = rewrite->parameterBlocks,
        .relatedInstructions = rewrite->replacedInstructions,
        .relatedValues = rewrite->createdValues,
        .requiredAnalyses = {.valueUses = true,
                             .effects = true,
                             .reachability = true,
                             .dominance = true,
                             .scalarPromotion = true}};
    placeRecord.relatedInstructions.insert(
        placeRecord.relatedInstructions.end(),
        rewrite->removedInstructions.begin(),
        rewrite->removedInstructions.end());
    std::sort(placeRecord.relatedInstructions.begin(),
              placeRecord.relatedInstructions.end());
    result.transformations.push_back(std::move(placeRecord));

    for (const MirInstructionId instructionId : rewrite->replacedInstructions) {
      const ConstInstructionLocation before =
          findInstruction(*source, instructionId);
      const InstructionLocation after = findInstruction(*target, instructionId);
      if (before.instruction == nullptr || before.block == nullptr ||
          after.instruction == nullptr || after.block == nullptr) {
        addError(result, promotion.body, 0, instructionId,
                 "MIR scalar replacement lost its stable instruction");
        continue;
      }
      std::vector<MirValueId> values;
      if (before.instruction->result) {
        values.push_back(*before.instruction->result);
      }
      for (const MirOperand &operand : after.instruction->operands) {
        if (operand.kind == MirOperandKind::Value) {
          values.push_back(operand.value);
        }
      }
      result.transformations.push_back(
          {.pass = pass,
           .kind = MirTransformKind::ReplaceInstruction,
           .proof = MirTransformProofKind::ScalarPromotion,
           .body = promotion.body,
           .block = before.block->id,
           .instruction = instructionId,
           .place = promotion.place,
           .sourceValue = before.instruction->hirValue,
           .sourceStatement = before.instruction->hirStatement,
           .relatedValues = std::move(values),
           .requiredAnalyses = {.valueUses = true,
                                .effects = true,
                                .reachability = true,
                                .dominance = true,
                                .scalarPromotion = true}});
    }
    for (const MirInstructionId instructionId : rewrite->removedInstructions) {
      const ConstInstructionLocation before =
          findInstruction(*source, instructionId);
      if (before.instruction == nullptr || before.block == nullptr) {
        addError(result, promotion.body, 0, instructionId,
                 "MIR scalar removal lost its stable instruction");
        continue;
      }
      result.transformations.push_back(
          {.pass = pass,
           .kind = MirTransformKind::RemoveInstruction,
           .proof = MirTransformProofKind::ScalarPromotion,
           .body = promotion.body,
           .block = before.block->id,
           .instruction = instructionId,
           .place = promotion.place,
           .sourceValue = before.instruction->hirValue,
           .sourceStatement = before.instruction->hirStatement,
           .relatedValues =
               before.instruction->result
                   ? std::vector<MirValueId>{*before.instruction->result}
                   : std::vector<MirValueId>{},
           .requiredAnalyses = {.valueUses = true,
                                .effects = true,
                                .reachability = true,
                                .dominance = true,
                                .scalarPromotion = true}});
    }
    if (std::find(touched.begin(), touched.end(), promotion.body) ==
        touched.end()) {
      touched.push_back(promotion.body);
    }
  }
  for (const UnreachableBlock &cleanup : queuedUnreachableBlocks) {
    MirBody *target = findMirBody(candidate, cleanup.body);
    const std::optional<MirUnreachableBlockRewrite> rewrite =
        target == nullptr ? std::nullopt
                          : rewriteMirUnreachableBlock(*target, cleanup.block);
    if (!rewrite) {
      addError(result, cleanup.body, cleanup.block, 0,
               "MIR unreachable-block removal failed during atomic mutation");
      continue;
    }
    result.transformations.push_back(
        {.pass = pass,
         .kind = MirTransformKind::RemoveBlock,
         .proof = MirTransformProofKind::UnreachableBlock,
         .body = cleanup.body,
         .block = cleanup.block,
         .sourceValue = rewrite->sourceValue,
         .sourceStatement = rewrite->sourceStatement,
         .relatedInstructions = rewrite->removedInstructions,
         .requiredAnalyses = {.valueUses = true,
                              .effects = true,
                              .reachability = true,
                              .dominance = true}});
    if (std::find(touched.begin(), touched.end(), cleanup.body) ==
        touched.end()) {
      touched.push_back(cleanup.body);
    }
  }
  // A failed rewrite leaves the candidate unchanged and a successful one
  // verifies its result, so the candidate needs verifying only once here.
  std::optional<bool> candidateVerified;
  for (const MirInstructionKey &cleanup : queuedDeadInstructions) {
    if (!candidateVerified) {
      candidateVerified = verifyMirProgram(candidate).valid();
    }
    const std::optional<MirDeadInstructionRewrite> rewrite =
        *candidateVerified ? mir_detail::rewriteVerifiedMirDeadInstruction(
                                 candidate, cleanup.body, cleanup.instruction)
                           : std::nullopt;
    if (!rewrite) {
      addError(result, cleanup.body, 0, cleanup.instruction,
               "MIR dead-instruction removal failed during atomic mutation");
      continue;
    }
    result.transformations.push_back(
        {.pass = pass,
         .kind = MirTransformKind::RemoveInstruction,
         .proof = MirTransformProofKind::DeadUnusedComputation,
         .body = cleanup.body,
         .block = rewrite->block,
         .instruction = rewrite->instruction,
         .sourceValue = rewrite->sourceValue,
         .sourceStatement = rewrite->sourceStatement,
         .relatedValues = rewrite->result
                              ? std::vector<MirValueId>{*rewrite->result}
                              : std::vector<MirValueId>{},
         .requiredAnalyses = {.valueUses = true, .effects = true}});
    if (std::find(touched.begin(), touched.end(), cleanup.body) ==
        touched.end()) {
      touched.push_back(cleanup.body);
    }
  }
  const bool safetyMetadataOnly =
      queued.empty() && queuedBranchFolds.empty() &&
      queuedScalarPromotions.empty() && queuedDeadInstructions.empty() &&
      queuedUnreachableBlocks.empty() && queuedExactReceivers.empty();
  std::optional<MirClosedCallSafetyAnalysis> ownedCandidateSafety;
  const MirClosedCallSafetyAnalysis *candidateSafety = sourceSafety;
  if (!queuedSafetyDischarges.empty() && !safetyMetadataOnly) {
    ownedCandidateSafety = analyzeMirClosedCallSafetyChecks(candidate);
    candidateSafety = &*ownedCandidateSafety;
  }
  for (const SafetyDischarge &claim : queuedSafetyDischarges) {
    MirBody *target = findMirBody(candidate, claim.target.body);
    InstructionLocation location =
        target == nullptr ? InstructionLocation{}
                          : findInstruction(*target, claim.target.instruction);
    const MirClosedCallBodyAnalysis *analysis =
        candidateSafety == nullptr ? nullptr
                                   : candidateSafety->find(claim.target.body);
    const MirSafetyProof *proof =
        analysis == nullptr
            ? nullptr
            : analysis->safety.find(claim.target.instruction, claim.discharge);
    if (location.block == nullptr || location.instruction == nullptr ||
        proof == nullptr) {
      addError(result, claim.target.body, 0, claim.target.instruction,
               "MIR safety-check proof became invalid before mutation");
      continue;
    }
    location.instruction->safetyDischarges.push_back(claim.discharge);
    std::sort(
        location.instruction->safetyDischarges.begin(),
        location.instruction->safetyDischarges.end(),
        [](const MirSafetyDischarge &left, const MirSafetyDischarge &right) {
          return std::tuple{static_cast<std::size_t>(left.family), left.origin,
                            static_cast<std::size_t>(left.outcome.code),
                            static_cast<std::size_t>(left.outcome.detail)} <
                 std::tuple{static_cast<std::size_t>(right.family),
                            right.origin,
                            static_cast<std::size_t>(right.outcome.code),
                            static_cast<std::size_t>(right.outcome.detail)};
        });
    result.transformations.push_back(
        {.pass = pass,
         .kind = MirTransformKind::DischargeSafetyCheck,
         .proof = mirSafetyTransformProof(claim.discharge.family),
         .body = claim.target.body,
         .block = location.block->id,
         .instruction = claim.target.instruction,
         .sourceValue = location.instruction->hirValue,
         .sourceStatement = location.instruction->hirStatement,
         .safetyDischarge = claim.discharge,
         .relatedBlocks = proof->relatedBlocks,
         .relatedInstructions = proof->relatedInstructions,
         .relatedValues = proof->relatedValues,
         .relatedPlaces = proof->relatedPlaces,
         .requiredAnalyses = {.valueUses = true,
                              .reachability = true,
                              .dominance = true,
                              .safetyChecks = true}});
    if (std::find(touched.begin(), touched.end(), claim.target.body) ==
        touched.end()) {
      touched.push_back(claim.target.body);
    }
  }
  if (!result.verification.valid()) {
    return result;
  }

  for (MirBodyAddress address : touched) {
    MirBody *target = findMirBody(candidate, address);
    if (target == nullptr || !rebuildMirValueUses(*target)) {
      addError(result, address, 0, 0,
               "MIR replacement produced invalid value uses");
    }
    rebuildMirReachability(*target);
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
  result.appliedPatches =
      queued.size() + queuedBranchFolds.size() + queuedDeadInstructions.size() +
      queuedUnreachableBlocks.size() + queuedSafetyDischarges.size() +
      queuedExactReceivers.size();
  for (const ScalarPromotion &promotion : queuedScalarPromotions) {
    (void)promotion;
    ++result.appliedPatches;
  }
  result.valueUsesRebuilt = true;
  result.invalidation.instructionFacts = true;
  result.invalidation.valueUses = true;
  result.invalidation.safetyChecks = true;
  if (!queuedBranchFolds.empty()) {
    result.invalidation.controlFlow = true;
    result.invalidation.reachability = true;
    result.invalidation.dominance = true;
  }
  if (!queuedScalarPromotions.empty()) {
    result.invalidation.instructionFacts = true;
    result.invalidation.valueUses = true;
    result.invalidation.dominance = true;
  }
  if (!queuedUnreachableBlocks.empty()) {
    result.invalidation.instructionFacts = true;
    result.invalidation.valueUses = true;
    result.invalidation.controlFlow = true;
    result.invalidation.reachability = true;
    result.invalidation.dominance = true;
  }
  if (!queuedDeadInstructions.empty()) {
    result.invalidation.instructionFacts = true;
    result.invalidation.valueUses = true;
  }
  std::sort(result.transformations.begin(), result.transformations.end(),
            mirTransformRecordLess);
  return result;
}

} // namespace lang
