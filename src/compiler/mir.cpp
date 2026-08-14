#include "gti/mir.h"
#include "gti/mir_dominance.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lang {

const std::vector<MirValueUse> &MirBody::usesOf(MirValueId id) const {
  static const std::vector<MirValueUse> empty;
  return id == 0 || id > valueUses.size() ? empty : valueUses[id - 1];
}

std::size_t MirBody::instructionCount() const {
  std::size_t result = 0;
  for (const MirBlock &block : blocks) {
    result += block.instructions.size();
  }
  return result;
}

std::size_t MirProgram::blockCount() const {
  std::size_t result = moduleBody.blocks.size();
  for (const MirClassInstance &instance : classes) {
    result += instance.fieldInitializers.blocks.size();
    result += instance.staticFieldInitializers.blocks.size();
  }
  for (const MirFunctionInstance &instance : functions) {
    result += instance.body.blocks.size();
  }
  for (const MirConstructorInstance &instance : constructors) {
    result += instance.body.blocks.size();
  }
  for (const MirDestructorInstance &instance : destructors) {
    result += instance.body.blocks.size();
  }
  for (const MirLambdaInstance &instance : lambdas) {
    result += instance.body.blocks.size();
  }
  return result;
}

namespace {

[[nodiscard]] MirVerificationResult
failure(const MirBody &body, std::size_t owner, std::string message,
        MirBlockId block = 0, MirInstructionId instruction = 0) {
  MirVerificationResult result;
  result.errors.push_back({.bodyKind = body.kind,
                           .owner = owner,
                           .block = block,
                           .instruction = instruction,
                           .message = std::move(message)});
  return result;
}

[[nodiscard]] std::vector<bool> reachableBlocks(const MirBody &body) {
  std::vector<bool> reachable(body.blocks.size(), false);
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return reachable;
  }

  std::queue<MirBlockId> pending;
  pending.push(body.entry);
  while (!pending.empty()) {
    const MirBlockId id = pending.front();
    pending.pop();
    if (id == 0 || id > body.blocks.size() || reachable[id - 1]) {
      continue;
    }

    reachable[id - 1] = true;
    const MirTerminator &terminator = body.blocks[id - 1].terminator;
    const auto enqueue = [&](MirBlockId target) {
      if (target != 0) {
        pending.push(target);
      }
    };
    switch (terminator.kind) {
    case MirTerminatorKind::Goto:
      enqueue(terminator.target);
      break;
    case MirTerminatorKind::Branch:
      enqueue(terminator.target);
      enqueue(terminator.elseTarget);
      break;
    case MirTerminatorKind::Switch:
      enqueue(terminator.target);
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        enqueue(target.target);
      }
      break;
    case MirTerminatorKind::None:
    case MirTerminatorKind::Return:
    case MirTerminatorKind::Unreachable:
    case MirTerminatorKind::Exit:
      break;
    }
  }
  return reachable;
}

void append(MirVerificationResult &destination, MirVerificationResult source) {
  destination.errors.insert(destination.errors.end(),
                            std::make_move_iterator(source.errors.begin()),
                            std::make_move_iterator(source.errors.end()));
}

[[nodiscard]] bool literalMatchesType(const Literal &literal,
                                      const SemanticType &type) {
  if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
    (void)integer;
    // The lexical magnitude of a signed minimum literal is one greater than
    // the positive range and becomes valid only when consumed by unary
    // negation. Semantic analysis owns that contextual proof; MIR verifies the
    // literal alternative and integer result domain without rejecting the
    // intermediate lexical magnitude.
    return type == SemanticType::Unknown ||
           constantIntegerDomain(type).has_value();
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&literal)) {
    return validBinaryFloat(*floating) &&
           semanticFloatFormat(type) == floating->format;
  }
  if (std::holds_alternative<CharacterLiteral>(literal)) {
    return type == SemanticType::Char;
  }
  if (std::holds_alternative<std::string>(literal)) {
    return type == SemanticType::StringView;
  }
  if (std::holds_alternative<bool>(literal)) {
    return type == SemanticType::Bool;
  }
  if (std::holds_alternative<std::nullptr_t>(literal)) {
    return type == SemanticType::NullPtr;
  }
  return false;
}

enum class MirLoanFlowState : std::uint8_t {
  Inactive,
  Active,
  Suspended,
};

using MirLoanState = std::vector<MirLoanFlowState>;

struct MirCanonicalPlace {
  MirPlaceRootKind root = MirPlaceRootKind::Value;
  HirBindingId binding = 0;
  SymbolId symbol = 0;
  MirTemporaryId temporary = 0;
  MirValueId value = 0;
  std::vector<MirPlaceProjection> projections;
  MirLoanId throughLoan = 0;
  bool ambiguous = false;
};

[[nodiscard]] MirCanonicalPlace
canonicalPlace(const MirBody &body, MirPlaceId placeId,
               const std::unordered_map<HirBindingId, std::vector<MirLoanId>>
                   &bindingLoans,
               const MirLoanState &state) {
  const auto carrierLoan = [&](HirBindingId binding) {
    const auto found = bindingLoans.find(binding);
    if (found == bindingLoans.end()) {
      return MirLoanId{0};
    }
    MirLoanId candidate = 0;
    for (const MirLoanId loan : found->second) {
      if (state[loan - 1] == MirLoanFlowState::Inactive) {
        continue;
      }
      if (candidate != 0) {
        return MirLoanId{0};
      }
      candidate = loan;
    }
    if (candidate == 0 && found->second.size() == 1) {
      candidate = found->second.front();
    }
    return candidate;
  };

  const auto resolve = [&](const auto &self, MirPlaceId id,
                           std::size_t depth) -> MirCanonicalPlace {
    const MirPlace *place = body.findPlace(id);
    if (place == nullptr || depth > body.loans.size() + 1) {
      return {.ambiguous = true};
    }

    MirLoanId throughLoan = 0;
    std::size_t projectionOffset = 0;
    if (place->root == MirPlaceRootKind::Loan) {
      throughLoan = place->loan;
    } else if (place->root == MirPlaceRootKind::Binding &&
               !place->projections.empty() &&
               place->projections.front().kind ==
                   MirProjectionKind::Dereference) {
      throughLoan = carrierLoan(place->binding);
      projectionOffset = throughLoan == 0 ? 0 : 1;
    }

    if (throughLoan != 0) {
      const MirLoan *loan = body.findLoan(throughLoan);
      if (loan == nullptr || loan->source == id) {
        return {.ambiguous = true};
      }
      MirCanonicalPlace result = self(self, loan->source, depth + 1);
      result.throughLoan = throughLoan;
      result.projections.insert(
          result.projections.end(),
          place->projections.begin() +
              static_cast<std::ptrdiff_t>(projectionOffset),
          place->projections.end());
      return result;
    }

    return {.root = place->root,
            .binding = place->binding,
            .symbol = place->symbol,
            .temporary = place->temporary,
            .value = place->value,
            .projections = place->projections,
            .ambiguous = place->root == MirPlaceRootKind::Loan};
  };
  return resolve(resolve, placeId, 0);
}

[[nodiscard]] MirCanonicalPlace canonicalBorrowOriginPlace(
    const MirBody &body, MirPlaceId placeId,
    const std::unordered_map<HirBindingId, std::vector<MirLoanId>>
        &bindingLoans) {
  const auto carrierLoan = [&](HirBindingId binding) {
    const auto found = bindingLoans.find(binding);
    if (found == bindingLoans.end() || found->second.size() != 1) {
      return MirLoanId{0};
    }
    return found->second.front();
  };
  const auto resolve = [&](const auto &self, MirPlaceId id,
                           std::size_t depth) -> MirCanonicalPlace {
    const MirPlace *place = body.findPlace(id);
    if (place == nullptr || depth > body.loans.size() + 1) {
      return {.ambiguous = true};
    }

    MirLoanId throughLoan = 0;
    std::size_t projectionOffset = 0;
    if (place->root == MirPlaceRootKind::Loan) {
      throughLoan = place->loan;
    } else if (place->root == MirPlaceRootKind::Binding) {
      const MirLoanId carrier = carrierLoan(place->binding);
      const bool referenceProjection =
          !place->projections.empty() &&
          place->projections.front().kind == MirProjectionKind::Dereference;
      if (carrier != 0 && (referenceProjection ||
                           (place->type.kind != SemanticType::Reference &&
                            place->traits.containsBorrowedState))) {
        throughLoan = carrier;
        projectionOffset = referenceProjection ? 1 : 0;
      }
    }
    if (throughLoan != 0) {
      const MirLoan *loan = body.findLoan(throughLoan);
      if (loan == nullptr || loan->source == id) {
        return {.ambiguous = true};
      }
      MirCanonicalPlace result = self(self, loan->source, depth + 1);
      result.throughLoan = throughLoan;
      result.projections.insert(
          result.projections.end(),
          place->projections.begin() +
              static_cast<std::ptrdiff_t>(projectionOffset),
          place->projections.end());
      return result;
    }
    return {.root = place->root,
            .binding = place->binding,
            .symbol = place->symbol,
            .temporary = place->temporary,
            .value = place->value,
            .projections = place->projections,
            .ambiguous = place->root == MirPlaceRootKind::Loan};
  };
  return resolve(resolve, placeId, 0);
}

[[nodiscard]] bool sameCanonicalRoot(const MirCanonicalPlace &left,
                                     const MirCanonicalPlace &right) {
  if (left.ambiguous || right.ambiguous) {
    return true;
  }
  if (left.root != right.root) {
    return false;
  }
  switch (left.root) {
  case MirPlaceRootKind::Binding:
    return left.binding == right.binding;
  case MirPlaceRootKind::Symbol:
    return left.symbol == right.symbol;
  case MirPlaceRootKind::This:
    return true;
  case MirPlaceRootKind::Temporary:
    return left.temporary == right.temporary;
  case MirPlaceRootKind::Value:
    return left.value == right.value;
  case MirPlaceRootKind::Loan:
    return true;
  }
  return true;
}

[[nodiscard]] bool sameCanonicalPlace(const MirCanonicalPlace &left,
                                      const MirCanonicalPlace &right) {
  if (left.ambiguous || right.ambiguous || !sameCanonicalRoot(left, right) ||
      left.projections.size() != right.projections.size()) {
    return false;
  }
  return std::equal(
      left.projections.begin(), left.projections.end(),
      right.projections.begin(),
      [](const MirPlaceProjection &lhs, const MirPlaceProjection &rhs) {
        return lhs.kind == rhs.kind && lhs.field == rhs.field &&
               lhs.index == rhs.index &&
               lhs.constantIndex == rhs.constantIndex &&
               lhs.selection == rhs.selection;
      });
}

[[nodiscard]] bool sameGlobalBorrowPlace(const BorrowOriginPlace &expected,
                                         const MirCanonicalPlace &actual) {
  if (!expected.valid() || actual.ambiguous ||
      actual.root != MirPlaceRootKind::Symbol ||
      actual.symbol != expected.root ||
      actual.projections.size() != expected.projections.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.projections.size(); ++index) {
    const PlaceProjection &semantic = expected.projections[index];
    const MirPlaceProjection &mir = actual.projections[index];
    switch (semantic.kind) {
    case PlaceProjectionKind::Field:
      if (mir.kind != MirProjectionKind::Field || mir.field != semantic.field) {
        return false;
      }
      break;
    case PlaceProjectionKind::Dereference:
      if (mir.kind != MirProjectionKind::Dereference &&
          mir.kind != MirProjectionKind::RawDereference) {
        return false;
      }
      break;
    case PlaceProjectionKind::ConstantIndex:
      if (mir.kind != MirProjectionKind::Index ||
          mir.constantIndex != semantic.index) {
        return false;
      }
      break;
    case PlaceProjectionKind::DynamicIndex:
      if (mir.kind != MirProjectionKind::Index || mir.constantIndex ||
          mir.selection != semantic.selection) {
        return false;
      }
      break;
    }
  }
  return true;
}

[[nodiscard]] bool canonicalPlacesOverlap(const MirCanonicalPlace &left,
                                          const MirCanonicalPlace &right) {
  if (left.ambiguous || right.ambiguous) {
    return true;
  }
  if (!sameCanonicalRoot(left, right)) {
    return false;
  }
  const std::size_t common =
      std::min(left.projections.size(), right.projections.size());
  for (std::size_t index = 0; index < common; ++index) {
    const MirPlaceProjection &lhs = left.projections[index];
    const MirPlaceProjection &rhs = right.projections[index];
    if (lhs.kind == MirProjectionKind::Field &&
        rhs.kind == MirProjectionKind::Field && lhs.field != 0 &&
        rhs.field != 0 && lhs.field != rhs.field) {
      return false;
    }
    if (lhs.kind == MirProjectionKind::Index &&
        rhs.kind == MirProjectionKind::Index && lhs.constantIndex &&
        rhs.constantIndex && lhs.constantIndex != rhs.constantIndex) {
      return false;
    }
    if (lhs.kind != rhs.kind) {
      return true;
    }
  }
  return true;
}

[[nodiscard]] bool canonicalPlaceContains(const MirCanonicalPlace &parent,
                                          const MirCanonicalPlace &child) {
  if (parent.ambiguous || child.ambiguous ||
      !sameCanonicalRoot(parent, child) ||
      parent.projections.size() > child.projections.size()) {
    return false;
  }
  for (std::size_t index = 0; index < parent.projections.size(); ++index) {
    const MirPlaceProjection &outer = parent.projections[index];
    const MirPlaceProjection &inner = child.projections[index];
    if (outer.kind != inner.kind) {
      return false;
    }
    if (outer.kind == MirProjectionKind::Field &&
        (outer.field == 0 || outer.field != inner.field)) {
      return false;
    }
    if ((outer.kind == MirProjectionKind::Index ||
         outer.kind == MirProjectionKind::RawIndex) &&
        (outer.kind == MirProjectionKind::RawIndex ||
         (!outer.constantIndex && outer.selection == 0))) {
      if (outer.index == 0 || outer.index != inner.index) {
        return false;
      }
    }
    if (outer.kind == MirProjectionKind::Index) {
      if (outer.constantIndex != inner.constantIndex ||
          outer.selection != inner.selection) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] std::optional<PlaceKey>
structuralPlaceKey(const MirBody &body, const MirPlace &place) {
  PlaceKey result{.domain = body.placeDomain};
  switch (place.root) {
  case MirPlaceRootKind::Binding:
    if (place.symbol == 0) {
      return std::nullopt;
    }
    result.root = place.symbol;
    break;
  case MirPlaceRootKind::Symbol:
    if (place.symbol == 0) {
      return std::nullopt;
    }
    result.root = place.symbol;
    break;
  case MirPlaceRootKind::This:
    result.receiver = true;
    break;
  case MirPlaceRootKind::Temporary:
  case MirPlaceRootKind::Value:
  case MirPlaceRootKind::Loan:
    return std::nullopt;
  }

  for (const MirPlaceProjection &projection : place.projections) {
    switch (projection.kind) {
    case MirProjectionKind::Field:
      if (projection.field == 0) {
        return std::nullopt;
      }
      result.projections.push_back(
          {.kind = PlaceProjectionKind::Field, .field = projection.field});
      break;
    case MirProjectionKind::Index:
      if (projection.constantIndex) {
        result.projections.push_back(
            {.kind = PlaceProjectionKind::ConstantIndex,
             .index = *projection.constantIndex});
      } else {
        const std::size_t selection =
            projection.selection != 0 ? projection.selection : projection.index;
        if (selection == 0) {
          return std::nullopt;
        }
        result.projections.push_back({.kind = PlaceProjectionKind::DynamicIndex,
                                      .selection = selection});
      }
      break;
    case MirProjectionKind::Dereference:
      result.projections.push_back({.kind = PlaceProjectionKind::Dereference});
      break;
    case MirProjectionKind::RawIndex:
    case MirProjectionKind::RawDereference:
      return std::nullopt;
    }
  }
  return result;
}

[[nodiscard]] std::optional<PlaceKey> ownershipPlaceKey(const MirBody &body,
                                                        MirPlaceId placeId) {
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  return place->key ? place->key : structuralPlaceKey(body, *place);
}

struct MirOwnershipPlaceState {
  PlaceKey place;
  OwnershipStateSet state = OwnershipStateSet::Available;
};

using MirOwnershipFlowState = std::vector<MirOwnershipPlaceState>;

[[nodiscard]] OwnershipStateSet
ownershipStateAt(const MirOwnershipFlowState &state, const PlaceKey &place) {
  const auto found = std::find_if(state.begin(), state.end(),
                                  [&](const MirOwnershipPlaceState &candidate) {
                                    return candidate.place == place;
                                  });
  return found == state.end() ? OwnershipStateSet::Available : found->state;
}

void setOwnershipState(MirOwnershipFlowState &state, const PlaceKey &place,
                       OwnershipStateSet value) {
  const auto found = std::find_if(state.begin(), state.end(),
                                  [&](const MirOwnershipPlaceState &candidate) {
                                    return candidate.place == place;
                                  });
  if (value == OwnershipStateSet::Available) {
    if (found != state.end()) {
      state.erase(found);
    }
    return;
  }
  if (found == state.end()) {
    state.push_back({.place = place, .state = value});
  } else {
    found->state = value;
  }
}

[[nodiscard]] bool sameOwnershipState(const MirOwnershipFlowState &left,
                                      const MirOwnershipFlowState &right) {
  return left.size() == right.size() &&
         std::all_of(left.begin(), left.end(),
                     [&](const MirOwnershipPlaceState &candidate) {
                       return ownershipStateAt(right, candidate.place) ==
                              candidate.state;
                     });
}

[[nodiscard]] MirOwnershipFlowState
joinOwnershipState(const MirOwnershipFlowState &left,
                   const MirOwnershipFlowState &right) {
  MirOwnershipFlowState result = left;
  for (MirOwnershipPlaceState &candidate : result) {
    candidate.state =
        candidate.state | ownershipStateAt(right, candidate.place);
  }
  for (const MirOwnershipPlaceState &candidate : right) {
    if (std::none_of(result.begin(), result.end(),
                     [&](const MirOwnershipPlaceState &existing) {
                       return existing.place == candidate.place;
                     })) {
      const OwnershipStateSet joined =
          OwnershipStateSet::Available | candidate.state;
      if (joined != OwnershipStateSet::Available) {
        result.push_back({.place = candidate.place, .state = joined});
      }
    }
  }
  std::erase_if(result, [](const MirOwnershipPlaceState &candidate) {
    return candidate.state == OwnershipStateSet::Available;
  });
  return result;
}

[[nodiscard]] const MirOwnershipPlaceState *
unavailableOwnershipState(const MirOwnershipFlowState &state,
                          const PlaceKey &place) {
  const auto found = std::find_if(
      state.begin(), state.end(), [&](const MirOwnershipPlaceState &candidate) {
        if (candidate.state == OwnershipStateSet::Available) {
          return false;
        }
        const PlaceRelationResult relation =
            placeRelation(candidate.place, place);
        return !relation.compatibleDomain ||
               relation.relation != PlaceRelation::Disjoint;
      });
  return found == state.end() ? nullptr : &*found;
}

[[nodiscard]] bool hasDynamicIndex(const PlaceKey &place) {
  return std::any_of(place.projections.begin(), place.projections.end(),
                     [](const PlaceProjection &projection) {
                       return projection.kind ==
                              PlaceProjectionKind::DynamicIndex;
                     });
}

[[nodiscard]] PlaceKey ownershipRoot(PlaceKey place) {
  place.projections.clear();
  return place;
}

[[nodiscard]] bool tracksOwnershipRoot(std::span<const PlaceKey> roots,
                                       const PlaceKey &place) {
  const PlaceKey root = ownershipRoot(place);
  return std::find(roots.begin(), roots.end(), root) != roots.end();
}

void restoreOwnershipPlace(MirOwnershipFlowState &state,
                           const PlaceKey &place) {
  std::erase_if(state, [&](const MirOwnershipPlaceState &candidate) {
    const PlaceRelationResult relation = placeRelation(place, candidate.place);
    return relation.compatibleDomain &&
           (relation.relation == PlaceRelation::Equal ||
            relation.relation == PlaceRelation::LeftStrictPrefix);
  });
}

[[nodiscard]] std::vector<MirBlockId>
ownershipSuccessors(const MirTerminator &terminator) {
  std::vector<MirBlockId> result;
  const auto append = [&](MirBlockId target) {
    if (target != 0 &&
        std::find(result.begin(), result.end(), target) == result.end()) {
      result.push_back(target);
    }
  };
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    append(terminator.target);
    break;
  case MirTerminatorKind::Branch:
    append(terminator.target);
    append(terminator.elseTarget);
    break;
  case MirTerminatorKind::Switch:
    append(terminator.target);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      append(target.target);
    }
    break;
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
    break;
  }
  return result;
}

[[nodiscard]] MirVerificationResult verifyMirOwnershipFlow(const MirBody &body,
                                                           std::size_t owner) {
  std::vector<PlaceKey> trackedRoots;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!instruction.ownership ||
          instruction.ownership->kind != OwnershipEventKind::Move) {
        continue;
      }
      PlaceKey root = ownershipRoot(instruction.ownership->place);
      if (root.valid() && std::find(trackedRoots.begin(), trackedRoots.end(),
                                    root) == trackedRoots.end()) {
        trackedRoots.push_back(std::move(root));
      }
    }
  }
  std::vector<std::optional<MirOwnershipFlowState>> entryStates(
      body.blocks.size());
  MirOwnershipFlowState initialState;
  for (const MirPlace &place : body.places) {
    if (place.key && place.root == MirPlaceRootKind::Binding &&
        place.projections.empty() && !place.initiallyAvailable &&
        tracksOwnershipRoot(trackedRoots, *place.key)) {
      setOwnershipState(initialState, *place.key,
                        OwnershipStateSet::Uninitialized);
    }
  }
  entryStates[body.entry - 1] = std::move(initialState);
  std::queue<MirBlockId> pending;
  pending.push(body.entry);

  const auto requireAvailable =
      [&](const MirOwnershipFlowState &state, MirPlaceId placeId,
          const MirBlock &block, const MirInstruction &instruction,
          std::string_view operation) -> std::optional<MirVerificationResult> {
    const std::optional<PlaceKey> place = ownershipPlaceKey(body, placeId);
    if (!place) {
      return std::nullopt;
    }
    if (const MirOwnershipPlaceState *unavailable =
            unavailableOwnershipState(state, *place)) {
      return failure(body, owner,
                     std::string(operation) +
                         " requires a definitely available place (state " +
                         std::to_string(unavailable->state.bits) + ")",
                     block.id, instruction.id);
    }
    return std::nullopt;
  };

  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock *block = body.findBlock(blockId);
    if (block == nullptr || !entryStates[blockId - 1]) {
      continue;
    }
    MirOwnershipFlowState state = *entryStates[blockId - 1];

    for (const MirInstruction &instruction : block->instructions) {
      if (instruction.ownership &&
          (!instruction.ownership->place.valid() ||
           instruction.ownership->place.domain != body.placeDomain)) {
        return failure(body, owner,
                       "ownership event has an invalid place domain or key",
                       block->id, instruction.id);
      }
      if (instruction.ownership && !instruction.ownership->reachable) {
        continue;
      }
      const auto checkOperand = [&](const MirOperand &operand)
          -> std::optional<MirVerificationResult> {
        switch (operand.kind) {
        case MirOperandKind::Address:
        case MirOperandKind::Copy:
        case MirOperandKind::BorrowRead:
        case MirOperandKind::BorrowWrite:
          return requireAvailable(state, operand.place, *block, instruction,
                                  "place access");
        case MirOperandKind::Move:
        case MirOperandKind::Value:
        case MirOperandKind::Constant:
        case MirOperandKind::Loan:
          return std::nullopt;
        }
        return std::nullopt;
      };
      if (instruction.receiver) {
        if (std::optional<MirVerificationResult> invalid =
                checkOperand(*instruction.receiver)) {
          return std::move(*invalid);
        }
      }
      for (const MirOperand &operand : instruction.operands) {
        if (std::optional<MirVerificationResult> invalid =
                checkOperand(operand)) {
          return std::move(*invalid);
        }
      }

      if (instruction.kind == MirInstructionKind::Move &&
          !instruction.operands.empty()) {
        const MirPlaceId source = instruction.operands.front().place;
        const std::optional<PlaceKey> place = ownershipPlaceKey(body, source);
        if (!place) {
          continue;
        }
        if (hasDynamicIndex(*place)) {
          return failure(body, owner,
                         "move uses a dynamic-index place without an alias "
                         "proof",
                         block->id, instruction.id);
        }
        if (std::optional<MirVerificationResult> invalid =
                requireAvailable(state, source, *block, instruction, "move")) {
          return std::move(*invalid);
        }
        if (!instruction.ownership ||
            instruction.ownership->kind != OwnershipEventKind::Move ||
            instruction.ownership->place != *place ||
            instruction.ownership->before != OwnershipStateSet::Available ||
            instruction.ownership->after != OwnershipStateSet::Moved) {
          return failure(body, owner,
                         "move does not preserve its semantic ownership "
                         "event and place identity",
                         block->id, instruction.id);
        }
        setOwnershipState(state, *place, OwnershipStateSet::Moved);
        continue;
      }

      if (instruction.destination) {
        const std::optional<PlaceKey> destination =
            ownershipPlaceKey(body, *instruction.destination);
        if (!destination) {
          continue;
        }
        if (instruction.kind == MirInstructionKind::Initialize) {
          const MirPlace *destinationPlace =
              body.findPlace(*instruction.destination);
          if (tracksOwnershipRoot(trackedRoots, *destination) &&
              destinationPlace != nullptr &&
              destinationPlace->root == MirPlaceRootKind::Binding &&
              destinationPlace->projections.empty() &&
              !destinationPlace->initiallyAvailable &&
              !ownershipStateAt(state, *destination)
                   .definitely(OwnershipState::Uninitialized)) {
            return failure(body, owner,
                           "initialization targets a place whose lifetime is "
                           "already active",
                           block->id, instruction.id);
          }
          restoreOwnershipPlace(state, *destination);
        } else if (instruction.kind == MirInstructionKind::Drop) {
          if (tracksOwnershipRoot(trackedRoots, *destination) &&
              ownershipStateAt(state, *destination)
                  .contains(OwnershipState::Uninitialized)) {
            return failure(body, owner,
                           "drop requires a definitely active place lifetime",
                           block->id, instruction.id);
          }
          restoreOwnershipPlace(state, *destination);
          if (tracksOwnershipRoot(trackedRoots, *destination)) {
            setOwnershipState(state, *destination,
                              OwnershipStateSet::Uninitialized);
          }
        } else if (instruction.kind == MirInstructionKind::Assign &&
                   instruction.operation == MirOperation::Assign) {
          if (!tracksOwnershipRoot(trackedRoots, *destination)) {
            continue;
          }
          const bool indexedOwnershipPlace = std::any_of(
              destination->projections.begin(), destination->projections.end(),
              [](const PlaceProjection &projection) {
                return projection.kind == PlaceProjectionKind::ConstantIndex ||
                       projection.kind == PlaceProjectionKind::DynamicIndex;
              });
          if (indexedOwnershipPlace && !instruction.ownership) {
            return failure(body, owner,
                           "assignment does not preserve its semantic "
                           "reinitialization event and place identity",
                           block->id, instruction.id);
          }
          if (!instruction.ownership) {
            continue;
          }
          if (instruction.ownership->kind != OwnershipEventKind::Reinitialize ||
              instruction.ownership->place != *destination ||
              instruction.ownership->before !=
                  ownershipStateAt(state, *destination) ||
              instruction.ownership->after != OwnershipStateSet::Available) {
            return failure(body, owner,
                           "assignment ownership event disagrees with its "
                           "destination place",
                           block->id, instruction.id);
          }
          const MirOwnershipPlaceState *unavailableAncestor =
              unavailableOwnershipState(state, *destination);
          if (unavailableAncestor != nullptr) {
            const PlaceRelationResult relation =
                placeRelation(unavailableAncestor->place, *destination);
            if (!relation.compatibleDomain ||
                relation.relation == PlaceRelation::LeftStrictPrefix ||
                relation.relation == PlaceRelation::MayAlias) {
              return failure(body, owner,
                             "assignment cannot prove that its destination "
                             "is independent of unavailable storage",
                             block->id, instruction.id);
            }
          }
          restoreOwnershipPlace(state, *destination);
        } else if (instruction.kind == MirInstructionKind::Assign ||
                   instruction.kind == MirInstructionKind::Modify) {
          if (std::optional<MirVerificationResult> invalid =
                  requireAvailable(state, *instruction.destination, *block,
                                   instruction, "mutation")) {
            return std::move(*invalid);
          }
        }
      }
    }

    for (const MirBlockId successor : ownershipSuccessors(block->terminator)) {
      if (successor == 0 || successor > body.blocks.size()) {
        continue;
      }
      std::optional<MirOwnershipFlowState> &incoming =
          entryStates[successor - 1];
      const MirOwnershipFlowState merged =
          incoming ? joinOwnershipState(*incoming, state) : state;
      if (!incoming || !sameOwnershipState(*incoming, merged)) {
        incoming = merged;
        pending.push(successor);
      }
    }
  }
  return {};
}

[[nodiscard]] bool isLoanAncestor(const MirBody &body, MirLoanId ancestor,
                                  MirLoanId descendant) {
  if (ancestor == 0 || descendant == 0 || ancestor == descendant) {
    return false;
  }
  std::unordered_set<MirLoanId> visited;
  MirLoanId current = descendant;
  while (current != 0 && visited.insert(current).second) {
    const MirLoan *loan = body.findLoan(current);
    if (loan == nullptr) {
      return false;
    }
    current = loan->parent;
    if (current == ancestor) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::vector<MirBlockId>
successors(const MirTerminator &terminator) {
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    return {terminator.target};
  case MirTerminatorKind::Branch:
    return {terminator.target, terminator.elseTarget};
  case MirTerminatorKind::Switch: {
    std::vector<MirBlockId> result{terminator.target};
    result.reserve(terminator.switchTargets.size() + 1);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      result.push_back(target.target);
    }
    return result;
  }
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
    return {};
  }
  return {};
}

[[nodiscard]] MirVerificationResult verifyMirLoanFlow(const MirBody &body,
                                                      std::size_t owner) {
  std::vector<std::size_t> producerCounts(body.loans.size(), 0);
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  std::vector<std::vector<MirLoanId>> children(body.loans.size());
  for (const MirLoan &loan : body.loans) {
    for (const HirBindingId carrier : loan.carriers) {
      bindingLoans[carrier].push_back(loan.id);
    }
    if (loan.parent != 0) {
      children[loan.parent - 1].push_back(loan.id);
    }
  }
  const MirLoanState inactive(body.loans.size(), MirLoanFlowState::Inactive);
  for (const MirLoan &loan : body.loans) {
    if (loan.parent == 0) {
      continue;
    }
    const MirCanonicalPlace parentPlace = canonicalPlace(
        body, body.loans[loan.parent - 1].source, bindingLoans, inactive);
    const MirCanonicalPlace childPlace =
        canonicalPlace(body, loan.source, bindingLoans, inactive);
    if (!canonicalPlaceContains(parentPlace, childPlace)) {
      return failure(body, owner,
                     "child loan " + std::to_string(loan.id) +
                         " is not contained within storage protected by "
                         "parent loan " +
                         std::to_string(loan.parent));
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!instruction.loan ||
          instruction.kind == MirInstructionKind::EndBorrow) {
        continue;
      }
      if (instruction.kind != MirInstructionKind::Borrow &&
          instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct) {
        return failure(body, owner,
                       "only borrow, call, and construct instructions may "
                       "produce a loan",
                       block.id, instruction.id);
      }
      ++producerCounts[*instruction.loan - 1];
    }
  }
  for (std::size_t index = 0; index < producerCounts.size(); ++index) {
    const MirLoan &loan = body.loans[index];
    const std::size_t expected = loan.entry ? 0 : 1;
    if (producerCounts[index] != expected) {
      return failure(body, owner,
                     "loan " + std::to_string(index + 1) +
                         (loan.entry ? " is an entry loan and must not have a "
                                       "producing instruction"
                                     : " must have exactly one producing "
                                       "instruction"));
    }
  }

  std::vector<std::optional<MirLoanState>> blockEntries(body.blocks.size());
  MirLoanState entryState(body.loans.size(), MirLoanFlowState::Inactive);
  for (const MirLoan &loan : body.loans) {
    if (loan.entry) {
      entryState[loan.id - 1] = MirLoanFlowState::Active;
    }
  }
  blockEntries[body.entry - 1] = std::move(entryState);
  std::queue<MirBlockId> pending;
  pending.push(body.entry);

  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock &block = body.blocks[blockId - 1];
    MirLoanState active = *blockEntries[blockId - 1];

    const auto requireActive =
        [&](MirLoanId loan, const char *context,
            MirInstructionId instruction =
                0) -> std::optional<MirVerificationResult> {
      if (active[loan - 1] == MirLoanFlowState::Inactive) {
        return failure(body, owner,
                       "loan " + std::to_string(loan) +
                           " is used after its borrow has ended in " + context,
                       block.id, instruction);
      }
      if (active[loan - 1] == MirLoanFlowState::Suspended) {
        return failure(body, owner,
                       "loan " + std::to_string(loan) +
                           " is used while an exclusive child reborrow is "
                           "active in " +
                           context,
                       block.id, instruction);
      }
      return std::nullopt;
    };
    const auto checkPlace = [&](MirPlaceId placeId, bool write,
                                const char *context,
                                MirInstructionId instruction =
                                    0) -> std::optional<MirVerificationResult> {
      const MirCanonicalPlace place =
          canonicalPlace(body, placeId, bindingLoans, active);
      if (place.ambiguous) {
        return failure(body, owner,
                       "cannot resolve the canonical storage for a place in " +
                           std::string(context),
                       block.id, instruction);
      }
      if (place.throughLoan != 0) {
        const MirLoanFlowState throughState = active[place.throughLoan - 1];
        if (throughState == MirLoanFlowState::Inactive) {
          if (auto invalid =
                  requireActive(place.throughLoan, context, instruction)) {
            return invalid;
          }
        } else if (throughState == MirLoanFlowState::Suspended) {
          const bool overlapsDescendant = std::any_of(
              body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
                if (active[loan.id - 1] == MirLoanFlowState::Inactive ||
                    !isLoanAncestor(body, place.throughLoan, loan.id)) {
                  return false;
                }
                const MirCanonicalPlace childPlace =
                    canonicalPlace(body, loan.source, bindingLoans, active);
                return canonicalPlacesOverlap(place, childPlace);
              });
          if (overlapsDescendant) {
            return failure(body, owner,
                           "loan " + std::to_string(place.throughLoan) +
                               " is used through storage covered by an active "
                               "child reborrow in " +
                               context,
                           block.id, instruction);
          }
        }
        if (write &&
            body.loans[place.throughLoan - 1].access != AccessMode::Mutable) {
          return failure(body, owner,
                         "read-only loan " + std::to_string(place.throughLoan) +
                             " is used for a write in " + context,
                         block.id, instruction);
        }
      }

      for (const MirLoan &loan : body.loans) {
        if (active[loan.id - 1] == MirLoanFlowState::Inactive ||
            loan.id == place.throughLoan ||
            (place.throughLoan != 0 &&
             isLoanAncestor(body, loan.id, place.throughLoan))) {
          continue;
        }
        const MirCanonicalPlace protectedPlace =
            canonicalPlace(body, loan.source, bindingLoans, active);
        if (!canonicalPlacesOverlap(place, protectedPlace)) {
          continue;
        }
        if (!write && loan.access != AccessMode::Mutable) {
          continue;
        }
        return failure(body, owner,
                       std::string(write ? "write" : "read") +
                           " of a place conflicts with active " +
                           (loan.access == AccessMode::Mutable ? "mutable "
                                                               : "read-only ") +
                           "loan " + std::to_string(loan.id) + " in " + context,
                       block.id, instruction);
      }
      return std::nullopt;
    };
    const auto callInputSource = [&](const MirOperand &operand) {
      if (operand.kind != MirOperandKind::Value) {
        return &operand;
      }
      const MirValue *value = body.findValue(operand.value);
      const MirBlock *definitionBlock =
          value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
      if (definitionBlock == nullptr) {
        return &operand;
      }
      const auto definition =
          std::find_if(definitionBlock->instructions.begin(),
                       definitionBlock->instructions.end(),
                       [&](const MirInstruction &candidate) {
                         return candidate.id == value->definition;
                       });
      return definition != definitionBlock->instructions.end() &&
                     definition->kind == MirInstructionKind::CallInput &&
                     definition->operands.size() == 1
                 ? &definition->operands.front()
                 : &operand;
    };
    const auto checkOperand =
        [&](const MirOperand &operand, const char *context,
            MirInstructionId instruction =
                0) -> std::optional<MirVerificationResult> {
      const MirOperand &resolved = *callInputSource(operand);
      switch (resolved.kind) {
      case MirOperandKind::Loan: {
        if (auto invalid = requireActive(resolved.loan, context, instruction)) {
          return invalid;
        }
        if (resolved.type.kind == SemanticType::Reference &&
            resolved.type.referenceAccess == AccessMode::Mutable &&
            body.loans[resolved.loan - 1].access != AccessMode::Mutable) {
          return failure(body, owner,
                         "read-only loan " + std::to_string(resolved.loan) +
                             " is used as a mutable reference in " + context,
                         block.id, instruction);
        }
        return std::nullopt;
      }
      case MirOperandKind::Copy:
      case MirOperandKind::Address:
      case MirOperandKind::BorrowRead:
        return checkPlace(resolved.place, false, context, instruction);
      case MirOperandKind::Move: {
        const MirPlace *moved = body.findPlace(resolved.place);
        if (moved != nullptr && moved->root == MirPlaceRootKind::Binding &&
            moved->projections.empty()) {
          const auto found = bindingLoans.find(moved->binding);
          MirLoanId carrierLoan = 0;
          bool ambiguous = false;
          if (found != bindingLoans.end()) {
            for (const MirLoanId loan : found->second) {
              if (active[loan - 1] == MirLoanFlowState::Inactive) {
                continue;
              }
              if (carrierLoan != 0) {
                ambiguous = true;
                break;
              }
              carrierLoan = loan;
            }
          }
          if (!ambiguous && carrierLoan != 0) {
            return requireActive(carrierLoan, context, instruction);
          }
        }
        return checkPlace(resolved.place, true, context, instruction);
      }
      case MirOperandKind::BorrowWrite:
        return checkPlace(resolved.place, true, context, instruction);
      case MirOperandKind::Value:
      case MirOperandKind::Constant:
        return std::nullopt;
      }
      return std::nullopt;
    };
    const auto checkCallBorrowAliases = [&](const MirInstruction &instruction)
        -> std::optional<MirVerificationResult> {
      if (instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct) {
        return std::nullopt;
      }
      struct CallBorrow {
        MirCanonicalPlace place;
        bool write = false;
      };
      std::vector<CallBorrow> borrows;
      const auto appendBorrow = [&](const MirOperand &operand) {
        const MirOperand &resolved = *callInputSource(operand);
        if (resolved.kind == MirOperandKind::BorrowRead ||
            resolved.kind == MirOperandKind::BorrowWrite) {
          borrows.push_back(
              {.place =
                   canonicalPlace(body, resolved.place, bindingLoans, active),
               .write = resolved.kind == MirOperandKind::BorrowWrite});
        } else if (resolved.kind == MirOperandKind::Loan) {
          const MirLoan *loan = body.findLoan(resolved.loan);
          if (loan != nullptr) {
            borrows.push_back(
                {.place =
                     canonicalPlace(body, loan->source, bindingLoans, active),
                 .write =
                     resolved.type.kind == SemanticType::Reference &&
                     resolved.type.referenceAccess == AccessMode::Mutable});
          }
        }
      };
      if (instruction.receiver) {
        appendBorrow(*instruction.receiver);
      }
      for (const MirOperand &operand : instruction.operands) {
        appendBorrow(operand);
      }
      for (std::size_t left = 0; left < borrows.size(); ++left) {
        for (std::size_t right = left + 1; right < borrows.size(); ++right) {
          if (!borrows[left].write && !borrows[right].write) {
            continue;
          }
          if (canonicalPlacesOverlap(borrows[left].place,
                                     borrows[right].place)) {
            return failure(
                body, owner,
                "overlapping call-duration reference operands include a "
                "mutable borrow",
                block.id, instruction.id);
          }
        }
      }
      return std::nullopt;
    };

    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::EndBorrow) {
        const MirLoanId loan = *instruction.loan;
        if (active[loan - 1] == MirLoanFlowState::Inactive) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is ended while it is inactive",
                         block.id, instruction.id);
        }
        if (active[loan - 1] == MirLoanFlowState::Suspended) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is ended before its active child reborrow",
                         block.id, instruction.id);
        }
        if (std::any_of(children[loan - 1].begin(), children[loan - 1].end(),
                        [&](MirLoanId child) {
                          return active[child - 1] !=
                                 MirLoanFlowState::Inactive;
                        })) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is ended before its child reborrow",
                         block.id, instruction.id);
        }
        active[loan - 1] = MirLoanFlowState::Inactive;
        const MirLoanId parent = body.loans[loan - 1].parent;
        if (parent != 0) {
          const bool suspendsParent =
              body.loans[parent - 1].access == AccessMode::Mutable;
          const MirLoanFlowState expectedParent =
              suspendsParent ? MirLoanFlowState::Suspended
                             : MirLoanFlowState::Active;
          if (active[parent - 1] != expectedParent) {
            return failure(body, owner,
                           "ending child loan " + std::to_string(loan) +
                               " finds parent loan " + std::to_string(parent) +
                               " in an inconsistent flow state",
                           block.id, instruction.id);
          }
          if (suspendsParent) {
            const bool siblingLive = std::any_of(
                children[parent - 1].begin(), children[parent - 1].end(),
                [&](MirLoanId child) {
                  return active[child - 1] != MirLoanFlowState::Inactive;
                });
            if (!siblingLive) {
              active[parent - 1] = MirLoanFlowState::Active;
            }
          }
        }
        continue;
      }

      if (instruction.kind == MirInstructionKind::Borrow && instruction.loan &&
          body.loans[*instruction.loan - 1].parent != 0) {
        const MirLoanId loan = *instruction.loan;
        const MirLoanId parent = body.loans[loan - 1].parent;
        if (active[loan - 1] != MirLoanFlowState::Inactive) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is produced while it is already live",
                         block.id, instruction.id);
        }
        if (instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Loan ||
            instruction.operands.front().loan != parent) {
          return failure(body, owner,
                         "child loan " + std::to_string(loan) +
                             " must borrow from its declared parent loan",
                         block.id, instruction.id);
        }
        if (active[parent - 1] == MirLoanFlowState::Inactive) {
          return failure(body, owner,
                         "child loan " + std::to_string(loan) +
                             " is created after parent loan " +
                             std::to_string(parent) + " has ended",
                         block.id, instruction.id);
        }
        if (body.loans[parent - 1].access != AccessMode::Mutable) {
          return failure(body, owner,
                         "child loan " + std::to_string(loan) +
                             " cannot exclusively reborrow a read-only parent",
                         block.id, instruction.id);
        }
        const MirCanonicalPlace childPlace = canonicalPlace(
            body, body.loans[loan - 1].source, bindingLoans, active);
        const MirCanonicalPlace parentPlace = canonicalPlace(
            body, body.loans[parent - 1].source, bindingLoans, active);
        if (!canonicalPlaceContains(parentPlace, childPlace)) {
          return failure(body, owner,
                         "child loan " + std::to_string(loan) +
                             " is not contained within storage protected by "
                             "parent loan " +
                             std::to_string(parent),
                         block.id, instruction.id);
        }
        for (const MirLoan &other : body.loans) {
          if (other.id == parent ||
              active[other.id - 1] == MirLoanFlowState::Inactive ||
              isLoanAncestor(body, other.id, parent)) {
            continue;
          }
          const MirCanonicalPlace otherPlace =
              canonicalPlace(body, other.source, bindingLoans, active);
          const bool sharedEphemeralAccess =
              body.loans[loan - 1].kind == MirLoanKind::CallResult &&
              other.kind == MirLoanKind::CallResult &&
              body.loans[loan - 1].access == AccessMode::ReadOnly &&
              other.access == AccessMode::ReadOnly;
          if (canonicalPlacesOverlap(childPlace, otherPlace) &&
              !sharedEphemeralAccess) {
            return failure(body, owner,
                           "child loan " + std::to_string(loan) +
                               " conflicts with active sibling loan " +
                               std::to_string(other.id),
                           block.id, instruction.id);
          }
        }
        if (body.loans[parent - 1].access == AccessMode::Mutable) {
          active[parent - 1] = MirLoanFlowState::Suspended;
        }
        active[loan - 1] = MirLoanFlowState::Active;
        continue;
      }

      if (auto invalid = checkCallBorrowAliases(instruction)) {
        return *invalid;
      }

      const auto permitsSuspendedSharedOrigin = [&](const MirOperand &operand,
                                                    bool selectedOrigin) {
        const MirOperand &resolved = *callInputSource(operand);
        if (!selectedOrigin || instruction.kind != MirInstructionKind::Call ||
            !instruction.loan ||
            (resolved.kind != MirOperandKind::Loan &&
             resolved.kind != MirOperandKind::BorrowRead)) {
          return false;
        }
        const MirLoan &produced = body.loans[*instruction.loan - 1];
        const MirLoanId parent = produced.parent;
        if (parent == 0 || produced.kind != MirLoanKind::CallResult ||
            produced.access != AccessMode::ReadOnly ||
            active[parent - 1] != MirLoanFlowState::Suspended) {
          return false;
        }
        bool throughParent =
            resolved.kind == MirOperandKind::Loan && resolved.loan == parent;
        if (!throughParent && resolved.place != 0) {
          const MirPlace *place = body.findPlace(resolved.place);
          throughParent = place != nullptr &&
                          place->root == MirPlaceRootKind::Binding &&
                          std::find(body.loans[parent - 1].carriers.begin(),
                                    body.loans[parent - 1].carriers.end(),
                                    place->binding) !=
                              body.loans[parent - 1].carriers.end();
          if (!throughParent) {
            throughParent =
                canonicalPlace(body, resolved.place, bindingLoans, active)
                    .throughLoan == parent;
          }
        }
        return throughParent &&
               std::all_of(children[parent - 1].begin(),
                           children[parent - 1].end(), [&](MirLoanId child) {
                             const MirLoan &sibling = body.loans[child - 1];
                             return active[child - 1] ==
                                        MirLoanFlowState::Inactive ||
                                    (sibling.kind == MirLoanKind::CallResult &&
                                     sibling.access == AccessMode::ReadOnly);
                           });
      };

      if (instruction.receiver) {
        const bool selectedOrigin =
            instruction.borrowOrigin == BorrowOriginKind::Receiver;
        if (!permitsSuspendedSharedOrigin(*instruction.receiver,
                                          selectedOrigin)) {
          if (auto invalid = checkOperand(*instruction.receiver, "a receiver",
                                          instruction.id)) {
            return *invalid;
          }
        }
      }
      for (std::size_t operandIndex = 0;
           operandIndex < instruction.operands.size(); ++operandIndex) {
        const MirOperand &operand = instruction.operands[operandIndex];
        const bool selectedOrigin =
            instruction.borrowOrigin == BorrowOriginKind::Argument &&
            instruction.borrowArgument == operandIndex;
        if (!permitsSuspendedSharedOrigin(operand, selectedOrigin)) {
          if (auto invalid = checkOperand(operand, "an instruction operand",
                                          instruction.id)) {
            return *invalid;
          }
        }
      }
      if (instruction.destination) {
        bool carrierDrop = false;
        if (instruction.kind == MirInstructionKind::Drop) {
          const MirPlace *dropped = body.findPlace(*instruction.destination);
          if (dropped != nullptr &&
              dropped->root == MirPlaceRootKind::Binding &&
              dropped->projections.empty() &&
              dropped->traits.containsBorrowedState) {
            const auto found = bindingLoans.find(dropped->binding);
            MirLoanId carrierLoan = 0;
            bool ambiguous = false;
            if (found != bindingLoans.end()) {
              for (const MirLoanId loan : found->second) {
                if (active[loan - 1] == MirLoanFlowState::Inactive) {
                  continue;
                }
                if (carrierLoan != 0) {
                  ambiguous = true;
                  break;
                }
                carrierLoan = loan;
              }
            }
            if (!ambiguous && carrierLoan != 0) {
              carrierDrop = true;
              if (auto invalid =
                      requireActive(carrierLoan, "an instruction destination",
                                    instruction.id)) {
                return *invalid;
              }
            }
          }
        }
        if (!carrierDrop) {
          if (auto invalid =
                  checkPlace(*instruction.destination, true,
                             "an instruction destination", instruction.id)) {
            return *invalid;
          }
        }
      }

      if (instruction.loan) {
        const MirLoanId loan = *instruction.loan;
        const MirLoanId parent = body.loans[loan - 1].parent;
        if (parent != 0) {
          const auto operandFromParent = [&](const MirOperand &operand) {
            const MirOperand &resolved = *callInputSource(operand);
            if (resolved.kind == MirOperandKind::Loan) {
              return resolved.loan == parent;
            }
            if (resolved.place == 0) {
              return false;
            }
            const MirPlace *place = body.findPlace(resolved.place);
            if (place != nullptr && place->root == MirPlaceRootKind::Binding &&
                std::find(body.loans[parent - 1].carriers.begin(),
                          body.loans[parent - 1].carriers.end(),
                          place->binding) !=
                    body.loans[parent - 1].carriers.end()) {
              return true;
            }
            const MirCanonicalPlace operandPlace =
                canonicalPlace(body, resolved.place, bindingLoans, active);
            return operandPlace.throughLoan == parent;
          };
          bool selectedOriginFromParent = false;
          if (instruction.borrowOrigin == BorrowOriginKind::Receiver &&
              instruction.receiver) {
            selectedOriginFromParent = operandFromParent(*instruction.receiver);
          } else if (instruction.borrowOrigin == BorrowOriginKind::Argument &&
                     instruction.borrowArgument < instruction.operands.size()) {
            selectedOriginFromParent = operandFromParent(
                instruction.operands[instruction.borrowArgument]);
          }
          if (instruction.kind != MirInstructionKind::Call ||
              body.loans[loan - 1].kind != MirLoanKind::CallResult ||
              !selectedOriginFromParent) {
            return failure(body, owner,
                           "derived call-result loan " + std::to_string(loan) +
                               " must be produced through its declared parent "
                               "loan",
                           block.id, instruction.id);
          }
          if (active[loan - 1] != MirLoanFlowState::Inactive) {
            return failure(body, owner,
                           "loan " + std::to_string(loan) +
                               " is produced while it is already live",
                           block.id, instruction.id);
          }
          if (active[parent - 1] == MirLoanFlowState::Inactive) {
            return failure(body, owner,
                           "derived call-result loan " + std::to_string(loan) +
                               " is created after parent loan " +
                               std::to_string(parent) + " has ended",
                           block.id, instruction.id);
          }
          if (body.loans[parent - 1].access != AccessMode::Mutable &&
              body.loans[loan - 1].access != AccessMode::ReadOnly) {
            return failure(body, owner,
                           "derived call-result loan " + std::to_string(loan) +
                               " cannot mutably reborrow a read-only parent",
                           block.id, instruction.id);
          }
          const MirCanonicalPlace childPlace = canonicalPlace(
              body, body.loans[loan - 1].source, bindingLoans, active);
          const MirCanonicalPlace parentPlace = canonicalPlace(
              body, body.loans[parent - 1].source, bindingLoans, active);
          if (!canonicalPlaceContains(parentPlace, childPlace)) {
            return failure(body, owner,
                           "derived call-result loan " + std::to_string(loan) +
                               " is not contained within storage protected by "
                               "parent loan " +
                               std::to_string(parent),
                           block.id, instruction.id);
          }
          for (const MirLoan &other : body.loans) {
            if (other.id == parent ||
                active[other.id - 1] == MirLoanFlowState::Inactive ||
                isLoanAncestor(body, other.id, parent)) {
              continue;
            }
            const MirCanonicalPlace otherPlace =
                canonicalPlace(body, other.source, bindingLoans, active);
            const bool compatibleReadOnlyDependency =
                body.loans[loan - 1].kind == MirLoanKind::CallResult &&
                body.loans[loan - 1].access == AccessMode::ReadOnly &&
                other.access == AccessMode::ReadOnly &&
                (other.kind == MirLoanKind::CallResult ||
                 (other.kind == MirLoanKind::Stored && other.parent == 0));
            if (canonicalPlacesOverlap(childPlace, otherPlace) &&
                !compatibleReadOnlyDependency) {
              return failure(body, owner,
                             "derived call-result loan " +
                                 std::to_string(loan) +
                                 " conflicts with active sibling loan " +
                                 std::to_string(other.id),
                             block.id, instruction.id);
            }
          }
          if (body.loans[parent - 1].access == AccessMode::Mutable) {
            active[parent - 1] = MirLoanFlowState::Suspended;
          }
          active[loan - 1] = MirLoanFlowState::Active;
          continue;
        }
        if (active[loan - 1] != MirLoanFlowState::Inactive) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is produced while it is already live",
                         block.id, instruction.id);
        }
        active[loan - 1] = MirLoanFlowState::Active;
      }
    }

    if (block.terminator.value) {
      if (auto invalid =
              checkOperand(*block.terminator.value, "a terminator")) {
        return *invalid;
      }
    }
    if (block.terminator.returnLoan) {
      if (auto invalid = requireActive(*block.terminator.returnLoan,
                                       "a return dependency")) {
        return *invalid;
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Return ||
        block.terminator.kind == MirTerminatorKind::Exit) {
      for (const MirLoan &loan : body.loans) {
        if (active[loan.id - 1] == MirLoanFlowState::Inactive) {
          continue;
        }
        if (!loan.escapes) {
          return failure(body, owner,
                         "non-escaping loan " + std::to_string(loan.id) +
                             " remains active at a normal body exit",
                         block.id);
        }
        if (loan.kind != MirLoanKind::Stored &&
            (!block.terminator.returnLoan ||
             *block.terminator.returnLoan != loan.id)) {
          return failure(body, owner,
                         "escaping loan " + std::to_string(loan.id) +
                             " remains active on a return edge that does not "
                             "return it",
                         block.id);
        }
      }
    }

    for (const MirBlockId successor : successors(block.terminator)) {
      std::optional<MirLoanState> &entry = blockEntries[successor - 1];
      if (!entry) {
        entry = active;
        pending.push(successor);
        continue;
      }
      if (*entry == active) {
        continue;
      }
      const auto mismatch =
          std::mismatch(entry->begin(), entry->end(), active.begin());
      const MirLoanId loan = static_cast<MirLoanId>(std::distance(
                                 entry->begin(), mismatch.first)) +
                             1;
      return failure(body, owner,
                     "loan " + std::to_string(loan) +
                         " has inconsistent active/suspended state at CFG join",
                     successor);
    }
  }
  return {};
}

using MirLifecycleState = std::vector<OwnershipStateSet>;

[[nodiscard]] MirLifecycleState
joinLifecycleState(const MirLifecycleState &left,
                   const MirLifecycleState &right) {
  MirLifecycleState result = left;
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = result[index] | right[index];
  }
  return result;
}

[[nodiscard]] MirVerificationResult verifyMirLifecycleFlow(const MirBody &body,
                                                           std::size_t owner) {
  if (body.dropObligations.empty()) {
    return {};
  }

  MirLifecycleState initial(body.dropObligations.size(),
                            OwnershipStateSet::Uninitialized);
  for (const MirDropObligation &obligation : body.dropObligations) {
    if (obligation.initiallyActive) {
      initial[obligation.id - 1] = OwnershipStateSet::Available;
    }
  }
  std::vector<std::optional<MirLifecycleState>> entries(body.blocks.size());
  entries[body.entry - 1] = initial;
  std::queue<MirBlockId> pending;
  pending.push(body.entry);

  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock &block = body.blocks[blockId - 1];
    MirLifecycleState state = *entries[blockId - 1];

    const auto require = [&](MirDropObligationId obligation,
                             OwnershipState required, const char *operation,
                             MirInstructionId instruction)
        -> std::optional<MirVerificationResult> {
      if (!state[obligation - 1].definitely(required)) {
        return failure(body, owner,
                       std::string(operation) + " requires obligation " +
                           std::to_string(obligation) +
                           " in a definite state (actual " +
                           std::to_string(state[obligation - 1].bits) + ")",
                       block.id, instruction);
      }
      return std::nullopt;
    };

    for (const MirInstruction &instruction : block.instructions) {
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          if (auto invalid =
                  require(event.target, OwnershipState::Uninitialized,
                          "initialization", instruction.id)) {
            return *invalid;
          }
          state[event.target - 1] = OwnershipStateSet::Available;
          break;
        case MirLifecycleEventKind::Move:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "move", instruction.id)) {
            return *invalid;
          }
          if (auto invalid =
                  require(event.target, OwnershipState::Uninitialized,
                          "move destination", instruction.id)) {
            return *invalid;
          }
          state[event.source - 1] = OwnershipStateSet::Moved;
          state[event.target - 1] = OwnershipStateSet::Available;
          break;
        case MirLifecycleEventKind::Reparent:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "reparent", instruction.id)) {
            return *invalid;
          }
          if (auto invalid =
                  require(event.target, OwnershipState::Uninitialized,
                          "reparent destination", instruction.id)) {
            return *invalid;
          }
          state[event.source - 1] = OwnershipStateSet::Uninitialized;
          state[event.target - 1] = OwnershipStateSet::Available;
          break;
        case MirLifecycleEventKind::Replace:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "replacement source", instruction.id)) {
            return *invalid;
          }
          if (event.target != 0 &&
              state[event.target - 1].contains(OwnershipState::Uninitialized)) {
            return failure(body, owner,
                           "replacement targets an inactive obligation",
                           block.id, instruction.id);
          }
          state[event.source - 1] = OwnershipStateSet::Moved;
          if (event.target != 0) {
            state[event.target - 1] = OwnershipStateSet::Available;
          }
          break;
        case MirLifecycleEventKind::TransferOut:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "transfer", instruction.id)) {
            return *invalid;
          }
          state[event.source - 1] = OwnershipStateSet::Uninitialized;
          break;
        case MirLifecycleEventKind::Drop: {
          const OwnershipStateSet before = state[event.source - 1];
          if (before.definitely(OwnershipState::Uninitialized)) {
            return failure(body, owner, "drop targets an inactive obligation",
                           block.id, instruction.id);
          }
          const bool requiresConditional =
              before.contains(OwnershipState::Uninitialized);
          if (event.conditional != requiresConditional) {
            return failure(body, owner,
                           "drop conditionality disagrees with path state",
                           block.id, instruction.id);
          }
          state[event.source - 1] = OwnershipStateSet::Uninitialized;
          break;
        }
        }
      }
      if (instruction.fullExpressionEnd != 0) {
        for (const MirDropObligation &obligation : body.dropObligations) {
          if (obligation.fullExpression != instruction.fullExpressionEnd) {
            continue;
          }
          if (!state[obligation.id - 1].definitely(
                  OwnershipState::Uninitialized)) {
            return failure(
                body, owner,
                "full-expression boundary retains active drop obligation " +
                    std::to_string(obligation.id),
                block.id, instruction.id);
          }
        }
      }
      if (instruction.cleanupBoundaryEnd != 0) {
        const MirCleanupBoundary &boundary =
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1];
        for (const MirDropObligationId obligation : boundary.obligations) {
          if (!state[obligation - 1].definitely(
                  OwnershipState::Uninitialized)) {
            return failure(
                body, owner,
                "lexical cleanup boundary retains active drop obligation " +
                    std::to_string(obligation),
                block.id, instruction.id);
          }
        }
      }
    }

    if (block.terminator.kind == MirTerminatorKind::Return ||
        block.terminator.kind == MirTerminatorKind::Exit) {
      const auto live = std::find_if(
          state.begin(), state.end(), [](OwnershipStateSet candidate) {
            return !candidate.definitely(OwnershipState::Uninitialized);
          });
      if (live != state.end()) {
        return failure(
            body, owner,
            "normal exit retains active drop obligation " +
                std::to_string(std::distance(state.begin(), live) + 1),
            block.id);
      }
    }

    for (const MirBlockId successor : successors(block.terminator)) {
      std::optional<MirLifecycleState> &entry = entries[successor - 1];
      const MirLifecycleState merged =
          entry ? joinLifecycleState(*entry, state) : state;
      if (!entry || *entry != merged) {
        entry = merged;
        pending.push(successor);
      }
    }
  }
  return {};
}

} // namespace

void rebuildMirReachability(MirBody &body) {
  const std::vector<bool> reachable = reachableBlocks(body);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    body.blocks[index].reachable = reachable[index];
  }
}

bool rebuildMirValueUses(MirBody &body) {
  body.valueUses.assign(body.values.size(), {});
  bool valid = true;
  const auto addUse = [&](MirValueUse use) {
    if (use.value == 0 || use.value > body.valueUses.size()) {
      valid = false;
      return;
    }
    body.valueUses[use.value - 1].push_back(std::move(use));
  };

  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value) {
      addUse({.value = place.value,
              .kind = MirValueUseKind::PlaceRoot,
              .place = place.id});
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if (projection.kind == MirProjectionKind::Index ||
          projection.kind == MirProjectionKind::RawIndex) {
        if (projection.index == 0) {
          continue;
        }
        addUse({.value = projection.index,
                .kind = MirValueUseKind::PlaceIndex,
                .place = place.id});
      }
    }
  }

  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value) {
        addUse({.value = instruction.receiver->value,
                .kind = MirValueUseKind::InstructionReceiver,
                .block = block.id,
                .instruction = instruction.id});
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind == MirOperandKind::Value) {
          addUse({.value = instruction.operands[index].value,
                  .kind = MirValueUseKind::InstructionOperand,
                  .block = block.id,
                  .instruction = instruction.id,
                  .operandIndex = index});
        }
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value) {
      addUse({.value = block.terminator.value->value,
              .kind = MirValueUseKind::Terminator,
              .block = block.id});
    }
  }
  return valid;
}

namespace {

[[nodiscard]] const MirInstruction *linearValueDefinition(const MirBody &body,
                                                          MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirBlock *block =
      value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
  if (block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const MirInstruction &instruction) {
                     return instruction.id == value->definition &&
                            instruction.result == valueId;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

[[nodiscard]] bool hasOneExactValueUse(const MirBody &body, MirValueId valueId,
                                       MirValueUseKind expectedKind,
                                       MirInstructionId expectedInstruction,
                                       std::size_t expectedOperandIndex) {
  std::size_t count = 0;
  bool matched = false;
  const auto record = [&](MirValueUseKind kind,
                          MirInstructionId instruction = 0,
                          std::size_t operandIndex = 0) {
    ++count;
    matched = matched ||
              (kind == expectedKind && instruction == expectedInstruction &&
               (kind != MirValueUseKind::InstructionOperand ||
                operandIndex == expectedOperandIndex));
  };

  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value && place.value == valueId) {
      record(MirValueUseKind::PlaceRoot);
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Index ||
           projection.kind == MirProjectionKind::RawIndex) &&
          projection.index == valueId) {
        record(MirValueUseKind::PlaceIndex);
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value &&
          instruction.receiver->value == valueId) {
        record(MirValueUseKind::InstructionReceiver, instruction.id);
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind == MirOperandKind::Value &&
            instruction.operands[index].value == valueId) {
          record(MirValueUseKind::InstructionOperand, instruction.id, index);
        }
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value &&
        block.terminator.value->value == valueId) {
      record(MirValueUseKind::Terminator);
    }
  }
  return count == 1 && matched;
}

[[nodiscard]] bool consumedCallableReceiver(
    const MirBody &body, const MirOperand &operand,
    MirValueUseKind expectedUseKind, MirInstructionId expectedUseInstruction,
    std::size_t expectedOperandIndex = 0, std::size_t depth = 0) {
  if (depth > body.values.size() + body.instructionCount() ||
      operand.kind != MirOperandKind::Value) {
    return false;
  }
  if (!hasOneExactValueUse(body, operand.value, expectedUseKind,
                           expectedUseInstruction, expectedOperandIndex)) {
    return false;
  }

  const MirInstruction *definition = linearValueDefinition(body, operand.value);
  if (definition == nullptr || definition->operands.size() != 1) {
    return false;
  }
  if (definition->kind == MirInstructionKind::Move) {
    return definition->operands.front().kind == MirOperandKind::Move &&
           definition->ownership &&
           definition->ownership->kind == OwnershipEventKind::Move &&
           definition->ownership->before == OwnershipStateSet::Available &&
           definition->ownership->after == OwnershipStateSet::Moved;
  }
  if (definition->kind == MirInstructionKind::CallInput &&
      definition->callInputKind == HirCallInputKind::MoveValue) {
    return consumedCallableReceiver(body, definition->operands.front(),
                                    MirValueUseKind::InstructionOperand,
                                    definition->id, 0, depth + 1);
  }
  return definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Identity &&
         consumedCallableReceiver(body, definition->operands.front(),
                                  MirValueUseKind::InstructionOperand,
                                  definition->id, 0, depth + 1);
}

[[nodiscard]] bool
movedValueIntoInstruction(const MirBody &body, const MirOperand &operand,
                          MirInstructionId expectedInstruction,
                          std::size_t expectedOperandIndex) {
  if (operand.kind != MirOperandKind::Value) {
    return false;
  }
  const MirInstruction *definition = linearValueDefinition(body, operand.value);
  if (definition == nullptr || definition->kind != MirInstructionKind::Move ||
      definition->operands.size() != 1 ||
      definition->operands.front().kind != MirOperandKind::Move ||
      !definition->ownership ||
      definition->ownership->kind != OwnershipEventKind::Move ||
      definition->ownership->before != OwnershipStateSet::Available ||
      definition->ownership->after != OwnershipStateSet::Moved) {
    return false;
  }

  std::size_t executableUses = 0;
  bool matched = false;
  std::optional<MirPlaceId> bookkeepingPlace;
  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value && place.value == operand.value) {
      const bool exactObligation = std::any_of(
          body.dropObligations.begin(), body.dropObligations.end(),
          [&](const MirDropObligation &obligation) {
            return obligation.kind == MirDropObligationKind::Value &&
                   obligation.place == place.id &&
                   obligation.value == definition->hirValue;
          });
      if (bookkeepingPlace || !place.projections.empty() ||
          place.sourceValue != definition->hirValue || !exactObligation) {
        return false;
      }
      bookkeepingPlace = place.id;
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Index ||
           projection.kind == MirProjectionKind::RawIndex) &&
          projection.index == operand.value) {
        return false;
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (bookkeepingPlace &&
          ((instruction.destination &&
            *instruction.destination == *bookkeepingPlace) ||
           (instruction.receiver &&
            instruction.receiver->place == *bookkeepingPlace) ||
           std::any_of(instruction.operands.begin(), instruction.operands.end(),
                       [&](const MirOperand &candidate) {
                         return candidate.place == *bookkeepingPlace;
                       }))) {
        return false;
      }
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value &&
          instruction.receiver->value == operand.value) {
        ++executableUses;
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind != MirOperandKind::Value ||
            instruction.operands[index].value != operand.value) {
          continue;
        }
        ++executableUses;
        matched = matched || (instruction.id == expectedInstruction &&
                              index == expectedOperandIndex);
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value &&
        block.terminator.value->value == operand.value) {
      ++executableUses;
    }
    if (bookkeepingPlace && block.terminator.value &&
        block.terminator.value->place == *bookkeepingPlace) {
      return false;
    }
  }
  return executableUses == 1 && matched;
}

[[nodiscard]] constexpr bool validAtomicMemoryOrder(AtomicMemoryOrder order) {
  return static_cast<std::size_t>(order) <
         static_cast<std::size_t>(AtomicMemoryOrder::Count);
}

[[nodiscard]] constexpr bool validLoadOrder(AtomicMemoryOrder order) {
  return order == AtomicMemoryOrder::Relaxed ||
         order == AtomicMemoryOrder::Acquire ||
         order == AtomicMemoryOrder::SequentiallyConsistent;
}

[[nodiscard]] constexpr bool validStoreOrder(AtomicMemoryOrder order) {
  return order == AtomicMemoryOrder::Relaxed ||
         order == AtomicMemoryOrder::Release ||
         order == AtomicMemoryOrder::SequentiallyConsistent;
}

[[nodiscard]] constexpr bool
validCompareExchangeFailureOrder(AtomicMemoryOrder success,
                                 AtomicMemoryOrder failure) {
  if (failure == AtomicMemoryOrder::Release ||
      failure == AtomicMemoryOrder::AcquireRelease) {
    return false;
  }
  switch (success) {
  case AtomicMemoryOrder::Relaxed:
  case AtomicMemoryOrder::Release:
    return failure == AtomicMemoryOrder::Relaxed;
  case AtomicMemoryOrder::Acquire:
  case AtomicMemoryOrder::AcquireRelease:
    return failure == AtomicMemoryOrder::Relaxed ||
           failure == AtomicMemoryOrder::Acquire;
  case AtomicMemoryOrder::SequentiallyConsistent:
    return failure == AtomicMemoryOrder::Relaxed ||
           failure == AtomicMemoryOrder::Acquire ||
           failure == AtomicMemoryOrder::SequentiallyConsistent;
  case AtomicMemoryOrder::Count:
    return false;
  }
  return false;
}

[[nodiscard]] constexpr bool
validSynchronizationOperation(const SynchronizationOperation &operation) {
  if (static_cast<std::size_t>(operation.kind) >=
      static_cast<std::size_t>(SynchronizationOperationKind::Count)) {
    return false;
  }
  if ((operation.order && !validAtomicMemoryOrder(*operation.order)) ||
      (operation.failureOrder &&
       !validAtomicMemoryOrder(*operation.failureOrder))) {
    return false;
  }
  switch (operation.kind) {
  case SynchronizationOperationKind::None:
  case SynchronizationOperationKind::ThreadSpawn:
  case SynchronizationOperationKind::ThreadJoin:
  case SynchronizationOperationKind::MutexLock:
  case SynchronizationOperationKind::MutexUnlock:
    return !operation.order && !operation.failureOrder;
  case SynchronizationOperationKind::AtomicLoad:
    return operation.order && validLoadOrder(*operation.order) &&
           !operation.failureOrder;
  case SynchronizationOperationKind::AtomicStore:
    return operation.order && validStoreOrder(*operation.order) &&
           !operation.failureOrder;
  case SynchronizationOperationKind::AtomicReadModifyWrite:
    return operation.order && !operation.failureOrder;
  case SynchronizationOperationKind::AtomicCompareExchange:
    return operation.order && operation.failureOrder &&
           validCompareExchangeFailureOrder(*operation.order,
                                            *operation.failureOrder);
  case SynchronizationOperationKind::Count:
    return false;
  }
  return false;
}

[[nodiscard]] bool
validDefinedFailureOperation(const DefinedFailureOperation &operation) {
  if (operation.propagation >= FailurePropagationKind::Count) {
    return false;
  }
  for (std::size_t originIndex = 0; originIndex < operation.localOrigins.size();
       ++originIndex) {
    const DefinedFailureOrigin &origin = operation.localOrigins[originIndex];
    if (origin.outcomes.empty() || origin.sourceUnit == 0 ||
        origin.end <= origin.start || origin.line < 1) {
      return false;
    }
    for (std::size_t previousIndex = 0; previousIndex < originIndex;
         ++previousIndex) {
      const DefinedFailureOrigin &previous =
          operation.localOrigins[previousIndex];
      if (previous.sourceUnit == origin.sourceUnit &&
          previous.start == origin.start && previous.end == origin.end &&
          previous.line == origin.line &&
          previous.outcomes == origin.outcomes) {
        return false;
      }
    }
    for (std::size_t index = 0; index < origin.outcomes.size(); ++index) {
      const DefinedFailureOutcome outcome = origin.outcomes[index];
      if (!validDefinedFailureOutcome(outcome)) {
        return false;
      }
      if (index != 0) {
        const DefinedFailureOutcome previous = origin.outcomes[index - 1];
        if (previous.code > outcome.code ||
            (previous.code == outcome.code &&
             previous.detail >= outcome.detail)) {
          return false;
        }
      }
    }
  }
  return true;
}

[[nodiscard]] bool
validFailureInstructionShape(const MirInstruction &instruction) {
  if (!validDefinedFailureOperation(instruction.definedFailure)) {
    return false;
  }
  if (!instruction.definedFailure.localOrigins.empty() &&
      instruction.kind != MirInstructionKind::Compute &&
      instruction.kind != MirInstructionKind::Load &&
      instruction.kind != MirInstructionKind::Assign &&
      instruction.kind != MirInstructionKind::Modify &&
      instruction.kind != MirInstructionKind::Move &&
      instruction.kind != MirInstructionKind::Borrow &&
      instruction.kind != MirInstructionKind::CallInput &&
      instruction.kind != MirInstructionKind::Call &&
      instruction.kind != MirInstructionKind::Construct) {
    return false;
  }
  switch (instruction.definedFailure.propagation) {
  case FailurePropagationKind::None:
    return true;
  case FailurePropagationKind::DirectCall:
    return instruction.kind == MirInstructionKind::Call &&
           instruction.dispatch == CallDispatch::Static &&
           instruction.functionTarget.has_value();
  case FailurePropagationKind::VirtualCall:
    return instruction.kind == MirInstructionKind::Call &&
           instruction.dispatch == CallDispatch::Virtual &&
           instruction.functionTarget.has_value();
  case FailurePropagationKind::Constructor:
    return instruction.kind == MirInstructionKind::Construct &&
           instruction.constructorTarget.has_value();
  case FailurePropagationKind::Callable:
    return instruction.kind == MirInstructionKind::Call &&
           (instruction.lambdaTarget.has_value() ||
            instruction.callableInvocation.has_value());
  case FailurePropagationKind::TaskJoin:
    return instruction.kind == MirInstructionKind::Call &&
           instruction.synchronization.kind ==
               SynchronizationOperationKind::ThreadJoin;
  case FailurePropagationKind::Count:
    return false;
  }
  return false;
}

} // namespace

MirVerificationResult verifyMirBody(const MirBody &body, std::size_t owner) {
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return failure(body, owner, "entry block is outside the body");
  }

  const auto validPlace = [&](MirPlaceId id) {
    return body.findPlace(id) != nullptr;
  };
  const auto validLoan = [&](MirLoanId id) {
    return body.findLoan(id) != nullptr;
  };
  const auto validValue = [&](MirValueId id) {
    return body.findValue(id) != nullptr;
  };
  std::unordered_set<HirFullExpressionId> hirFullExpressions;
  for (std::size_t index = 0; index < body.fullExpressions.size(); ++index) {
    const MirFullExpression &expression = body.fullExpressions[index];
    if (expression.id != index + 1 || expression.hirExpression == 0 ||
        (index != 0 && expression.hirExpression <=
                           body.fullExpressions[index - 1].hirExpression) ||
        !hirFullExpressions.insert(expression.hirExpression).second ||
        expression.roots.empty() ||
        ((expression.statement == 0) ==
         (expression.constructorInitializer == 0)) ||
        std::any_of(expression.roots.begin(), expression.roots.end(),
                    [&](HirValueId root) { return root == 0; })) {
      return failure(body, owner,
                     "full-expression table has an invalid identity or root");
    }
  }
  for (std::size_t index = 0; index < body.cleanupBoundaries.size(); ++index) {
    const MirCleanupBoundary &boundary = body.cleanupBoundaries[index];
    std::size_t previous = std::numeric_limits<std::size_t>::max();
    if (boundary.id != index + 1 || boundary.obligations.empty()) {
      return failure(body, owner,
                     "cleanup-boundary table has an invalid identity");
    }
    for (const MirDropObligationId obligationId : boundary.obligations) {
      const MirDropObligation *obligation =
          body.findDropObligation(obligationId);
      if (obligation == nullptr ||
          obligation->kind != MirDropObligationKind::Binding ||
          obligation->constructionOrder >= previous) {
        return failure(body, owner,
                       "cleanup-boundary obligations are not an exact "
                       "reverse construction sequence");
      }
      previous = obligation->constructionOrder;
    }
  }
  const auto validOperand = [&](const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value: {
      const MirValue *value = body.findValue(operand.value);
      return value != nullptr && operand.type == value->info.type;
    }
    case MirOperandKind::Constant:
      return operand.literal &&
             literalMatchesType(*operand.literal, operand.type);
    case MirOperandKind::Address:
      return validPlace(operand.place);
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      return validPlace(operand.place);
    case MirOperandKind::Loan:
      return validLoan(operand.loan);
    }
    return false;
  };
  const auto isBinaryOperation = [](MirOperation operation) {
    switch (operation) {
    case MirOperation::Comma:
    case MirOperation::Add:
    case MirOperation::Subtract:
    case MirOperation::Multiply:
    case MirOperation::Divide:
    case MirOperation::Remainder:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::ShiftLeft:
    case MirOperation::ShiftRight:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Index:
    case MirOperation::PointerAdd:
    case MirOperation::PointerSubtract:
    case MirOperation::PointerDifference:
      return true;
    case MirOperation::None:
    case MirOperation::Literal:
    case MirOperation::EnumConstant:
    case MirOperation::Aggregate:
    case MirOperation::Identity:
    case MirOperation::Convert:
    case MirOperation::ExpectedHasValue:
    case MirOperation::Closure:
    case MirOperation::PackExpansion:
    case MirOperation::PayloadConstruct:
    case MirOperation::PayloadExtract:
    case MirOperation::Unexpected:
    case MirOperation::AddressOf:
    case MirOperation::Positive:
    case MirOperation::Negate:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
    case MirOperation::Assign:
    case MirOperation::AddAssign:
    case MirOperation::SubtractAssign:
    case MirOperation::MultiplyAssign:
    case MirOperation::DivideAssign:
    case MirOperation::RemainderAssign:
    case MirOperation::BitwiseAndAssign:
    case MirOperation::BitwiseOrAssign:
    case MirOperation::BitwiseXorAssign:
    case MirOperation::ShiftLeftAssign:
    case MirOperation::ShiftRightAssign:
    case MirOperation::PreIncrement:
    case MirOperation::PreDecrement:
    case MirOperation::PostIncrement:
    case MirOperation::PostDecrement:
    case MirOperation::Count:
      return false;
    }
    return false;
  };
  const auto isUnaryOperation = [](MirOperation operation) {
    switch (operation) {
    case MirOperation::Identity:
    case MirOperation::Convert:
    case MirOperation::ExpectedHasValue:
    case MirOperation::Unexpected:
    case MirOperation::Positive:
    case MirOperation::Negate:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
    case MirOperation::AddressOf:
      return true;
    default:
      return false;
    }
  };
  const auto isIntegerType = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
      return true;
    default:
      return false;
    }
  };
  const auto validRawPointer = [](const SemanticType &type) {
    return type.kind == SemanticType::RawPointer &&
           type.arguments.size() == 1 &&
           type.arguments.front() != SemanticType::Void;
  };
  const auto validRawMemberCallReceiver =
      [&](const MirInstruction &instruction) {
        if (instruction.kind != MirInstructionKind::Call ||
            instruction.unsafeOperation != UnsafeOperationKind::RawMember) {
          return true;
        }
        if (!instruction.rawMemoryAccess || !instruction.receiver ||
            instruction.receiver->place == 0 ||
            instruction.receiver->type.kind != SemanticType::Class) {
          return false;
        }
        const MirPlace *place = body.findPlace(instruction.receiver->place);
        if (place == nullptr || place->root != MirPlaceRootKind::Value ||
            place->projections.size() != 1 ||
            place->projections.front().kind !=
                MirProjectionKind::RawDereference ||
            place->type != instruction.receiver->type) {
          return false;
        }
        const MirValue *pointer = body.findValue(place->value);
        return pointer != nullptr && validRawPointer(pointer->info.type) &&
               pointer->info.type.arguments.front() == place->type &&
               pointer->info.type.pointerAccess == place->access &&
               (instruction.receiver->kind != MirOperandKind::BorrowWrite ||
                place->access == AccessMode::Mutable);
      };
  const auto validCompute = [&](const MirInstruction &instruction) {
    if (!instruction.result || instruction.operation == MirOperation::None ||
        instruction.operation == MirOperation::Count) {
      return false;
    }
    const MirValue *result = body.findValue(*instruction.result);
    if (result == nullptr) {
      return false;
    }
    const SemanticType &resultType = result->info.type;
    if (instruction.operation == MirOperation::AddressOf) {
      if (instruction.operands.size() != 1 ||
          instruction.operands.front().kind != MirOperandKind::Address ||
          resultType.kind != SemanticType::RawPointer ||
          resultType.arguments.size() != 1 ||
          resultType.arguments.front() != instruction.operands.front().type) {
        return false;
      }
      const MirPlace *place =
          body.findPlace(instruction.operands.front().place);
      return place != nullptr && resultType.pointerAccess == place->access;
    }
    if (instruction.operation == MirOperation::PointerAdd) {
      if (instruction.operands.size() != 2 || !validRawPointer(resultType)) {
        return false;
      }
      const SemanticType &left = instruction.operands[0].type;
      const SemanticType &right = instruction.operands[1].type;
      return (left == resultType && isIntegerType(right)) ||
             (isIntegerType(left) && right == resultType);
    }
    if (instruction.operation == MirOperation::PointerSubtract) {
      return instruction.operands.size() == 2 && validRawPointer(resultType) &&
             instruction.operands[0].type == resultType &&
             isIntegerType(instruction.operands[1].type);
    }
    if (instruction.operation == MirOperation::PointerDifference) {
      return instruction.operands.size() == 2 &&
             resultType == SemanticType::Int64 &&
             validRawPointer(instruction.operands[0].type) &&
             instruction.operands[0].type == instruction.operands[1].type;
    }
    if (isBinaryOperation(instruction.operation)) {
      return instruction.operands.size() == 2;
    }
    if (instruction.operation == MirOperation::Identity) {
      return instruction.operands.size() == 1 &&
             instruction.operands.front().type == resultType;
    }
    if (isUnaryOperation(instruction.operation)) {
      return instruction.operands.size() == 1;
    }
    switch (instruction.operation) {
    case MirOperation::Literal:
      return instruction.operands.empty() && instruction.literal &&
             literalMatchesType(*instruction.literal, resultType);
    case MirOperation::EnumConstant:
      return instruction.operands.empty() && instruction.enumOwner &&
             *instruction.enumOwner != 0 && instruction.enumValue;
    case MirOperation::Aggregate:
      return true;
    case MirOperation::Closure:
      return instruction.lambdaTarget && *instruction.lambdaTarget != 0 &&
             instruction.closureCaptureTypes.size() ==
                 instruction.operands.size() &&
             instruction.closureCaptureModes.size() ==
                 instruction.operands.size();
    case MirOperation::PackExpansion:
      return instruction.operands.empty();
    case MirOperation::PayloadConstruct:
      return instruction.enumOwner && instruction.enumVariant &&
             !instruction.payloadIndex &&
             resultType.kind == SemanticType::Enum &&
             resultType.enumId == *instruction.enumOwner;
    case MirOperation::PayloadExtract:
      return instruction.enumOwner && instruction.enumVariant &&
             instruction.payloadIndex && instruction.operands.size() == 1 &&
             instruction.operands.front().type.kind == SemanticType::Enum &&
             instruction.operands.front().type.enumId == *instruction.enumOwner;
    default:
      return false;
    }
  };
  const auto validInstructionShape = [&](const MirInstruction &instruction) {
    const bool hasResult = instruction.result.has_value();
    const bool noOperation = instruction.operation == MirOperation::None;
    const bool callable = instruction.kind == MirInstructionKind::Call ||
                          instruction.kind == MirInstructionKind::Construct;
    const bool callInput = instruction.kind == MirInstructionKind::CallInput;
    const bool targetlessBorrowProbe =
        instruction.kind == MirInstructionKind::Call &&
        instruction.parameterTypes.empty() && instruction.lifecycle.empty() &&
        !instruction.functionTarget && !instruction.constructorTarget &&
        !instruction.lambdaTarget;
    if (!validSynchronizationOperation(instruction.synchronization) ||
        (instruction.synchronization.kind !=
             SynchronizationOperationKind::None &&
         instruction.kind != MirInstructionKind::Call) ||
        (callable && !targetlessBorrowProbe &&
         instruction.parameterTypes.size() != instruction.operands.size()) ||
        (!callable && !instruction.parameterTypes.empty()) ||
        (callInput != instruction.callInputRole.has_value()) ||
        (callInput && instruction.callSite == 0) ||
        (!callInput && instruction.callInputIndex != 0) ||
        (!callInput && instruction.callInputRole) ||
        (instruction.kind != MirInstructionKind::CallInput &&
         instruction.kind != MirInstructionKind::Call &&
         instruction.kind != MirInstructionKind::Construct &&
         instruction.callSite != 0) ||
        ((instruction.kind == MirInstructionKind::Call ||
          instruction.kind == MirInstructionKind::Construct) &&
         instruction.callSite != 0 &&
         instruction.callSite != instruction.hirValue) ||
        (instruction.kind != MirInstructionKind::Call &&
         (instruction.callableInvocation || instruction.callableBoundary ||
          !instruction.callableArguments.empty())) ||
        (instruction.operation != MirOperation::Closure &&
         (!instruction.closureCaptureTypes.empty() ||
          !instruction.closureCaptureModes.empty()))) {
      return false;
    }
    const auto validCallableInvocation = [&]() {
      if (!instruction.callableInvocation) {
        return !instruction.callableBoundary;
      }
      if (*instruction.callableInvocation ==
          CallableInvocationCapability::Once) {
        return instruction.receiver &&
               consumedCallableReceiver(body, *instruction.receiver,
                                        MirValueUseKind::InstructionReceiver,
                                        instruction.id);
      }
      if (!instruction.receiver) {
        return false;
      }
      if (instruction.receiver->kind == MirOperandKind::Value ||
          instruction.receiver->kind == MirOperandKind::Move ||
          (*instruction.callableInvocation ==
               CallableInvocationCapability::Read &&
           (instruction.receiver->kind == MirOperandKind::BorrowRead ||
            instruction.receiver->kind == MirOperandKind::BorrowWrite))) {
        return true;
      }
      if (*instruction.callableInvocation ==
              CallableInvocationCapability::Mutable &&
          instruction.receiver->kind == MirOperandKind::BorrowWrite) {
        return true;
      }
      if (instruction.receiver->kind != MirOperandKind::Loan) {
        return false;
      }
      const MirLoan *loan = body.findLoan(instruction.receiver->loan);
      return loan != nullptr && (*instruction.callableInvocation ==
                                     CallableInvocationCapability::Read ||
                                 loan->access == AccessMode::Mutable);
    };
    const auto validCallInputType = [](const SemanticType &source,
                                       const SemanticType &parameter,
                                       HirCallInputKind kind) {
      if (source == parameter) {
        return true;
      }
      if (kind != HirCallInputKind::Value ||
          parameter.kind != SemanticType::RawPointer) {
        return false;
      }
      if (source == SemanticType::NullPtr) {
        return true;
      }
      return source.kind == SemanticType::RawPointer &&
             parameter.arguments.size() == 1 && source.arguments.size() == 1 &&
             parameter.arguments.front() == source.arguments.front() &&
             parameter.pointerAccess == AccessMode::ReadOnly &&
             source.pointerAccess == AccessMode::Mutable;
    };
    const auto sameCallInputTraits = [](const SemanticTypeTraits &left,
                                        const SemanticTypeTraits &right) {
      return left.ownership == right.ownership && left.drop == right.drop &&
             left.copyable == right.copyable && left.movable == right.movable &&
             left.copyAssignable == right.copyAssignable &&
             left.moveAssignable == right.moveAssignable &&
             left.containsBorrowedState == right.containsBorrowedState &&
             left.transferCapable == right.transferCapable &&
             left.shareCapable == right.shareCapable;
    };
    const auto validClassCopyInput = [&](const MirInstruction &input,
                                         const MirOperand &operand) {
      const MirPlace *place = body.findPlace(operand.place);
      return input.info.type.kind == SemanticType::Class &&
             input.info.traits.copyable &&
             !input.info.traits.containsBorrowedState &&
             input.lifecycle.empty() && operand.kind == MirOperandKind::Copy &&
             place != nullptr && place->type == input.info.type &&
             place->sourceValue == input.hirValue && place->traits.copyable &&
             !place->traits.containsBorrowedState &&
             sameCallInputTraits(place->traits, input.info.traits);
    };
    const auto validClassMoveInput = [&](const MirInstruction &input,
                                         const MirOperand &operand) {
      if (input.info.type.kind != SemanticType::Class ||
          !input.info.traits.movable ||
          input.info.traits.containsBorrowedState ||
          operand.kind != MirOperandKind::Value) {
        return false;
      }
      const MirValue *source = body.findValue(operand.value);
      const MirInstruction *definition =
          source == nullptr ? nullptr
                            : linearValueDefinition(body, operand.value);
      if (source == nullptr || definition == nullptr ||
          source->sourceValue != input.hirValue ||
          source->info.type != input.info.type ||
          source->info.category != ValueCategory::Value ||
          definition->hirValue != input.hirValue ||
          definition->kind == MirInstructionKind::Load ||
          definition->kind == MirInstructionKind::CallInput ||
          !sameCallInputTraits(source->info.traits, input.info.traits) ||
          !sameCallInputTraits(definition->info.traits, input.info.traits)) {
        return false;
      }
      std::vector<MirDropObligationId> obligations;
      for (const MirDropObligation &obligation : body.dropObligations) {
        if (obligation.kind == MirDropObligationKind::Value &&
            obligation.value == source->sourceValue &&
            obligation.dropType.type == input.info.type) {
          obligations.push_back(obligation.id);
        }
      }
      if (obligations.empty()) {
        return input.lifecycle.empty();
      }
      return obligations.size() == 1 && input.lifecycle.size() == 1 &&
             input.lifecycle.front().kind ==
                 MirLifecycleEventKind::TransferOut &&
             input.lifecycle.front().source == obligations.front() &&
             input.lifecycle.front().target == 0 &&
             !input.lifecycle.front().conditional;
    };
    switch (instruction.kind) {
    case MirInstructionKind::Compute:
      return !instruction.receiver && !instruction.loan &&
             !instruction.functionTarget && !instruction.constructorTarget &&
             (instruction.operation == MirOperation::Closure ||
              !instruction.lambdaTarget) &&
             validCompute(instruction);
    case MirInstructionKind::Load:
      return noOperation && hasResult && !instruction.destination &&
             instruction.operands.size() == 1 &&
             instruction.operands.front().kind == MirOperandKind::Copy;
    case MirInstructionKind::Initialize:
      return noOperation && !hasResult && instruction.destination &&
             instruction.operands.size() <= 1;
    case MirInstructionKind::Assign:
      return hasResult && instruction.destination &&
             instruction.operands.size() == 1 &&
             (instruction.operation == MirOperation::Assign ||
              instruction.operation == MirOperation::AddAssign ||
              instruction.operation == MirOperation::SubtractAssign ||
              instruction.operation == MirOperation::MultiplyAssign ||
              instruction.operation == MirOperation::DivideAssign ||
              instruction.operation == MirOperation::RemainderAssign ||
              instruction.operation == MirOperation::BitwiseAndAssign ||
              instruction.operation == MirOperation::BitwiseOrAssign ||
              instruction.operation == MirOperation::BitwiseXorAssign ||
              instruction.operation == MirOperation::ShiftLeftAssign ||
              instruction.operation == MirOperation::ShiftRightAssign);
    case MirInstructionKind::Modify:
      return hasResult && instruction.destination &&
             instruction.operands.empty() &&
             (instruction.operation == MirOperation::PreIncrement ||
              instruction.operation == MirOperation::PreDecrement ||
              instruction.operation == MirOperation::PostIncrement ||
              instruction.operation == MirOperation::PostDecrement);
    case MirInstructionKind::Move:
      return noOperation && hasResult && instruction.operands.size() == 1 &&
             instruction.operands.front().kind == MirOperandKind::Move;
    case MirInstructionKind::Borrow:
      return noOperation && !hasResult && instruction.loan &&
             instruction.operands.size() == 1 &&
             (instruction.operands.front().kind == MirOperandKind::BorrowRead ||
              instruction.operands.front().kind ==
                  MirOperandKind::BorrowWrite ||
              instruction.operands.front().kind == MirOperandKind::Loan);
    case MirInstructionKind::CallInput: {
      if (!noOperation || !hasResult || instruction.destination ||
          instruction.receiver || instruction.loan ||
          instruction.functionTarget || instruction.constructorTarget ||
          instruction.lambdaTarget || instruction.fullExpressionEnd != 0 ||
          instruction.cleanupBoundaryEnd != 0 ||
          instruction.operands.size() != 1 ||
          !validCallInputType(instruction.operands.front().type,
                              instruction.info.type,
                              instruction.callInputKind) ||
          (*instruction.callInputRole == MirCallInputRole::Receiver &&
           (instruction.callInputIndex != 0 ||
            instruction.callInputKind == HirCallInputKind::CopyValue))) {
        return false;
      }
      const MirOperand &operand = instruction.operands.front();
      switch (instruction.callInputKind) {
      case HirCallInputKind::Value:
        return instruction.lifecycle.empty() &&
               operand.kind == MirOperandKind::Value;
      case HirCallInputKind::CopyValue:
        return validClassCopyInput(instruction, operand);
      case HirCallInputKind::MoveValue:
        return validClassMoveInput(instruction, operand);
      case HirCallInputKind::ReadBorrow:
        return instruction.lifecycle.empty() &&
               (operand.kind == MirOperandKind::BorrowRead ||
                operand.kind == MirOperandKind::Loan);
      case HirCallInputKind::MutableBorrow:
        if (!instruction.lifecycle.empty()) {
          return false;
        }
        if (operand.kind == MirOperandKind::BorrowWrite) {
          return true;
        }
        if (operand.kind != MirOperandKind::Loan) {
          return false;
        }
        const MirLoan *loan = body.findLoan(operand.loan);
        return loan != nullptr && loan->access == AccessMode::Mutable;
      }
      return false;
    }
    case MirInstructionKind::Call:
      return noOperation &&
             hasResult == (instruction.info.type.kind != SemanticType::Void) &&
             (instruction.callSite == 0 ||
              (instruction.functionTarget &&
               instruction.intrinsic == IntrinsicKind::None &&
               !instruction.constructorTarget && !instruction.lambdaTarget)) &&
             validCallableInvocation() &&
             (!instruction.callableBoundary ||
              *instruction.callableBoundary == CallableBoundary::Confined) &&
             (!instruction.constructorTarget ||
              ((instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
                instruction.intrinsic == IntrinsicKind::StorageConstruct) &&
               !instruction.functionTarget && !instruction.lambdaTarget)) &&
             std::all_of(
                 instruction.callableArguments.begin(),
                 instruction.callableArguments.end(),
                 [&](const CallableArgumentBoundary &argument) {
                   return argument.parameterIndex <
                              instruction.operands.size() &&
                          (argument.boundary == CallableBoundary::Confined ||
                           argument.boundary == CallableBoundary::Owned);
                 }) &&
             std::adjacent_find(instruction.callableArguments.begin(),
                                instruction.callableArguments.end(),
                                [](const CallableArgumentBoundary &left,
                                   const CallableArgumentBoundary &right) {
                                  return left.parameterIndex >=
                                         right.parameterIndex;
                                }) == instruction.callableArguments.end() &&
             (instruction.dispatch != CallDispatch::Virtual ||
              (instruction.functionTarget && instruction.receiver &&
               instruction.dispatchOwner.kind == SemanticType::Class)) &&
             validRawMemberCallReceiver(instruction);
    case MirInstructionKind::Construct:
      return noOperation && hasResult && !instruction.receiver &&
             instruction.info.type.kind == SemanticType::Class &&
             instruction.intrinsic == IntrinsicKind::None &&
             !instruction.functionTarget && !instruction.lambdaTarget &&
             (instruction.callSite == 0 ||
              (instruction.constructorTarget &&
               instruction.constructorKind == ConstructorKind::Ordinary &&
               !instruction.operands.empty())) &&
             (instruction.constructorKind == ConstructorKind::Ordinary ||
              (!instruction.constructorTarget &&
               instruction.operands.size() == 1));
    case MirInstructionKind::Drop:
      return noOperation && !hasResult && instruction.destination &&
             instruction.operands.empty();
    case MirInstructionKind::EndBorrow:
      return noOperation && !hasResult && instruction.loan &&
             instruction.operands.empty();
    case MirInstructionKind::Lifecycle:
      return noOperation && !hasResult && !instruction.destination &&
             !instruction.receiver && instruction.operands.empty() &&
             !instruction.loan && !instruction.functionTarget &&
             !instruction.constructorTarget && !instruction.lambdaTarget &&
             ((!instruction.lifecycle.empty() &&
               instruction.fullExpressionEnd == 0 &&
               instruction.cleanupBoundaryEnd == 0) ||
              (instruction.lifecycle.empty() &&
               ((instruction.fullExpressionEnd != 0) !=
                (instruction.cleanupBoundaryEnd != 0))));
    case MirInstructionKind::Count:
      return false;
    }
    return false;
  };

  std::size_t expectedUseCount = 0;
  for (std::size_t index = 0; index < body.places.size(); ++index) {
    const MirPlace &place = body.places[index];
    if (place.id != index + 1 ||
        (place.root == MirPlaceRootKind::Binding && place.binding == 0) ||
        (place.root == MirPlaceRootKind::Symbol && place.symbol == 0) ||
        (place.capture != 0 && (place.root != MirPlaceRootKind::Symbol ||
                                body.kind != MirBodyKind::Lambda)) ||
        (place.root == MirPlaceRootKind::Temporary && place.temporary == 0) ||
        (place.root == MirPlaceRootKind::Value && !validValue(place.value)) ||
        (place.root == MirPlaceRootKind::Loan && !validLoan(place.loan))) {
      return failure(body, owner,
                     "place " + std::to_string(place.id) +
                         " has an invalid identity or root");
    }
    if (place.key) {
      if (!place.key->valid() || place.key->domain != body.placeDomain) {
        return failure(body, owner,
                       "place " + std::to_string(place.id) +
                           " has an invalid ownership domain or key");
      }
    }
    expectedUseCount += place.root == MirPlaceRootKind::Value ? 1 : 0;
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Field &&
           projection.field == 0) ||
          (projection.kind == MirProjectionKind::RawIndex &&
           !validValue(projection.index)) ||
          (projection.kind == MirProjectionKind::Index &&
           ((projection.index == 0 && !projection.constantIndex &&
             projection.selection == 0) ||
            (projection.index != 0 && !validValue(projection.index)) ||
            (projection.constantIndex && projection.selection != 0)))) {
        return failure(body, owner,
                       "place " + std::to_string(place.id) +
                           " has an invalid projection");
      }
      if (projection.kind == MirProjectionKind::RawDereference ||
          projection.kind == MirProjectionKind::RawIndex) {
        const bool firstProjection = &projection == &place.projections.front();
        const MirValue *root = place.root == MirPlaceRootKind::Value
                                   ? body.findValue(place.value)
                                   : nullptr;
        if (!firstProjection || root == nullptr ||
            !validRawPointer(root->info.type)) {
          return failure(body, owner,
                         "place " + std::to_string(place.id) +
                             " has a raw projection without a valid raw "
                             "pointer root");
        }
        if (projection.kind == MirProjectionKind::RawIndex) {
          const MirValue *indexValue = body.findValue(projection.index);
          if (indexValue == nullptr || !isIntegerType(indexValue->info.type)) {
            return failure(body, owner,
                           "place " + std::to_string(place.id) +
                               " has a non-integer raw pointer index");
          }
        }
      }
      expectedUseCount += (projection.kind == MirProjectionKind::Index ||
                           projection.kind == MirProjectionKind::RawIndex) &&
                                  projection.index != 0
                              ? 1
                              : 0;
    }
  }

  std::unordered_set<SemanticLoanId> semanticLoans;
  for (std::size_t index = 0; index < body.loans.size(); ++index) {
    const MirLoan &loan = body.loans[index];
    if (loan.id != index + 1 || !validPlace(loan.source) ||
        (loan.parent != 0 &&
         (!validLoan(loan.parent) || loan.parent == loan.id))) {
      return failure(body, owner,
                     "loan " + std::to_string(loan.id) +
                         " has an invalid identity, parent, or source place");
    }
    if (loan.parent != 0) {
      const MirLoan &parent = body.loans[loan.parent - 1];
      if (loan.escapes || (loan.kind != MirLoanKind::Local &&
                           loan.kind != MirLoanKind::CallResult)) {
        return failure(body, owner,
                       "child loan " + std::to_string(loan.id) +
                           " must be a non-escaping Local or CallResult loan");
      }
      const bool sharedDerivedCall = loan.kind == MirLoanKind::CallResult &&
                                     loan.access == AccessMode::ReadOnly &&
                                     parent.access == AccessMode::ReadOnly;
      if (loan.entry ||
          (parent.access != AccessMode::Mutable && !sharedDerivedCall)) {
        return failure(body, owner,
                       "child loan " + std::to_string(loan.id) +
                           " must be non-entry and either have a mutable "
                           "parent or be a read-only derived call result");
      }
      std::unordered_set<MirLoanId> ancestors;
      MirLoanId ancestor = loan.parent;
      while (ancestor != 0) {
        if (!ancestors.insert(ancestor).second || ancestor == loan.id) {
          return failure(body, owner,
                         "loan " + std::to_string(loan.id) +
                             " participates in a parent cycle");
        }
        ancestor = body.loans[ancestor - 1].parent;
      }
    }
    std::unordered_set<HirBindingId> carriers;
    for (const HirBindingId carrier : loan.carriers) {
      if (carrier == 0 || !carriers.insert(carrier).second) {
        return failure(body, owner,
                       "loan " + std::to_string(loan.id) +
                           " has an invalid or duplicate carrier binding");
      }
    }
    if (loan.semanticLoan != 0 &&
        !semanticLoans.insert(loan.semanticLoan).second) {
      return failure(body, owner,
                     "semantic loan " + std::to_string(loan.semanticLoan) +
                         " is represented by more than one MIR loan");
    }
    if (loan.entry) {
      const MirPlace *source = body.findPlace(loan.source);
      if (loan.producedBy != 0 || source == nullptr ||
          source->root != MirPlaceRootKind::Binding ||
          std::find(loan.carriers.begin(), loan.carriers.end(),
                    source->binding) == loan.carriers.end()) {
        return failure(body, owner,
                       "entry loan " + std::to_string(loan.id) +
                           " must originate at a formal parameter binding "
                           "that remains one of its carriers");
      }
    }
    if (loan.kind == MirLoanKind::Parameter && !loan.entry) {
      return failure(body, owner,
                     "parameter loan " + std::to_string(loan.id) +
                         " is not active at body entry");
    }
  }

  std::unordered_set<HirDropObligationId> hirDropObligations;
  for (std::size_t index = 0; index < body.dropObligations.size(); ++index) {
    const MirDropObligation &obligation = body.dropObligations[index];
    const MirPlace *place = body.findPlace(obligation.place);
    const bool bindingKind = obligation.kind == MirDropObligationKind::Binding;
    if (obligation.id != index + 1 || obligation.hirObligation == 0 ||
        obligation.constructionOrder == 0 ||
        !hirDropObligations.insert(obligation.hirObligation).second ||
        place == nullptr || place->type != obligation.dropType.type ||
        obligation.dropType.type == SemanticType::Unknown ||
        obligation.dropType.type.kind == SemanticType::Reference ||
        (bindingKind != (obligation.binding != 0)) ||
        (bindingKind != (obligation.value == 0)) ||
        (bindingKind != (obligation.fullExpression == 0)) ||
        (bindingKind != (obligation.hirFullExpression == 0)) ||
        (!bindingKind &&
         (obligation.fullExpression == 0 ||
          obligation.fullExpression > body.fullExpressions.size() ||
          body.fullExpressions[obligation.fullExpression - 1].hirExpression !=
              obligation.hirFullExpression)) ||
        (obligation.initiallyActive && !bindingKind) ||
        (obligation.dropType.type.kind == SemanticType::Class &&
         !obligation.dropType.classInstance) ||
        (obligation.dropType.classInstance &&
         obligation.dropType.type.kind != SemanticType::Class) ||
        (obligation.dropType.destructor &&
         !obligation.dropType.classInstance)) {
      return failure(body, owner,
                     "drop obligation " + std::to_string(obligation.id) +
                         " has an invalid identity, place, or type");
    }
    if (bindingKind && (place->root != MirPlaceRootKind::Binding ||
                        place->binding != obligation.binding)) {
      return failure(body, owner,
                     "binding drop obligation does not name its exact place");
    }
    if (!bindingKind && place->sourceValue != obligation.value) {
      return failure(body, owner,
                     "value drop obligation does not retain source identity");
    }
  }

  const auto operandCarriesObligation =
      [&](const MirOperand &operand, const MirDropObligation &obligation) {
        if (operand.type != obligation.dropType.type) {
          return false;
        }
        if (operand.kind == MirOperandKind::Value) {
          const MirValue *value = body.findValue(operand.value);
          return obligation.kind == MirDropObligationKind::Value &&
                 value != nullptr && value->sourceValue == obligation.value;
        }
        if (operand.kind == MirOperandKind::Copy ||
            operand.kind == MirOperandKind::Move ||
            operand.kind == MirOperandKind::Address ||
            operand.kind == MirOperandKind::BorrowRead ||
            operand.kind == MirOperandKind::BorrowWrite) {
          const MirPlace *place = body.findPlace(operand.place);
          const MirPlace *required = body.findPlace(obligation.place);
          if (place == nullptr || required == nullptr) {
            return false;
          }
          if (place->id == required->id) {
            return true;
          }
          if (obligation.kind == MirDropObligationKind::Binding) {
            return place->root == MirPlaceRootKind::Binding &&
                   place->binding == obligation.binding &&
                   place->projections.empty();
          }
          return place->sourceValue == obligation.value &&
                 place->projections.empty();
        }
        return false;
      };
  const auto instructionConsumesObligation =
      [&](const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          const MirOperand &operand = instruction.operands[index];
          bool consuming = operand.kind == MirOperandKind::Move;
          if (operand.kind == MirOperandKind::Value) {
            switch (instruction.kind) {
            case MirInstructionKind::Initialize:
            case MirInstructionKind::Assign:
              consuming = index == 0;
              break;
            case MirInstructionKind::Call:
            case MirInstructionKind::Construct:
              consuming = index < instruction.parameterTypes.size() &&
                          instruction.parameterTypes[index].kind !=
                              SemanticType::Reference;
              break;
            case MirInstructionKind::CallInput:
              consuming = index == 0 && instruction.callInputKind ==
                                            HirCallInputKind::MoveValue;
              break;
            case MirInstructionKind::Compute:
              consuming = instruction.operation == MirOperation::Aggregate ||
                          instruction.operation == MirOperation::Unexpected ||
                          (instruction.operation == MirOperation::Closure &&
                           index < instruction.closureCaptureModes.size() &&
                           instruction.closureCaptureModes[index] ==
                               LambdaCaptureMode::Move) ||
                          ((instruction.operation == MirOperation::Identity ||
                            instruction.operation == MirOperation::Convert ||
                            instruction.operation == MirOperation::Comma) &&
                           index + 1 == instruction.operands.size());
              break;
            default:
              consuming = false;
              break;
            }
          }
          if (consuming && operandCarriesObligation(operand, obligation)) {
            return true;
          }
        }
        return false;
      };
  const auto instructionProducesObligation =
      [&](const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        const MirPlace *place = body.findPlace(obligation.place);
        const MirValue *result =
            instruction.result ? body.findValue(*instruction.result) : nullptr;
        if (obligation.kind != MirDropObligationKind::Value ||
            place == nullptr || result == nullptr ||
            result->sourceValue != obligation.value ||
            instruction.hirValue != obligation.value ||
            instruction.info.type != obligation.dropType.type) {
          return false;
        }
        if (place->root == MirPlaceRootKind::Value) {
          return place->value == result->id && !instruction.destination;
        }
        return place->root == MirPlaceRootKind::Temporary &&
               instruction.destination == place->id;
      };
  const auto initializationTargetsObligation =
      [&](const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        const MirPlace *place = body.findPlace(obligation.place);
        return instruction.kind == MirInstructionKind::Initialize &&
               place != nullptr && instruction.destination == place->id &&
               instruction.info.type == obligation.dropType.type;
      };
  const auto materializingInstructionKind = [](MirInstructionKind kind) {
    return kind == MirInstructionKind::Compute ||
           kind == MirInstructionKind::Move ||
           kind == MirInstructionKind::Call ||
           kind == MirInstructionKind::Construct;
  };

  if (body.valueUses.size() != body.values.size()) {
    return failure(body, owner,
                   "value-use index size does not match the value table");
  }
  std::vector<std::size_t> definitionCounts(body.values.size(), 0);
  for (std::size_t index = 0; index < body.values.size(); ++index) {
    const MirValue &value = body.values[index];
    if (value.id != index + 1 || value.sourceValue == 0 ||
        value.definitionBlock == 0 || value.definition == 0) {
      return failure(body, owner,
                     "value " + std::to_string(value.id) +
                         " has an invalid identity, provenance, or definition");
    }
    for (const MirValueUse &use : body.valueUses[index]) {
      if (use.value != value.id) {
        return failure(body, owner,
                       "value-use index is stored under the wrong value");
      }
    }
  }

  std::unordered_set<MirInstructionId> instructionIds;
  std::unordered_map<MirInstructionId, std::size_t> instructionOrders;
  std::unordered_map<MirInstructionId, MirBlockId> instructionBlocks;
  std::unordered_map<MirInstructionId, const MirInstruction *> instructionsById;
  std::vector<std::size_t> fullExpressionMarkers(body.fullExpressions.size(),
                                                 0);
  std::vector<std::size_t> cleanupBoundaryMarkers(body.cleanupBoundaries.size(),
                                                  0);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const MirBlock &block = body.blocks[index];
    if (block.id != index + 1) {
      return failure(body, owner,
                     "block identity does not match stored block order");
    }
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions.size(); ++instructionIndex) {
      const MirInstruction &instruction = block.instructions[instructionIndex];
      if (!validFailureInstructionShape(instruction)) {
        const DefinedFailureOrigin *firstOrigin =
            instruction.definedFailure.localOrigins.empty()
                ? nullptr
                : &instruction.definedFailure.localOrigins.front();
        return failure(
            body, owner,
            "defined-failure metadata does not match instruction kind " +
                std::to_string(static_cast<unsigned>(instruction.kind)) +
                " and propagation " +
                std::to_string(static_cast<unsigned>(
                    instruction.definedFailure.propagation)) +
                "; local origins=" +
                std::to_string(instruction.definedFailure.localOrigins.size()) +
                (firstOrigin == nullptr
                     ? std::string{}
                     : ", first anchor unit=" +
                           std::to_string(firstOrigin->sourceUnit) +
                           " outcomes=" +
                           std::to_string(firstOrigin->outcomes.size())),
            block.id, instruction.id);
      }
      if (instruction.id == 0 ||
          !instructionIds.insert(instruction.id).second ||
          instruction.intrinsic == IntrinsicKind::Count ||
          (instruction.destination && !validPlace(*instruction.destination)) ||
          (instruction.receiver && !validOperand(*instruction.receiver)) ||
          (instruction.loan && !validLoan(*instruction.loan)) ||
          (instruction.result && !validValue(*instruction.result)) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return !validOperand(operand);
                      }) ||
          !validInstructionShape(instruction)) {
        return failure(body, owner,
                       "instruction has an invalid shape or reference",
                       block.id, instruction.id);
      }
      if (instruction.fullExpressionEnd != 0) {
        if (instruction.fullExpressionEnd > body.fullExpressions.size()) {
          return failure(body, owner,
                         "full-expression marker names an invalid boundary",
                         block.id, instruction.id);
        }
        ++fullExpressionMarkers[instruction.fullExpressionEnd - 1];
        const MirFullExpression &expression =
            body.fullExpressions[instruction.fullExpressionEnd - 1];
        const bool statementBoundary = expression.statement != 0;
        if (instruction.hirStatement != expression.statement ||
            (statementBoundary &&
             std::find(expression.roots.begin(), expression.roots.end(),
                       instruction.hirValue) == expression.roots.end()) ||
            (!statementBoundary && instruction.hirValue != 0)) {
          return failure(body, owner,
                         "full-expression marker does not retain its exact "
                         "HIR boundary identity",
                         block.id, instruction.id);
        }
      }
      if (instruction.cleanupBoundaryEnd != 0) {
        if (instruction.cleanupBoundaryEnd > body.cleanupBoundaries.size()) {
          return failure(body, owner,
                         "cleanup marker names an invalid boundary", block.id,
                         instruction.id);
        }
        ++cleanupBoundaryMarkers[instruction.cleanupBoundaryEnd - 1];
      }
      const auto validLifecycleObligation = [&](MirDropObligationId id) {
        return id != 0 && body.findDropObligation(id) != nullptr;
      };
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        const bool hasSource = event.source != 0;
        const bool hasTarget = event.target != 0;
        bool validEvent = !event.conditional;
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          validEvent = !hasSource && hasTarget && !event.conditional;
          break;
        case MirLifecycleEventKind::Move:
        case MirLifecycleEventKind::Reparent:
          validEvent = hasSource && hasTarget && event.source != event.target &&
                       !event.conditional;
          break;
        case MirLifecycleEventKind::Replace:
          validEvent =
              hasSource && event.source != event.target && !event.conditional;
          break;
        case MirLifecycleEventKind::TransferOut:
          validEvent = hasSource && !hasTarget && !event.conditional;
          break;
        case MirLifecycleEventKind::Drop:
          validEvent = hasSource && !hasTarget;
          break;
        }
        validEvent = validEvent &&
                     (!hasSource || validLifecycleObligation(event.source)) &&
                     (!hasTarget || validLifecycleObligation(event.target));
        if (!validEvent) {
          return failure(body, owner,
                         "instruction has an invalid lifecycle event", block.id,
                         instruction.id);
        }
        const MirDropObligation *source =
            hasSource ? body.findDropObligation(event.source) : nullptr;
        const MirDropObligation *target =
            hasTarget ? body.findDropObligation(event.target) : nullptr;
        if (source != nullptr && target != nullptr &&
            source->dropType.type != target->dropType.type) {
          return failure(body, owner,
                         "lifecycle source and target types do not match",
                         block.id, instruction.id);
        }
        bool boundToInstruction = true;
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          boundToInstruction =
              target != nullptr &&
              (initializationTargetsObligation(instruction, *target) ||
               ((instruction.kind == MirInstructionKind::Compute ||
                 instruction.kind == MirInstructionKind::Move ||
                 instruction.kind == MirInstructionKind::Call ||
                 instruction.kind == MirInstructionKind::Construct) &&
                instructionProducesObligation(instruction, *target)));
          break;
        case MirLifecycleEventKind::Move:
          boundToInstruction =
              source != nullptr && target != nullptr &&
              instruction.kind == MirInstructionKind::Move &&
              instructionConsumesObligation(instruction, *source) &&
              instructionProducesObligation(instruction, *target);
          break;
        case MirLifecycleEventKind::Reparent:
          boundToInstruction =
              source != nullptr && target != nullptr &&
              instructionConsumesObligation(instruction, *source) &&
              (initializationTargetsObligation(instruction, *target) ||
               (instruction.kind == MirInstructionKind::Compute &&
                instructionProducesObligation(instruction, *target)));
          break;
        case MirLifecycleEventKind::Replace: {
          const MirPlace *destination =
              instruction.destination ? body.findPlace(*instruction.destination)
                                      : nullptr;
          boundToInstruction =
              source != nullptr &&
              instruction.kind == MirInstructionKind::Assign &&
              destination != nullptr &&
              destination->type == source->dropType.type &&
              instructionConsumesObligation(instruction, *source) &&
              (target == nullptr || target->place == destination->id);
          break;
        }
        case MirLifecycleEventKind::TransferOut:
          boundToInstruction =
              source != nullptr &&
              ((instruction.kind == MirInstructionKind::Lifecycle &&
                source->kind == MirDropObligationKind::Value &&
                instruction.hirValue == source->value) ||
               ((instruction.kind == MirInstructionKind::Compute ||
                 instruction.kind == MirInstructionKind::Initialize ||
                 instruction.kind == MirInstructionKind::CallInput ||
                 instruction.kind == MirInstructionKind::Call ||
                 instruction.kind == MirInstructionKind::Construct) &&
                instructionConsumesObligation(instruction, *source)));
          break;
        case MirLifecycleEventKind::Drop:
          break;
        }
        if (!boundToInstruction) {
          return failure(body, owner,
                         "lifecycle event does not match its instruction "
                         "input, output, or destination",
                         block.id, instruction.id);
        }
        if ((event.kind == MirLifecycleEventKind::Drop) !=
            (instruction.kind == MirInstructionKind::Drop)) {
          return failure(body, owner,
                         "drop lifecycle event does not match its instruction",
                         block.id, instruction.id);
        }
        if (event.kind == MirLifecycleEventKind::Drop) {
          const MirDropObligation &obligation =
              *body.findDropObligation(event.source);
          if (!instruction.destination ||
              *instruction.destination != obligation.place) {
            return failure(body, owner,
                           "drop lifecycle event names a different place",
                           block.id, instruction.id);
          }
        }
      }
      if (instruction.destination &&
          materializingInstructionKind(instruction.kind)) {
        const MirPlace *destination = body.findPlace(*instruction.destination);
        const bool boundResultSlot =
            destination != nullptr &&
            destination->root == MirPlaceRootKind::Temporary &&
            std::any_of(
                instruction.lifecycle.begin(), instruction.lifecycle.end(),
                [&](const MirLifecycleEvent &event) {
                  if (event.target == 0 ||
                      (event.kind != MirLifecycleEventKind::Initialize &&
                       event.kind != MirLifecycleEventKind::Move &&
                       event.kind != MirLifecycleEventKind::Reparent)) {
                    return false;
                  }
                  const MirDropObligation *obligation =
                      body.findDropObligation(event.target);
                  return obligation != nullptr &&
                         obligation->place == destination->id &&
                         instructionProducesObligation(instruction,
                                                       *obligation);
                });
        if (!boundResultSlot) {
          return failure(body, owner,
                         "materializing instruction has an unbound result slot",
                         block.id, instruction.id);
        }
      }
      instructionOrders.emplace(instruction.id, instructionIndex);
      instructionBlocks.emplace(instruction.id, block.id);
      instructionsById.emplace(instruction.id, &instruction);
      if (instruction.kind == MirInstructionKind::Borrow) {
        const MirLoan &produced = body.loans[*instruction.loan - 1];
        const MirOperand &source = instruction.operands.front();
        if ((produced.parent == 0 && source.kind == MirOperandKind::Loan) ||
            (produced.parent != 0 && (source.kind != MirOperandKind::Loan ||
                                      source.loan != produced.parent))) {
          return failure(body, owner,
                         "borrow instruction does not match the loan's parent",
                         block.id, instruction.id);
        }
      }
      if (instruction.result) {
        const MirValue &result = body.values[*instruction.result - 1];
        if (result.definitionBlock != block.id ||
            result.definition != instruction.id ||
            result.info.type != instruction.info.type) {
          return failure(
              body, owner,
              "instruction result does not match its value definition",
              block.id, instruction.id);
        }
        ++definitionCounts[*instruction.result - 1];
      }
      expectedUseCount += static_cast<std::size_t>(instruction.receiver &&
                                                   instruction.receiver->kind ==
                                                       MirOperandKind::Value);
      expectedUseCount += static_cast<std::size_t>(std::count_if(
          instruction.operands.begin(), instruction.operands.end(),
          [](const MirOperand &operand) {
            return operand.kind == MirOperandKind::Value;
          }));
    }

    if (block.terminator.kind == MirTerminatorKind::None) {
      return failure(body, owner, "block has no terminator", block.id);
    }
    const auto validTarget = [&](MirBlockId target) {
      return target > 0 && target <= body.blocks.size();
    };
    if (block.terminator.kind == MirTerminatorKind::Goto &&
        !validTarget(block.terminator.target)) {
      return failure(body, owner, "goto target is outside the body", block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Branch &&
        (!validTarget(block.terminator.target) ||
         !validTarget(block.terminator.elseTarget) || !block.terminator.value ||
         !validOperand(*block.terminator.value) ||
         block.terminator.value->type != SemanticType::Bool)) {
      return failure(body, owner, "branch condition or target is invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Switch &&
        (!block.terminator.value || !validOperand(*block.terminator.value) ||
         !validTarget(block.terminator.target) ||
         std::any_of(block.terminator.switchTargets.begin(),
                     block.terminator.switchTargets.end(),
                     [&](const MirSwitchTarget &target) {
                       return !validTarget(target.target);
                     }))) {
      return failure(body, owner, "switch value or target is invalid",
                     block.id);
    }
    if ((block.terminator.returnLoan &&
         block.terminator.kind != MirTerminatorKind::Return) ||
        (block.terminator.returnLoan &&
         !validLoan(*block.terminator.returnLoan))) {
      return failure(body, owner, "return dependency loan is invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Return) {
      if (block.terminator.value && !validOperand(*block.terminator.value)) {
        return failure(body, owner, "return operand is invalid", block.id);
      }
      if (block.terminator.returnLoan) {
        const MirLoan &loan = *body.findLoan(*block.terminator.returnLoan);
        if (loan.kind != MirLoanKind::Return || !loan.escapes) {
          return failure(body, owner,
                         "return dependency loan is not marked as escaping",
                         block.id);
        }
      }
    }
    expectedUseCount += static_cast<std::size_t>(block.terminator.value &&
                                                 block.terminator.value->kind ==
                                                     MirOperandKind::Value);
  }
  if (std::any_of(fullExpressionMarkers.begin(), fullExpressionMarkers.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "full-expression must have exactly one boundary marker");
  }
  if (std::any_of(cleanupBoundaryMarkers.begin(), cleanupBoundaryMarkers.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "cleanup sequence must have exactly one boundary marker");
  }
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.cleanupBoundaryEnd == 0) {
        continue;
      }
      const MirCleanupBoundary &boundary =
          body.cleanupBoundaries[marker.cleanupBoundaryEnd - 1];
      if (boundary.obligations.size() > markerIndex) {
        return failure(body, owner,
                       "cleanup sequence is not contiguous with its marker",
                       block.id, marker.id);
      }
      const std::size_t firstDrop = markerIndex - boundary.obligations.size();
      for (std::size_t dropIndex = 0; dropIndex < boundary.obligations.size();
           ++dropIndex) {
        const MirInstruction &drop = block.instructions[firstDrop + dropIndex];
        if (drop.kind != MirInstructionKind::Drop ||
            drop.lifecycle.size() != 1 ||
            drop.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop.lifecycle.front().source != boundary.obligations[dropIndex]) {
          return failure(body, owner,
                         "cleanup sequence is not the exact contiguous "
                         "reverse construction order",
                         block.id, drop.id);
        }
      }
    }
  }
  std::unordered_map<MirInstructionId, std::size_t> bindingDropCoverage;
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.cleanupBoundaryEnd == 0) {
        continue;
      }
      const MirCleanupBoundary &boundary =
          body.cleanupBoundaries[marker.cleanupBoundaryEnd - 1];
      const std::size_t firstDrop = markerIndex - boundary.obligations.size();
      for (std::size_t dropIndex = 0; dropIndex < boundary.obligations.size();
           ++dropIndex) {
        ++bindingDropCoverage[block.instructions[firstDrop + dropIndex].id];
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1) {
        continue;
      }
      const MirDropObligation *obligation =
          body.findDropObligation(instruction.lifecycle.front().source);
      if (obligation != nullptr &&
          obligation->kind == MirDropObligationKind::Binding &&
          bindingDropCoverage[instruction.id] != 1) {
        return failure(body, owner,
                       "binding drop is not covered by exactly one lexical "
                       "cleanup boundary",
                       block.id, instruction.id);
      }
    }
  }

  if (std::any_of(definitionCounts.begin(), definitionCounts.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "every value must have exactly one instruction definition");
  }

  const std::vector<bool> expectedReachability = reachableBlocks(body);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (body.blocks[index].reachable != expectedReachability[index]) {
      return failure(body, owner, "block reachability index is stale",
                     body.blocks[index].id);
    }
  }

  std::size_t indexedUseCount = 0;
  for (const std::vector<MirValueUse> &uses : body.valueUses) {
    for (const MirValueUse &use : uses) {
      const MirBlock *block = body.findBlock(use.block);
      const MirPlace *place = body.findPlace(use.place);
      const MirInstruction *instruction = nullptr;
      if (block != nullptr && use.instruction != 0) {
        const auto found =
            std::find_if(block->instructions.begin(), block->instructions.end(),
                         [&](const MirInstruction &candidate) {
                           return candidate.id == use.instruction;
                         });
        instruction = found == block->instructions.end() ? nullptr : &*found;
      }
      switch (use.kind) {
      case MirValueUseKind::InstructionOperand:
        if (instruction == nullptr ||
            use.operandIndex >= instruction->operands.size() ||
            instruction->operands[use.operandIndex].kind !=
                MirOperandKind::Value ||
            instruction->operands[use.operandIndex].value != use.value) {
          return failure(body, owner,
                         "indexed instruction operand use does not match MIR");
        }
        break;
      case MirValueUseKind::InstructionReceiver:
        if (instruction == nullptr || !instruction->receiver ||
            instruction->receiver->kind != MirOperandKind::Value ||
            instruction->receiver->value != use.value) {
          return failure(body, owner,
                         "indexed receiver use does not match MIR");
        }
        break;
      case MirValueUseKind::Terminator:
        if (block == nullptr || !block->terminator.value ||
            block->terminator.value->kind != MirOperandKind::Value ||
            block->terminator.value->value != use.value) {
          return failure(body, owner,
                         "indexed terminator use does not match MIR");
        }
        break;
      case MirValueUseKind::PlaceRoot:
        if (place == nullptr || place->root != MirPlaceRootKind::Value ||
            place->value != use.value) {
          return failure(body, owner,
                         "indexed place-root use does not match MIR");
        }
        break;
      case MirValueUseKind::PlaceIndex:
        if (place == nullptr ||
            std::none_of(
                place->projections.begin(), place->projections.end(),
                [&](const MirPlaceProjection &projection) {
                  return (projection.kind == MirProjectionKind::Index ||
                          projection.kind == MirProjectionKind::RawIndex) &&
                         projection.index == use.value;
                })) {
          return failure(body, owner,
                         "indexed place-projection use does not match MIR");
        }
        break;
      }
    }
    indexedUseCount += uses.size();
  }
  if (indexedUseCount != expectedUseCount) {
    return failure(body, owner,
                   "value-use index count does not match MIR operands");
  }

  const std::optional<MirDominanceInfo> dominance = computeMirDominance(body);
  if (!dominance) {
    return failure(body, owner, "MIR dominance analysis rejected the body CFG");
  }
  const auto strictlyPrecedes = [&](const MirInstruction &before,
                                    const MirInstruction &after) {
    const auto beforeBlock = instructionBlocks.find(before.id);
    const auto afterBlock = instructionBlocks.find(after.id);
    const auto beforeOrder = instructionOrders.find(before.id);
    const auto afterOrder = instructionOrders.find(after.id);
    if (beforeBlock == instructionBlocks.end() ||
        afterBlock == instructionBlocks.end() ||
        beforeOrder == instructionOrders.end() ||
        afterOrder == instructionOrders.end()) {
      return false;
    }
    if (beforeBlock->second == afterBlock->second) {
      return beforeOrder->second < afterOrder->second;
    }
    return dominance->dominates(beforeBlock->second, afterBlock->second);
  };
  std::unordered_map<HirValueId, std::size_t> orderedInvocationCounts;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if ((instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Construct) &&
          instruction.callSite != 0 &&
          ++orderedInvocationCounts[instruction.callSite] != 1) {
        return failure(body, owner,
                       "ordered invocation site is executed more than once",
                       block.id, instruction.id);
      }
      if (instruction.kind != MirInstructionKind::CallInput) {
        continue;
      }
      const std::vector<MirValueUse> &uses = body.usesOf(*instruction.result);
      if (uses.size() != 1) {
        return failure(body, owner,
                       "call input must have exactly one executable use",
                       block.id, instruction.id);
      }
      const MirValueUse &use = uses.front();
      const auto callEntry = instructionsById.find(use.instruction);
      const MirInstruction *invocation =
          callEntry == instructionsById.end() ? nullptr : callEntry->second;
      const bool receiverUse =
          *instruction.callInputRole == MirCallInputRole::Receiver &&
          use.kind == MirValueUseKind::InstructionReceiver;
      const bool argumentUse =
          *instruction.callInputRole == MirCallInputRole::Argument &&
          use.kind == MirValueUseKind::InstructionOperand &&
          use.operandIndex == instruction.callInputIndex;
      const bool callLike = invocation != nullptr &&
                            (invocation->kind == MirInstructionKind::Call ||
                             invocation->kind == MirInstructionKind::Construct);
      if (!callLike || invocation->callSite != instruction.callSite ||
          (invocation->kind == MirInstructionKind::Construct && receiverUse) ||
          (!receiverUse && !argumentUse)) {
        return failure(body, owner,
                       "call input use does not match its ordered invocation",
                       block.id, instruction.id);
      }
    }
  }
  const auto callInputFor =
      [&](const MirOperand &operand) -> const MirInstruction * {
    if (operand.kind != MirOperandKind::Value) {
      return nullptr;
    }
    const MirValue *value = body.findValue(operand.value);
    if (value == nullptr) {
      return nullptr;
    }
    const auto definition = instructionsById.find(value->definition);
    return definition == instructionsById.end() ||
                   definition->second->kind != MirInstructionKind::CallInput
               ? nullptr
               : definition->second;
  };
  const auto exactReceiverInputKind = [&](const MirInstruction &invocation,
                                          const MirInstruction &receiver) {
    if (!invocation.callableInvocation) {
      return receiver.callInputKind == HirCallInputKind::Value ||
             receiver.callInputKind == HirCallInputKind::ReadBorrow ||
             receiver.callInputKind == HirCallInputKind::MutableBorrow;
    }
    const bool exactMovedReceiver =
        receiver.callInputKind == HirCallInputKind::MoveValue &&
        invocation.receiver &&
        consumedCallableReceiver(body, *invocation.receiver,
                                 MirValueUseKind::InstructionReceiver,
                                 invocation.id);
    switch (*invocation.callableInvocation) {
    case CallableInvocationCapability::Read:
      return receiver.callInputKind == HirCallInputKind::Value ||
             receiver.callInputKind == HirCallInputKind::ReadBorrow ||
             exactMovedReceiver;
    case CallableInvocationCapability::Mutable:
      return receiver.callInputKind == HirCallInputKind::Value ||
             receiver.callInputKind == HirCallInputKind::MutableBorrow ||
             exactMovedReceiver;
    case CallableInvocationCapability::Once:
      return exactMovedReceiver;
    }
    return false;
  };
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &invocation : block.instructions) {
      if ((invocation.kind != MirInstructionKind::Call &&
           invocation.kind != MirInstructionKind::Construct) ||
          invocation.callSite == 0) {
        continue;
      }
      std::vector<const MirInstruction *> inputs;
      if (invocation.receiver) {
        const MirInstruction *receiver = callInputFor(*invocation.receiver);
        if (receiver == nullptr || receiver->callSite != invocation.callSite ||
            *receiver->callInputRole != MirCallInputRole::Receiver ||
            !exactReceiverInputKind(invocation, *receiver) ||
            receiver->info.type != invocation.receiver->type) {
          return failure(body, owner,
                         "ordered call receiver is not prepared by its exact "
                         "receiver input",
                         block.id, invocation.id);
        }
        inputs.push_back(receiver);
      }
      for (std::size_t index = 0; index < invocation.operands.size(); ++index) {
        const MirInstruction *argument =
            callInputFor(invocation.operands[index]);
        const SemanticType &parameter = invocation.parameterTypes[index];
        const bool exactKind =
            argument != nullptr &&
            (parameter.kind == SemanticType::Class
                 ? (argument->callInputKind == HirCallInputKind::CopyValue ||
                    argument->callInputKind == HirCallInputKind::MoveValue)
                 : argument->callInputKind ==
                       (parameter.kind != SemanticType::Reference
                            ? HirCallInputKind::Value
                            : (parameter.referenceAccess == AccessMode::Mutable
                                   ? HirCallInputKind::MutableBorrow
                                   : HirCallInputKind::ReadBorrow)));
        if (argument == nullptr || argument->callSite != invocation.callSite ||
            *argument->callInputRole != MirCallInputRole::Argument ||
            argument->callInputIndex != index || !exactKind ||
            argument->info.type != invocation.parameterTypes[index]) {
          return failure(body, owner,
                         "ordered invocation argument is not prepared by its "
                         "exact "
                         "indexed input",
                         block.id, invocation.id);
        }
        inputs.push_back(argument);
      }
      const MirInstruction *previous = nullptr;
      for (const MirInstruction *input : inputs) {
        if ((previous != nullptr && !strictlyPrecedes(*previous, *input)) ||
            !strictlyPrecedes(*input, invocation)) {
          return failure(body, owner,
                         "ordered invocation inputs must form a strict "
                         "receiver, argument, invocation chain",
                         block.id, invocation.id);
        }
        previous = input;
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.fullExpressionEnd == 0) {
        continue;
      }
      const MirFullExpression &expression =
          body.fullExpressions[marker.fullExpressionEnd - 1];
      std::vector<MirDropObligationId> expressionDrops;
      for (const MirBlock &candidateBlock : body.blocks) {
        for (const MirInstruction &instruction : candidateBlock.instructions) {
          for (const MirLifecycleEvent &event : instruction.lifecycle) {
            const MirDropObligation *obligation =
                event.kind == MirLifecycleEventKind::Drop
                    ? body.findDropObligation(event.source)
                    : nullptr;
            if (obligation != nullptr &&
                obligation->fullExpression == expression.id) {
              if (candidateBlock.id != block.id) {
                return failure(
                    body, owner,
                    "full-expression drop is not contiguous with its "
                    "boundary marker",
                    candidateBlock.id, instruction.id);
              }
              expressionDrops.push_back(event.source);
            }
          }
        }
      }
      if (expressionDrops.size() > markerIndex) {
        return failure(body, owner,
                       "full-expression drop precedes its root or boundary",
                       block.id, marker.id);
      }
      const std::size_t firstBoundaryInstruction =
          markerIndex - expressionDrops.size();
      std::size_t previousDrop = std::numeric_limits<std::size_t>::max();
      for (std::size_t dropIndex = 0; dropIndex < expressionDrops.size();
           ++dropIndex) {
        const MirInstruction &drop =
            block.instructions[firstBoundaryInstruction + dropIndex];
        const MirDropObligationId obligation = expressionDrops[dropIndex];
        const MirDropObligation *dropDescriptor =
            body.findDropObligation(obligation);
        if (drop.kind != MirInstructionKind::Drop ||
            drop.lifecycle.size() != 1 ||
            drop.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop.lifecycle.front().source != obligation ||
            dropDescriptor == nullptr ||
            dropDescriptor->constructionOrder >= previousDrop) {
          return failure(
              body, owner,
              "full-expression drops must be contiguous and in reverse "
              "obligation order",
              block.id, drop.id);
        }
        previousDrop = dropDescriptor->constructionOrder;
      }
      for (const HirValueId root : expression.roots) {
        bool rootBeforeBoundary = false;
        bool hasConcreteValue = false;
        for (const MirValue &value : body.values) {
          if (value.sourceValue != root) {
            continue;
          }
          hasConcreteValue = true;
          if (value.definitionBlock == block.id) {
            const auto order = instructionOrders.find(value.definition);
            rootBeforeBoundary = rootBeforeBoundary ||
                                 (order != instructionOrders.end() &&
                                  order->second < firstBoundaryInstruction);
          } else if (value.definitionBlock != 0 &&
                     dominance->dominates(value.definitionBlock, block.id)) {
            rootBeforeBoundary = true;
          }
        }
        for (const MirBlock &definitionBlock : body.blocks) {
          for (std::size_t definitionIndex = 0;
               definitionIndex < definitionBlock.instructions.size();
               ++definitionIndex) {
            const MirInstruction &definition =
                definitionBlock.instructions[definitionIndex];
            if (definition.fullExpressionEnd != 0 ||
                definition.hirValue != root) {
              continue;
            }
            if ((definitionBlock.id == block.id &&
                 definitionIndex < firstBoundaryInstruction) ||
                (definitionBlock.id != block.id &&
                 dominance->dominates(definitionBlock.id, block.id))) {
              rootBeforeBoundary = true;
            }
          }
        }
        const bool explicitVoidCompletion =
            !hasConcreteValue && marker.hirValue == root;
        if (!rootBeforeBoundary && !explicitVoidCompletion) {
          return failure(body, owner,
                         "full-expression drop or marker precedes one of its "
                         "roots",
                         block.id, marker.id);
        }
      }
    }
  }

  const auto appendValue = [](std::vector<MirValueId> &values,
                              MirValueId value) {
    if (value != 0 &&
        std::find(values.begin(), values.end(), value) == values.end()) {
      values.push_back(value);
    }
  };
  const auto appendPlaceValues = [&](std::vector<MirValueId> &values,
                                     MirPlaceId placeId) {
    const MirPlace *place = body.findPlace(placeId);
    if (place == nullptr) {
      return;
    }
    if (place->root == MirPlaceRootKind::Value) {
      appendValue(values, place->value);
    }
    for (const MirPlaceProjection &projection : place->projections) {
      if (projection.kind == MirProjectionKind::Index ||
          projection.kind == MirProjectionKind::RawIndex) {
        appendValue(values, projection.index);
      }
    }
  };
  const auto appendOperandValues = [&](std::vector<MirValueId> &values,
                                       const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value:
      appendValue(values, operand.value);
      break;
    case MirOperandKind::Address:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      appendPlaceValues(values, operand.place);
      break;
    case MirOperandKind::Constant:
    case MirOperandKind::Loan:
      break;
    }
  };
  const auto verifyAvailable = [&](MirValueId valueId, MirBlockId useBlock,
                                   std::size_t useOrder,
                                   MirInstructionId instruction)
      -> std::optional<MirVerificationResult> {
    const MirValue *value = body.findValue(valueId);
    if (value == nullptr) {
      return failure(body, owner, "value use has an invalid identity", useBlock,
                     instruction);
    }
    if (value->definitionBlock == useBlock) {
      const auto definition = instructionOrders.find(value->definition);
      if (definition == instructionOrders.end() ||
          definition->second >= useOrder) {
        return failure(body, owner,
                       "value " + std::to_string(valueId) +
                           " is used before its definition in the same block",
                       useBlock, instruction);
      }
      return std::nullopt;
    }
    // Unreachable blocks have no forward-dominance relation to the entry and
    // cannot execute. Same-block ordering above is still checked so their
    // local representation remains coherent.
    if (!dominance->isReachable(useBlock)) {
      return std::nullopt;
    }
    if (!dominance->dominates(value->definitionBlock, useBlock)) {
      return failure(body, owner,
                     "value " + std::to_string(valueId) +
                         " is used in a block not dominated by its definition",
                     useBlock, instruction);
    }
    return std::nullopt;
  };

  for (const MirBlock &block : body.blocks) {
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions.size(); ++instructionIndex) {
      const MirInstruction &instruction = block.instructions[instructionIndex];
      std::vector<MirValueId> usedValues;
      if (instruction.destination) {
        appendPlaceValues(usedValues, *instruction.destination);
      }
      if (instruction.receiver) {
        appendOperandValues(usedValues, *instruction.receiver);
      }
      for (const MirOperand &operand : instruction.operands) {
        appendOperandValues(usedValues, operand);
      }
      for (const MirValueId used : usedValues) {
        if (std::optional<MirVerificationResult> invalid = verifyAvailable(
                used, block.id, instructionIndex, instruction.id)) {
          return std::move(*invalid);
        }
      }
    }

    if (block.terminator.value) {
      std::vector<MirValueId> usedValues;
      appendOperandValues(usedValues, *block.terminator.value);
      for (const MirValueId used : usedValues) {
        if (std::optional<MirVerificationResult> invalid =
                verifyAvailable(used, block.id, block.instructions.size(), 0)) {
          return std::move(*invalid);
        }
      }
    }
  }
  if (MirVerificationResult lifecycle = verifyMirLifecycleFlow(body, owner);
      !lifecycle.valid()) {
    return lifecycle;
  }
  if (MirVerificationResult ownership = verifyMirOwnershipFlow(body, owner);
      !ownership.valid()) {
    return ownership;
  }
  return verifyMirLoanFlow(body, owner);
}

namespace {

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirBlock *block =
      value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
  if (block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const MirInstruction &instruction) {
                     return instruction.id == value->definition;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

[[nodiscard]] bool callableContractRequiresOnce(
    const MirProgram &program, HirFunctionInstanceId function,
    std::size_t parameterIndex,
    std::vector<std::pair<HirFunctionInstanceId, std::size_t>> &visiting) {
  const MirFunctionInstance *target = program.findFunctionInstance(function);
  if (target == nullptr) {
    return false;
  }
  const auto contract = std::find_if(
      target->callableParameters.begin(), target->callableParameters.end(),
      [&](const MirCallableParameter &candidate) {
        return candidate.parameterIndex == parameterIndex;
      });
  if (contract == target->callableParameters.end() ||
      contract->boundary != CallableBoundary::Confined) {
    return false;
  }
  if (std::any_of(contract->signatures.begin(), contract->signatures.end(),
                  [](const MirCallableSignature &signature) {
                    return signature.requiredCapability ==
                           CallableInvocationCapability::Once;
                  })) {
    return true;
  }

  const std::pair key{function, parameterIndex};
  if (std::find(visiting.begin(), visiting.end(), key) != visiting.end()) {
    return false;
  }
  visiting.push_back(key);
  const bool requiresOnce =
      std::any_of(contract->forwardings.begin(), contract->forwardings.end(),
                  [&](const MirCallableForwarding &forwarding) {
                    return forwarding.functionTarget &&
                           callableContractRequiresOnce(
                               program, *forwarding.functionTarget,
                               forwarding.parameterIndex, visiting);
                  });
  visiting.pop_back();
  return requiresOnce;
}

[[nodiscard]] bool callableContractRequiresOnce(const MirProgram &program,
                                                HirFunctionInstanceId function,
                                                std::size_t parameterIndex) {
  std::vector<std::pair<HirFunctionInstanceId, std::size_t>> visiting;
  return callableContractRequiresOnce(program, function, parameterIndex,
                                      visiting);
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForOperand(const MirBody &body, const MirOperand &operand,
                                std::size_t depth = 0);

[[nodiscard]] bool exactMoveFromBinding(const MirBody &body,
                                        const MirOperand &operand,
                                        HirBindingId binding,
                                        std::size_t depth = 0) {
  if (depth > body.values.size() + body.instructionCount() ||
      operand.kind != MirOperandKind::Value || binding == 0) {
    return false;
  }
  const MirInstruction *definition = definitionFor(body, operand.value);
  if (definition == nullptr || definition->operands.size() != 1) {
    return false;
  }
  if (definition->kind == MirInstructionKind::Move) {
    return definition->operands.front().kind == MirOperandKind::Move &&
           definition->ownership &&
           definition->ownership->kind == OwnershipEventKind::Move &&
           definition->ownership->before == OwnershipStateSet::Available &&
           definition->ownership->after == OwnershipStateSet::Moved &&
           callableSourceBindingForOperand(body,
                                           definition->operands.front()) ==
               std::optional<HirBindingId>{binding};
  }
  return definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Identity &&
         exactMoveFromBinding(body, definition->operands.front(), binding,
                              depth + 1);
}

[[nodiscard]] bool
hasOneExactExecutableValueUse(const MirBody &body, MirValueId valueId,
                              MirValueUseKind expectedKind,
                              MirInstructionId expectedInstruction = 0,
                              std::size_t expectedOperandIndex = 0) {
  const MirInstruction *definition = linearValueDefinition(body, valueId);
  if (definition == nullptr) {
    return false;
  }

  std::optional<MirPlaceId> bookkeepingPlace;
  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value && place.value == valueId) {
      const bool exactObligation = std::any_of(
          body.dropObligations.begin(), body.dropObligations.end(),
          [&](const MirDropObligation &obligation) {
            return obligation.kind == MirDropObligationKind::Value &&
                   obligation.place == place.id &&
                   obligation.value == definition->hirValue;
          });
      if (bookkeepingPlace || !place.projections.empty() ||
          place.sourceValue != definition->hirValue || !exactObligation) {
        return false;
      }
      bookkeepingPlace = place.id;
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Index ||
           projection.kind == MirProjectionKind::RawIndex) &&
          projection.index == valueId) {
        return false;
      }
    }
  }

  std::size_t executableUses = 0;
  bool matched = false;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (bookkeepingPlace &&
          ((instruction.destination &&
            *instruction.destination == *bookkeepingPlace) ||
           (instruction.receiver &&
            instruction.receiver->place == *bookkeepingPlace) ||
           std::any_of(instruction.operands.begin(), instruction.operands.end(),
                       [&](const MirOperand &candidate) {
                         return candidate.place == *bookkeepingPlace;
                       }))) {
        return false;
      }
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value &&
          instruction.receiver->value == valueId) {
        ++executableUses;
        matched =
            matched || (expectedKind == MirValueUseKind::InstructionReceiver &&
                        instruction.id == expectedInstruction);
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind != MirOperandKind::Value ||
            instruction.operands[index].value != valueId) {
          continue;
        }
        ++executableUses;
        matched =
            matched || (expectedKind == MirValueUseKind::InstructionOperand &&
                        instruction.id == expectedInstruction &&
                        index == expectedOperandIndex);
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value &&
        block.terminator.value->value == valueId) {
      ++executableUses;
      matched = matched || expectedKind == MirValueUseKind::Terminator;
    }
    if (bookkeepingPlace && block.terminator.value &&
        block.terminator.value->place == *bookkeepingPlace) {
      return false;
    }
  }
  return executableUses == 1 && matched;
}

[[nodiscard]] bool exactReturnedMoveFromBinding(
    const MirBody &body, const MirOperand &operand, HirBindingId binding,
    MirValueUseKind expectedKind = MirValueUseKind::Terminator,
    MirInstructionId expectedInstruction = 0,
    std::size_t expectedOperandIndex = 0, std::size_t depth = 0) {
  if (depth > body.values.size() + body.instructionCount() ||
      operand.kind != MirOperandKind::Value || binding == 0 ||
      !hasOneExactExecutableValueUse(body, operand.value, expectedKind,
                                     expectedInstruction,
                                     expectedOperandIndex)) {
    return false;
  }
  const MirInstruction *definition = linearValueDefinition(body, operand.value);
  if (definition == nullptr || definition->operands.size() != 1) {
    return false;
  }
  if (definition->kind == MirInstructionKind::Move) {
    return definition->operands.front().kind == MirOperandKind::Move &&
           definition->ownership &&
           definition->ownership->kind == OwnershipEventKind::Move &&
           definition->ownership->before == OwnershipStateSet::Available &&
           definition->ownership->after == OwnershipStateSet::Moved &&
           callableSourceBindingForOperand(body,
                                           definition->operands.front()) ==
               std::optional<HirBindingId>{binding};
  }
  return definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Identity &&
         exactReturnedMoveFromBinding(
             body, definition->operands.front(), binding,
             MirValueUseKind::InstructionOperand, definition->id, 0, depth + 1);
}

[[nodiscard]] bool valueTransferredOut(const MirBody &body,
                                       const MirOperand &operand,
                                       bool directDestination = false) {
  if (operand.kind != MirOperandKind::Value) {
    return false;
  }
  const MirInstruction *definition = linearValueDefinition(body, operand.value);
  if (definition == nullptr) {
    return false;
  }
  if (definition->info.traits.drop != DropKind::Lexical) {
    return true;
  }
  std::vector<MirDropObligationId> obligations;
  for (const MirPlace &place : body.places) {
    if (place.root != MirPlaceRootKind::Value || place.value != operand.value) {
      continue;
    }
    for (const MirDropObligation &obligation : body.dropObligations) {
      if (obligation.kind == MirDropObligationKind::Value &&
          obligation.place == place.id &&
          obligation.value == definition->hirValue) {
        obligations.push_back(obligation.id);
      }
    }
  }
  if (obligations.empty()) {
    return directDestination;
  }
  if (obligations.size() != 1) {
    return false;
  }
  const MirDropObligationId obligation = obligations.front();
  std::size_t transfers = 0;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      transfers += static_cast<std::size_t>(std::count_if(
          instruction.lifecycle.begin(), instruction.lifecycle.end(),
          [&](const MirLifecycleEvent &event) {
            return event.kind == MirLifecycleEventKind::TransferOut &&
                   event.source == obligation;
          }));
    }
  }
  return transfers == 1;
}

[[nodiscard]] const MirInstruction *
exactReturnedDefinition(const MirFunctionInstance &instance,
                        const MirBlock &block) {
  if (!block.reachable || block.terminator.kind != MirTerminatorKind::Return ||
      !block.terminator.value ||
      block.terminator.value->kind != MirOperandKind::Value) {
    return nullptr;
  }
  return definitionFor(instance.body, block.terminator.value->value);
}

[[nodiscard]] bool constructorOwnsExactParameter(
    const MirProgram &program, HirConstructorInstanceId target, SymbolId field,
    const SemanticType &callableType, const SemanticType &ownerType) {
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(target);
  const MirClassInstance *owner =
      constructor == nullptr ? nullptr
                             : program.findClassInstance(constructor->owner);
  const auto ownedField =
      owner == nullptr ? std::vector<MirClassFieldInfo>::const_iterator{}
                       : std::find_if(owner->declaredFields.begin(),
                                      owner->declaredFields.end(),
                                      [&](const MirClassFieldInfo &candidate) {
                                        return candidate.symbol == field &&
                                               candidate.type == callableType;
                                      });
  if (constructor == nullptr || constructor->parameterTypes.size() != 1 ||
      constructor->parameterBindings.size() != 1 ||
      constructor->parameterTypes.front() != callableType || owner == nullptr ||
      owner->type != ownerType || ownedField == owner->declaredFields.end() ||
      (ownedField->dropKind == DropKind::Lexical &&
       std::none_of(owner->fieldDropOrder.begin(), owner->fieldDropOrder.end(),
                    [&](const MirFieldDrop &candidate) {
                      return candidate.symbol == field &&
                             candidate.type == callableType;
                    }))) {
    return false;
  }
  const auto initializer = std::find_if(
      constructor->initializers.begin(), constructor->initializers.end(),
      [&](const MirConstructorInitializer &candidate) {
        return candidate.kind == ConstructorInitializerTargetKind::Field &&
               candidate.field == field &&
               candidate.targetType == callableType &&
               candidate.ownedParameter == std::optional<std::size_t>{0} &&
               candidate.arguments.size() == 1;
      });
  if (initializer == constructor->initializers.end()) {
    return false;
  }
  const auto value = std::find_if(
      constructor->body.values.begin(), constructor->body.values.end(),
      [&](const MirValue &candidate) {
        return candidate.sourceValue == initializer->arguments.front();
      });
  return value != constructor->body.values.end() &&
         std::count_if(
             constructor->body.values.begin(), constructor->body.values.end(),
             [&](const MirValue &candidate) {
               return candidate.sourceValue == initializer->arguments.front();
             }) == 1 &&
         exactMoveFromBinding(constructor->body,
                              MirOperand{.kind = MirOperandKind::Value,
                                         .value = value->id,
                                         .type = value->info.type},
                              constructor->parameterBindings.front()) &&
         valueTransferredOut(constructor->body,
                             MirOperand{.kind = MirOperandKind::Value,
                                        .value = value->id,
                                        .type = value->info.type},
                             true);
}

[[nodiscard]] std::optional<std::string>
ownedTransportFailure(const MirProgram &program,
                      const MirFunctionInstance &instance,
                      const MirCallableParameter &parameter) {
  if (!parameter.ownedTransport ||
      parameter.parameterIndex >= instance.parameterBindings.size() ||
      parameter.parameterIndex >= instance.parameterTypes.size()) {
    return "owned transport does not name an exact formal parameter";
  }
  const CallableOwnedTransport &transport = *parameter.ownedTransport;
  const SemanticType &callableType =
      instance.parameterTypes[parameter.parameterIndex];
  if (transport.destinationType != instance.returnType) {
    return "owned transport destination does not match the function return";
  }
  bool foundReturn = false;
  for (const MirBlock &block : instance.body.blocks) {
    if (!block.reachable ||
        block.terminator.kind != MirTerminatorKind::Return) {
      continue;
    }
    foundReturn = true;
    if (!block.terminator.value) {
      return "owned transport has a reachable valueless return";
    }
    if (transport.kind == CallableOwnedTransportKind::ExactReturn) {
      if (transport.field != 0 || instance.returnType != callableType ||
          block.terminator.value->type != callableType ||
          !exactReturnedMoveFromBinding(
              instance.body, *block.terminator.value,
              instance.parameterBindings[parameter.parameterIndex]) ||
          !valueTransferredOut(instance.body, *block.terminator.value)) {
        return "exact-return transport is not rooted in the parameter move";
      }
      continue;
    }
    const MirInstruction *construction =
        exactReturnedDefinition(instance, block);
    if (transport.field == 0 ||
        instance.returnType.kind != SemanticType::Class ||
        construction == nullptr ||
        construction->kind != MirInstructionKind::Construct ||
        construction->info.type != instance.returnType ||
        !construction->result || !construction->constructorTarget ||
        construction->operands.size() != 1 ||
        !hasOneExactExecutableValueUse(instance.body, *construction->result,
                                       MirValueUseKind::Terminator) ||
        !valueTransferredOut(instance.body,
                             MirOperand{.kind = MirOperandKind::Value,
                                        .value = *construction->result,
                                        .type = construction->info.type},
                             true) ||
        !exactMoveFromBinding(
            instance.body, construction->operands.front(),
            instance.parameterBindings[parameter.parameterIndex]) ||
        !movedValueIntoInstruction(instance.body,
                                   construction->operands.front(),
                                   construction->id, 0) ||
        !constructorOwnsExactParameter(
            program, *construction->constructorTarget, transport.field,
            callableType, instance.returnType)) {
      return "exact-field transport does not match its constructor and field";
    }
  }
  return foundReturn ? std::nullopt
                     : std::optional<std::string>{
                           "owned transport has no reachable return"};
}

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId valueId,
                     std::size_t depth);

void appendCanonicalParameterRoles(const SemanticType &type,
                                   std::vector<SemanticType> &roles) {
  if (type.kind != SemanticType::TypePack) {
    roles.push_back(type);
    return;
  }
  if (!type.concretePack) {
    // An empty pack instance currently retains its declaration-level symbolic
    // pack identity. Preserve that single role so equal empty/symbolic pack
    // groupings remain exact while unrelated pack identities still differ.
    roles.push_back(type);
    return;
  }
  for (const SemanticType &element : type.arguments) {
    appendCanonicalParameterRoles(element, roles);
  }
}

[[nodiscard]] bool
exactParameterRoles(const std::vector<SemanticType> &call,
                    const std::vector<SemanticType> &target) {
  std::vector<SemanticType> callRoles;
  std::vector<SemanticType> targetRoles;
  for (const SemanticType &type : call) {
    appendCanonicalParameterRoles(type, callRoles);
  }
  for (const SemanticType &type : target) {
    appendCanonicalParameterRoles(type, targetRoles);
  }
  return callRoles == targetRoles;
}

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForPlace(const MirBody &body, MirPlaceId placeId,
                     std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  if (place->root == MirPlaceRootKind::Loan) {
    const MirLoan *loan = body.findLoan(place->loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  if (place->root == MirPlaceRootKind::Value) {
    const std::optional<MirPlaceId> source =
        borrowSourceForValue(body, place->value, depth + 1);
    return source ? source : std::optional<MirPlaceId>{placeId};
  }
  if (place->root == MirPlaceRootKind::Binding) {
    const MirLoan *carrier = nullptr;
    for (const MirLoan &loan : body.loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    place->binding) == loan.carriers.end()) {
        continue;
      }
      if (carrier != nullptr && carrier->source != loan.source) {
        return std::nullopt;
      }
      carrier = &loan;
    }
    if (carrier != nullptr) {
      return carrier->source;
    }
  }
  return placeId;
}

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForOperand(const MirBody &body, const MirOperand &operand,
                       std::size_t depth = 0) {
  switch (operand.kind) {
  case MirOperandKind::Loan: {
    const MirLoan *loan = body.findLoan(operand.loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  case MirOperandKind::Address:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
    return borrowSourceForPlace(body, operand.place, depth + 1);
  case MirOperandKind::Value:
    return borrowSourceForValue(body, operand.value, depth + 1);
  case MirOperandKind::Constant:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId valueId,
                     std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirInstruction *definition = definitionFor(body, valueId);
  if (definition == nullptr) {
    return std::nullopt;
  }
  if (definition->loan) {
    const MirLoan *loan = body.findLoan(*definition->loan);
    if (loan != nullptr) {
      return loan->source;
    }
  }
  if (definition->operands.size() == 1) {
    return borrowSourceForOperand(body, definition->operands.front(),
                                  depth + 1);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForValue(const MirBody &body, MirValueId valueId,
                              std::size_t depth);

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForPlace(const MirBody &body, MirPlaceId placeId,
                              std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size() +
                  body.instructionCount()) {
    return std::nullopt;
  }
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  if (place->root == MirPlaceRootKind::Binding && place->projections.empty()) {
    return place->binding == 0 ? std::nullopt
                               : std::optional<HirBindingId>{place->binding};
  }
  if (place->root == MirPlaceRootKind::Value) {
    return callableSourceBindingForValue(body, place->value, depth + 1);
  }
  if (place->root == MirPlaceRootKind::Loan) {
    const MirLoan *loan = body.findLoan(place->loan);
    return loan == nullptr
               ? std::nullopt
               : callableSourceBindingForPlace(body, loan->source, depth + 1);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForOperand(const MirBody &body, const MirOperand &operand,
                                std::size_t depth) {
  switch (operand.kind) {
  case MirOperandKind::Loan: {
    const MirLoan *loan = body.findLoan(operand.loan);
    return loan == nullptr
               ? std::nullopt
               : callableSourceBindingForPlace(body, loan->source, depth + 1);
  }
  case MirOperandKind::Address:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
    return callableSourceBindingForPlace(body, operand.place, depth + 1);
  case MirOperandKind::Value:
    return callableSourceBindingForValue(body, operand.value, depth + 1);
  case MirOperandKind::Constant:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForValue(const MirBody &body, MirValueId valueId,
                              std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size() +
                  body.instructionCount()) {
    return std::nullopt;
  }
  const MirInstruction *definition = definitionFor(body, valueId);
  if (definition == nullptr || definition->operands.size() != 1) {
    return std::nullopt;
  }
  return callableSourceBindingForOperand(body, definition->operands.front(),
                                         depth + 1);
}

[[nodiscard]] bool isCallOperatorTarget(const MirFunctionInstance *target) {
  return target != nullptr &&
         target->overloadedOperator == OverloadedOperator::Call;
}

[[nodiscard]] bool
callableSignatureMatches(const MirCallableSignature &signature,
                         const MirInstruction &instruction) {
  const bool sameTarget =
      (signature.functionTarget &&
       signature.functionTarget == instruction.functionTarget &&
       !instruction.lambdaTarget) ||
      (signature.lambdaTarget &&
       signature.lambdaTarget == instruction.lambdaTarget &&
       !instruction.functionTarget);
  return sameTarget && signature.selectedCapability &&
         instruction.callableInvocation == signature.selectedCapability &&
         signature.returnType == instruction.info.type &&
         signature.parameterTypes == instruction.parameterTypes;
}

[[nodiscard]] MirVerificationResult
verifyMirCallableMetadata(const MirProgram &program, const MirBody &body,
                          std::size_t owner,
                          const MirFunctionInstance *caller = nullptr) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Call) {
        continue;
      }

      const MirFunctionInstance *functionTarget =
          instruction.functionTarget
              ? program.findFunctionInstance(*instruction.functionTarget)
              : nullptr;
      const MirLambdaInstance *lambdaTarget =
          instruction.lambdaTarget
              ? program.findLambda(*instruction.lambdaTarget)
              : nullptr;
      const bool exactFunctionCallable =
          isCallOperatorTarget(functionTarget) && !instruction.lambdaTarget;
      const bool exactLambdaCallable =
          lambdaTarget != nullptr && !instruction.functionTarget;
      const bool exactCallableTarget =
          exactFunctionCallable != exactLambdaCallable;

      if (instruction.callableInvocation.has_value() != exactCallableTarget) {
        return failure(body, owner,
                       "callable invocation capability does not match an "
                       "exact lambda or operator() target",
                       block.id, instruction.id);
      }
      if (instruction.callableInvocation) {
        const CallableInvocationCapability expected =
            exactFunctionCallable ? callableInvocationCapability(
                                        functionTarget->receiverMutability)
                                  : CallableInvocationCapability::Read;
        if (*instruction.callableInvocation != expected) {
          return failure(body, owner,
                         "callable invocation capability does not match its "
                         "exact target",
                         block.id, instruction.id);
        }
      }
      if (exactFunctionCallable &&
          (instruction.info.type != functionTarget->returnType ||
           !exactParameterRoles(instruction.parameterTypes,
                                functionTarget->parameterTypes))) {
        return failure(body, owner,
                       "operator() invocation does not match its exact target "
                       "signature",
                       block.id, instruction.id);
      }
      if (exactLambdaCallable &&
          (instruction.info.type != lambdaTarget->returnType ||
           !exactParameterRoles(instruction.parameterTypes,
                                lambdaTarget->parameterTypes))) {
        return failure(body, owner,
                       "lambda invocation does not match its exact target "
                       "signature",
                       block.id, instruction.id);
      }

      const std::optional<HirBindingId> receiverBinding =
          instruction.receiver
              ? callableSourceBindingForOperand(body, *instruction.receiver)
              : std::nullopt;
      const MirCallableParameter *matchedContract = nullptr;
      const MirCallableSignature *matchedSignature = nullptr;
      if (caller != nullptr && receiverBinding) {
        for (const MirCallableParameter &parameter :
             caller->callableParameters) {
          if (parameter.parameterIndex >= caller->parameterBindings.size() ||
              caller->parameterBindings[parameter.parameterIndex] !=
                  *receiverBinding) {
            continue;
          }
          const auto signature = std::find_if(
              parameter.signatures.begin(), parameter.signatures.end(),
              [&](const MirCallableSignature &candidate) {
                return callableSignatureMatches(candidate, instruction);
              });
          if (signature != parameter.signatures.end()) {
            matchedContract = &parameter;
            matchedSignature = &*signature;
            break;
          }
        }
      }
      if (instruction.callableBoundary.has_value() !=
          (matchedContract != nullptr)) {
        return failure(body, owner,
                       "callable invocation boundary does not match its "
                       "enclosing parameter binding",
                       block.id, instruction.id);
      }
      if (instruction.callableBoundary &&
          *instruction.callableBoundary != matchedContract->boundary) {
        return failure(body, owner,
                       "confined callable invocation does not match an "
                       "enclosing parameter signature",
                       block.id, instruction.id);
      }
      if (matchedSignature != nullptr &&
          matchedSignature->requiredCapability ==
              CallableInvocationCapability::Once &&
          (!instruction.receiver ||
           !consumedCallableReceiver(body, *instruction.receiver,
                                     MirValueUseKind::InstructionReceiver,
                                     instruction.id))) {
        return failure(body, owner,
                       "once-callable invocation is not rooted in an exact "
                       "ownership move",
                       block.id, instruction.id);
      }

      if (!instruction.callableArguments.empty() && functionTarget == nullptr) {
        return failure(body, owner,
                       "callable argument descriptors require an exact "
                       "function target",
                       block.id, instruction.id);
      }
      if (functionTarget != nullptr) {
        const auto &expected = functionTarget->callableParameters;
        if (instruction.callableArguments.size() != expected.size()) {
          return failure(body, owner,
                         "callable argument descriptors do not match the "
                         "target function contract",
                         block.id, instruction.id);
        }
        for (std::size_t index = 0; index < expected.size(); ++index) {
          if (instruction.callableArguments[index].parameterIndex !=
                  expected[index].parameterIndex ||
              instruction.callableArguments[index].boundary !=
                  expected[index].boundary) {
            return failure(body, owner,
                           "callable argument descriptor does not match the "
                           "target parameter contract",
                           block.id, instruction.id);
          }
          if (expected[index].boundary == CallableBoundary::Owned) {
            const std::size_t operandIndex = expected[index].parameterIndex;
            if (operandIndex >= instruction.operands.size() ||
                operandIndex >= functionTarget->parameterTypes.size() ||
                instruction.operands[operandIndex].type !=
                    functionTarget->parameterTypes[operandIndex] ||
                !movedValueIntoInstruction(body,
                                           instruction.operands[operandIndex],
                                           instruction.id, operandIndex)) {
              return failure(body, owner,
                             "owned callable argument is not rooted in its "
                             "exact ownership move",
                             block.id, instruction.id);
            }
          }
        }
      }
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirBorrowProducers(const MirProgram &program, const MirBody &body,
                         std::size_t owner) {
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  for (const MirLoan &loan : body.loans) {
    for (const HirBindingId carrier : loan.carriers) {
      bindingLoans[carrier].push_back(loan.id);
    }
  }
  const auto selectedSource = [&](const MirOperand &operand) {
    if (operand.kind == MirOperandKind::Loan) {
      const MirLoan *loan = body.findLoan(operand.loan);
      return loan == nullptr ? std::optional<MirPlaceId>{}
                             : std::optional<MirPlaceId>{loan->source};
    }
    if (operand.place != 0) {
      return std::optional<MirPlaceId>{operand.place};
    }
    const std::optional<MirPlaceId> source =
        borrowSourceForOperand(body, operand);
    if (source && operand.kind == MirOperandKind::Value) {
      const MirInstruction *definition = definitionFor(body, operand.value);
      if (definition != nullptr &&
          definition->kind == MirInstructionKind::CallInput &&
          definition->operands.size() == 1) {
        const MirOperand &prepared = definition->operands.front();
        if (prepared.kind == MirOperandKind::Loan) {
          const MirLoan *loan = body.findLoan(prepared.loan);
          if (loan != nullptr) {
            return std::optional<MirPlaceId>{loan->source};
          }
        }
        if (prepared.place != 0) {
          return std::optional<MirPlaceId>{prepared.place};
        }
      }
    }
    return source;
  };
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct) {
        if (instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::Closure) {
          const MirLambdaInstance *lambda =
              instruction.lambdaTarget
                  ? program.findLambda(*instruction.lambdaTarget)
                  : nullptr;
          const bool exactCaptures =
              lambda != nullptr &&
              instruction.info.type.kind == SemanticType::Lambda &&
              lambda->type == instruction.info.type &&
              instruction.closureCaptureTypes == lambda->captureTypes &&
              instruction.closureCaptureModes == lambda->captureModes &&
              instruction.operands.size() == lambda->captureTypes.size();
          if (!exactCaptures) {
            return failure(body, owner,
                           "closure construction does not match its exact "
                           "concrete lambda instance",
                           block.id, instruction.id);
          }
          for (std::size_t index = 0;
               lambda != nullptr && index < instruction.operands.size();
               ++index) {
            const MirOperand &capture = instruction.operands[index];
            const bool exactType = capture.type == lambda->captureTypes[index];
            const bool exactMode =
                lambda->captureModes[index] == LambdaCaptureMode::Copy
                    ? capture.kind == MirOperandKind::Copy && capture.place != 0
                    : movedValueIntoInstruction(body, capture, instruction.id,
                                                index);
            if (!exactType || !exactMode) {
              return failure(body, owner,
                             "closure capture initialization does not match "
                             "its exact ownership mode",
                             block.id, instruction.id);
            }
          }
        }
        if (instruction.borrowOrigin != BorrowOriginKind::None) {
          return failure(body, owner,
                         "only call and construct instructions may carry a "
                         "borrow-result origin",
                         block.id, instruction.id);
        }
        continue;
      }

      const auto transfersOut = [](const MirInstruction &candidate) {
        return std::any_of(
            candidate.lifecycle.begin(), candidate.lifecycle.end(),
            [](const MirLifecycleEvent &event) {
              return event.kind == MirLifecycleEventKind::TransferOut;
            });
      };
      const bool preparedOwnershipTransfer =
          (instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Construct) &&
          ((instruction.receiver &&
            [&] {
              const MirInstruction *prepared =
                  instruction.receiver->kind == MirOperandKind::Value
                      ? definitionFor(body, instruction.receiver->value)
                      : nullptr;
              return prepared != nullptr &&
                     prepared->kind == MirInstructionKind::CallInput &&
                     prepared->callInputKind == HirCallInputKind::MoveValue &&
                     transfersOut(*prepared);
            }()) ||
           std::any_of(instruction.operands.begin(), instruction.operands.end(),
                       [&](const MirOperand &operand) {
                         const MirInstruction *prepared =
                             operand.kind == MirOperandKind::Value
                                 ? definitionFor(body, operand.value)
                                 : nullptr;
                         return prepared != nullptr &&
                                prepared->kind ==
                                    MirInstructionKind::CallInput &&
                                prepared->callInputKind ==
                                    HirCallInputKind::MoveValue &&
                                transfersOut(*prepared);
                       }));
      const bool transfersOwnership =
          transfersOut(instruction) || preparedOwnershipTransfer;
      if (instruction.kind == MirInstructionKind::Call && transfersOwnership &&
          instruction.intrinsic == IntrinsicKind::None &&
          !instruction.functionTarget && !instruction.lambdaTarget) {
        return failure(body, owner,
                       "ordinary call is missing its exact target identity",
                       block.id, instruction.id);
      }
      if (instruction.kind == MirInstructionKind::Construct &&
          instruction.constructorKind == ConstructorKind::Ordinary &&
          transfersOwnership && !instruction.constructorTarget) {
        return failure(
            body, owner,
            "ordinary construction is missing its exact constructor target",
            block.id, instruction.id);
      }

      const MirFunctionInstance *target = nullptr;
      if (instruction.functionTarget) {
        target = program.findFunctionInstance(*instruction.functionTarget);
        if (target == nullptr) {
          return failure(body, owner,
                         "call borrow contract references an invalid function "
                         "target",
                         block.id, instruction.id);
        }
        if (instruction.borrowOrigin != target->returnBorrowOrigin ||
            instruction.borrowArgument != target->returnBorrowParameter ||
            instruction.borrowAccess != target->returnBorrowAccess ||
            instruction.borrowPlace != target->returnBorrowPlace) {
          return failure(body, owner,
                         "call-result borrow origin does not match the target "
                         "function summary",
                         block.id, instruction.id);
        }
        if (!exactParameterRoles(instruction.parameterTypes,
                                 target->parameterTypes)) {
          return failure(body, owner,
                         "call parameter roles do not match the exact target "
                         "signature",
                         block.id, instruction.id);
        }
        const FailurePropagationKind expectedPropagation =
            target->linkage == LanguageLinkage::C ||
                    instruction.intrinsic != IntrinsicKind::None
                ? FailurePropagationKind::None
            : instruction.dispatch == CallDispatch::Virtual
                ? FailurePropagationKind::VirtualCall
                : FailurePropagationKind::DirectCall;
        if (instruction.definedFailure.propagation != expectedPropagation) {
          return failure(body, owner,
                         "call failure propagation does not match its exact "
                         "target and dispatch",
                         block.id, instruction.id);
        }
      }
      const MirConstructorInstance *constructorTarget = nullptr;
      if (instruction.constructorTarget) {
        constructorTarget =
            program.findConstructorInstance(*instruction.constructorTarget);
        if (constructorTarget == nullptr) {
          return failure(body, owner,
                         "construct borrow contract references an invalid "
                         "constructor target",
                         block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Construct &&
            (instruction.borrowOrigin != constructorTarget->borrowOrigin ||
             instruction.borrowArgument != constructorTarget->borrowParameter ||
             instruction.borrowAccess != constructorTarget->borrowAccess)) {
          return failure(body, owner,
                         "construct-result borrow origin does not match the "
                         "target constructor summary",
                         block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Construct &&
            !exactParameterRoles(instruction.parameterTypes,
                                 constructorTarget->parameterTypes)) {
          return failure(body, owner,
                         "construct parameter roles do not match the exact "
                         "target signature",
                         block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Construct &&
            instruction.definedFailure.propagation !=
                FailurePropagationKind::Constructor) {
          return failure(body, owner,
                         "construction failure propagation does not match "
                         "its exact constructor target",
                         block.id, instruction.id);
        }
      }
      if (instruction.lambdaTarget && !instruction.functionTarget) {
        const MirLambdaInstance *lambda =
            program.findLambda(*instruction.lambdaTarget);
        if (lambda == nullptr ||
            !exactParameterRoles(instruction.parameterTypes,
                                 lambda->parameterTypes)) {
          return failure(body, owner,
                         "lambda-call parameter roles do not match the exact "
                         "target signature",
                         block.id, instruction.id);
        }
        if (instruction.definedFailure.propagation !=
            FailurePropagationKind::Callable) {
          return failure(body, owner,
                         "lambda-call failure propagation does not preserve "
                         "the callable channel",
                         block.id, instruction.id);
        }
      }
      if (instruction.kind == MirInstructionKind::Call &&
          !instruction.functionTarget && !instruction.lambdaTarget &&
          instruction.intrinsic == IntrinsicKind::None &&
          instruction.callableInvocation &&
          instruction.definedFailure.propagation !=
              FailurePropagationKind::Callable) {
        return failure(body, owner,
                       "deferred callable invocation is missing its failure "
                       "propagation channel",
                       block.id, instruction.id);
      }

      if (instruction.borrowOrigin == BorrowOriginKind::None) {
        if (instruction.loan || instruction.borrowPlace) {
          return failure(body, owner,
                         "call or construct produces a loan without a borrow "
                         "origin",
                         block.id, instruction.id);
        }
        continue;
      }
      if (!instruction.loan) {
        return failure(body, owner,
                       "owner-dependent call or construct is missing its "
                       "result loan",
                       block.id, instruction.id);
      }
      const MirLoan *loan = body.findLoan(*instruction.loan);
      if (loan == nullptr || loan->access != instruction.borrowAccess) {
        return failure(body, owner,
                       "call-result loan access does not match its borrow "
                       "origin",
                       block.id, instruction.id);
      }

      std::optional<MirPlaceId> expectedSource;
      if (instruction.borrowOrigin == BorrowOriginKind::Receiver) {
        if (instruction.kind != MirInstructionKind::Call ||
            !instruction.receiver) {
          return failure(body, owner,
                         "receiver-dependent result has no call receiver",
                         block.id, instruction.id);
        }
        expectedSource = selectedSource(*instruction.receiver);
      } else if (instruction.borrowOrigin == BorrowOriginKind::Argument) {
        if (instruction.borrowArgument >= instruction.operands.size()) {
          return failure(body, owner,
                         "owner-dependent argument index is outside the call "
                         "operands",
                         block.id, instruction.id);
        }
        expectedSource =
            selectedSource(instruction.operands[instruction.borrowArgument]);
      }
      const MirCanonicalPlace expectedPlace =
          expectedSource
              ? canonicalBorrowOriginPlace(body, *expectedSource, bindingLoans)
              : MirCanonicalPlace{.ambiguous = true};
      const MirCanonicalPlace actualPlace =
          canonicalBorrowOriginPlace(body, loan->source, bindingLoans);
      if (instruction.borrowOrigin == BorrowOriginKind::Global) {
        if (!instruction.borrowPlace ||
            !sameGlobalBorrowPlace(*instruction.borrowPlace, actualPlace)) {
          return failure(body, owner,
                         "global call-result loan does not preserve its "
                         "summarized static storage place",
                         block.id, instruction.id);
        }
        continue;
      }
      if (instruction.borrowPlace) {
        return failure(body, owner,
                       "non-global call-result carries a global origin place",
                       block.id, instruction.id);
      }
      if (!sameCanonicalPlace(expectedPlace, actualPlace)) {
        const std::string expected =
            expectedSource ? std::to_string(*expectedSource) : "unresolved";
        return failure(body, owner,
                       "call-result loan does not preserve the selected "
                       "receiver or argument source identity (expected " +
                           expected + ", found " +
                           std::to_string(loan->source) + ")",
                       block.id, instruction.id);
      }
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirFunctionBorrowSummary(const MirProgram &program,
                               const MirFunctionInstance &instance) {
  const MirBody &body = instance.body;
  const auto invalidSummary = [&](std::string message) {
    return failure(body, instance.id, std::move(message));
  };
  switch (instance.returnBorrowOrigin) {
  case BorrowOriginKind::None:
    if (instance.returnBorrowParameter != 0 || instance.returnBorrowPlace) {
      return invalidSummary(
          "function without a return dependency has a nonzero origin index");
    }
    break;
  case BorrowOriginKind::Receiver:
    if (!instance.owner || instance.staticMember ||
        *instance.owner > program.classInstances().size() ||
        instance.returnBorrowParameter != 0 || instance.returnBorrowPlace) {
      return invalidSummary(
          "receiver return dependency requires a non-static class method");
    }
    break;
  case BorrowOriginKind::Argument:
    if (instance.returnBorrowParameter >= instance.parameterTypes.size()) {
      return invalidSummary(
          "return dependency argument index is outside the formal parameters");
    }
    if ((!instance.owner || instance.staticMember) &&
        instance.returnBorrowAccess != AccessMode::ReadOnly) {
      return invalidSummary(
          "ordinary free or static return dependency must be read-only");
    }
    if (instance.returnBorrowPlace) {
      return invalidSummary(
          "argument return dependency carries a global origin place");
    }
    break;
  case BorrowOriginKind::Global:
    if ((instance.owner && !instance.staticMember) ||
        instance.returnBorrowParameter != 0 || !instance.returnBorrowPlace ||
        !instance.returnBorrowPlace->valid()) {
      return invalidSummary(
          "global return dependency requires a free or static function and "
          "one exact static storage place");
    }
    break;
  }

  std::unordered_set<MirLoanId> returnedLoans;
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind != MirTerminatorKind::Return) {
      continue;
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::None) {
      if (block.terminator.returnLoan) {
        return failure(body, instance.id,
                       "return escapes a loan but the function has no return "
                       "dependency summary",
                       block.id);
      }
      continue;
    }
    if (!block.terminator.returnLoan) {
      return failure(body, instance.id,
                     "owner-dependent return is missing its escaping loan",
                     block.id);
    }
    const MirLoan *loan = body.findLoan(*block.terminator.returnLoan);
    if (loan == nullptr || loan->kind != MirLoanKind::Return ||
        !loan->escapes || loan->access != instance.returnBorrowAccess) {
      return failure(body, instance.id,
                     "owner-dependent return loan does not match the function "
                     "summary",
                     block.id);
    }
    const std::optional<MirPlaceId> canonicalSource =
        borrowSourceForPlace(body, loan->source, 0);
    const MirPlace *source =
        canonicalSource ? body.findPlace(*canonicalSource) : nullptr;
    if (source == nullptr) {
      return failure(body, instance.id,
                     "owner-dependent return loan has no source identity",
                     block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Receiver &&
        source->root != MirPlaceRootKind::This) {
      return failure(body, instance.id,
                     "receiver-dependent return does not originate from this",
                     block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Argument &&
        (instance.returnBorrowParameter >= instance.parameterBindings.size() ||
         source->root != MirPlaceRootKind::Binding ||
         source->binding !=
             instance.parameterBindings[instance.returnBorrowParameter])) {
      return failure(body, instance.id,
                     "argument-dependent return does not originate from the "
                     "summarized formal parameter",
                     block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Global) {
      const MirCanonicalPlace actual = canonicalBorrowOriginPlace(
          body, loan->source,
          std::unordered_map<HirBindingId, std::vector<MirLoanId>>{});
      if (!instance.returnBorrowPlace ||
          !sameGlobalBorrowPlace(*instance.returnBorrowPlace, actual)) {
        return failure(body, instance.id,
                       "global-dependent return does not originate from the "
                       "summarized static storage place",
                       block.id);
      }
    }
    returnedLoans.insert(loan->id);
  }
  for (const MirLoan &loan : body.loans) {
    if (loan.kind == MirLoanKind::Return && loan.escapes &&
        !returnedLoans.contains(loan.id)) {
      return invalidSummary(
          "escaping return loan is not attached to a return terminator");
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirConstructorBorrowSummary(const MirConstructorInstance &instance) {
  const MirBody &body = instance.body;
  const auto invalidSummary = [&](std::string message) {
    return failure(body, instance.id, std::move(message));
  };
  if (instance.parameterBindings.size() != instance.parameterTypes.size()) {
    return invalidSummary(
        "constructor parameter bindings do not match its formal parameters");
  }
  switch (instance.borrowOrigin) {
  case BorrowOriginKind::None:
    if (instance.borrowParameter != 0) {
      return invalidSummary(
          "constructor without a borrow dependency has a nonzero origin "
          "index");
    }
    break;
  case BorrowOriginKind::Receiver:
    return invalidSummary(
        "constructor result cannot depend on a receiver lifetime");
  case BorrowOriginKind::Argument:
    if (instance.borrowParameter >= instance.parameterTypes.size()) {
      return invalidSummary(
          "constructor borrow dependency is outside the formal parameters");
    }
    if (instance.parameterBindings[instance.borrowParameter] == 0 ||
        instance.parameterTypes[instance.borrowParameter].kind !=
            SemanticType::Reference ||
        instance.parameterTypes[instance.borrowParameter].referenceAccess !=
            instance.borrowAccess) {
      return invalidSummary(
          "constructor borrow dependency does not identify an exact "
          "reference formal parameter");
    }
    break;
  case BorrowOriginKind::Global:
    return invalidSummary("constructor result cannot depend on global storage");
  }

  std::unordered_set<MirLoanId> initializerLoans;
  std::size_t storedInitializers = 0;
  for (const MirConstructorInitializer &initializer : instance.initializers) {
    if (!initializer.storesReference) {
      continue;
    }
    ++storedInitializers;
    if (instance.borrowOrigin != BorrowOriginKind::Argument ||
        initializer.field == 0 || initializer.arguments.size() != 1 ||
        initializer.borrowAccess != instance.borrowAccess) {
      return invalidSummary(
          "stored-reference initializer does not match the constructor "
          "borrow summary");
    }
    const auto matching = std::find_if(
        body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
          return loan.kind == MirLoanKind::Stored && loan.escapes &&
                 loan.storedField == initializer.field &&
                 loan.producedBy == initializer.arguments.front();
        });
    if (matching == body.loans.end() ||
        matching->access != initializer.borrowAccess) {
      return invalidSummary(
          "stored-reference initializer is missing its exact escaping loan");
    }
    const std::optional<MirPlaceId> canonicalSource =
        borrowSourceForPlace(body, matching->source, 0);
    const MirPlace *source =
        canonicalSource ? body.findPlace(*canonicalSource) : nullptr;
    if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
        source->binding !=
            instance.parameterBindings[instance.borrowParameter]) {
      return invalidSummary(
          "stored-reference initializer loan does not originate from the "
          "summarized formal parameter");
    }
    if (!initializerLoans.insert(matching->id).second) {
      return invalidSummary(
          "multiple stored-reference initializers reuse one escaping loan");
    }
  }
  if ((instance.borrowOrigin == BorrowOriginKind::Argument) !=
      (storedInitializers == 1)) {
    return invalidSummary(
        "constructor borrow summary does not match its stored-reference "
        "initializer");
  }
  for (const MirLoan &loan : body.loans) {
    if (loan.kind == MirLoanKind::Stored && loan.escapes &&
        !initializerLoans.contains(loan.id)) {
      return invalidSummary(
          "escaping stored-reference loan is not attached to a constructor "
          "initializer");
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirProgramBorrowContracts(const MirProgram &program) {
  MirVerificationResult result;
  append(result,
         verifyMirCallableMetadata(program, program.module(), 0, nullptr));
  append(result, verifyMirBorrowProducers(program, program.module(), 0));
  for (const MirClassInstance &instance : program.classInstances()) {
    append(result, verifyMirCallableMetadata(
                       program, instance.fieldInitializers, instance.id));
    append(result, verifyMirBorrowProducers(program, instance.fieldInitializers,
                                            instance.id));
    append(result, verifyMirCallableMetadata(
                       program, instance.staticFieldInitializers, instance.id));
    append(result, verifyMirBorrowProducers(
                       program, instance.staticFieldInitializers, instance.id));
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    append(result, verifyMirCallableMetadata(program, instance.body,
                                             instance.id, &instance));
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
    append(result, verifyMirFunctionBorrowSummary(program, instance));
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    append(result,
           verifyMirCallableMetadata(program, instance.body, instance.id));
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
    append(result, verifyMirConstructorBorrowSummary(instance));
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    append(result,
           verifyMirCallableMetadata(program, instance.body, instance.id));
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    append(result,
           verifyMirCallableMetadata(program, instance.body, instance.id));
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
  }
  return result;
}

[[nodiscard]] MirVerificationResult verifyMirDropTargets(
    const MirProgram &program, const MirBody &body, std::size_t owner,
    const std::function<std::optional<bool>(const SemanticType &)>
        &typeRequiresActiveCleanup) {
  for (const MirDropObligation &obligation : body.dropObligations) {
    std::optional<bool> exactActiveCleanup;
    if (obligation.dropType.type.kind == SemanticType::Lambda &&
        obligation.dropType.lambdaInstance) {
      const MirLambdaInstance *lambda =
          program.findLambda(*obligation.dropType.lambdaInstance);
      if (lambda != nullptr &&
          lambda->captureTypes.size() ==
              lambda->captureRequiresActiveCleanup.size()) {
        exactActiveCleanup =
            std::any_of(lambda->captureRequiresActiveCleanup.begin(),
                        lambda->captureRequiresActiveCleanup.end(),
                        [](bool active) { return active; });
      }
    } else {
      exactActiveCleanup = typeRequiresActiveCleanup(obligation.dropType.type);
    }
    if (!exactActiveCleanup ||
        *exactActiveCleanup != obligation.dropType.requiresActiveCleanup) {
      return failure(body, owner,
                     "drop obligation " + std::to_string(obligation.id) +
                         " does not name its exact active-cleanup descriptor");
    }
    if (obligation.dropType.type.kind == SemanticType::Lambda) {
      const MirLambdaInstance *lambda =
          obligation.dropType.lambdaInstance
              ? program.findLambda(*obligation.dropType.lambdaInstance)
              : nullptr;
      if (lambda == nullptr || lambda->type != obligation.dropType.type ||
          obligation.dropType.classInstance || obligation.dropType.destructor) {
        return failure(body, owner,
                       "lambda drop obligation does not name its exact "
                       "concrete cleanup descriptor");
      }
      continue;
    }
    if (obligation.dropType.type.kind != SemanticType::Class) {
      if (obligation.dropType.classInstance ||
          obligation.dropType.lambdaInstance ||
          obligation.dropType.destructor) {
        return failure(
            body, owner,
            "non-class drop obligation names class cleanup metadata");
      }
      continue;
    }
    const MirClassInstance *classInstance =
        obligation.dropType.classInstance
            ? program.findClassInstance(*obligation.dropType.classInstance)
            : nullptr;
    if (classInstance == nullptr || obligation.dropType.lambdaInstance ||
        classInstance->type != obligation.dropType.type ||
        classInstance->destructor != obligation.dropType.destructor) {
      return failure(body, owner,
                     "drop obligation " + std::to_string(obligation.id) +
                         " does not name the exact class cleanup descriptor");
    }
    if (obligation.dropType.destructor) {
      const MirDestructorInstance *destructor =
          program.findDestructorInstance(*obligation.dropType.destructor);
      if (destructor == nullptr || destructor->owner != classInstance->id) {
        return failure(body, owner,
                       "drop obligation names a destructor for another class");
      }
    }
  }
  return {};
}

} // namespace

MirVerificationResult verifyMirProgram(const MirProgram &program) {
  MirVerificationResult result;
  if (!program.valid()) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .message = "MIR program is marked invalid"});
  }

  const auto verifyBody = [&](const MirBody &body, std::size_t owner) {
    append(result, verifyMirBody(body, owner));
    if (program.executionProfile() != ExecutionProfile::SingleThreaded) {
      return;
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.synchronization.kind ==
            SynchronizationOperationKind::None) {
          continue;
        }
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .block = block.id,
             .instruction = instruction.id,
             .message = "synchronization operation is unavailable in the "
                        "single-threaded execution profile"});
      }
    }
  };

  verifyBody(program.module(), 0);
  const auto typeRequiresActiveCleanup =
      [&](const SemanticType &type, const auto &query) -> std::optional<bool> {
    switch (type.kind) {
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
      return true;
    case SemanticType::Array:
    case SemanticType::Unexpected:
      return type.arguments.size() == 1 ? query(type.arguments.front(), query)
                                        : std::nullopt;
    case SemanticType::Expected: {
      bool unknown = false;
      for (const SemanticType &argument : type.arguments) {
        if (argument == SemanticType::Void) {
          continue;
        }
        const std::optional<bool> active = query(argument, query);
        if (active && *active) {
          return true;
        }
        unknown = unknown || !active;
      }
      return unknown ? std::nullopt : std::optional<bool>{false};
    }
    case SemanticType::Class: {
      const auto instance = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const MirClassInstance &candidate) {
            return candidate.type == type;
          });
      return instance == program.classInstances().end()
                 ? std::nullopt
                 : std::optional<bool>{instance->requiresActiveCleanup};
    }
    case SemanticType::Lambda:
      if (type.lambdaId == 0) {
        return std::nullopt;
      }
      {
        std::optional<bool> result;
        for (const MirLambdaInstance &lambda : program.lambdaInstances()) {
          if (lambda.type != type) {
            continue;
          }
          if (lambda.captureTypes.size() !=
              lambda.captureRequiresActiveCleanup.size()) {
            return std::nullopt;
          }
          const bool active =
              std::any_of(lambda.captureRequiresActiveCleanup.begin(),
                          lambda.captureRequiresActiveCleanup.end(),
                          [](bool capture) { return capture; });
          if (result && *result != active) {
            return std::nullopt;
          }
          result = active;
        }
        return result;
      }
    default:
      return false;
    }
  };
  for (std::size_t index = 0; index < program.classInstances().size();
       ++index) {
    const MirClassInstance &instance = program.classInstances()[index];
    if (instance.id != index + 1 || instance.declaration == 0) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class instance identity or declaration is invalid"});
    }
    if (instance.requiresActiveDropState != instance.destructor.has_value()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class active-drop state does not match its destructor"});
    }
    if (instance.requiresActiveDropState && !instance.requiresActiveCleanup) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message =
               "class active-drop state is missing active-cleanup metadata"});
    }
    const bool exactBaseProjection =
        instance.bases.size() == instance.structuralBases.size() &&
        std::equal(
            instance.bases.begin(), instance.bases.end(),
            instance.structuralBases.begin(),
            [](const HirBaseInstance &actual, const HirBaseInstance &expected) {
              return actual.instance == expected.instance &&
                     actual.type == expected.type &&
                     actual.interface == expected.interface;
            });
    if (!exactBaseProjection) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class base metadata omits or changes a structural "
                      "base"});
    }
    for (const HirBaseInstance &base : instance.structuralBases) {
      const MirClassInstance *baseInstance =
          program.findClassInstance(base.instance);
      if (base.instance == instance.id || baseInstance == nullptr ||
          baseInstance->type != base.type ||
          base.interface != (baseInstance->kind == ClassKind::Interface)) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message =
                 "class base metadata does not name its exact instance"});
      }
    }
    std::unordered_set<HirBindingId> declaredClassFields;
    std::unordered_set<SymbolId> declaredClassSymbols;
    bool validDeclaredFields = true;
    for (const MirClassFieldInfo &field : instance.declaredFields) {
      const std::optional<bool> fieldRequiresActiveCleanup =
          typeRequiresActiveCleanup(field.type, typeRequiresActiveCleanup);
      validDeclaredFields =
          validDeclaredFields && field.field != 0 && field.symbol != 0 &&
          declaredClassFields.insert(field.field).second &&
          declaredClassSymbols.insert(field.symbol).second &&
          field.type != SemanticType::Unknown && fieldRequiresActiveCleanup &&
          *fieldRequiresActiveCleanup == field.requiresActiveCleanup;
    }
    const std::vector<MirClassFieldInfo> declaredLifecycleFields = [&] {
      std::vector<MirClassFieldInfo> fields;
      std::copy_if(instance.declaredFields.begin(),
                   instance.declaredFields.end(), std::back_inserter(fields),
                   [](const MirClassFieldInfo &field) {
                     return field.dropKind == DropKind::Lexical;
                   });
      return fields;
    }();
    validDeclaredFields =
        validDeclaredFields &&
        declaredLifecycleFields.size() == instance.fields.size() &&
        std::equal(declaredLifecycleFields.begin(),
                   declaredLifecycleFields.end(), instance.fields.begin(),
                   [](const MirClassFieldInfo &declared,
                      const MirClassFieldLifecycle &lifecycle) {
                     return declared.field == lifecycle.field &&
                            declared.symbol == lifecycle.symbol &&
                            declared.type == lifecycle.type &&
                            declared.dropKind == lifecycle.dropKind &&
                            declared.requiresActiveCleanup ==
                                lifecycle.requiresActiveCleanup;
                   });
    if (!validDeclaredFields) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class declared-field metadata is invalid or does not "
                      "match its lifecycle projection"});
    }
    std::unordered_set<HirBindingId> classFields;
    for (const MirClassFieldLifecycle &field : instance.fields) {
      const std::optional<bool> fieldRequiresActiveCleanup =
          typeRequiresActiveCleanup(field.type, typeRequiresActiveCleanup);
      if (field.field == 0 || field.symbol == 0 ||
          !classFields.insert(field.field).second ||
          field.type == SemanticType::Unknown ||
          field.dropKind != DropKind::Lexical || !fieldRequiresActiveCleanup ||
          *fieldRequiresActiveCleanup != field.requiresActiveCleanup) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "class field active-cleanup metadata does not match "
                        "its type"});
      }
    }
    std::vector<MirFieldDrop> exactFieldDropOrder;
    for (auto field = instance.fields.rbegin(); field != instance.fields.rend();
         ++field) {
      if (field->dropKind == DropKind::Lexical) {
        exactFieldDropOrder.push_back(
            {.field = field->field,
             .symbol = field->symbol,
             .type = field->type,
             .requiresActiveCleanup = field->requiresActiveCleanup});
      }
    }
    const bool exactFieldDropProjection =
        instance.fieldDropOrder.size() == exactFieldDropOrder.size() &&
        std::equal(
            instance.fieldDropOrder.begin(), instance.fieldDropOrder.end(),
            exactFieldDropOrder.begin(),
            [](const MirFieldDrop &actual, const MirFieldDrop &expected) {
              return actual.field == expected.field &&
                     actual.symbol == expected.symbol &&
                     actual.type == expected.type &&
                     actual.requiresActiveCleanup ==
                         expected.requiresActiveCleanup;
            });
    if (!exactFieldDropProjection) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class field-drop order is not the exact reverse "
                      "projection of its lifecycle fields"});
    }
    const bool structurallyRequiresActiveCleanup =
        instance.destructor.has_value() ||
        std::any_of(instance.structuralBases.begin(),
                    instance.structuralBases.end(),
                    [&](const HirBaseInstance &base) {
                      const MirClassInstance *baseInstance =
                          program.findClassInstance(base.instance);
                      return !base.interface && baseInstance != nullptr &&
                             baseInstance->requiresActiveCleanup;
                    }) ||
        std::any_of(instance.fields.begin(), instance.fields.end(),
                    [](const MirClassFieldLifecycle &field) {
                      return field.requiresActiveCleanup;
                    });
    if (instance.requiresActiveCleanup != structurallyRequiresActiveCleanup) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message =
               "class active-cleanup metadata does not match its structure"});
    }
    if (instance.destructor) {
      const MirDestructorInstance *destructor =
          program.findDestructorInstance(*instance.destructor);
      if (destructor == nullptr || destructor->owner != instance.id) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message =
                 "class cleanup descriptor names an invalid destructor"});
      }
    }
    if (instance.cAbiRecord) {
      if (instance.kind != ClassKind::Struct || !instance.bases.empty() ||
          instance.abstract || instance.polymorphic || !instance.cAbiLayout ||
          instance.destructor || instance.requiresActiveDropState ||
          instance.requiresActiveCleanup || !instance.fields.empty() ||
          !instance.fieldDropOrder.empty()) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "C ABI record class metadata is structurally invalid"});
      } else {
        const CAbiRecordLayout &layout = *instance.cAbiLayout;
        bool validLayout = layout.sizeBytes != 0 &&
                           layout.abiAlignmentBytes != 0 &&
                           layout.sizeBytes % layout.abiAlignmentBytes == 0 &&
                           !layout.fields.empty();
        std::uint64_t previousEnd = 0;
        for (const CAbiRecordFieldLayout &field : layout.fields) {
          const bool fieldEndValid =
              field.offsetBytes <=
              std::numeric_limits<std::uint64_t>::max() - field.sizeBytes;
          const std::uint64_t fieldEnd =
              fieldEndValid ? field.offsetBytes + field.sizeBytes : 0;
          validLayout = validLayout && field.declaration != nullptr &&
                        field.type != SemanticType::Unknown &&
                        field.sizeBytes != 0 && field.abiAlignmentBytes != 0 &&
                        field.offsetBytes % field.abiAlignmentBytes == 0 &&
                        field.offsetBytes >= previousEnd && fieldEndValid &&
                        fieldEnd <= layout.sizeBytes;
          previousEnd = fieldEnd;
        }
        if (!validLayout) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::FieldInitializers,
               .owner = instance.id,
               .message = "C ABI record layout metadata is invalid"});
        }
      }
    } else if (instance.cAbiLayout) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "ordinary class carries C ABI record layout metadata"});
    }
    if (instance.kind == ClassKind::Union) {
      bool validLayout =
          instance.unionLayout.has_value() && instance.bases.empty() &&
          !instance.abstract && !instance.polymorphic && !instance.cAbiRecord &&
          !instance.cAbiLayout && !instance.destructor &&
          !instance.requiresActiveDropState &&
          !instance.requiresActiveCleanup && instance.fields.empty() &&
          instance.fieldDropOrder.empty();
      if (instance.unionLayout) {
        const UnionLayout &layout = *instance.unionLayout;
        validLayout = validLayout && layout.sizeBytes != 0 &&
                      layout.abiAlignmentBytes != 0 &&
                      layout.sizeBytes % layout.abiAlignmentBytes == 0 &&
                      !layout.fields.empty() &&
                      layout.fields.size() == instance.declaredFields.size();
        for (const UnionFieldLayout &field : layout.fields) {
          validLayout = validLayout && field.declaration != nullptr &&
                        field.type != SemanticType::Unknown &&
                        field.sizeBytes != 0 && field.abiAlignmentBytes != 0 &&
                        field.sizeBytes <= layout.sizeBytes &&
                        layout.abiAlignmentBytes % field.abiAlignmentBytes == 0;
        }
      }
      if (!validLayout) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "union layout metadata is structurally invalid"});
      }
    } else if (instance.unionLayout) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "non-union class carries union layout metadata"});
    }
    verifyBody(instance.fieldInitializers, instance.id);
    verifyBody(instance.staticFieldInitializers, instance.id);
  }
  std::size_t entryPoints = 0;
  for (std::size_t index = 0; index < program.functionInstances().size();
       ++index) {
    const MirFunctionInstance &instance = program.functionInstances()[index];
    if (instance.id != index + 1) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message =
               "function instance identity does not match stored order"});
    }
    if (instance.linkage == LanguageLinkage::C &&
        instance.externalSymbol.empty()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "C-linkage function is missing its external symbol"});
    }
    if (instance.linkage == LanguageLinkage::Gti &&
        !instance.externalSymbol.empty()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "GTI-linkage function has an external C symbol"});
    }
    bool validCallableContracts = true;
    std::string callableContractError;
    const auto rejectCallableContract = [&](std::string message) {
      validCallableContracts = false;
      if (callableContractError.empty()) {
        callableContractError = std::move(message);
      }
    };
    std::size_t previousCallableParameter = 0;
    bool firstCallableParameter = true;
    for (const MirCallableParameter &parameter : instance.callableParameters) {
      const CallableInvocationCapability parameterCapability =
          callableInvocationCapability(parameter.access);
      if (parameter.parameterIndex >= instance.parameterTypes.size() ||
          parameter.callableType !=
              instance.parameterTypes[parameter.parameterIndex]) {
        rejectCallableContract("parameter identity or type is invalid");
      }
      if (parameter.boundary == CallableBoundary::Confined) {
        if (parameter.ownedTransport) {
          rejectCallableContract(
              "confined parameter carries owned transport metadata");
        }
      } else if (!parameter.ownedTransport ||
                 parameter.ownedTransport->destinationType ==
                     SemanticType::Unknown ||
                 !parameter.signatures.empty() ||
                 !parameter.forwardings.empty() ||
                 parameter.access != AccessMode::ReadOnly) {
        rejectCallableContract("owned parameter contract is invalid");
      }
      if (parameter.boundary == CallableBoundary::Owned) {
        if (const std::optional<std::string> failure =
                ownedTransportFailure(program, instance, parameter)) {
          rejectCallableContract(*failure);
        }
      }
      if (!firstCallableParameter &&
          previousCallableParameter >= parameter.parameterIndex) {
        rejectCallableContract("parameter descriptors are not ordered");
      }
      firstCallableParameter = false;
      previousCallableParameter = parameter.parameterIndex;

      for (std::size_t signatureIndex = 0;
           signatureIndex < parameter.signatures.size(); ++signatureIndex) {
        const MirCallableSignature &signature =
            parameter.signatures[signatureIndex];
        const bool hasTarget = signature.functionTarget.has_value() !=
                               signature.lambdaTarget.has_value();
        const std::string prefix =
            "signature " + std::to_string(signatureIndex) + " ";
        if (!hasTarget) {
          rejectCallableContract(prefix +
                                 "does not name exactly one selected target");
        }
        if (signature.requiredCapability !=
                CallableInvocationCapability::Once &&
            signature.requiredCapability != parameterCapability) {
          rejectCallableContract(
              prefix + "does not match the callable parameter access");
        }
        if (!signature.selectedCapability) {
          rejectCallableContract(prefix + "has no selected capability");
        } else if (!callableCapabilitySatisfies(*signature.selectedCapability,
                                                signature.requiredCapability)) {
          rejectCallableContract(prefix +
                                 "does not satisfy the required capability");
        }
        if (!hasTarget || !signature.selectedCapability) {
          continue;
        }

        CallableInvocationCapability expected =
            CallableInvocationCapability::Read;
        if (signature.functionTarget) {
          if (*signature.functionTarget == 0 ||
              *signature.functionTarget > program.functionInstances().size()) {
            rejectCallableContract(prefix + "has an invalid function target");
            continue;
          }
          const MirFunctionInstance &target =
              program.functionInstances().at(*signature.functionTarget - 1);
          if (target.overloadedOperator != OverloadedOperator::Call ||
              !target.owner || target.staticMember ||
              signature.returnType != target.returnType ||
              signature.parameterTypes != target.parameterTypes) {
            rejectCallableContract(prefix +
                                   "does not match an exact operator() target");
          }
          expected = callableInvocationCapability(target.receiverMutability);
        } else if (signature.lambdaTarget) {
          const MirLambdaInstance *target =
              program.findLambda(*signature.lambdaTarget);
          if (target == nullptr || target->returnType != signature.returnType ||
              target->parameterTypes != signature.parameterTypes ||
              target->type != parameter.callableType) {
            rejectCallableContract(prefix +
                                   "does not match an exact lambda target");
          }
        }
        if (*signature.selectedCapability != expected) {
          rejectCallableContract(
              prefix + "disagrees with the selected target capability");
        }
      }

      for (std::size_t forwardingIndex = 0;
           forwardingIndex < parameter.forwardings.size(); ++forwardingIndex) {
        const MirCallableForwarding &forwarding =
            parameter.forwardings[forwardingIndex];
        const std::string prefix =
            "forwarding " + std::to_string(forwardingIndex) + " ";
        const bool duplicate = std::any_of(
            parameter.forwardings.begin(),
            parameter.forwardings.begin() +
                static_cast<std::ptrdiff_t>(forwardingIndex),
            [&](const MirCallableForwarding &candidate) {
              return candidate.functionTarget == forwarding.functionTarget &&
                     candidate.parameterIndex == forwarding.parameterIndex;
            });
        if (duplicate) {
          rejectCallableContract(prefix + "is duplicated");
        }
        if (!forwarding.functionTarget || *forwarding.functionTarget == 0 ||
            *forwarding.functionTarget > program.functionInstances().size()) {
          rejectCallableContract(prefix + "has an invalid function target");
          continue;
        }

        const MirFunctionInstance &target =
            program.functionInstances().at(*forwarding.functionTarget - 1);
        const auto targetContract = std::find_if(
            target.callableParameters.begin(), target.callableParameters.end(),
            [&](const MirCallableParameter &candidate) {
              return candidate.parameterIndex == forwarding.parameterIndex;
            });
        if (forwarding.parameterIndex >= target.parameterTypes.size() ||
            targetContract == target.callableParameters.end() ||
            targetContract->boundary != CallableBoundary::Confined ||
            target.parameterTypes[forwarding.parameterIndex] !=
                parameter.callableType) {
          rejectCallableContract(prefix +
                                 "does not match a confined target parameter");
        }

        const std::optional<HirBindingId> sourceBinding =
            parameter.parameterIndex < instance.parameterBindings.size()
                ? std::optional<HirBindingId>{instance.parameterBindings
                                                  [parameter.parameterIndex]}
                : std::nullopt;
        const bool targetRequiresOnce = callableContractRequiresOnce(
            program, *forwarding.functionTarget, forwarding.parameterIndex);
        bool exactCallEdge = false;
        bool exactOnceMoveProof = true;
        if (sourceBinding) {
          for (const MirBlock &block : instance.body.blocks) {
            for (const MirInstruction &instruction : block.instructions) {
              if (instruction.kind != MirInstructionKind::Call ||
                  instruction.functionTarget != forwarding.functionTarget ||
                  forwarding.parameterIndex >= instruction.operands.size()) {
                continue;
              }
              const MirOperand &operand =
                  instruction.operands[forwarding.parameterIndex];
              const bool matchingEdge =
                  callableSourceBindingForOperand(instance.body, operand) ==
                      sourceBinding &&
                  std::any_of(instruction.callableArguments.begin(),
                              instruction.callableArguments.end(),
                              [&](const CallableArgumentBoundary &argument) {
                                return argument.parameterIndex ==
                                           forwarding.parameterIndex &&
                                       argument.boundary ==
                                           CallableBoundary::Confined;
                              });
              if (!matchingEdge) {
                continue;
              }
              exactCallEdge = true;
              exactOnceMoveProof =
                  exactOnceMoveProof &&
                  (!targetRequiresOnce ||
                   consumedCallableReceiver(instance.body, operand,
                                            MirValueUseKind::InstructionOperand,
                                            instruction.id,
                                            forwarding.parameterIndex));
            }
          }
        }
        if (!exactCallEdge) {
          rejectCallableContract(prefix +
                                 "does not match a concrete call edge");
        } else if (!exactOnceMoveProof) {
          rejectCallableContract(
              prefix + "into a once-confined target is not rooted in an "
                       "exact ownership move");
        }
      }
    }
    if (!validCallableContracts) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "function callable capability metadata is invalid: " +
                      callableContractError});
    }
    if (instance.entryKind != ProgramEntryKind::None) {
      ++entryPoints;
      if (instance.owner || instance.staticMember ||
          instance.linkage != LanguageLinkage::Gti) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Function,
             .owner = instance.id,
             .message = "program entry point must be a free GTI function"});
      }
    }
    if (instance.entryKind == ProgramEntryKind::None &&
        instance.entryArgumentAppendTarget) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "non-entry function has a program-argument adapter"});
    } else if (instance.entryKind == ProgramEntryKind::NoArguments &&
               (instance.returnType != SemanticType::Int32 ||
                !instance.parameterTypes.empty() ||
                instance.entryArgumentAppendTarget)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "no-argument entry point has invalid adapter metadata"});
    } else if (instance.entryKind == ProgramEntryKind::OwnedArguments) {
      const bool validEntryShape =
          instance.returnType == SemanticType::Int32 &&
          instance.parameterTypes.size() == 2 &&
          instance.parameterTypes[0] == SemanticType::Int32 &&
          instance.parameterTypes[1].kind == SemanticType::Class &&
          instance.parameterTypes[1].arguments.size() == 1 &&
          instance.parameterTypes[1].arguments.front().kind ==
              SemanticType::Class &&
          instance.entryArgumentAppendTarget &&
          *instance.entryArgumentAppendTarget != instance.id &&
          *instance.entryArgumentAppendTarget <=
              program.functionInstances().size();
      bool validAppendTarget = false;
      if (validEntryShape) {
        const MirFunctionInstance &append = program.functionInstances().at(
            *instance.entryArgumentAppendTarget - 1);
        validAppendTarget =
            append.owner && *append.owner <= program.classInstances().size() &&
            program.classInstances().at(*append.owner - 1).type ==
                instance.parameterTypes[1] &&
            append.returnType == SemanticType::Void &&
            append.parameterTypes.size() == 1 &&
            append.parameterTypes.front() ==
                instance.parameterTypes[1].arguments.front() &&
            !append.staticMember &&
            append.entryKind == ProgramEntryKind::None &&
            append.linkage == LanguageLinkage::Gti;
      }
      if (!validEntryShape || !validAppendTarget) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Function,
             .owner = instance.id,
             .message = "owned-argument entry point has invalid adapter "
                        "metadata"});
      }
    }
    verifyBody(instance.body, instance.id);
  }
  if (entryPoints > 1) {
    result.errors.push_back(
        {.bodyKind = MirBodyKind::Module,
         .message = "MIR program contains multiple entry points"});
  }
  for (std::size_t index = 0; index < program.constructorInstances().size();
       ++index) {
    const MirConstructorInstance &instance =
        program.constructorInstances()[index];
    if (instance.id != index + 1 || instance.owner == 0 ||
        instance.owner > program.classInstances().size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor instance identity or owner is invalid"});
    }
    for (const MirConstructorInitializer &initializer : instance.initializers) {
      if (!initializer.ownedParameter) {
        continue;
      }
      const std::size_t parameter = *initializer.ownedParameter;
      const MirClassInstance *owner = program.findClassInstance(instance.owner);
      const bool exactField =
          owner != nullptr &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          initializer.field != 0 && initializer.arguments.size() == 1 &&
          parameter < instance.parameterTypes.size() &&
          parameter < instance.parameterBindings.size() &&
          initializer.targetType == instance.parameterTypes[parameter] &&
          std::any_of(
              owner->declaredFields.begin(), owner->declaredFields.end(),
              [&](const MirClassFieldInfo &field) {
                return field.symbol == initializer.field &&
                       field.type == initializer.targetType;
              });
      const HirValueId ownedSource =
          exactField ? initializer.arguments.front() : 0;
      const auto value = std::find_if(
          instance.body.values.begin(), instance.body.values.end(),
          [&](const MirValue &candidate) {
            return ownedSource != 0 && candidate.sourceValue == ownedSource;
          });
      if (!exactField || value == instance.body.values.end() ||
          std::count_if(instance.body.values.begin(),
                        instance.body.values.end(),
                        [&](const MirValue &item) {
                          return item.sourceValue == ownedSource;
                        }) != 1 ||
          !exactMoveFromBinding(instance.body,
                                MirOperand{.kind = MirOperandKind::Value,
                                           .value = value->id,
                                           .type = value->info.type},
                                instance.parameterBindings[parameter]) ||
          !valueTransferredOut(instance.body,
                               MirOperand{.kind = MirOperandKind::Value,
                                          .value = value->id,
                                          .type = value->info.type},
                               true)) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Constructor,
             .owner = instance.id,
             .message = "owned field initializer is not rooted in the exact "
                        "constructor parameter move"});
      }
    }
    verifyBody(instance.body, instance.id);
  }
  for (std::size_t index = 0; index < program.destructorInstances().size();
       ++index) {
    const MirDestructorInstance &instance =
        program.destructorInstances()[index];
    if (instance.id != index + 1 || instance.owner == 0 ||
        instance.owner > program.classInstances().size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "destructor instance identity or owner is invalid"});
    }
    verifyBody(instance.body, instance.id);
  }
  for (std::size_t index = 0; index < program.lambdaInstances().size();
       ++index) {
    const MirLambdaInstance &instance = program.lambdaInstances()[index];
    if (instance.id != index + 1 || instance.declaration == 0) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Lambda,
           .owner = instance.id,
           .message = "lambda instance identity or declaration is invalid"});
    }
    const std::span<const SemanticType> exactParameters =
        instance.type.lambdaParameterTypes();
    const std::span<const SemanticType> exactCaptures =
        instance.type.lambdaCaptureTypes();
    const SemanticType *exactReturn = instance.type.lambdaReturnType();
    if (instance.type.kind != SemanticType::Lambda ||
        instance.type.lambdaId != instance.declaration ||
        !instance.type.hasLambdaShape() || exactReturn == nullptr ||
        *exactReturn != instance.returnType ||
        exactParameters.size() != instance.parameterTypes.size() ||
        !std::equal(exactParameters.begin(), exactParameters.end(),
                    instance.parameterTypes.begin()) ||
        exactCaptures.size() != instance.captureTypes.size() ||
        !std::equal(exactCaptures.begin(), exactCaptures.end(),
                    instance.captureTypes.begin())) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Lambda,
           .owner = instance.id,
           .message = "lambda instance type does not match its declaration, "
                      "signature, or captures"});
    }
    if (instance.captureTypes.size() != instance.captureModes.size() ||
        instance.captureTypes.size() != instance.captureSymbols.size() ||
        instance.captureTypes.size() !=
            instance.captureRequiresActiveCleanup.size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Lambda,
           .owner = instance.id,
           .message = "lambda capture ownership metadata has the wrong size"});
    } else {
      std::unordered_set<SymbolId> exactCaptureSymbols;
      for (std::size_t capture = 0; capture < instance.captureTypes.size();
           ++capture) {
        if ((instance.captureModes[capture] != LambdaCaptureMode::Copy &&
             instance.captureModes[capture] != LambdaCaptureMode::Move) ||
            instance.captureSymbols[capture] == 0 ||
            !exactCaptureSymbols.insert(instance.captureSymbols[capture])
                 .second) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Lambda,
               .owner = instance.id,
               .message = "lambda capture mode or symbol identity is "
                          "invalid or duplicated"});
        }
        const std::optional<bool> exactActiveCleanup =
            typeRequiresActiveCleanup(instance.captureTypes[capture],
                                      typeRequiresActiveCleanup);
        if (!exactActiveCleanup ||
            *exactActiveCleanup !=
                instance.captureRequiresActiveCleanup[capture]) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Lambda,
               .owner = instance.id,
               .message =
                   "lambda capture cleanup metadata does not match its type"});
        }
      }
      for (const MirPlace &place : instance.body.places) {
        const auto capture =
            std::find(instance.captureSymbols.begin(),
                      instance.captureSymbols.end(), place.symbol);
        const std::size_t exactCapture =
            capture == instance.captureSymbols.end()
                ? 0
                : static_cast<std::size_t>(
                      std::distance(instance.captureSymbols.begin(), capture)) +
                      1;
        if (place.capture != exactCapture ||
            (exactCapture != 0 &&
             (place.root != MirPlaceRootKind::Symbol ||
              place.type != instance.captureTypes[exactCapture - 1] ||
              place.access != AccessMode::ReadOnly))) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Lambda,
               .owner = instance.id,
               .message = "lambda capture place does not match its exact "
                          "environment field"});
        }
      }
    }
    verifyBody(instance.body, instance.id);
  }
  const auto exactActiveCleanup = [&](const SemanticType &type) {
    return typeRequiresActiveCleanup(type, typeRequiresActiveCleanup);
  };
  append(result, verifyMirProgramBorrowContracts(program));
  append(result, verifyMirDropTargets(program, program.module(), 0,
                                      exactActiveCleanup));
  for (const MirClassInstance &instance : program.classInstances()) {
    append(result, verifyMirDropTargets(program, instance.fieldInitializers,
                                        instance.id, exactActiveCleanup));
    append(result,
           verifyMirDropTargets(program, instance.staticFieldInitializers,
                                instance.id, exactActiveCleanup));
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    append(result, verifyMirDropTargets(program, instance.body, instance.id,
                                        exactActiveCleanup));
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    append(result, verifyMirDropTargets(program, instance.body, instance.id,
                                        exactActiveCleanup));
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    append(result, verifyMirDropTargets(program, instance.body, instance.id,
                                        exactActiveCleanup));
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    append(result, verifyMirDropTargets(program, instance.body, instance.id,
                                        exactActiveCleanup));
  }
  return result;
}

} // namespace lang
