#include "gti/optimization/pass.h"

#include <algorithm>
#include <array>
#include <queue>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] std::vector<MirBlockId>
successors(const MirTerminator &terminator) {
  std::vector<MirBlockId> result;
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    result.push_back(terminator.target);
    break;
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Invoke:
    result.push_back(terminator.target);
    result.push_back(terminator.elseTarget);
    break;
  case MirTerminatorKind::Switch:
    result.push_back(terminator.target);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      result.push_back(target.target);
    }
    break;
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    break;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

[[nodiscard]] std::optional<MirCfgAnalysis> analyzeCfg(const MirBody &body) {
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return std::nullopt;
  }
  MirCfgAnalysis result;
  result.successors.resize(body.blocks.size());
  result.predecessors.resize(body.blocks.size());
  result.reachable.resize(body.blocks.size(), false);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (body.blocks[index].id != index + 1) {
      return std::nullopt;
    }
    result.successors[index] = successors(body.blocks[index].terminator);
    for (const MirBlockId target : result.successors[index]) {
      if (target == 0 || target > body.blocks.size()) {
        return std::nullopt;
      }
      result.predecessors[target - 1].push_back(index + 1);
    }
  }

  std::queue<MirBlockId> pending;
  pending.push(body.entry);
  while (!pending.empty()) {
    const MirBlockId block = pending.front();
    pending.pop();
    if (result.reachable[block - 1]) {
      continue;
    }
    result.reachable[block - 1] = true;
    for (const MirBlockId target : result.successors[block - 1]) {
      pending.push(target);
    }
  }

  std::vector<std::size_t> incoming(body.blocks.size(), 0);
  std::queue<MirBlockId> ready;
  std::size_t reachableCount = 0;
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (!result.reachable[index]) {
      continue;
    }
    ++reachableCount;
    incoming[index] = static_cast<std::size_t>(std::count_if(
        result.predecessors[index].begin(), result.predecessors[index].end(),
        [&](MirBlockId predecessor) {
          return result.reachable[predecessor - 1];
        }));
    if (incoming[index] == 0) {
      ready.push(index + 1);
    }
  }
  std::size_t visited = 0;
  while (!ready.empty()) {
    const MirBlockId block = ready.front();
    ready.pop();
    ++visited;
    for (const MirBlockId target : result.successors[block - 1]) {
      if (!result.reachable[target - 1] || incoming[target - 1] == 0) {
        continue;
      }
      --incoming[target - 1];
      if (incoming[target - 1] == 0) {
        ready.push(target);
      }
    }
  }
  result.acyclic = visited == reachableCount;
  return result;
}

[[nodiscard]] bool sameStorage(const MirPlace &left, const MirPlace &right) {
  if (left.key && right.key) {
    return *left.key == *right.key;
  }
  return left.root == MirPlaceRootKind::Binding &&
         right.root == MirPlaceRootKind::Binding && left.binding != 0 &&
         left.binding == right.binding;
}

[[nodiscard]] MirScalarPromotionInputs
analyzeScalarPromotion(const MirProgram &program, MirBodyAddress address,
                       const MirBody &body, const MirCfgAnalysis &cfg) {
  MirScalarPromotionInputs result;
  const bool failureSensitive =
      !body.failureRecords.empty() || !body.cleanupBoundaries.empty() ||
      std::any_of(
          body.blocks.begin(), body.blocks.end(), [](const MirBlock &block) {
            return block.activeFailure != 0 || block.failureParameter != 0 ||
                   block.terminator.kind == MirTerminatorKind::Invoke;
          });

  for (const MirPlace &place : body.places) {
    MirScalarPromotionMissReason miss =
        MirScalarPromotionMissReason::UnsupportedAccess;
    MirScalarPromotionCandidate candidate{.place = place.id};
    bool eligible = true;
    if (place.root != MirPlaceRootKind::Binding || place.binding == 0 ||
        !place.projections.empty() || place.access != AccessMode::Mutable) {
      miss = MirScalarPromotionMissReason::NotMutableBinding;
      eligible = false;
    } else if (!isMirBlockParameterType(place.type)) {
      miss = MirScalarPromotionMissReason::UnsupportedType;
      eligible = false;
    } else if (place.traits.drop != DropKind::Trivial ||
               place.traits.containsBorrowedState) {
      miss = MirScalarPromotionMissReason::OwningOrDropped;
      eligible = false;
    } else if (place.initiallyAvailable) {
      miss = MirScalarPromotionMissReason::InitiallyAvailable;
      eligible = false;
    } else if (failureSensitive) {
      miss = MirScalarPromotionMissReason::FailureSensitive;
      eligible = false;
    } else if (!cfg.acyclic) {
      miss = MirScalarPromotionMissReason::CyclicControlFlow;
      eligible = false;
    }

    if (eligible && std::any_of(body.places.begin(), body.places.end(),
                                [&](const MirPlace &other) {
                                  return other.id != place.id &&
                                         sameStorage(place, other);
                                })) {
      miss = MirScalarPromotionMissReason::AliasedOrProjected;
      eligible = false;
    }
    if (eligible &&
        (std::any_of(
             body.loans.begin(), body.loans.end(),
             [&](const MirLoan &loan) { return loan.source == place.id; }) ||
         std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                     [&](const MirDropObligation &drop) {
                       return drop.place == place.id;
                     }))) {
      miss = MirScalarPromotionMissReason::Loaned;
      eligible = false;
    }

    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (!eligible || instruction.tombstone) {
          continue;
        }
        const bool destination =
            instruction.destination && *instruction.destination == place.id;
        const bool operand = std::any_of(
            instruction.operands.begin(), instruction.operands.end(),
            [&](const MirOperand &value) { return value.place == place.id; });
        const bool receiver =
            instruction.receiver && instruction.receiver->place == place.id;
        if (destination) {
          const bool simpleWrite =
              (instruction.kind == MirInstructionKind::Initialize ||
               (instruction.kind == MirInstructionKind::Assign &&
                instruction.operation == MirOperation::Assign)) &&
              instruction.operands.size() == 1 &&
              (instruction.operands.front().kind == MirOperandKind::Value ||
               instruction.operands.front().kind == MirOperandKind::Constant) &&
              instruction.unsafeOperation == UnsafeOperationKind::None &&
              !instruction.rawMemoryAccess &&
              instruction.synchronization.kind ==
                  SynchronizationOperationKind::None &&
              instruction.definedFailure.empty() &&
              instruction.localFailureSites.empty() &&
              instruction.lifecycle.empty() &&
              instruction.fullExpressionEnd == 0 &&
              instruction.cleanupBoundaryEnd == 0;
          if (!simpleWrite || receiver ||
              (operand && instruction.kind != MirInstructionKind::Initialize &&
               instruction.kind != MirInstructionKind::Assign)) {
            miss = MirScalarPromotionMissReason::UnsupportedAccess;
            eligible = false;
          } else {
            candidate.writes.push_back(instruction.id);
          }
          continue;
        }
        if (operand) {
          const bool simpleLoad =
              instruction.kind == MirInstructionKind::Load &&
              instruction.result && instruction.operands.size() == 1 &&
              instruction.operands.front().kind == MirOperandKind::Copy &&
              instruction.unsafeOperation == UnsafeOperationKind::None &&
              !instruction.rawMemoryAccess &&
              instruction.synchronization.kind ==
                  SynchronizationOperationKind::None &&
              instruction.definedFailure.empty() &&
              instruction.localFailureSites.empty() &&
              instruction.lifecycle.empty() &&
              instruction.fullExpressionEnd == 0 &&
              instruction.cleanupBoundaryEnd == 0;
          if (!simpleLoad || receiver) {
            miss = MirScalarPromotionMissReason::UnsupportedAccess;
            eligible = false;
          } else {
            candidate.loads.push_back(instruction.id);
          }
        } else if (receiver) {
          miss = MirScalarPromotionMissReason::UnsupportedAccess;
          eligible = false;
        }
      }
      if (eligible && block.terminator.value &&
          block.terminator.value->place == place.id) {
        miss = MirScalarPromotionMissReason::UnsupportedAccess;
        eligible = false;
      }
    }

    if (eligible && candidate.loads.empty()) {
      miss = MirScalarPromotionMissReason::NoUsefulLoad;
      eligible = false;
    }
    if (eligible && candidate.writes.empty() && !place.initiallyAvailable) {
      miss = MirScalarPromotionMissReason::UnsupportedAccess;
      eligible = false;
    }
    if (eligible) {
      MirBody proof = body;
      if (rewriteMirScalarPlace(proof, place.id)) {
        result.candidates.push_back(std::move(candidate));
      } else {
        result.misses.push_back(
            {.place = place.id,
             .reason = MirScalarPromotionMissReason::UnsupportedAccess});
      }
    } else {
      result.misses.push_back({.place = place.id, .reason = miss});
    }
  }
  (void)program;
  (void)address;
  return result;
}

} // namespace

std::string_view name(MirAnalysisKind kind) {
  constexpr std::array names{std::string_view{"cfg-reachability"},
                             std::string_view{"dominance"},
                             std::string_view{"value-uses"},
                             std::string_view{"effects"},
                             std::string_view{"scalar-promotion-inputs"},
                             std::string_view{"safety-checks"},
                             std::string_view{"place-alias-escape"},
                             std::string_view{"loops"},
                             std::string_view{"sparse-constants"}};
  const std::size_t index = static_cast<std::size_t>(kind);
  return index < names.size() ? names[index] : "invalid";
}

std::string_view name(MirScalarPromotionMissReason reason) {
  constexpr std::array names{
      std::string_view{"not a mutable local binding"},
      std::string_view{"type is not a non-owning scalar"},
      std::string_view{"storage is projected or has an aliasing place"},
      std::string_view{"storage participates in a loan"},
      std::string_view{"storage owns cleanup or borrowed state"},
      std::string_view{"storage is already available at body entry"},
      std::string_view{"storage has an unsupported access"},
      std::string_view{"body has failure-sensitive control flow"},
      std::string_view{"body has cyclic control flow"},
      std::string_view{"storage has no promotable load"}};
  const std::size_t index = static_cast<std::size_t>(reason);
  return index < names.size() ? names[index] : "invalid";
}

MirPreservedAnalyses MirPreservedAnalyses::all() {
  MirPreservedAnalyses result;
  result.mask =
      (std::uint32_t{1} << static_cast<std::uint32_t>(MirAnalysisKind::Count)) -
      1;
  return result;
}

MirPreservedAnalyses MirPreservedAnalyses::none() { return {}; }

MirPreservedAnalyses
MirPreservedAnalyses::only(std::initializer_list<MirAnalysisKind> analyses) {
  MirPreservedAnalyses result;
  for (const MirAnalysisKind analysis : analyses) {
    if (analysis != MirAnalysisKind::Count) {
      result.mask |= std::uint32_t{1} << static_cast<std::uint32_t>(analysis);
    }
  }
  return result;
}

bool MirPreservedAnalyses::preserves(MirAnalysisKind analysis) const {
  return analysis != MirAnalysisKind::Count &&
         (mask & (std::uint32_t{1} << static_cast<std::uint32_t>(analysis))) !=
             0;
}

MirBodyAnalysisManager::Entry &
MirBodyAnalysisManager::entry(MirBodyAddress address) {
  const auto found =
      std::find_if(entries.begin(), entries.end(), [&](const Entry &candidate) {
        return candidate.address == address;
      });
  if (found != entries.end()) {
    return *found;
  }
  entries.push_back({.address = address});
  return entries.back();
}

const MirBodyAnalysisManager::Entry *
MirBodyAnalysisManager::findEntry(MirBodyAddress address) const {
  const auto found =
      std::find_if(entries.begin(), entries.end(), [&](const Entry &candidate) {
        return candidate.address == address;
      });
  return found == entries.end() ? nullptr : &*found;
}

bool MirBodyAnalysisManager::accepts(const MirProgram &program) {
  if (programIdentity == nullptr) {
    programIdentity = &program;
  }
  return programIdentity == &program;
}

std::size_t MirBodyAnalysisManager::revision(MirBodyAddress address) const {
  const Entry *cached = findEntry(address);
  return cached == nullptr ? 0 : cached->revision;
}

const MirCfgAnalysis *MirBodyAnalysisManager::cfg(const MirProgram &program,
                                                  MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.cfg) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      return nullptr;
    }
    cached.cfg = analyzeCfg(*body);
  }
  return cached.cfg ? &*cached.cfg : nullptr;
}

const MirDominanceInfo *
MirBodyAnalysisManager::dominance(const MirProgram &program,
                                  MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.dominance) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      return nullptr;
    }
    cached.dominance = computeMirDominance(*body);
  }
  return cached.dominance ? &*cached.dominance : nullptr;
}

const std::vector<std::vector<MirValueUse>> *
MirBodyAnalysisManager::valueUses(const MirProgram &program,
                                  MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.valueUses) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      return nullptr;
    }
    cached.valueUses = body->valueUses;
  }
  return &*cached.valueUses;
}

const std::vector<MirInstructionEffectFact> *
MirBodyAnalysisManager::effects(const MirProgram &program,
                                MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.effects) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      return nullptr;
    }
    cached.effects.emplace();
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        cached.effects->push_back(
            {.instruction = instruction.id,
             .effects = lang::effects(program, instruction)});
      }
    }
  }
  return &*cached.effects;
}

const MirScalarPromotionInputs *
MirBodyAnalysisManager::scalarPromotionInputs(const MirProgram &program,
                                              MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.scalarPromotionInputs) {
    const MirBody *body = findMirBody(program, address);
    const MirCfgAnalysis *controlFlow = cfg(program, address);
    if (body == nullptr || controlFlow == nullptr) {
      return nullptr;
    }
    cached.scalarPromotionInputs =
        analyzeScalarPromotion(program, address, *body, *controlFlow);
  }
  return &*cached.scalarPromotionInputs;
}

const MirSafetyAnalysis *
MirBodyAnalysisManager::safetyChecks(const MirProgram &program,
                                     MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.safetyChecks) {
    const MirPlaceAliasEscapeAnalysis *aliases =
        placeAliasEscape(program, address);
    if (findMirBody(program, address) == nullptr || aliases == nullptr) {
      return nullptr;
    }
    cached.safetyChecks = analyzeMirSafetyChecks(program, address, *aliases);
  }
  return &*cached.safetyChecks;
}

const MirPlaceAliasEscapeAnalysis *
MirBodyAnalysisManager::placeAliasEscape(const MirProgram &program,
                                         MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.placeAliasEscape) {
    if (findMirBody(program, address) == nullptr) {
      return nullptr;
    }
    cached.placeAliasEscape = analyzeMirPlaceAliasEscape(program, address);
  }
  return &*cached.placeAliasEscape;
}

const MirLoopAnalysis *MirBodyAnalysisManager::loops(const MirProgram &program,
                                                     MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.loops) {
    const MirBody *body = findMirBody(program, address);
    const MirDominanceInfo *dominanceInfo = dominance(program, address);
    if (body == nullptr || dominanceInfo == nullptr) {
      return nullptr;
    }
    cached.loops = analyzeMirLoops(*body, *dominanceInfo);
  }
  return &*cached.loops;
}

const MirSccpAnalysis *
MirBodyAnalysisManager::sparseConstants(const MirProgram &program,
                                        MirBodyAddress address) {
  if (!accepts(program)) {
    return nullptr;
  }
  Entry &cached = entry(address);
  if (!cached.sparseConstants) {
    if (findMirBody(program, address) == nullptr) {
      return nullptr;
    }
    cached.sparseConstants = analyzeMirSccp(program, address);
  }
  return &*cached.sparseConstants;
}

std::vector<std::string>
MirBodyAnalysisManager::invalidate(MirBodyAddress address,
                                   const MirPreservedAnalyses &preserved) {
  Entry &cached = entry(address);
  ++cached.revision;
  std::vector<std::string> invalidated;
  const auto drop = [&](MirAnalysisKind kind, auto &value) {
    if (!preserved.preserves(kind) && value.has_value()) {
      invalidated.emplace_back(name(kind));
      value.reset();
    }
  };
  drop(MirAnalysisKind::CfgReachability, cached.cfg);
  drop(MirAnalysisKind::Dominance, cached.dominance);
  drop(MirAnalysisKind::ValueUses, cached.valueUses);
  drop(MirAnalysisKind::Effects, cached.effects);
  drop(MirAnalysisKind::ScalarPromotionInputs, cached.scalarPromotionInputs);
  drop(MirAnalysisKind::SafetyChecks, cached.safetyChecks);
  drop(MirAnalysisKind::PlaceAliasEscape, cached.placeAliasEscape);
  drop(MirAnalysisKind::Loops, cached.loops);
  drop(MirAnalysisKind::SparseConstants, cached.sparseConstants);
  return invalidated;
}

} // namespace lang
