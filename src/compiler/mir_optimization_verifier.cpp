#include "gti/mir.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/devirtualization.h"
#include "gti/optimization/safety.h"
#include "gti/optimization/sccp.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lang {
MirVerificationResult
verifyMirOptimizationCoherence(const MirProgram &source,
                               const MirProgram &optimized,
                               const std::vector<MirTransformRecord> &records) {
  MirVerificationResult result;
  const MirVerificationResult sourceVerification = verifyMirProgram(source);
  if (!sourceVerification.valid()) {
    return sourceVerification;
  }
  const MirVerificationResult optimizedVerification =
      verifyMirProgram(optimized);
  if (!optimizedVerification.valid()) {
    return optimizedVerification;
  }

  const auto reject = [&](MirBodyAddress address, std::string message,
                          MirBlockId block = 0,
                          MirInstructionId instruction = 0) {
    result.errors.push_back({.bodyKind = address.kind,
                             .owner = address.owner,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };

  if (!std::is_sorted(records.begin(), records.end(), mirTransformRecordLess)) {
    reject({}, "MIR transformation records are not in deterministic order");
    return result;
  }
  if (std::any_of(
          records.begin(), records.end(), [](const MirTransformRecord &record) {
            return record.pass.empty() ||
                   record.kind == MirTransformKind::Count ||
                   record.proof == MirTransformProofKind::Count ||
                   (record.kind != MirTransformKind::DischargeSafetyCheck &&
                    (record.safetyDischarge.has_value() ||
                     (record.kind != MirTransformKind::DevirtualizeCall &&
                      !record.relatedPlaces.empty()) ||
                     record.requiredAnalyses.safetyChecks));
          })) {
    reject({}, "MIR transformation record has an invalid pass, kind, or "
               "safety-proof payload");
    return result;
  }

  if (source.executionProfile() != optimized.executionProfile() ||
      source.programKind() != optimized.programKind() ||
      source.embeddingPlan() != optimized.embeddingPlan() ||
      source.classInstances().size() != optimized.classInstances().size() ||
      source.functionInstances().size() !=
          optimized.functionInstances().size() ||
      source.constructorInstances().size() !=
          optimized.constructorInstances().size() ||
      source.destructorInstances().size() !=
          optimized.destructorInstances().size() ||
      source.lambdaInstances().size() != optimized.lambdaInstances().size()) {
    reject({}, "optimized MIR changed program/export policy or concrete "
               "instance cardinality");
    return result;
  }

  MirProgram candidate = source;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(source)) {
    const MirBody *sourceBody = findMirBody(source, address);
    const MirBody *optimizedBody = findMirBody(optimized, address);
    if (sourceBody == nullptr || optimizedBody == nullptr ||
        sourceBody->blocks.size() != optimizedBody->blocks.size()) {
      reject(address, "optimized MIR changed a source body or its stable block "
                      "schedule");
      return result;
    }
    for (const MirBlock &block : sourceBody->blocks) {
      if (block.terminator.provenance.kind !=
          MirTerminatorProvenanceKind::None) {
        reject(address,
               "source MIR already contains optimizer terminator provenance",
               block.id);
        return result;
      }
      for (const MirInstruction &instruction : block.instructions) {
        if (!instruction.safetyDischarges.empty()) {
          reject(address,
                 "source MIR already contains optimizer safety discharges",
                 block.id, instruction.id);
          return result;
        }
        if (instruction.tombstone || (instruction.literalProvenance.kind !=
                                          MirLiteralProvenanceKind::None &&
                                      instruction.literalProvenance.kind !=
                                          MirLiteralProvenanceKind::Source)) {
          reject(address,
                 "source MIR already contains a destructive optimizer edit",
                 block.id, instruction.id);
          return result;
        }
      }
    }
  }

  std::vector<bool> consumed(records.size(), false);
  const auto claim = [&](const auto &predicate) -> const MirTransformRecord * {
    std::optional<std::size_t> match;
    for (std::size_t index = 0; index < records.size(); ++index) {
      if (consumed[index] || !predicate(records[index])) {
        continue;
      }
      if (match) {
        return nullptr;
      }
      match = index;
    }
    if (!match) {
      return nullptr;
    }
    consumed[*match] = true;
    return &records[*match];
  };
  const auto required = [](const MirTransformRecord &record, bool valueUses,
                           bool effects, bool reachability, bool dominance,
                           bool scalarPromotion) {
    return record.requiredAnalyses ==
           MirTransformRequiredAnalyses{.valueUses = valueUses,
                                        .effects = effects,
                                        .reachability = reachability,
                                        .dominance = dominance,
                                        .scalarPromotion = scalarPromotion};
  };
  struct InstructionLocation {
    MirBlock *block = nullptr;
    MirInstruction *instruction = nullptr;
  };
  const auto findInstruction = [](MirBody &body,
                                  MirInstructionId id) -> InstructionLocation {
    for (MirBlock &block : body.blocks) {
      const auto found =
          std::find_if(block.instructions.begin(), block.instructions.end(),
                       [&](const MirInstruction &instruction) {
                         return instruction.id == id;
                       });
      if (found != block.instructions.end()) {
        return {.block = &block, .instruction = &*found};
      }
    }
    return {};
  };
  const auto literalDefinition =
      [&](const MirBody &body, MirValueId value) -> const MirInstruction * {
    const MirValue *definition = body.findValue(value);
    const MirBlock *block = definition == nullptr
                                ? nullptr
                                : body.findBlock(definition->definitionBlock);
    if (block == nullptr) {
      return nullptr;
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const MirInstruction &instruction) {
                       return instruction.id == definition->definition &&
                              instruction.result == value;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };
  const auto chaseLiteral = [&](const MirBody &body,
                                MirValueId value) -> std::optional<Literal> {
    std::unordered_set<MirValueId> visited;
    while (value != 0 && visited.size() <= body.values.size() &&
           visited.insert(value).second) {
      const MirInstruction *definition = literalDefinition(body, value);
      if (definition == nullptr || definition->tombstone ||
          definition->kind != MirInstructionKind::Compute) {
        return std::nullopt;
      }
      if (definition->operation == MirOperation::Literal &&
          definition->literal) {
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
  };

  // Scalar promotion is replayed first because it establishes block values
  // that later local folds and dead-value cleanup may consume.
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (consumed[index] ||
        records[index].kind != MirTransformKind::PromotePlace ||
        records[index].proof != MirTransformProofKind::ScalarPromotion) {
      continue;
    }
    const MirTransformRecord &placeRecord = records[index];
    MirBody *body = findMirBody(candidate, placeRecord.body);
    if (body == nullptr || placeRecord.block != 0 ||
        placeRecord.instruction != 0 || placeRecord.place == 0 ||
        placeRecord.replacementLiteral) {
      reject(placeRecord.body, "scalar-promotion record has an invalid shape");
      return result;
    }
    const MirBody before = *body;
    const MirPlace *place = before.findPlace(placeRecord.place);
    const std::optional<MirScalarPromotionRewrite> rewrite =
        rewriteMirScalarPlace(*body, placeRecord.place);
    std::vector<MirInstructionId> relatedInstructions;
    if (rewrite) {
      relatedInstructions = rewrite->replacedInstructions;
      relatedInstructions.insert(relatedInstructions.end(),
                                 rewrite->removedInstructions.begin(),
                                 rewrite->removedInstructions.end());
      std::sort(relatedInstructions.begin(), relatedInstructions.end());
    }
    if (place == nullptr || !rewrite ||
        placeRecord.sourceValue != place->sourceValue ||
        placeRecord.sourceStatement != 0 ||
        placeRecord.relatedBlocks != rewrite->parameterBlocks ||
        placeRecord.relatedInstructions != relatedInstructions ||
        placeRecord.relatedValues != rewrite->createdValues ||
        !required(placeRecord, true, true, true, true, true)) {
      reject(placeRecord.body,
             "scalar-promotion record does not replay from canonical MIR", 0,
             0);
      return result;
    }
    consumed[index] = true;

    for (const MirInstructionId instructionId : rewrite->replacedInstructions) {
      MirBody beforeCopy = before;
      const InstructionLocation sourceLocation =
          findInstruction(beforeCopy, instructionId);
      const InstructionLocation targetLocation =
          findInstruction(*body, instructionId);
      std::vector<MirValueId> values;
      if (sourceLocation.instruction != nullptr &&
          sourceLocation.instruction->result) {
        values.push_back(*sourceLocation.instruction->result);
      }
      if (targetLocation.instruction != nullptr) {
        for (const MirOperand &operand : targetLocation.instruction->operands) {
          if (operand.kind == MirOperandKind::Value) {
            values.push_back(operand.value);
          }
        }
      }
      const MirTransformRecord *record =
          claim([&](const auto &candidateRecord) {
            return candidateRecord.body == placeRecord.body &&
                   candidateRecord.kind ==
                       MirTransformKind::ReplaceInstruction &&
                   candidateRecord.proof ==
                       MirTransformProofKind::ScalarPromotion &&
                   candidateRecord.instruction == instructionId;
          });
      if (record == nullptr || sourceLocation.block == nullptr ||
          sourceLocation.instruction == nullptr ||
          targetLocation.instruction == nullptr ||
          record->pass != placeRecord.pass ||
          record->block != sourceLocation.block->id ||
          record->place != placeRecord.place || record->replacementLiteral ||
          record->sourceValue != sourceLocation.instruction->hirValue ||
          record->sourceStatement != sourceLocation.instruction->hirStatement ||
          !record->relatedBlocks.empty() ||
          !record->relatedInstructions.empty() ||
          record->relatedValues != values ||
          !required(*record, true, true, true, true, true)) {
        reject(placeRecord.body,
               "scalar promotion is missing an exact replacement record", 0,
               instructionId);
        return result;
      }
    }
    for (const MirInstructionId instructionId : rewrite->removedInstructions) {
      MirBody beforeCopy = before;
      const InstructionLocation sourceLocation =
          findInstruction(beforeCopy, instructionId);
      const MirTransformRecord *record =
          claim([&](const auto &candidateRecord) {
            return candidateRecord.body == placeRecord.body &&
                   candidateRecord.kind ==
                       MirTransformKind::RemoveInstruction &&
                   candidateRecord.proof ==
                       MirTransformProofKind::ScalarPromotion &&
                   candidateRecord.instruction == instructionId;
          });
      const std::vector<MirValueId> values =
          sourceLocation.instruction != nullptr &&
                  sourceLocation.instruction->result
              ? std::vector<MirValueId>{*sourceLocation.instruction->result}
              : std::vector<MirValueId>{};
      if (record == nullptr || sourceLocation.block == nullptr ||
          sourceLocation.instruction == nullptr ||
          record->pass != placeRecord.pass ||
          record->block != sourceLocation.block->id ||
          record->place != placeRecord.place || record->replacementLiteral ||
          record->sourceValue != sourceLocation.instruction->hirValue ||
          record->sourceStatement != sourceLocation.instruction->hirStatement ||
          !record->relatedBlocks.empty() ||
          !record->relatedInstructions.empty() ||
          record->relatedValues != values ||
          !required(*record, true, true, true, true, true)) {
        reject(placeRecord.body,
               "scalar promotion is missing an exact removal record", 0,
               instructionId);
        return result;
      }
    }
  }

  // Fold records may form short identity/compute chains. Replay to a bounded
  // fixed point from the candidate rather than accepting the final literal.
  for (std::size_t round = 0; round <= records.size(); ++round) {
    bool progress = false;
    for (std::size_t index = 0; index < records.size(); ++index) {
      const MirTransformRecord &record = records[index];
      if (consumed[index] ||
          record.kind != MirTransformKind::ReplaceInstruction ||
          (record.proof != MirTransformProofKind::IdentityFold &&
           record.proof != MirTransformProofKind::ComputeFold)) {
        continue;
      }
      MirBody *body = findMirBody(candidate, record.body);
      if (body == nullptr) {
        continue;
      }
      const InstructionLocation location =
          findInstruction(*body, record.instruction);
      if (location.block == nullptr || location.instruction == nullptr ||
          location.block->id != record.block || !record.replacementLiteral ||
          record.place != 0 || !record.relatedBlocks.empty() ||
          !record.relatedInstructions.empty() ||
          record.sourceValue != location.instruction->hirValue ||
          record.sourceStatement != location.instruction->hirStatement ||
          !required(record, true, false, false, true, false)) {
        continue;
      }
      MirInstruction &instruction = *location.instruction;
      std::optional<Literal> folded;
      MirLiteralProvenance provenance;
      if (record.proof == MirTransformProofKind::IdentityFold &&
          instruction.kind == MirInstructionKind::Compute &&
          instruction.operation == MirOperation::Identity &&
          instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Value) {
        folded = chaseLiteral(*body, instruction.operands.front().value);
        provenance = {.kind = MirLiteralProvenanceKind::IdentityFold,
                      .sourceValue = instruction.operands.front().value};
        if (record.relatedValues !=
            std::vector<MirValueId>{instruction.operands.front().value}) {
          folded.reset();
        }
      } else if (record.proof == MirTransformProofKind::ComputeFold &&
                 instruction.kind == MirInstructionKind::Compute &&
                 instruction.operation != MirOperation::Identity &&
                 instruction.operation != MirOperation::Literal &&
                 !instruction.operands.empty() &&
                 instruction.operands.size() <= 2 &&
                 instruction.definedFailure.empty() &&
                 instruction.localFailureSites.empty()) {
        std::vector<MirComputeFoldOperand> operands;
        std::vector<MirValueId> values;
        for (const MirOperand &operand : instruction.operands) {
          const MirInstruction *definition =
              operand.kind == MirOperandKind::Value
                  ? literalDefinition(*body, operand.value)
                  : nullptr;
          const MirValue *value = operand.kind == MirOperandKind::Value
                                      ? body->findValue(operand.value)
                                      : nullptr;
          if (definition == nullptr || value == nullptr ||
              definition->operation != MirOperation::Literal ||
              !definition->literal) {
            operands.clear();
            break;
          }
          operands.push_back(
              {.literal = *definition->literal, .type = value->info.type});
          values.push_back(operand.value);
        }
        if (!operands.empty() && values == record.relatedValues) {
          folded = evaluateMirComputeFold(instruction.operation, operands,
                                          instruction.info.type);
          provenance = {.kind = MirLiteralProvenanceKind::ComputeFold,
                        .sourceOperation = instruction.operation,
                        .sourceValues = std::move(values)};
        }
      }
      if (!folded || *folded != *record.replacementLiteral) {
        continue;
      }
      instruction.operation = MirOperation::Literal;
      instruction.operands.clear();
      instruction.literal = *folded;
      instruction.literalProvenance = std::move(provenance);
      consumed[index] = true;
      progress = true;
    }
    if (!progress) {
      break;
    }
  }

  // Sparse constants are replayed as one snapshot-scoped batch after local
  // folds. SCCP is rerun before any sparse edit, so one replacement cannot
  // manufacture the evidence used to authorize another replacement.
  struct SparseSnapshot {
    MirBodyAddress address;
    MirSccpAnalysis analysis;
  };
  std::vector<SparseSnapshot> sparseSnapshots;
  for (const MirTransformRecord &record : records) {
    if (record.kind != MirTransformKind::ReplaceInstruction ||
        record.proof != MirTransformProofKind::SparseConstant ||
        std::any_of(sparseSnapshots.begin(), sparseSnapshots.end(),
                    [&](const SparseSnapshot &snapshot) {
                      return snapshot.address == record.body;
                    })) {
      continue;
    }
    sparseSnapshots.push_back(
        {.address = record.body,
         .analysis = analyzeMirSccp(candidate, record.body)});
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const MirTransformRecord &record = records[index];
    if (consumed[index] ||
        record.kind != MirTransformKind::ReplaceInstruction ||
        record.proof != MirTransformProofKind::SparseConstant) {
      continue;
    }
    MirBody *body = findMirBody(candidate, record.body);
    const InstructionLocation location =
        body == nullptr ? InstructionLocation{}
                        : findInstruction(*body, record.instruction);
    const auto snapshot =
        std::find_if(sparseSnapshots.begin(), sparseSnapshots.end(),
                     [&](const SparseSnapshot &value) {
                       return value.address == record.body;
                     });
    const MirSccpValueFact *fact =
        snapshot != sparseSnapshots.end() && location.instruction != nullptr &&
                location.instruction->result
            ? snapshot->analysis.find(*location.instruction->result)
            : nullptr;
    const MirTransformRequiredAnalyses requiredSparse{.valueUses = true,
                                                      .reachability = true,
                                                      .dominance = true,
                                                      .sparseConstants = true};
    if (location.block == nullptr || location.instruction == nullptr ||
        fact == nullptr || !snapshot->analysis.complete ||
        location.block->id != record.block || record.place != 0 ||
        !record.replacementLiteral || !record.relatedPlaces.empty() ||
        location.instruction->kind != MirInstructionKind::Compute ||
        location.instruction->operation == MirOperation::Literal ||
        fact->kind != MirSccpLatticeKind::Constant ||
        fact->literal != record.replacementLiteral ||
        record.sourceValue != location.instruction->hirValue ||
        record.sourceStatement != location.instruction->hirStatement ||
        record.relatedBlocks != fact->evidence.blocks ||
        record.relatedInstructions != fact->evidence.instructions ||
        record.relatedValues != fact->evidence.values ||
        record.requiredAnalyses != requiredSparse) {
      reject(record.body,
             "sparse-constant record does not replay from canonical SCCP",
             record.block, record.instruction);
      return result;
    }
    MirInstruction &instruction = *location.instruction;
    instruction.literalProvenance = {
        .kind = MirLiteralProvenanceKind::SparseConstant,
        .sourceOperation = instruction.operation,
        .sourceValues = record.relatedValues};
    instruction.operation = MirOperation::Literal;
    instruction.operands.clear();
    instruction.literal = record.replacementLiteral;
    consumed[index] = true;
  }

  for (std::size_t index = 0; index < records.size(); ++index) {
    const MirTransformRecord &record = records[index];
    if (consumed[index] || record.kind != MirTransformKind::ReplaceTerminator ||
        record.proof != MirTransformProofKind::BranchFold) {
      continue;
    }
    MirBody *body = findMirBody(candidate, record.body);
    MirBlock *block = body == nullptr ? nullptr : body->findBlock(record.block);
    if (block == nullptr ||
        block->terminator.kind != MirTerminatorKind::Branch ||
        !block->terminator.value ||
        block->terminator.value->kind != MirOperandKind::Value) {
      reject(record.body, "branch-fold record does not name a source branch",
             record.block);
      return result;
    }
    const MirTerminator sourceTerminator = block->terminator;
    const MirValueId condition = sourceTerminator.value->value;
    const std::optional<Literal> literal = chaseLiteral(*body, condition);
    const bool *taken = literal ? std::get_if<bool>(&*literal) : nullptr;
    if (taken == nullptr || record.instruction != 0 || record.place != 0 ||
        record.replacementLiteral ||
        record.sourceValue != sourceTerminator.hirValue ||
        record.sourceStatement != sourceTerminator.hirStatement ||
        !record.relatedInstructions.empty() ||
        record.relatedValues != std::vector<MirValueId>{condition} ||
        record.relatedBlocks !=
            std::vector<MirBlockId>{sourceTerminator.target,
                                    sourceTerminator.elseTarget} ||
        !required(record, true, false, true, true, false)) {
      reject(record.body,
             "branch-fold record does not replay from its retained condition",
             record.block);
      return result;
    }
    block->terminator.kind = MirTerminatorKind::Goto;
    block->terminator.targetArguments = *taken
                                            ? sourceTerminator.targetArguments
                                            : sourceTerminator.elseArguments;
    block->terminator.target =
        *taken ? sourceTerminator.target : sourceTerminator.elseTarget;
    block->terminator.elseTarget = 0;
    block->terminator.elseArguments.clear();
    block->terminator.value.reset();
    block->terminator.provenance = {.kind =
                                        MirTerminatorProvenanceKind::BranchFold,
                                    .foldSourceValue = condition};
    if (!rebuildMirValueUses(*body)) {
      reject(record.body,
             "branch-fold replay could not rebuild exact MIR value uses",
             record.block);
      return result;
    }
    rebuildMirReachability(*body);
    consumed[index] = true;
  }

  // Literal replacements alter operand-use tables even when no branch in the
  // same body is folded. Restore a fully valid intermediate snapshot before
  // invoking canonical destructive rewrites, which deliberately fail closed
  // when presented with stale MIR analyses.
  for (const MirBodyAddress address : enumerateMirBodyAddresses(candidate)) {
    MirBody *body = findMirBody(candidate, address);
    if (body == nullptr || !rebuildMirValueUses(*body)) {
      reject(address,
             "fold replay could not rebuild exact MIR value uses before "
             "destructive cleanup");
      return result;
    }
    rebuildMirReachability(*body);
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto &record = records[index];
    if (consumed[index] || record.kind != MirTransformKind::DevirtualizeCall)
      continue;
    auto replay = rewriteMirExactReceiver(candidate, record.body,
                                          record.instruction, record.pass);
    if (!replay || *replay != record) {
      reject(record.body,
             "exact receiver transformation does not replay from canonical MIR",
             record.block, record.instruction);
      return result;
    }
    consumed[index] = true;
    auto *body = findMirBody(candidate, record.body);
    if (!body || !rebuildMirValueUses(*body)) {
      reject(record.body, "receiver refinement failed to rebuild MIR uses");
      return result;
    }
  }
  const MirVerificationResult foldedVerification = verifyMirProgram(candidate);
  if (!foldedVerification.valid()) {
    return foldedVerification;
  }

  // Safety discharges are replayed from one immutable canonical analysis of
  // the candidate after value/branch folds and before destructive cleanup.
  // Attaching a discharge changes metadata only, so every record can be
  // checked against the same independently derived whole-program snapshot.
  // O0/O1 snapshots carry no such records and do not pay for an analysis that
  // could not authorize any replay step.
  std::optional<MirClosedCallSafetyAnalysis> replaySafety;
  if (std::any_of(
          records.begin(), records.end(), [](const MirTransformRecord &record) {
            return record.kind == MirTransformKind::DischargeSafetyCheck;
          })) {
    replaySafety = analyzeMirClosedCallSafetyChecks(candidate);
  }
  for (std::size_t index = 0; index < records.size(); ++index) {
    const MirTransformRecord &record = records[index];
    if (consumed[index] ||
        record.kind != MirTransformKind::DischargeSafetyCheck) {
      continue;
    }
    MirBody *body = findMirBody(candidate, record.body);
    const InstructionLocation location =
        body == nullptr ? InstructionLocation{}
                        : findInstruction(*body, record.instruction);
    const MirClosedCallBodyAnalysis *analysis =
        replaySafety ? replaySafety->find(record.body) : nullptr;
    const MirSafetyProof *proof =
        record.safetyDischarge && analysis != nullptr
            ? analysis->safety.find(record.instruction, *record.safetyDischarge)
            : nullptr;
    const MirTransformRequiredAnalyses requiredSafety{.valueUses = true,
                                                      .reachability = true,
                                                      .dominance = true,
                                                      .safetyChecks = true};
    if (location.block == nullptr || location.instruction == nullptr ||
        proof == nullptr || location.block->id != record.block ||
        record.place != 0 || record.replacementLiteral ||
        record.proof !=
            mirSafetyTransformProof(record.safetyDischarge->family) ||
        record.sourceValue != location.instruction->hirValue ||
        record.sourceStatement != location.instruction->hirStatement ||
        record.relatedBlocks != proof->relatedBlocks ||
        record.relatedInstructions != proof->relatedInstructions ||
        record.relatedValues != proof->relatedValues ||
        record.relatedPlaces != proof->relatedPlaces ||
        record.requiredAnalyses != requiredSafety ||
        hasMirSafetyDischarge(*location.instruction, *record.safetyDischarge)) {
      reject(record.body,
             "safety-check discharge record does not replay from canonical "
             "MIR facts",
             record.block, record.instruction);
      return result;
    }
    location.instruction->safetyDischarges.push_back(*record.safetyDischarge);
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
    consumed[index] = true;
  }

  const MirVerificationResult safetyVerification = verifyMirProgram(candidate);
  if (!safetyVerification.valid()) {
    return safetyVerification;
  }

  for (std::size_t index = 0; index < records.size(); ++index) {
    const MirTransformRecord &record = records[index];
    if (consumed[index] || record.kind != MirTransformKind::RemoveBlock ||
        record.proof != MirTransformProofKind::UnreachableBlock) {
      continue;
    }
    MirBody *body = findMirBody(candidate, record.body);
    const std::optional<MirUnreachableBlockRewrite> rewrite =
        body == nullptr ? std::nullopt
                        : rewriteMirUnreachableBlock(*body, record.block);
    if (!rewrite) {
      reject(record.body,
             "unreachable-block record names a block that canonical CFG "
             "cleanup cannot remove (block " +
                 std::to_string(record.block) + ")",
             record.block);
      return result;
    }
    if (record.instruction != 0 || record.place != 0 ||
        record.replacementLiteral ||
        record.sourceValue != rewrite->sourceValue ||
        record.sourceStatement != rewrite->sourceStatement ||
        !record.relatedBlocks.empty() || !record.relatedValues.empty() ||
        record.relatedInstructions != rewrite->removedInstructions ||
        !required(record, true, true, true, true, false)) {
      reject(record.body,
             "unreachable-block record payload does not match canonical CFG "
             "cleanup",
             record.block);
      return result;
    }
    consumed[index] = true;
  }

  // Removing one unused computation may make its operands unused. Replay the
  // bounded cleanup set to a fixed point using exact current value uses.
  for (std::size_t round = 0; round <= records.size(); ++round) {
    bool progress = false;
    for (std::size_t index = 0; index < records.size(); ++index) {
      const MirTransformRecord &record = records[index];
      if (consumed[index] ||
          record.kind != MirTransformKind::RemoveInstruction ||
          record.proof != MirTransformProofKind::DeadUnusedComputation) {
        continue;
      }
      MirProgram replay = candidate;
      const std::optional<MirDeadInstructionRewrite> rewrite =
          rewriteMirDeadInstruction(replay, record.body, record.instruction);
      const std::vector<MirValueId> values =
          rewrite && rewrite->result ? std::vector<MirValueId>{*rewrite->result}
                                     : std::vector<MirValueId>{};
      if (!rewrite) {
        continue;
      }
      if (record.block != rewrite->block || record.place != 0 ||
          record.replacementLiteral ||
          record.sourceValue != rewrite->sourceValue ||
          record.sourceStatement != rewrite->sourceStatement ||
          !record.relatedBlocks.empty() ||
          !record.relatedInstructions.empty() ||
          record.relatedValues != values ||
          !required(record, true, true, false, false, false)) {
        reject(record.body,
               "dead-computation record does not match exact uses and effects",
               record.block, record.instruction);
        return result;
      }
      candidate = std::move(replay);
      consumed[index] = true;
      progress = true;
    }
    if (!progress) {
      break;
    }
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(candidate)) {
    MirBody *body = findMirBody(candidate, address);
    if (body == nullptr || !rebuildMirValueUses(*body)) {
      reject(address,
             "authorized MIR rewrites could not rebuild exact value uses");
      return result;
    }
    rebuildMirReachability(*body);
  }

  const MirVerificationResult candidateVerification =
      verifyMirProgram(candidate);
  if (!candidateVerification.valid()) {
    return candidateVerification;
  }
  if (candidate != optimized) {
    const std::vector<MirBodyAddress> bodyAddresses =
        enumerateMirBodyAddresses(candidate);
    const auto mismatchedBody =
        std::find_if(bodyAddresses.begin(), bodyAddresses.end(),
                     [&](const MirBodyAddress address) {
                       const MirBody *replayed =
                           findMirBody(candidate, address);
                       const MirBody *actual = findMirBody(optimized, address);
                       return replayed == nullptr || actual == nullptr ||
                              *replayed != *actual;
                     });
    if (mismatchedBody != bodyAddresses.end()) {
      const MirBody *replayed = findMirBody(candidate, *mismatchedBody);
      const MirBody *actual = findMirBody(optimized, *mismatchedBody);
      MirBlockId block = 0;
      MirInstructionId instruction = 0;
      std::string component = "body metadata";
      if (replayed != nullptr && actual != nullptr &&
          replayed->blocks.size() == actual->blocks.size()) {
        for (std::size_t blockIndex = 0; blockIndex < replayed->blocks.size();
             ++blockIndex) {
          const MirBlock &replayedBlock = replayed->blocks[blockIndex];
          const MirBlock &actualBlock = actual->blocks[blockIndex];
          if (replayedBlock == actualBlock) {
            continue;
          }
          block = replayedBlock.id;
          component = "block metadata or terminator";
          const std::size_t instructionCount =
              std::min(replayedBlock.instructions.size(),
                       actualBlock.instructions.size());
          for (std::size_t instructionIndex = 0;
               instructionIndex < instructionCount; ++instructionIndex) {
            if (replayedBlock.instructions[instructionIndex] ==
                actualBlock.instructions[instructionIndex]) {
              continue;
            }
            instruction = replayedBlock.instructions[instructionIndex].id;
            component = "instruction";
            break;
          }
          break;
        }
      }
      reject(
          *mismatchedBody,
          "optimized MIR body contains a change not authorized by MIR "
          "rewrite provenance (body kind " +
              std::to_string(static_cast<std::size_t>(mismatchedBody->kind)) +
              ", owner " + std::to_string(mismatchedBody->owner) + ", " +
              component +
              (block == 0 ? std::string{}
                          : " in block " + std::to_string(block)) +
              (instruction == 0
                   ? std::string{}
                   : " at instruction " + std::to_string(instruction)) +
              ")",
          block, instruction);
    } else {
      reject({}, "optimized MIR contains a non-body change not authorized by "
                 "MIR rewrite provenance");
    }
  } else if (std::any_of(consumed.begin(), consumed.end(),
                         [](bool value) { return !value; })) {
    reject({}, "MIR transformation record does not describe an optimized edit");
  }
  return result;
}

std::optional<Literal>
evaluateMirComputeFold(MirOperation operation,
                       const std::vector<MirComputeFoldOperand> &operands,
                       const SemanticType &resultType) {
  const auto comparisonToken = [&]() -> std::optional<TokenKind> {
    switch (operation) {
    case MirOperation::Equal:
      return TokenKind::EQUAL_EQUAL;
    case MirOperation::NotEqual:
      return TokenKind::BANG_EQUAL;
    case MirOperation::Less:
      return TokenKind::LESS;
    case MirOperation::LessEqual:
      return TokenKind::LESS_EQUAL;
    case MirOperation::Greater:
      return TokenKind::GREATER;
    case MirOperation::GreaterEqual:
      return TokenKind::GREATER_EQUAL;
    default:
      return std::nullopt;
    }
  }();
  std::vector<ConstantValue> values;
  values.reserve(operands.size());
  for (const MirComputeFoldOperand &operand : operands) {
    const ConstantEvaluation evaluated = evaluateConstantLiteral(
        operand.literal, constantIntegerDomain(operand.type));
    if (!evaluated.value) {
      return std::nullopt;
    }
    values.push_back(*evaluated.value);
  }
  if (operation == MirOperation::Convert) {
    const std::optional<BinaryFloatFormat> format =
        semanticFloatFormat(resultType);
    if (values.size() != 1 || !format) {
      return std::nullopt;
    }
    const ConstantEvaluation converted =
        convertConstantFloat(values.front(), *format);
    const auto *folded =
        converted.value ? std::get_if<BinaryFloat>(&*converted.value) : nullptr;
    return folded == nullptr ? std::nullopt
                             : std::optional<Literal>{Literal{*folded}};
  }

  const auto arithmeticToken = [&]() -> std::optional<TokenKind> {
    switch (operation) {
    case MirOperation::Add:
      return TokenKind::PLUS;
    case MirOperation::Subtract:
      return TokenKind::MINUS;
    case MirOperation::Multiply:
      return TokenKind::STAR;
    case MirOperation::Divide:
      return TokenKind::SLASH;
    default:
      return std::nullopt;
    }
  }();
  if (arithmeticToken) {
    const std::optional<BinaryFloatFormat> format =
        semanticFloatFormat(resultType);
    if (values.size() != 2 || !format) {
      return std::nullopt;
    }
    const ConstantEvaluation evaluated = evaluateConstantBinary(
        *arithmeticToken, values[0], values[1], std::nullopt, *format);
    const auto *folded =
        evaluated.value ? std::get_if<BinaryFloat>(&*evaluated.value) : nullptr;
    return folded == nullptr ? std::nullopt
                             : std::optional<Literal>{Literal{*folded}};
  }

  if (resultType.kind != SemanticType::Bool) {
    return std::nullopt;
  }
  ConstantEvaluation result;
  if (operation == MirOperation::LogicalNot) {
    if (values.size() != 1) {
      return std::nullopt;
    }
    result = evaluateConstantUnary(TokenKind::BANG, values.front());
  } else if (comparisonToken) {
    if (values.size() != 2) {
      return std::nullopt;
    }
    result = evaluateConstantComparison(*comparisonToken, values[0], values[1]);
  } else {
    return std::nullopt;
  }
  if (!result.value) {
    return std::nullopt;
  }
  const bool *folded = std::get_if<bool>(&*result.value);
  if (folded == nullptr) {
    return std::nullopt;
  }
  return Literal{*folded};
}

} // namespace lang
