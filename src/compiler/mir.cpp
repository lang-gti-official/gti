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
  std::size_t result = 0;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(*this)) {
    if (const MirBody *body = findMirBody(*this, address)) {
      result += body->blocks.size();
    }
  }
  return result;
}

std::vector<MirBodyAddress>
enumerateMirBodyAddresses(const MirProgram &program) {
  std::vector<MirBodyAddress> result;
  result.reserve(1 + program.classInstances().size() * 2 +
                 program.functionInstances().size() +
                 program.constructorInstances().size() +
                 program.destructorInstances().size() +
                 program.lambdaInstances().size() +
                 (program.hostedStartup() == nullptr ? 0 : 1));
  result.push_back({.kind = MirBodyKind::Module});
  for (const MirClassInstance &instance : program.classInstances()) {
    result.push_back(
        {.kind = MirBodyKind::FieldInitializers, .owner = instance.id});
    result.push_back(
        {.kind = MirBodyKind::StaticFieldInitializers, .owner = instance.id});
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    result.push_back({.kind = MirBodyKind::Function, .owner = instance.id});
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    result.push_back({.kind = MirBodyKind::Constructor, .owner = instance.id});
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    result.push_back({.kind = MirBodyKind::Destructor, .owner = instance.id});
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    result.push_back({.kind = MirBodyKind::Lambda, .owner = instance.id});
  }
  if (const std::optional<MirHostedStartupPlan> &hosted =
          program.hostedStartupPlan();
      hosted && program.hostedStartup() != nullptr) {
    result.push_back(
        {.kind = MirBodyKind::HostedStartup, .owner = hosted->entry});
  }
  return result;
}

const MirBody *findMirBody(const MirProgram &program, MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Module:
    return address.owner == 0 ? &program.module() : nullptr;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    if (const MirClassInstance *instance =
            program.findClassInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return address.kind == MirBodyKind::FieldInitializers
                 ? &instance->fieldInitializers
                 : &instance->staticFieldInitializers;
    }
    return nullptr;
  case MirBodyKind::Function:
    if (const MirFunctionInstance *instance =
            program.findFunctionInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Constructor:
    if (const MirConstructorInstance *instance =
            program.findConstructorInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Destructor:
    if (const MirDestructorInstance *instance =
            program.findDestructorInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Lambda:
    if (const MirLambdaInstance *instance = program.findLambda(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::HostedStartup:
    return address.owner != 0 && program.hostedStartupPlan() &&
                   program.hostedStartupPlan()->entry == address.owner
               ? program.hostedStartup()
               : nullptr;
  }
  return nullptr;
}

MirBody *findMirBody(MirProgram &program, MirBodyAddress address) {
  return const_cast<MirBody *>(findMirBody(std::as_const(program), address));
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
    case MirTerminatorKind::Invoke:
      enqueue(terminator.target);
      enqueue(terminator.elseTarget);
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

[[nodiscard]] std::optional<Literal>
programConstantLiteral(const ConstantValue &constant) {
  if (const auto *integer = std::get_if<ConstantInteger>(&constant)) {
    return Literal{integer->magnitude};
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&constant)) {
    return Literal{*floating};
  }
  if (const auto *character = std::get_if<CharacterLiteral>(&constant)) {
    return Literal{*character};
  }
  if (const auto *string = std::get_if<std::string>(&constant)) {
    return Literal{*string};
  }
  if (const auto *boolean = std::get_if<bool>(&constant)) {
    return Literal{*boolean};
  }
  if (std::holds_alternative<NullConstant>(constant)) {
    return Literal{nullptr};
  }
  return std::nullopt;
}

[[nodiscard]] bool programConstantMatchesType(const ConstantValue &constant,
                                              const SemanticType &type) {
  if (const auto *integer = std::get_if<ConstantInteger>(&constant)) {
    if (type.kind == SemanticType::Enum) {
      return type.enumId != 0;
    }
    const std::optional<CheckedIntegerDomain> domain =
        constantIntegerDomain(type);
    return domain && *domain == integer->domain;
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&constant)) {
    return semanticFloatFormat(type) == floating->format;
  }
  if (std::holds_alternative<CharacterLiteral>(constant)) {
    return type == SemanticType::Char;
  }
  if (std::holds_alternative<std::string>(constant)) {
    return type == SemanticType::StringView;
  }
  if (std::holds_alternative<bool>(constant)) {
    return type == SemanticType::Bool;
  }
  if (std::holds_alternative<NullConstant>(constant)) {
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

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId valueId,
                     std::size_t depth);

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
    if (place->root == MirPlaceRootKind::Value) {
      const std::optional<MirPlaceId> source =
          borrowSourceForValue(body, place->value, depth + 1);
      if (source && *source != id) {
        MirCanonicalPlace result = self(self, *source, depth + 1);
        result.projections.insert(result.projections.end(),
                                  place->projections.begin(),
                                  place->projections.end());
        return result;
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
    if ((lhs.kind == MirProjectionKind::Index ||
         lhs.kind == MirProjectionKind::PackElement) &&
        lhs.kind == rhs.kind && lhs.constantIndex && rhs.constantIndex &&
        lhs.constantIndex != rhs.constantIndex) {
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
         outer.kind == MirProjectionKind::RawIndex ||
         outer.kind == MirProjectionKind::PackElement) &&
        (outer.kind == MirProjectionKind::RawIndex ||
         (outer.kind != MirProjectionKind::PackElement &&
          !outer.constantIndex && outer.selection == 0))) {
      if (outer.index == 0 || outer.index != inner.index) {
        return false;
      }
    }
    if (outer.kind == MirProjectionKind::Index ||
        outer.kind == MirProjectionKind::PackElement) {
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
    case MirProjectionKind::PackElement:
      if (!projection.constantIndex) {
        return std::nullopt;
      }
      result.projections.push_back({.kind = PlaceProjectionKind::ConstantIndex,
                                    .index = *projection.constantIndex});
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
  case MirTerminatorKind::Invoke:
    append(terminator.target);
    append(terminator.elseTarget);
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
  return result;
}

[[nodiscard]] bool tracksScalarInitialization(const MirPlace &place) {
  if ((place.root != MirPlaceRootKind::Binding &&
       place.root != MirPlaceRootKind::Temporary) ||
      !place.projections.empty() || place.traits.drop != DropKind::Trivial ||
      place.traits.containsBorrowedState ||
      (place.key && (place.key->receiver || !place.key->projections.empty()))) {
    return false;
  }
  switch (place.type.kind) {
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
  case SemanticType::Bool:
  case SemanticType::Char:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] bool sameScalarInitializationStorage(const MirPlace &left,
                                                   const MirPlace &right) {
  if (left.root != right.root) {
    return false;
  }
  if (left.root == MirPlaceRootKind::Temporary) {
    return left.temporary != 0 && left.temporary == right.temporary;
  }
  if (left.root != MirPlaceRootKind::Binding) {
    return false;
  }
  if (left.key && right.key) {
    return *left.key == *right.key;
  }
  return left.binding != 0 && left.binding == right.binding;
}

[[nodiscard]] MirVerificationResult
verifyMirScalarInitializationFlow(const MirBody &body, std::size_t owner) {
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return failure(body, owner,
                   "scalar initialization flow has an invalid entry block");
  }
  constexpr std::size_t noStorage = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> storageForPlace(body.places.size(), noStorage);
  std::vector<MirPlaceId> representatives;
  std::vector<bool> initial;
  for (const MirPlace &place : body.places) {
    if (place.id == 0 || place.id > body.places.size() ||
        !tracksScalarInitialization(place)) {
      continue;
    }
    const auto existing =
        std::find_if(representatives.begin(), representatives.end(),
                     [&](MirPlaceId representative) {
                       return sameScalarInitializationStorage(
                           place, body.places[representative - 1]);
                     });
    const std::size_t storage = existing == representatives.end()
                                    ? representatives.size()
                                    : static_cast<std::size_t>(std::distance(
                                          representatives.begin(), existing));
    if (existing == representatives.end()) {
      representatives.push_back(place.id);
      initial.push_back(false);
    }
    storageForPlace[place.id - 1] = storage;
    initial[storage] = initial[storage] || place.initiallyAvailable;
  }

  std::vector<std::optional<std::vector<bool>>> entryStates(body.blocks.size());
  entryStates[body.entry - 1] = std::move(initial);
  std::queue<MirBlockId> pending;
  pending.push(body.entry);

  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock *block = body.findBlock(blockId);
    if (block == nullptr || !entryStates[blockId - 1]) {
      continue;
    }
    std::vector<bool> available = *entryStates[blockId - 1];
    for (const MirInstruction &instruction : block->instructions) {
      const auto requireAvailable = [&](const MirOperand &operand)
          -> std::optional<MirVerificationResult> {
        if ((operand.kind != MirOperandKind::Copy &&
             operand.kind != MirOperandKind::Move) ||
            operand.place == 0 || operand.place > storageForPlace.size()) {
          return std::nullopt;
        }
        const std::size_t storage = storageForPlace[operand.place - 1];
        if (storage == noStorage || available[storage]) {
          return std::nullopt;
        }
        return failure(body, owner,
                       "place access requires a definitely initialized "
                       "scalar place",
                       block->id, instruction.id);
      };
      if (instruction.receiver) {
        if (std::optional<MirVerificationResult> invalid =
                requireAvailable(*instruction.receiver)) {
          return std::move(*invalid);
        }
      }
      for (const MirOperand &operand : instruction.operands) {
        if (std::optional<MirVerificationResult> invalid =
                requireAvailable(operand)) {
          return std::move(*invalid);
        }
      }
      if (instruction.destination &&
          (instruction.kind == MirInstructionKind::Initialize ||
           instruction.kind == MirInstructionKind::Assign) &&
          *instruction.destination != 0 &&
          *instruction.destination <= storageForPlace.size() &&
          storageForPlace[*instruction.destination - 1] != noStorage) {
        const std::size_t storage =
            storageForPlace[*instruction.destination - 1];
        if (instruction.kind == MirInstructionKind::Initialize &&
            instruction.operands.empty()) {
          const MirPlace *destination =
              body.findPlace(*instruction.destination);
          if (body.kind == MirBodyKind::Module &&
              block->programInitializationStep != 0 && destination != nullptr &&
              destination->root == MirPlaceRootKind::Binding &&
              destination->projections.empty() &&
              !destination->initiallyAvailable) {
            available[storage] = true;
          }
          continue;
        }
        const bool initialized = available[storage];
        if (instruction.kind == MirInstructionKind::Assign && !initialized) {
          return failure(body, owner,
                         "assignment requires a definitely initialized "
                         "scalar destination",
                         block->id, instruction.id);
        }
        available[storage] = true;
      }
    }

    if (block->terminator.value) {
      const MirOperand &operand = *block->terminator.value;
      if ((operand.kind == MirOperandKind::Copy ||
           operand.kind == MirOperandKind::Move) &&
          operand.place != 0 && operand.place <= storageForPlace.size() &&
          storageForPlace[operand.place - 1] != noStorage &&
          !available[storageForPlace[operand.place - 1]]) {
        return failure(body, owner,
                       "terminator place access requires a definitely "
                       "initialized scalar place",
                       block->id);
      }
    }

    for (const MirBlockId successor : ownershipSuccessors(block->terminator)) {
      if (successor == 0 || successor > body.blocks.size()) {
        continue;
      }
      std::optional<std::vector<bool>> &incoming = entryStates[successor - 1];
      if (!incoming) {
        incoming = available;
        pending.push(successor);
        continue;
      }
      std::vector<bool> merged = *incoming;
      for (std::size_t index = 0; index < merged.size(); ++index) {
        merged[index] = merged[index] && available[index];
      }
      if (merged != *incoming) {
        incoming = std::move(merged);
        pending.push(successor);
      }
    }
  }
  return {};
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
  case MirTerminatorKind::Invoke:
    return {terminator.target, terminator.elseTarget};
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
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
        block.terminator.kind == MirTerminatorKind::PropagateFailure ||
        block.terminator.kind == MirTerminatorKind::ContainFailure ||
        block.terminator.kind == MirTerminatorKind::Exit) {
      for (const MirLoan &loan : body.loans) {
        if (active[loan.id - 1] == MirLoanFlowState::Inactive) {
          continue;
        }
        if (block.terminator.kind == MirTerminatorKind::PropagateFailure) {
          return failure(body, owner,
                         "loan " + std::to_string(loan.id) +
                             " remains active at a failure exit",
                         block.id);
        }
        if (!loan.escapes) {
          return failure(body, owner,
                         "non-escaping loan " + std::to_string(loan.id) +
                             " remains active at a body exit",
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
  const auto applyInstructions =
      [&](const MirBlock &block, MirLifecycleState &state,
          bool validate) -> std::optional<MirVerificationResult> {
    const auto require = [&](MirDropObligationId obligation,
                             OwnershipState required, const char *operation,
                             MirInstructionId instruction)
        -> std::optional<MirVerificationResult> {
      if (validate && !state[obligation - 1].definitely(required)) {
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
          if (validate && event.target != 0 &&
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
          if (validate && before.definitely(OwnershipState::Uninitialized)) {
            return failure(body, owner, "drop targets an inactive obligation",
                           block.id, instruction.id);
          }
          const bool requiresConditional =
              before.contains(OwnershipState::Uninitialized);
          if (validate && event.conditional != requiresConditional) {
            return failure(body, owner,
                           "drop conditionality disagrees with path state",
                           block.id, instruction.id);
          }
          state[event.source - 1] = OwnershipStateSet::Uninitialized;
          break;
        }
        }
      }
      if (validate && instruction.fullExpressionEnd != 0) {
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
      if (validate && instruction.cleanupBoundaryEnd != 0) {
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
    return std::nullopt;
  };

  const auto applyInvokeSuccess =
      [&](const MirBlock &block, MirLifecycleState &state,
          bool validate) -> std::optional<MirVerificationResult> {
    for (const MirLifecycleEvent &event : block.terminator.successLifecycle) {
      if (validate && (event.kind != MirLifecycleEventKind::Initialize ||
                       event.target == 0 || event.target > state.size() ||
                       !state[event.target - 1].definitely(
                           OwnershipState::Uninitialized))) {
        return failure(body, owner,
                       "invoke success initialization requires an exact "
                       "uninitialized obligation",
                       block.id, block.terminator.invokeInstruction);
      }
      if (event.target != 0 && event.target <= state.size()) {
        state[event.target - 1] = OwnershipStateSet::Available;
      }
    }
    return std::nullopt;
  };

  // First converge every block entry. Validating while the worklist is still
  // incomplete is order-dependent: a conditional drop may initially see only
  // the inactive predecessor and reject before the active predecessor joins.
  std::queue<MirBlockId> pending;
  pending.push(body.entry);
  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock &block = body.blocks[blockId - 1];
    MirLifecycleState state = *entries[blockId - 1];
    (void)applyInstructions(block, state, false);

    for (const MirBlockId successor : successors(block.terminator)) {
      MirLifecycleState outgoing = state;
      if (block.terminator.kind == MirTerminatorKind::Invoke &&
          successor == block.terminator.target) {
        (void)applyInvokeSuccess(block, outgoing, false);
      }
      std::optional<MirLifecycleState> &entry = entries[successor - 1];
      const MirLifecycleState merged =
          entry ? joinLifecycleState(*entry, outgoing) : outgoing;
      if (!entry || *entry != merged) {
        entry = merged;
        pending.push(successor);
      }
    }
  }

  // Then validate each reachable block against its final joined entry state.
  for (const MirBlock &block : body.blocks) {
    if (!entries[block.id - 1]) {
      continue;
    }
    MirLifecycleState state = *entries[block.id - 1];
    if (auto invalid = applyInstructions(block, state, true)) {
      return *invalid;
    }

    if (block.terminator.kind == MirTerminatorKind::Return ||
        block.terminator.kind == MirTerminatorKind::PropagateFailure ||
        block.terminator.kind == MirTerminatorKind::ContainFailure ||
        block.terminator.kind == MirTerminatorKind::Exit) {
      const auto live = std::find_if(
          state.begin(), state.end(), [](OwnershipStateSet candidate) {
            return !candidate.definitely(OwnershipState::Uninitialized);
          });
      if (live != state.end()) {
        return failure(
            body, owner,
            "body exit retains active drop obligation " +
                std::to_string(std::distance(state.begin(), live) + 1),
            block.id);
      }
    }

    if (block.terminator.kind == MirTerminatorKind::Invoke) {
      MirLifecycleState success = state;
      if (auto invalid = applyInvokeSuccess(block, success, true)) {
        return *invalid;
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
  if (instruction.localFailureSites.size() !=
          instruction.definedFailure.localOrigins.size() ||
      std::any_of(instruction.localFailureSites.begin(),
                  instruction.localFailureSites.end(),
                  [](FailureSiteId site) { return site == 0; })) {
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
  case FailurePropagationKind::BodyCall:
    return instruction.kind == MirInstructionKind::CallBody &&
           instruction.bodyTarget.has_value();
  case FailurePropagationKind::Destructor:
    return instruction.kind == MirInstructionKind::Drop;
  case FailurePropagationKind::Count:
    return false;
  }
  return false;
}

} // namespace

bool supportsMirFailureControlFlow(MirBodyKind kind) {
  // Constructor bodies are admitted now that partial-construction rollback
  // is represented: each completed subobject transferred into `this` arms a
  // ConstructionRollback obligation, every defined-failure edge drains the
  // armed set in reverse stage order, and normal completion retires it by
  // transfer to the caller. Field/static initializer bodies remain excluded
  // until their construction schedules are staged the same way.
  return kind == MirBodyKind::Module || kind == MirBodyKind::Function ||
         kind == MirBodyKind::Constructor ||
         kind == MirBodyKind::FieldInitializers ||
         kind == MirBodyKind::Destructor || kind == MirBodyKind::Lambda ||
         kind == MirBodyKind::HostedStartup;
}

bool mirBodyRoutesFailureEdges(const MirBody &body) {
  if (!supportsMirFailureControlFlow(body.kind)) {
    return false;
  }
  if (body.kind != MirBodyKind::Constructor &&
      body.kind != MirBodyKind::FieldInitializers) {
    return true;
  }
  // A constructor that silently transfers any subobject into `this` without
  // arming rollback routes no defined-failure edges at all: an edge anywhere
  // in the body could not drain that subobject, so the whole body stays on
  // the compatibility failure authority instead of leaking through verified
  // MIR. Rollback retirement transfers are exempt; they end obligations the
  // failure edges already drained or the caller now owns.
  // A TransferOut is unarmed when its source is not a rollback obligation
  // and its instruction carries no ownership continuation: the rollback
  // retirement (rollback-kind source) and the typed-transfer form (a paired
  // event that targets a live local obligation) are exempt.
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      bool continuesOwnership = false;
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        continuesOwnership = continuesOwnership || event.target != 0;
      }
      if (continuesOwnership) {
        continue;
      }
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.kind != MirLifecycleEventKind::TransferOut) {
          continue;
        }
        const MirDropObligation *source = body.findDropObligation(event.source);
        if (source != nullptr &&
            source->kind != MirDropObligationKind::ConstructionRollback) {
          return false;
        }
      }
    }
  }
  return true;
}

bool requiresMirFailureControlFlow(const MirInstruction &instruction,
                                   MirFailureControlFlowPosition position) {
  if (instruction.kind == MirInstructionKind::Drop &&
      instruction.definedFailure.localOrigins.empty() &&
      instruction.definedFailure.propagation ==
          FailurePropagationKind::Destructor &&
      instruction.destination && instruction.lifecycle.size() == 1 &&
      instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
      instruction.lifecycle.front().source != 0 &&
      instruction.lifecycle.front().target == 0) {
    return true;
  }
  // The eligible instruction shapes are position-independent: a checked
  // scalar computation, load, or eligible ordinary call carries the same
  // Invoke/record/cleanup contract whether it is a full-expression root, a
  // nested call argument after prepared owners, or any other nested value
  // position. Failure routing reads the live temporary/scope state at the
  // instruction, so the position parameter no longer restricts eligibility;
  // it remains the verifier's identity for the staged-owner cleanup contract.
  (void)position;
  // A state-preserving read leaves nothing for a failure edge to unwind, so it
  // does not disqualify an otherwise eligible operation. Any event that moves,
  // reinitializes, or otherwise transitions ownership still does, because the
  // failure edge would have to restore the prior state.
  const bool statePreservingOwnership =
      !instruction.ownership ||
      (instruction.ownership->kind == OwnershipEventKind::Read &&
       instruction.ownership->before == OwnershipStateSet::Available &&
       instruction.ownership->after == OwnershipStateSet::Available &&
       instruction.ownership->reachable);
  // A checked assignment to a trivially destroyed place writes nothing when it
  // fails and runs no lifecycle event, so its failure edge needs only the
  // ordinary temporary and scope cleanup. This covers the narrowing compound
  // forms that keep a closed instruction because semantics folds their
  // arithmetic and conversion into one origin. An assignment that replaces an
  // owning value still needs the destination's own unwinding rule.
  const bool trivialCommitDestination =
      instruction.kind == MirInstructionKind::Assign &&
      instruction.lifecycle.empty() &&
      instruction.info.traits.drop == DropKind::Trivial &&
      !instruction.info.traits.containsBorrowedState;
  const bool borrowedCallResult =
      instruction.kind == MirInstructionKind::Call && instruction.loan &&
      instruction.borrowOrigin != BorrowOriginKind::None &&
      !instruction.successResultDrop &&
      instruction.definedFailure.propagation != FailurePropagationKind::None;
  const bool directInitializationResult =
      (instruction.kind == MirInstructionKind::Call ||
       instruction.kind == MirInstructionKind::Construct) &&
      instruction.successResultDestination && instruction.result &&
      !instruction.destination && !instruction.loan &&
      !instruction.successResultDrop &&
      instruction.info.traits.drop == DropKind::Lexical;
  if (instruction.definedFailure.empty() ||
      (instruction.destination && !trivialCommitDestination) ||
      (instruction.loan && !borrowedCallResult) || !statePreservingOwnership ||
      instruction.info.type.kind == SemanticType::Reference) {
    return false;
  }
  if (trivialCommitDestination) {
    return true;
  }
  if (instruction.kind == MirInstructionKind::Compute ||
      instruction.kind == MirInstructionKind::Load) {
    return instruction.info.traits.drop == DropKind::Trivial &&
           instruction.lifecycle.empty() && !instruction.successResultDrop;
  }
  // A construction is an ordered invocation with the same failure shape as an
  // ordinary call: it may raise before producing anything, and its
  // cleanup-owning result must be initialized only on the success edge.
  if ((instruction.kind != MirInstructionKind::Call &&
       instruction.kind != MirInstructionKind::Construct) ||
      std::any_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                  [](const MirLifecycleEvent &event) {
                    return event.kind != MirLifecycleEventKind::TransferOut;
                  })) {
    return false;
  }
  if (borrowedCallResult) {
    return true;
  }
  if (directInitializationResult) {
    return true;
  }
  if (instruction.successResultDrop) {
    return instruction.info.traits.drop == DropKind::Lexical;
  }
  // Expected<scalar, E> crosses the transformed failure boundary through a
  // value out-parameter. A direct `return callee();` therefore has no
  // semantic result drop to arm: the callee publishes only on its successful
  // Invoke edge and the caller immediately transfers that value to its own
  // result. Keep this aligned with the backend boundary, which deliberately
  // admits only scalar/void Expected payloads here; Expected<Class, E> uses
  // the explicit placement-result contract below instead.
  const auto passiveExpectedPayload = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Void:
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::Bool:
    case SemanticType::Char:
      return true;
    default:
      return false;
    }
  };
  if (position == MirFailureControlFlowPosition::FullExpressionRoot &&
      instruction.kind == MirInstructionKind::Call && instruction.result &&
      instruction.info.type.kind == SemanticType::Expected &&
      instruction.info.type.arguments.size() == 2 &&
      passiveExpectedPayload(instruction.info.type.arguments.front()) &&
      instruction.info.traits.drop == DropKind::Lexical) {
    return true;
  }
  // A class-valued call used as the full-expression of a Return has no MIR
  // drop identity: on success its value is transferred directly to the
  // caller. It still needs an Invoke so failure can unwind live parameters
  // and scopes before propagation. Other discarded or staged owning results
  // carry either a successResultDrop or a destination, so this exact shape
  // does not manufacture storage for them.
  if (position == MirFailureControlFlowPosition::FullExpressionRoot &&
      instruction.kind == MirInstructionKind::Call && instruction.result &&
      instruction.info.type.kind == SemanticType::Class &&
      instruction.info.traits.drop == DropKind::Lexical) {
    return true;
  }
  return instruction.info.traits.drop == DropKind::Trivial;
}

namespace {

[[nodiscard]] MirVerificationResult
verifyMirHostedStartupBodyStructure(const MirBody &body, std::size_t owner) {
  if (owner == 0 || body.entry == 0 || body.entry > body.blocks.size() ||
      body.returnType != SemanticType::Int32 ||
      body.placeDomain.snapshot == 0 || body.placeDomain.body == 0) {
    return failure(
        body, owner,
        "hosted-startup body identity or return contract is invalid");
  }
  if (!body.loans.empty() || !body.fullExpressions.empty() ||
      !body.programConstantSubstitutions.empty()) {
    return failure(body, owner,
                   "hosted-startup body retains source-only tables");
  }

  const auto validValue = [&](MirValueId id) {
    return id != 0 && id <= body.values.size();
  };
  const auto validPlace = [&](MirPlaceId id) {
    return id != 0 && id <= body.places.size();
  };
  const auto validDrop = [&](MirDropObligationId id) {
    return id != 0 && id <= body.dropObligations.size();
  };
  const auto validFailureRecord = [&](MirFailureRecordId id) {
    return id != 0 && id <= body.failureRecords.size();
  };
  const auto validOperand = [&](const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value:
      return validValue(operand.value) && operand.place == 0 &&
             operand.loan == 0 && !operand.literal &&
             body.values[operand.value - 1].info.type == operand.type;
    case MirOperandKind::Constant:
      return operand.value == 0 && operand.place == 0 && operand.loan == 0 &&
             operand.literal &&
             literalMatchesType(*operand.literal, operand.type);
    case MirOperandKind::Address:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      return operand.value == 0 && validPlace(operand.place) &&
             operand.loan == 0 && !operand.literal &&
             body.places[operand.place - 1].type == operand.type;
    case MirOperandKind::Loan:
      return false;
    }
    return false;
  };

  for (std::size_t index = 0; index < body.values.size(); ++index) {
    const MirValue &value = body.values[index];
    if (value.id != index + 1 || value.hostedStartupOperation == 0 ||
        value.sourceValue != 0 || value.info.type == SemanticType::Unknown ||
        value.info.traits != semanticTraits(value.info.type) ||
        value.definitionBlock == 0 ||
        value.definitionBlock > body.blocks.size() || value.definition == 0) {
      return failure(body, owner,
                     "hosted-startup value has invalid generated provenance");
    }
  }

  std::unordered_set<MirTemporaryId> temporaries;
  for (std::size_t index = 0; index < body.places.size(); ++index) {
    const MirPlace &place = body.places[index];
    const bool temporary = place.root == MirPlaceRootKind::Temporary;
    const bool value = place.root == MirPlaceRootKind::Value;
    if (place.id != index + 1 || place.hostedStartupOperation == 0 ||
        (!temporary && !value) || place.binding != 0 || place.symbol != 0 ||
        place.capture != 0 || place.loan != 0 ||
        (temporary != (place.temporary != 0)) ||
        (value != (place.value != 0)) ||
        (temporary && !temporaries.insert(place.temporary).second) ||
        (value && !validValue(place.value)) || !place.projections.empty() ||
        place.type == SemanticType::Unknown ||
        place.traits != semanticTraits(place.type) || place.sourceValue != 0 ||
        place.key || place.initiallyAvailable) {
      return failure(body, owner,
                     "hosted-startup place has invalid generated provenance");
    }
    if (value && body.values[place.value - 1].info.type != place.type) {
      return failure(body, owner,
                     "hosted-startup value place has a mismatched type");
    }
  }

  for (std::size_t index = 0; index < body.dropObligations.size(); ++index) {
    const MirDropObligation &drop = body.dropObligations[index];
    const MirPlace *place =
        validPlace(drop.place) ? &body.places[drop.place - 1] : nullptr;
    const MirValue *value = validValue(drop.generatedValue)
                                ? &body.values[drop.generatedValue - 1]
                                : nullptr;
    const bool valueKind = drop.kind == MirDropObligationKind::Value;
    const bool preparedKind =
        drop.kind == MirDropObligationKind::PreparedParameter;
    if (drop.id != index + 1 || drop.hostedStartupOperation == 0 ||
        drop.hirObligation != 0 || drop.constructionOrder != index + 1 ||
        (!valueKind && !preparedKind) || place == nullptr || value == nullptr ||
        drop.binding != 0 || drop.value != 0 || drop.hirFullExpression != 0 ||
        drop.fullExpression != 0 || drop.initiallyActive ||
        drop.dropType.type.kind != SemanticType::Class ||
        !drop.dropType.classInstance || drop.dropType.lambdaInstance ||
        !drop.dropType.requiresActiveCleanup ||
        place->type != drop.dropType.type || value->info.type != place->type ||
        (valueKind && (place->root != MirPlaceRootKind::Value ||
                       place->value != drop.generatedValue)) ||
        (preparedKind && place->root != MirPlaceRootKind::Temporary)) {
      return failure(
          body, owner,
          "hosted-startup drop has invalid generated ownership provenance");
    }
  }

  for (std::size_t index = 0; index < body.cleanupBoundaries.size(); ++index) {
    const MirCleanupBoundary &boundary = body.cleanupBoundaries[index];
    if (boundary.id != index + 1 || boundary.hostedStartupOperation == 0 ||
        boundary.kind != MirCleanupBoundaryKind::Failure ||
        boundary.obligations.empty() ||
        std::any_of(boundary.obligations.begin(), boundary.obligations.end(),
                    [&](MirDropObligationId obligation) {
                      return !validDrop(obligation);
                    })) {
      return failure(body, owner,
                     "hosted-startup failure boundary has invalid generated "
                     "provenance");
    }
  }
  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    const MirFailureRecord &record = body.failureRecords[index];
    if (record.id != index + 1 || record.hostedStartupOperation == 0 ||
        record.producerBlock == 0 ||
        record.producerBlock > body.blocks.size() ||
        record.producerInstruction == 0 || record.parameterBlock == 0 ||
        record.parameterBlock > body.blocks.size() ||
        record.producerBlock == record.parameterBlock) {
      return failure(body, owner,
                     "hosted-startup failure record has invalid generated "
                     "provenance");
    }
  }

  std::unordered_set<MirInstructionId> instructionIds;
  std::vector<std::size_t> definitions(body.values.size(), 0);
  std::vector<std::size_t> cleanupBoundaryMarkers(body.cleanupBoundaries.size(),
                                                  0);
  std::vector<std::size_t> failureParameters(body.failureRecords.size(), 0);
  std::vector<std::size_t> failureInvokes(body.failureRecords.size(), 0);
  std::vector<std::size_t> failureEndpoints(body.failureRecords.size(), 0);
  std::unordered_map<MirInstructionId, const MirInstruction *> instructions;
  std::unordered_map<MirInstructionId, MirBlockId> instructionBlocks;
  for (std::size_t blockIndex = 0; blockIndex < body.blocks.size();
       ++blockIndex) {
    const MirBlock &block = body.blocks[blockIndex];
    if (block.id != blockIndex + 1 || block.programInitializationStep != 0 ||
        (block.failureParameter != 0 &&
         !validFailureRecord(block.failureParameter)) ||
        (block.activeFailure != 0 &&
         !validFailureRecord(block.activeFailure)) ||
        (block.failureParameter != 0 && block.activeFailure == 0)) {
      return failure(body, owner,
                     "hosted-startup block has source or failure provenance",
                     block.id);
    }
    if (block.failureParameter != 0) {
      const MirFailureRecord &parameter =
          body.failureRecords[block.failureParameter - 1];
      if (parameter.parameterBlock != block.id) {
        return failure(body, owner,
                       "hosted-startup failure parameter names another block",
                       block.id);
      }
      ++failureParameters[block.failureParameter - 1];
    }
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.id == 0 ||
          !instructionIds.insert(instruction.id).second ||
          instruction.hostedStartupOperation == 0 ||
          instruction.hirValue != 0 || instruction.hirStatement != 0 ||
          instruction.callSite != 0 ||
          instruction.constructorInitializer != 0 ||
          instruction.unsafeOperation != UnsafeOperationKind::None ||
          instruction.rawMemoryAccess || instruction.loan ||
          instruction.borrowOrigin != BorrowOriginKind::None ||
          instruction.borrowArgument != 0 ||
          instruction.borrowAccess != AccessMode::ReadOnly ||
          instruction.borrowPlace || instruction.literal ||
          instruction.literalProvenance != MirLiteralProvenance{} ||
          instruction.programConstantSubstitution || instruction.enumOwner ||
          instruction.enumValue || instruction.enumVariant ||
          instruction.payloadIndex ||
          instruction.intrinsic != IntrinsicKind::None ||
          instruction.synchronization != SynchronizationOperation{} ||
          instruction.dispatch != CallDispatch::Static ||
          instruction.dispatchOwner != SemanticType::Unknown ||
          instruction.lambdaTarget || !instruction.callableArguments.empty() ||
          instruction.callableBoundary || instruction.callableInvocation ||
          instruction.ownership || instruction.fullExpressionEnd != 0 ||
          (instruction.cleanupBoundaryEnd != 0 &&
           instruction.cleanupBoundaryEnd > body.cleanupBoundaries.size()) ||
          (instruction.kind != MirInstructionKind::CallBody &&
           instruction.bodyTarget) ||
          (instruction.kind == MirInstructionKind::CallBody &&
           !instruction.bodyTarget) ||
          (instruction.destination && !validPlace(*instruction.destination)) ||
          (instruction.result && !validValue(*instruction.result)) ||
          (instruction.receiver && !validOperand(*instruction.receiver)) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return !validOperand(operand);
                      }) ||
          (instruction.preparedParameterDrop &&
           !validDrop(*instruction.preparedParameterDrop)) ||
          (instruction.successResultDrop &&
           !validDrop(*instruction.successResultDrop)) ||
          instruction.successResultDestination ||
          instruction.localFailureSites.size() !=
              instruction.definedFailure.localOrigins.size() ||
          !validFailureInstructionShape(instruction)) {
        return failure(body, owner,
                       "hosted-startup instruction has invalid generated "
                       "shape or provenance",
                       block.id, instruction.id);
      }
      instructions.emplace(instruction.id, &instruction);
      instructionBlocks.emplace(instruction.id, block.id);
      if (instruction.cleanupBoundaryEnd != 0) {
        const MirCleanupBoundary &boundary =
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1];
        if (instruction.kind != MirInstructionKind::Lifecycle ||
            block.activeFailure == 0 ||
            boundary.kind != MirCleanupBoundaryKind::Failure) {
          return failure(body, owner,
                         "hosted-startup cleanup marker is attached to the "
                         "wrong generated control flow",
                         block.id, instruction.id);
        }
        ++cleanupBoundaryMarkers[instruction.cleanupBoundaryEnd - 1];
      }
      if (block.activeFailure != 0) {
        const bool failureDrop =
            instruction.kind == MirInstructionKind::Drop &&
            instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
            instruction.lifecycle.front().failureCleanup;
        const bool failureBoundary =
            instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.cleanupBoundaryEnd != 0;
        if (!failureDrop && !failureBoundary) {
          return failure(body, owner,
                         "hosted-startup active-failure block contains a "
                         "non-cleanup instruction",
                         block.id, instruction.id);
        }
      }
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        const bool source = event.source != 0;
        const bool target = event.target != 0;
        bool valid = !event.conditional;
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          valid = valid && !event.failureCleanup && !source && target;
          break;
        case MirLifecycleEventKind::Reparent:
          valid = valid && !event.failureCleanup && source && target &&
                  event.source != event.target;
          break;
        case MirLifecycleEventKind::TransferOut:
          valid = valid && !event.failureCleanup && source && !target;
          break;
        case MirLifecycleEventKind::Drop:
          valid = valid && source && !target &&
                  event.failureCleanup == (block.activeFailure != 0) &&
                  instruction.kind == MirInstructionKind::Drop &&
                  instruction.destination && validDrop(event.source) &&
                  body.dropObligations[event.source - 1].place ==
                      *instruction.destination;
          break;
        case MirLifecycleEventKind::Move:
        case MirLifecycleEventKind::Replace:
          valid = false;
          break;
        }
        if (!valid || (source && !validDrop(event.source)) ||
            (target && !validDrop(event.target))) {
          return failure(body, owner,
                         "hosted-startup lifecycle event is invalid", block.id,
                         instruction.id);
        }
      }
      if (instruction.result) {
        const MirValue &value = body.values[*instruction.result - 1];
        if (value.definitionBlock != block.id ||
            value.definition != instruction.id ||
            value.info != instruction.info) {
          return failure(body, owner,
                         "hosted-startup result does not match its value",
                         block.id, instruction.id);
        }
        ++definitions[*instruction.result - 1];
      }
    }

    const MirTerminator &terminator = block.terminator;
    const auto validTarget = [&](MirBlockId target) {
      return target != 0 && target <= body.blocks.size();
    };
    const bool validKind =
        (terminator.kind == MirTerminatorKind::Goto &&
         validTarget(terminator.target)) ||
        (terminator.kind == MirTerminatorKind::Branch &&
         validTarget(terminator.target) && validTarget(terminator.elseTarget) &&
         terminator.value && validOperand(*terminator.value) &&
         terminator.value->type == SemanticType::Bool) ||
        (terminator.kind == MirTerminatorKind::Return && terminator.value &&
         validOperand(*terminator.value) &&
         terminator.value->type == SemanticType::Int32) ||
        (terminator.kind == MirTerminatorKind::Invoke &&
         validTarget(terminator.target) && validTarget(terminator.elseTarget) &&
         terminator.target != terminator.elseTarget &&
         terminator.invokeInstruction != 0 &&
         validFailureRecord(terminator.failureRecord)) ||
        (terminator.kind == MirTerminatorKind::ContainFailure &&
         validFailureRecord(terminator.failureRecord)) ||
        (terminator.kind == MirTerminatorKind::TerminateCleanupFailure &&
         validFailureRecord(terminator.failureRecord));
    if (terminator.hostedStartupOperation == 0 || terminator.hirValue != 0 ||
        terminator.hirStatement != 0 || !validKind || terminator.returnLoan ||
        !terminator.switchTargets.empty() ||
        ((terminator.kind != MirTerminatorKind::Invoke) &&
         (!terminator.successLifecycle.empty() ||
          terminator.invokeInstruction != 0)) ||
        ((terminator.kind != MirTerminatorKind::Invoke &&
          terminator.kind != MirTerminatorKind::ContainFailure &&
          terminator.kind != MirTerminatorKind::TerminateCleanupFailure) &&
         terminator.failureRecord != 0)) {
      return failure(body, owner,
                     "hosted-startup terminator has invalid generated shape "
                     "or provenance",
                     block.id);
    }
    if (terminator.kind == MirTerminatorKind::Invoke) {
      const auto invocation = instructions.find(terminator.invokeInstruction);
      const MirFailureRecord &record =
          body.failureRecords[terminator.failureRecord - 1];
      const MirBlock &normal = body.blocks[terminator.target - 1];
      const MirBlock &failed = body.blocks[terminator.elseTarget - 1];
      const MirFailureRecordId expectedFailureActive =
          block.activeFailure == 0 ? terminator.failureRecord
                                   : block.activeFailure;
      const MirInstruction *producer =
          invocation == instructions.end() ? nullptr : invocation->second;
      const bool exactHostedFailureProducer =
          producer != nullptr && !producer->definedFailure.empty() &&
          ((producer->kind == MirInstructionKind::Load &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) ||
           (producer->kind == MirInstructionKind::Compute &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) ||
           (producer->kind == MirInstructionKind::Call &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::DirectCall) ||
           (producer->kind == MirInstructionKind::Construct &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Constructor) ||
           (producer->kind == MirInstructionKind::CallBody &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::BodyCall) ||
           (block.activeFailure != 0 &&
            producer->kind == MirInstructionKind::Drop &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Destructor));
      const bool exactSuccessLifecycle =
          producer != nullptr && producer->successResultDrop
              ? terminator.successLifecycle.size() == 1 &&
                    terminator.successLifecycle.front().kind ==
                        MirLifecycleEventKind::Initialize &&
                    terminator.successLifecycle.front().source == 0 &&
                    terminator.successLifecycle.front().target ==
                        *producer->successResultDrop &&
                    !terminator.successLifecycle.front().conditional &&
                    !terminator.successLifecycle.front().failureCleanup
              : terminator.successLifecycle.empty();
      if (producer == nullptr ||
          instructionBlocks[terminator.invokeInstruction] != block.id ||
          block.instructions.empty() ||
          block.instructions.back().id != terminator.invokeInstruction ||
          !exactHostedFailureProducer || record.producerBlock != block.id ||
          record.producerInstruction != terminator.invokeInstruction ||
          record.parameterBlock != terminator.elseTarget ||
          normal.failureParameter != 0 ||
          normal.activeFailure != block.activeFailure ||
          failed.failureParameter != terminator.failureRecord ||
          failed.activeFailure != expectedFailureActive ||
          terminator.failureRecord == block.activeFailure || terminator.value ||
          !exactSuccessLifecycle) {
        return failure(body, owner,
                       "hosted-startup invoke does not match its exact "
                       "generated record or successors",
                       block.id, terminator.invokeInstruction);
      }
      ++failureInvokes[terminator.failureRecord - 1];
    }
    const bool emptyFailureEndpoint =
        terminator.invokeInstruction == 0 && !terminator.value &&
        terminator.target == 0 && terminator.elseTarget == 0 &&
        terminator.switchTargets.empty() && terminator.successLifecycle.empty();
    if (terminator.kind == MirTerminatorKind::ContainFailure) {
      if (block.activeFailure == 0 ||
          block.activeFailure != terminator.failureRecord ||
          !emptyFailureEndpoint) {
        return failure(body, owner,
                       "hosted-startup containment does not consume its "
                       "exact primary record",
                       block.id);
      }
      ++failureEndpoints[terminator.failureRecord - 1];
    }
    if (terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      if (block.activeFailure == 0 || block.failureParameter == 0 ||
          block.failureParameter != terminator.failureRecord ||
          block.activeFailure == terminator.failureRecord ||
          !block.instructions.empty() || !emptyFailureEndpoint) {
        return failure(body, owner,
                       "hosted-startup cleanup secondary does not terminate "
                       "immediately",
                       block.id);
      }
      ++failureEndpoints[terminator.failureRecord - 1];
    }
  }

  if (instructionIds.size() != body.instructionCount()) {
    return failure(body, owner,
                   "hosted-startup instruction identities are not unique");
  }
  for (MirInstructionId id = 1; id <= body.instructionCount(); ++id) {
    if (!instructionIds.contains(id)) {
      return failure(body, owner,
                     "hosted-startup instruction identities are not dense");
    }
  }

  std::vector<std::size_t> failurePredecessors(body.failureRecords.size(), 0);
  for (const MirBlock &block : body.blocks) {
    if (block.activeFailure != 0 &&
        block.terminator.kind != MirTerminatorKind::Goto &&
        block.terminator.kind != MirTerminatorKind::Invoke &&
        block.terminator.kind != MirTerminatorKind::ContainFailure &&
        block.terminator.kind != MirTerminatorKind::TerminateCleanupFailure) {
      return failure(body, owner,
                     "hosted-startup active-failure path bypasses its exact "
                     "terminal endpoint",
                     block.id);
    }
    for (const MirBlockId successor : successors(block.terminator)) {
      const MirBlock &target = body.blocks[successor - 1];
      const bool invokeFailure =
          block.terminator.kind == MirTerminatorKind::Invoke &&
          block.terminator.elseTarget == successor;
      const MirFailureRecordId expectedActive =
          invokeFailure
              ? (block.activeFailure == 0 ? block.terminator.failureRecord
                                          : block.activeFailure)
              : block.activeFailure;
      if (target.activeFailure != expectedActive ||
          (!invokeFailure && target.failureParameter != 0)) {
        return failure(body, owner,
                       "hosted-startup edge does not preserve exact active "
                       "failure state",
                       successor);
      }
      if (target.failureParameter != 0) {
        if (!invokeFailure ||
            block.terminator.failureRecord != target.failureParameter) {
          return failure(body, owner,
                         "hosted-startup failure parameter has a non-failure "
                         "predecessor",
                         successor);
        }
        ++failurePredecessors[target.failureParameter - 1];
      }
    }
  }

  for (const MirFailureRecord &record : body.failureRecords) {
    const MirBlock *parameter = body.findBlock(record.parameterBlock);
    if (parameter == nullptr) {
      continue;
    }
    const bool primary = parameter->activeFailure == record.id;
    if (!primary) {
      const MirBlock *producer = body.findBlock(record.producerBlock);
      if (parameter->failureParameter != record.id ||
          parameter->activeFailure == 0 ||
          parameter->activeFailure == record.id || producer == nullptr ||
          producer->activeFailure != parameter->activeFailure ||
          !parameter->instructions.empty() ||
          parameter->terminator.kind !=
              MirTerminatorKind::TerminateCleanupFailure ||
          parameter->terminator.failureRecord != record.id) {
        return failure(body, owner,
                       "hosted-startup secondary failure is not the first "
                       "cleanup failure or does not terminate immediately",
                       parameter->id);
      }
      continue;
    }

    const MirBlock *cursor = parameter;
    std::unordered_set<MirBlockId> chain;
    std::size_t previousOrder = std::numeric_limits<std::size_t>::max();
    while (true) {
      if (cursor == nullptr || cursor->activeFailure != record.id ||
          !chain.insert(cursor->id).second ||
          (cursor->failureParameter != 0 &&
           cursor->failureParameter != record.id)) {
        return failure(body, owner,
                       "hosted-startup primary cleanup is cyclic or changes "
                       "its active record",
                       cursor == nullptr ? 0 : cursor->id);
      }
      for (const MirInstruction &instruction : cursor->instructions) {
        if (instruction.kind != MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1 ||
            !instruction.lifecycle.front().failureCleanup) {
          continue;
        }
        const MirDropObligation *drop =
            body.findDropObligation(instruction.lifecycle.front().source);
        if (drop == nullptr || drop->constructionOrder >= previousOrder) {
          return failure(body, owner,
                         "hosted-startup failure cleanup is not globally in "
                         "reverse construction order",
                         cursor->id, instruction.id);
        }
        previousOrder = drop->constructionOrder;
      }
      if (cursor->terminator.kind == MirTerminatorKind::ContainFailure) {
        if (cursor->terminator.failureRecord != record.id) {
          return failure(body, owner,
                         "hosted-startup primary cleanup contains another "
                         "record",
                         cursor->id);
        }
        break;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Goto) {
        cursor = body.findBlock(cursor->terminator.target);
        continue;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Invoke) {
        const MirBlock *secondary =
            body.findBlock(cursor->terminator.elseTarget);
        if (secondary == nullptr || secondary->activeFailure != record.id ||
            secondary->failureParameter != cursor->terminator.failureRecord ||
            secondary->failureParameter == record.id ||
            !secondary->instructions.empty() ||
            secondary->terminator.kind !=
                MirTerminatorKind::TerminateCleanupFailure ||
            secondary->terminator.failureRecord !=
                secondary->failureParameter) {
          return failure(body, owner,
                         "hosted-startup fallible cleanup does not route its "
                         "secondary directly to emergency termination",
                         cursor->id, cursor->terminator.invokeInstruction);
        }
        cursor = body.findBlock(cursor->terminator.target);
        continue;
      }
      return failure(body, owner,
                     "hosted-startup primary cleanup bypasses containment",
                     cursor->id);
    }
    for (const MirBlock &candidate : body.blocks) {
      const bool primaryState = candidate.activeFailure == record.id &&
                                (candidate.failureParameter == 0 ||
                                 candidate.failureParameter == record.id);
      if (primaryState && !chain.contains(candidate.id)) {
        return failure(body, owner,
                       "hosted-startup primary cleanup block is disconnected",
                       candidate.id);
      }
    }
  }

  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    if (failureParameters[index] != 1 || failureInvokes[index] != 1 ||
        failurePredecessors[index] != 1 || failureEndpoints[index] != 1) {
      return failure(body, owner,
                     "hosted-startup failure record lacks one exact invoke, "
                     "parameter, predecessor, or endpoint");
    }
  }

  std::unordered_map<MirInstructionId, std::size_t> failureDropCoverage;
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.cleanupBoundaryEnd == 0) {
        continue;
      }
      const MirCleanupBoundary &boundary =
          body.cleanupBoundaries[marker.cleanupBoundaryEnd - 1];
      std::vector<const MirInstruction *> reversed;
      const MirBlock *cursor = &block;
      std::size_t cursorIndex = markerIndex;
      std::unordered_set<MirBlockId> visited;
      while (reversed.size() < boundary.obligations.size()) {
        if (!visited.insert(cursor->id).second) {
          break;
        }
        while (cursorIndex != 0 &&
               reversed.size() < boundary.obligations.size()) {
          const MirInstruction &candidate =
              cursor->instructions[cursorIndex - 1];
          if (candidate.kind != MirInstructionKind::Drop) {
            break;
          }
          reversed.push_back(&candidate);
          --cursorIndex;
        }
        if (reversed.size() == boundary.obligations.size()) {
          break;
        }
        if (cursorIndex != 0) {
          break;
        }
        const MirBlock *predecessor = nullptr;
        std::size_t predecessorCount = 0;
        for (const MirBlock &candidate : body.blocks) {
          const std::vector<MirBlockId> outgoing =
              successors(candidate.terminator);
          if (std::find(outgoing.begin(), outgoing.end(), cursor->id) ==
              outgoing.end()) {
            continue;
          }
          ++predecessorCount;
          if (candidate.terminator.kind == MirTerminatorKind::Invoke &&
              candidate.terminator.target == cursor->id &&
              candidate.activeFailure == cursor->activeFailure) {
            predecessor = &candidate;
          }
        }
        if (predecessorCount != 1 || predecessor == nullptr) {
          break;
        }
        cursor = predecessor;
        cursorIndex = cursor->instructions.size();
      }
      std::reverse(reversed.begin(), reversed.end());
      if (reversed.size() != boundary.obligations.size()) {
        return failure(body, owner,
                       "hosted-startup failure cleanup is not connected to "
                       "its boundary marker",
                       block.id, marker.id);
      }
      for (std::size_t index = 0; index < reversed.size(); ++index) {
        const MirInstruction &drop = *reversed[index];
        if (drop.lifecycle.size() != 1 ||
            drop.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            !drop.lifecycle.front().failureCleanup ||
            drop.lifecycle.front().source != boundary.obligations[index]) {
          return failure(body, owner,
                         "hosted-startup failure boundary does not name its "
                         "exact drop sequence",
                         block.id, drop.id);
        }
        ++failureDropCoverage[drop.id];
      }
    }
  }
  if (std::any_of(cleanupBoundaryMarkers.begin(), cleanupBoundaryMarkers.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "hosted-startup cleanup boundary lacks one exact marker");
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Drop &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().failureCleanup &&
          failureDropCoverage[instruction.id] != 1) {
        return failure(body, owner,
                       "hosted-startup failure drop is not covered exactly "
                       "once",
                       block.id, instruction.id);
      }
    }
  }

  if (std::any_of(definitions.begin(), definitions.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "hosted-startup value lacks one exact definition");
  }
  const std::vector<bool> reachable = reachableBlocks(body);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (!reachable[index] || body.blocks[index].reachable != reachable[index]) {
      return failure(body, owner,
                     "hosted-startup reachability inventory is invalid",
                     body.blocks[index].id);
    }
  }
  MirBody indexed = body;
  if (!rebuildMirValueUses(indexed) || indexed.valueUses != body.valueUses) {
    return failure(body, owner,
                   "hosted-startup value-use inventory is invalid");
  }
  if (MirVerificationResult lifecycle = verifyMirLifecycleFlow(body, owner);
      !lifecycle.valid()) {
    return lifecycle;
  }
  return {};
}

[[nodiscard]] bool bodyHasHostedStartupProvenance(const MirBody &body) {
  return std::any_of(body.cleanupBoundaries.begin(),
                     body.cleanupBoundaries.end(),
                     [](const MirCleanupBoundary &boundary) {
                       return boundary.hostedStartupOperation != 0;
                     }) ||
         std::any_of(body.failureRecords.begin(), body.failureRecords.end(),
                     [](const MirFailureRecord &record) {
                       return record.hostedStartupOperation != 0;
                     }) ||
         std::any_of(body.places.begin(), body.places.end(),
                     [](const MirPlace &place) {
                       return place.hostedStartupOperation != 0;
                     }) ||
         std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                     [](const MirDropObligation &drop) {
                       return drop.hostedStartupOperation != 0 ||
                              drop.generatedValue != 0;
                     }) ||
         std::any_of(body.values.begin(), body.values.end(),
                     [](const MirValue &value) {
                       return value.hostedStartupOperation != 0;
                     }) ||
         std::any_of(
             body.blocks.begin(), body.blocks.end(), [](const MirBlock &block) {
               return block.terminator.hostedStartupOperation != 0 ||
                      std::any_of(
                          block.instructions.begin(), block.instructions.end(),
                          [](const MirInstruction &instruction) {
                            return instruction.hostedStartupOperation != 0 ||
                                   instruction.kind ==
                                       MirInstructionKind::CallBody ||
                                   instruction.bodyTarget.has_value();
                          });
             });
}

} // namespace

MirVerificationResult verifyMirBody(const MirBody &body, std::size_t owner) {
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return failure(body, owner, "entry block is outside the body");
  }
  if (body.kind == MirBodyKind::HostedStartup) {
    return verifyMirHostedStartupBodyStructure(body, owner);
  }
  if (bodyHasHostedStartupProvenance(body)) {
    return failure(body, owner,
                   "source MIR body retains generated hosted-startup "
                   "provenance");
  }

  std::unordered_map<HirValueId, const ConstantValue *>
      expectedProgramConstants;
  for (const MirProgramConstantSubstitution &substitution :
       body.programConstantSubstitutions) {
    if (substitution.hirValue == 0 ||
        std::holds_alternative<ConstantCheckedIntegerResult>(
            substitution.constant) ||
        !expectedProgramConstants
             .emplace(substitution.hirValue, &substitution.constant)
             .second) {
      return failure(body, owner,
                     "program-constant substitution inventory is invalid");
    }
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
    if (boundary.id != index + 1 || boundary.obligations.empty() ||
        boundary.kind >= MirCleanupBoundaryKind::Count) {
      return failure(body, owner,
                     "cleanup-boundary table has an invalid identity");
    }
    for (const MirDropObligationId obligationId : boundary.obligations) {
      const MirDropObligation *obligation =
          body.findDropObligation(obligationId);
      if (obligation == nullptr ||
          (boundary.kind == MirCleanupBoundaryKind::Normal &&
           obligation->kind != MirDropObligationKind::Binding) ||
          obligation->constructionOrder >= previous) {
        return failure(body, owner,
                       "cleanup-boundary obligations are not an exact "
                       "reverse construction sequence");
      }
      previous = obligation->constructionOrder;
    }
  }
  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    const MirFailureRecord &record = body.failureRecords[index];
    if (record.id != index + 1 || record.producerBlock == 0 ||
        record.producerBlock > body.blocks.size() ||
        record.producerInstruction == 0 || record.parameterBlock == 0 ||
        record.parameterBlock > body.blocks.size() ||
        record.producerBlock == record.parameterBlock) {
      return failure(body, owner,
                     "failure-record table has an invalid identity or edge");
    }
  }
  if ((body.kind == MirBodyKind::Constructor ||
       body.kind == MirBodyKind::FieldInitializers) &&
      !mirBodyRoutesFailureEdges(body) && !body.failureRecords.empty()) {
    return failure(body, owner,
                   "construction body with an unarmed subobject transfer "
                   "must not carry defined-failure edges");
  }
  const auto validOperand = [&](const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value: {
      const MirValue *value = body.findValue(operand.value);
      return value != nullptr && operand.place == 0 && operand.loan == 0 &&
             !operand.literal && operand.type == value->info.type;
    }
    case MirOperandKind::Constant:
      return operand.value == 0 && operand.place == 0 && operand.loan == 0 &&
             operand.literal &&
             literalMatchesType(*operand.literal, operand.type);
    case MirOperandKind::Address:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      return operand.value == 0 && operand.loan == 0 && !operand.literal &&
             validPlace(operand.place);
    case MirOperandKind::Loan:
      return operand.value == 0 && operand.place == 0 && !operand.literal &&
             validLoan(operand.loan);
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
    if (instruction.operation == MirOperation::ExpectedHasValue) {
      if (instruction.operands.size() != 1 ||
          resultType != SemanticType::Bool) {
        return false;
      }
      const MirOperand &operand = instruction.operands.front();
      if (operand.type.kind != SemanticType::Expected ||
          operand.type.arguments.size() != 2) {
        return false;
      }
      if (operand.kind == MirOperandKind::Value) {
        return true;
      }
      if (operand.kind != MirOperandKind::BorrowRead) {
        return false;
      }
      const MirPlace *place = body.findPlace(operand.place);
      return place != nullptr && place->type == operand.type;
    }
    if (isUnaryOperation(instruction.operation)) {
      return instruction.operands.size() == 1;
    }
    switch (instruction.operation) {
    case MirOperation::Literal: {
      const MirLiteralProvenance &provenance = instruction.literalProvenance;
      const MirValue *source = provenance.sourceValue == 0
                                   ? nullptr
                                   : body.findValue(provenance.sourceValue);
      const bool computeFoldShape =
          provenance.kind == MirLiteralProvenanceKind::ComputeFold &&
          provenance.sourceValue == 0 && !provenance.sourceValues.empty() &&
          provenance.sourceValues.size() <= 2 &&
          std::all_of(provenance.sourceValues.begin(),
                      provenance.sourceValues.end(),
                      [&](MirValueId sourceValue) {
                        return sourceValue != 0 &&
                               sourceValue != *instruction.result &&
                               body.findValue(sourceValue) != nullptr;
                      });
      return instruction.operands.empty() && instruction.literal &&
             literalMatchesType(*instruction.literal, resultType) &&
             ((provenance.kind == MirLiteralProvenanceKind::Source &&
               provenance.sourceValue == 0) ||
              (provenance.kind == MirLiteralProvenanceKind::IdentityFold &&
               provenance.sourceValue != 0 &&
               provenance.sourceValue != *instruction.result &&
               source != nullptr && source->info.type == resultType) ||
              computeFoldShape);
    }
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
    const MirPlace *successResultDestination =
        instruction.successResultDestination
            ? body.findPlace(*instruction.successResultDestination)
            : nullptr;
    if (!validSynchronizationOperation(instruction.synchronization) ||
        (instruction.constructorInitializer != 0 &&
         (body.kind != MirBodyKind::Constructor ||
          instruction.kind != MirInstructionKind::Initialize)) ||
        (instruction.synchronization.kind !=
             SynchronizationOperationKind::None &&
         instruction.kind != MirInstructionKind::Call) ||
        (callable && !targetlessBorrowProbe &&
         instruction.parameterTypes.size() != instruction.operands.size()) ||
        (!callable && !instruction.parameterTypes.empty()) ||
        (callInput != instruction.callInputRole.has_value()) ||
        (callInput && instruction.callSite == 0) ||
        (!callInput && instruction.preparedParameterDrop) ||
        (instruction.kind != MirInstructionKind::Call &&
         instruction.kind != MirInstructionKind::Construct &&
         instruction.successResultDrop) ||
        (instruction.kind != MirInstructionKind::Call &&
         instruction.kind != MirInstructionKind::Construct &&
         instruction.successResultDestination) ||
        (instruction.successResultDrop &&
         instruction.successResultDestination) ||
        (instruction.successResultDestination &&
         (successResultDestination == nullptr ||
          successResultDestination->root != MirPlaceRootKind::Binding ||
          !successResultDestination->projections.empty() ||
          successResultDestination->type != instruction.info.type)) ||
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
          !instruction.closureCaptureModes.empty())) ||
        (instruction.operation != MirOperation::Literal &&
         instruction.literal.has_value()) ||
        (instruction.operation != MirOperation::Literal &&
         (instruction.literalProvenance.kind !=
              MirLiteralProvenanceKind::None ||
          instruction.literalProvenance.sourceValue != 0)) ||
        (instruction.programConstantSubstitution &&
         (instruction.kind != MirInstructionKind::Compute ||
          (instruction.operation != MirOperation::Literal &&
           instruction.operation != MirOperation::EnumConstant &&
           instruction.operation != MirOperation::Negate))) ||
        (instruction.kind != MirInstructionKind::CallBody &&
         instruction.bodyTarget.has_value())) {
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
      if (kind != HirCallInputKind::Value) {
        return false;
      }
      if (parameter == SemanticType::CString) {
        return source == SemanticType::StringView ||
               source == SemanticType::NullPtr;
      }
      if (parameter.kind != SemanticType::RawPointer) {
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
      const bool exactSource =
          input.info.type.kind == SemanticType::Class &&
          input.info.traits.copyable &&
          !input.info.traits.containsBorrowedState &&
          operand.kind == MirOperandKind::Copy && place != nullptr &&
          place->type == input.info.type &&
          place->sourceValue == input.hirValue && place->traits.copyable &&
          !place->traits.containsBorrowedState &&
          sameCallInputTraits(place->traits, input.info.traits);
      if (!exactSource) {
        return false;
      }
      if (!input.preparedParameterDrop) {
        return !input.destination && input.lifecycle.empty();
      }
      const MirDropObligation *prepared =
          body.findDropObligation(*input.preparedParameterDrop);
      return prepared != nullptr &&
             prepared->kind == MirDropObligationKind::PreparedParameter &&
             input.destination == prepared->place &&
             prepared->dropType.type == input.info.type &&
             input.lifecycle.size() == 1 &&
             input.lifecycle.front().kind ==
                 MirLifecycleEventKind::Initialize &&
             input.lifecycle.front().source == 0 &&
             input.lifecycle.front().target == prepared->id;
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
      if (input.preparedParameterDrop) {
        const MirDropObligation *prepared =
            body.findDropObligation(*input.preparedParameterDrop);
        const bool exactLifecycle =
            obligations.empty()
                ? input.lifecycle.size() == 1 &&
                      input.lifecycle.front().kind ==
                          MirLifecycleEventKind::Initialize &&
                      input.lifecycle.front().source == 0
                : obligations.size() == 1 && input.lifecycle.size() == 1 &&
                      input.lifecycle.front().kind ==
                          MirLifecycleEventKind::Reparent &&
                      input.lifecycle.front().source == obligations.front();
        return prepared != nullptr && exactLifecycle &&
               prepared->kind == MirDropObligationKind::PreparedParameter &&
               input.destination == prepared->place &&
               prepared->dropType.type == input.info.type &&
               input.lifecycle.front().target == prepared->id &&
               !input.lifecycle.front().conditional;
      }
      if (obligations.empty()) {
        return !input.destination && input.lifecycle.empty();
      }
      return !input.destination && obligations.size() == 1 &&
             input.lifecycle.size() == 1 &&
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
      if (!noOperation || !hasResult ||
          (instruction.destination.has_value() !=
           instruction.preparedParameterDrop.has_value()) ||
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
        return !instruction.preparedParameterDrop &&
               instruction.lifecycle.empty() &&
               operand.kind == MirOperandKind::Value;
      case HirCallInputKind::CopyValue:
        return validClassCopyInput(instruction, operand);
      case HirCallInputKind::MoveValue:
        return validClassMoveInput(instruction, operand);
      case HirCallInputKind::ReadBorrow:
        return !instruction.preparedParameterDrop &&
               instruction.lifecycle.empty() &&
               (operand.kind == MirOperandKind::BorrowRead ||
                operand.kind == MirOperandKind::Loan);
      case HirCallInputKind::MutableBorrow:
        if (instruction.preparedParameterDrop ||
            !instruction.lifecycle.empty()) {
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
             (!instruction.successResultDestination ||
              (instruction.result && !instruction.destination &&
               !instruction.loan && !instruction.definedFailure.empty() &&
               instruction.info.traits.drop == DropKind::Lexical)) &&
             (instruction.callSite == 0 ||
              (!instruction.lambdaTarget &&
               ((instruction.functionTarget &&
                 instruction.intrinsic == IntrinsicKind::None &&
                 !instruction.constructorTarget) ||
                (!instruction.functionTarget &&
                 (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
                  instruction.intrinsic == IntrinsicKind::StorageConstruct ||
                  instruction.intrinsic == IntrinsicKind::PrefixStorageAppend ||
                  instruction.intrinsic ==
                      IntrinsicKind::PrefixStorageInsert))))) &&
             validCallableInvocation() &&
             (!instruction.callableBoundary ||
              *instruction.callableBoundary == CallableBoundary::Confined) &&
             (!instruction.constructorTarget ||
              ((instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
                instruction.intrinsic == IntrinsicKind::StorageConstruct ||
                instruction.intrinsic == IntrinsicKind::PrefixStorageAppend ||
                instruction.intrinsic == IntrinsicKind::PrefixStorageInsert) &&
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
             (!instruction.successResultDestination ||
              (!instruction.destination && !instruction.loan &&
               !instruction.definedFailure.empty() &&
               instruction.info.traits.drop == DropKind::Lexical)) &&
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
    case MirInstructionKind::CallBody:
      return noOperation && !hasResult && !instruction.destination &&
             !instruction.receiver && instruction.operands.empty() &&
             instruction.parameterTypes.empty() && !instruction.loan &&
             !instruction.functionTarget && !instruction.constructorTarget &&
             instruction.bodyTarget.has_value() && !instruction.lambdaTarget &&
             instruction.info.type == SemanticType::Void;
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
            (projection.constantIndex && projection.selection != 0))) ||
          (projection.kind == MirProjectionKind::PackElement &&
           (!projection.constantIndex || projection.field != 0 ||
            projection.index != 0 || projection.selection != 0))) {
        return failure(body, owner,
                       "place " + std::to_string(place.id) +
                           " has an invalid projection");
      }
      if (projection.kind == MirProjectionKind::PackElement) {
        const bool firstProjection = &projection == &place.projections.front();
        const auto root =
            std::find_if(body.places.begin(), body.places.end(),
                         [&](const MirPlace &candidate) {
                           return candidate.id != place.id &&
                                  candidate.root == MirPlaceRootKind::Binding &&
                                  candidate.binding == place.binding &&
                                  candidate.projections.empty();
                         });
        const std::size_t element =
            static_cast<std::size_t>(*projection.constantIndex);
        if (!firstProjection || place.projections.size() != 1 ||
            place.root != MirPlaceRootKind::Binding ||
            root == body.places.end() ||
            root->type.kind != SemanticType::TypePack ||
            !root->type.concretePack ||
            element >= root->type.arguments.size() ||
            root->type.arguments[element] != place.type ||
            place.access != AccessMode::ReadOnly ||
            (place.type.kind != SemanticType::Class &&
             place.traits != semanticTraits(place.type)) ||
            !place.initiallyAvailable) {
          return failure(body, owner,
                         "place " + std::to_string(place.id) +
                             " has an invalid concrete pack element");
        }
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
    const bool valueKind = obligation.kind == MirDropObligationKind::Value;
    const bool preparedKind =
        obligation.kind == MirDropObligationKind::PreparedParameter;
    // Compiler-generated for one completed constructor subobject: no HIR
    // obligation or full-expression identity, and the place is the exact
    // This-rooted field place of its stage.
    const bool rollbackKind =
        obligation.kind == MirDropObligationKind::ConstructionRollback;
    const bool validHirIdentity =
        preparedKind || rollbackKind
            ? obligation.hirObligation == 0
            : obligation.hirObligation != 0 &&
                  hirDropObligations.insert(obligation.hirObligation).second;
    const bool exactFullExpression =
        bindingKind || rollbackKind
            ? obligation.fullExpression == 0 &&
                  obligation.hirFullExpression == 0
            : obligation.fullExpression != 0 &&
                  obligation.fullExpression <= body.fullExpressions.size() &&
                  body.fullExpressions[obligation.fullExpression - 1]
                          .hirExpression == obligation.hirFullExpression;
    if (obligation.id != index + 1 ||
        obligation.constructionOrder != index + 1 || !validHirIdentity ||
        place == nullptr || place->type != obligation.dropType.type ||
        obligation.dropType.type == SemanticType::Unknown ||
        obligation.dropType.type.kind == SemanticType::Reference ||
        (bindingKind != (obligation.binding != 0)) ||
        ((valueKind != (obligation.value != 0)) && !rollbackKind) ||
        (rollbackKind && (obligation.value != 0 || obligation.binding != 0)) ||
        !exactFullExpression || (obligation.initiallyActive && !bindingKind) ||
        (rollbackKind && body.kind == MirBodyKind::Constructor &&
         (place->root != MirPlaceRootKind::This ||
          place->projections.size() != 1 ||
          place->projections.front().kind != MirProjectionKind::Field)) ||
        (rollbackKind && body.kind == MirBodyKind::FieldInitializers &&
         (place->root != MirPlaceRootKind::Binding || place->binding == 0 ||
          !place->projections.empty())) ||
        (rollbackKind && body.kind != MirBodyKind::Constructor &&
         body.kind != MirBodyKind::FieldInitializers) ||
        (preparedKind && (place->root != MirPlaceRootKind::Temporary ||
                          place->temporary == 0 || place->sourceValue == 0)) ||
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
    if (valueKind && place->sourceValue != obligation.value) {
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
          if (obligation.kind == MirDropObligationKind::Value) {
            return value != nullptr && value->sourceValue == obligation.value;
          }
          const MirInstruction *definition =
              value == nullptr ? nullptr
                               : linearValueDefinition(body, operand.value);
          return obligation.kind == MirDropObligationKind::PreparedParameter &&
                 definition != nullptr &&
                 definition->kind == MirInstructionKind::CallInput &&
                 definition->preparedParameterDrop == obligation.id;
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
          return obligation.kind == MirDropObligationKind::Value &&
                 place->sourceValue == obligation.value &&
                 place->projections.empty();
        }
        return false;
      };
  const auto instructionConsumesObligation =
      [&](const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        if (obligation.kind == MirDropObligationKind::PreparedParameter &&
            instruction.kind == MirInstructionKind::Call &&
            instruction.receiver &&
            operandCarriesObligation(*instruction.receiver, obligation)) {
          return true;
        }
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
        const bool valueResult =
            obligation.kind == MirDropObligationKind::Value &&
            result != nullptr && result->sourceValue == obligation.value &&
            instruction.hirValue == obligation.value;
        const bool preparedResult =
            obligation.kind == MirDropObligationKind::PreparedParameter &&
            instruction.kind == MirInstructionKind::CallInput &&
            instruction.preparedParameterDrop == obligation.id &&
            result != nullptr && result->sourceValue == instruction.hirValue;
        if ((!valueResult && !preparedResult) || place == nullptr ||
            instruction.info.type != obligation.dropType.type) {
          return false;
        }
        if (place->root == MirPlaceRootKind::Value) {
          return place->value == result->id && !instruction.destination;
        }
        if (place->root != MirPlaceRootKind::Temporary) {
          return false;
        }
        if (instruction.destination == place->id) {
          return true;
        }
        return (instruction.kind == MirInstructionKind::Call ||
                instruction.kind == MirInstructionKind::Construct) &&
               !instruction.destination &&
               instruction.successResultDrop == obligation.id;
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
           kind == MirInstructionKind::CallInput ||
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
  std::vector<std::size_t> failureParameterCounts(body.failureRecords.size(),
                                                  0);
  std::vector<std::size_t> failureInvokeCounts(body.failureRecords.size(), 0);
  std::vector<std::size_t> failureEndpointCounts(body.failureRecords.size(), 0);
  std::unordered_map<MirInstructionId, std::size_t> invokedInstructions;
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const MirBlock &block = body.blocks[index];
    if (block.id != index + 1) {
      return failure(body, owner,
                     "block identity does not match stored block order");
    }
    if (body.kind != MirBodyKind::Module &&
        block.programInitializationStep != 0) {
      return failure(body, owner,
                     "non-module block carries a program-initialization step",
                     block.id);
    }
    if (block.failureParameter != 0) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.failureParameter);
      if (record == nullptr || record->parameterBlock != block.id ||
          block.id == body.entry) {
        return failure(body, owner,
                       "failure block has an invalid fixed-record parameter",
                       block.id);
      }
      ++failureParameterCounts[block.failureParameter - 1];
    }
    if (block.activeFailure != 0) {
      const MirFailureRecord *active =
          body.findFailureRecord(block.activeFailure);
      if (active == nullptr || block.id == body.entry) {
        return failure(body, owner,
                       "failure-cleanup block has an invalid active record",
                       block.id);
      }
      if (block.failureParameter != 0) {
        const MirFailureRecord *parameter =
            body.findFailureRecord(block.failureParameter);
        const MirBlock *producer =
            parameter == nullptr ? nullptr
                                 : body.findBlock(parameter->producerBlock);
        const bool primary = block.failureParameter == block.activeFailure &&
                             producer != nullptr &&
                             producer->activeFailure == 0;
        const bool secondary = block.failureParameter != block.activeFailure &&
                               producer != nullptr &&
                               producer->activeFailure == block.activeFailure;
        if (!primary && !secondary) {
          return failure(body, owner,
                         "failure parameter does not bind a primary or the "
                         "first cleanup secondary",
                         block.id);
        }
      }
    } else if (block.failureParameter != 0) {
      return failure(body, owner,
                     "failure parameter is not carried as active cleanup",
                     block.id);
    }
    std::size_t previousFailureDrop = std::numeric_limits<std::size_t>::max();
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions.size(); ++instructionIndex) {
      const MirInstruction &instruction = block.instructions[instructionIndex];
      if (block.activeFailure != 0) {
        const bool failureDrop =
            instruction.kind == MirInstructionKind::Drop &&
            instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
            instruction.lifecycle.front().failureCleanup;
        const bool failureBoundary =
            instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.cleanupBoundaryEnd != 0 &&
            instruction.cleanupBoundaryEnd <= body.cleanupBoundaries.size() &&
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1].kind ==
                MirCleanupBoundaryKind::Failure;
        if (instruction.kind != MirInstructionKind::EndBorrow && !failureDrop &&
            !failureBoundary) {
          return failure(body, owner,
                         "failure block contains a non-cleanup instruction",
                         block.id, instruction.id);
        }
        if (failureDrop) {
          const MirDropObligation *obligation =
              body.findDropObligation(instruction.lifecycle.front().source);
          if (obligation == nullptr ||
              obligation->constructionOrder >= previousFailureDrop) {
            return failure(body, owner,
                           "failure cleanup sequence is not in reverse "
                           "construction order",
                           block.id, instruction.id);
          }
          previousFailureDrop = obligation->constructionOrder;
        }
      }
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
          (instruction.preparedParameterDrop &&
           body.findDropObligation(*instruction.preparedParameterDrop) ==
               nullptr) ||
          (instruction.successResultDrop &&
           body.findDropObligation(*instruction.successResultDrop) ==
               nullptr) ||
          (instruction.successResultDestination &&
           body.findPlace(*instruction.successResultDestination) == nullptr) ||
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
        const MirCleanupBoundary &boundary =
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1];
        if ((boundary.kind == MirCleanupBoundaryKind::Failure) !=
            (block.activeFailure != 0)) {
          return failure(body, owner,
                         "cleanup boundary is attached to the wrong control "
                         "flow kind",
                         block.id, instruction.id);
        }
        ++cleanupBoundaryMarkers[instruction.cleanupBoundaryEnd - 1];
      }
      const auto validLifecycleObligation = [&](MirDropObligationId id) {
        return id != 0 && body.findDropObligation(id) != nullptr;
      };
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        const bool hasSource = event.source != 0;
        const bool hasTarget = event.target != 0;
        bool validEvent = !event.conditional && !event.failureCleanup;
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          validEvent = !hasSource && hasTarget && !event.conditional &&
                       !event.failureCleanup;
          break;
        case MirLifecycleEventKind::Move:
        case MirLifecycleEventKind::Reparent:
          validEvent = hasSource && hasTarget && event.source != event.target &&
                       !event.conditional && !event.failureCleanup;
          break;
        case MirLifecycleEventKind::Replace:
          validEvent = hasSource && event.source != event.target &&
                       !event.conditional && !event.failureCleanup;
          break;
        case MirLifecycleEventKind::TransferOut:
          validEvent = hasSource && !hasTarget && !event.conditional &&
                       !event.failureCleanup;
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
                 instruction.kind == MirInstructionKind::CallInput ||
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
               ((instruction.kind == MirInstructionKind::Compute ||
                 instruction.kind == MirInstructionKind::CallInput) &&
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
               // Normal constructor completion retires each armed rollback
               // subobject by transferring it to the caller through one
               // standalone retirement instruction.
               (instruction.kind == MirInstructionKind::Lifecycle &&
                source->kind == MirDropObligationKind::ConstructionRollback &&
                (body.kind == MirBodyKind::Constructor ||
                 body.kind == MirBodyKind::FieldInitializers)) ||
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
          if (event.failureCleanup != (block.activeFailure != 0) ||
              !instruction.destination ||
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
    if (block.terminator.kind == MirTerminatorKind::Switch) {
      const SemanticType &switchType = block.terminator.value->type;
      if (!isIntegerType(switchType) && switchType != SemanticType::Char &&
          switchType.kind != SemanticType::Enum) {
        return failure(body, owner,
                       "switch selector is not an integer, char, or enum",
                       block.id);
      }
      for (std::size_t index = 0; index < block.terminator.switchTargets.size();
           ++index) {
        const MirSwitchTarget &target = block.terminator.switchTargets[index];
        const SwitchCaseValue *value = target.value ? &*target.value : nullptr;
        bool validCase =
            value != nullptr && value->type == switchType &&
            !(value->value.negative && value->value.magnitude == 0);
        if (validCase && value->kind == SwitchCaseKind::Integer) {
          const std::optional<CheckedIntegerDomain> domain =
              constantIntegerDomain(value->type);
          validCase = value->enumOwner == 0 && domain &&
                      checkedIntegerFits({.negative = value->value.negative,
                                          .magnitude = value->value.magnitude},
                                         *domain);
        } else if (validCase && value->kind == SwitchCaseKind::Character) {
          validCase = value->type == SemanticType::Char &&
                      value->enumOwner == 0 && !value->value.negative &&
                      value->value.magnitude <=
                          std::numeric_limits<std::uint8_t>::max();
        } else if (validCase && value->kind == SwitchCaseKind::Enumerator) {
          validCase = switchType.kind == SemanticType::Enum &&
                      switchType.enumId != 0 &&
                      value->enumOwner == switchType.enumId;
        } else {
          validCase = false;
        }
        const bool duplicate =
            value != nullptr &&
            std::any_of(block.terminator.switchTargets.begin(),
                        block.terminator.switchTargets.begin() +
                            static_cast<std::ptrdiff_t>(index),
                        [&](const MirSwitchTarget &earlier) {
                          return earlier.value == target.value;
                        });
        if (!validCase || duplicate) {
          return failure(body, owner,
                         "switch case is invalid, noncanonical, or duplicate",
                         block.id);
        }
      }
    }
    const bool hasFailureTerminatorState =
        block.terminator.invokeInstruction != 0 ||
        block.terminator.failureRecord != 0 ||
        !block.terminator.successLifecycle.empty();
    if (block.terminator.kind != MirTerminatorKind::Invoke &&
        block.terminator.kind != MirTerminatorKind::PropagateFailure &&
        block.terminator.kind != MirTerminatorKind::ContainFailure &&
        block.terminator.kind != MirTerminatorKind::TerminateCleanupFailure &&
        hasFailureTerminatorState) {
      return failure(body, owner,
                     "non-failure terminator retains failure-edge state",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Invoke) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      const auto invocation =
          instructionsById.find(block.terminator.invokeInstruction);
      const MirFailureRecordId expectedFailureActive =
          block.activeFailure == 0 ? block.terminator.failureRecord
                                   : block.activeFailure;
      if (!validTarget(block.terminator.target) ||
          !validTarget(block.terminator.elseTarget) ||
          block.terminator.target == block.terminator.elseTarget ||
          block.terminator.value || block.terminator.returnLoan ||
          !block.terminator.switchTargets.empty() || record == nullptr ||
          record->producerBlock != block.id ||
          record->producerInstruction != block.terminator.invokeInstruction ||
          record->parameterBlock != block.terminator.elseTarget ||
          body.blocks[block.terminator.target - 1].failureParameter != 0 ||
          body.blocks[block.terminator.target - 1].activeFailure !=
              block.activeFailure ||
          body.blocks[block.terminator.elseTarget - 1].failureParameter !=
              block.terminator.failureRecord ||
          body.blocks[block.terminator.elseTarget - 1].activeFailure !=
              expectedFailureActive ||
          block.terminator.failureRecord == block.activeFailure ||
          invocation == instructionsById.end() ||
          instructionBlocks.at(block.terminator.invokeInstruction) !=
              block.id ||
          block.instructions.empty() ||
          block.instructions.back().id != block.terminator.invokeInstruction ||
          !supportsMirFailureControlFlow(body.kind) ||
          !requiresMirFailureControlFlow(
              *invocation->second,
              MirFailureControlFlowPosition::FullExpressionRoot)) {
        return failure(body, owner,
                       "invoke terminator does not match its exact operation, "
                       "record, or successors",
                       block.id);
      }
      const std::optional<MirDropObligationId> successResultDrop =
          invocation->second->successResultDrop;
      const bool exactSuccessLifecycle =
          successResultDrop
              ? block.terminator.successLifecycle.size() == 1 &&
                    block.terminator.successLifecycle.front().kind ==
                        MirLifecycleEventKind::Initialize &&
                    block.terminator.successLifecycle.front().source == 0 &&
                    block.terminator.successLifecycle.front().target ==
                        *successResultDrop &&
                    !block.terminator.successLifecycle.front().conditional &&
                    !block.terminator.successLifecycle.front().failureCleanup
              : block.terminator.successLifecycle.empty();
      const MirDropObligation *successResult =
          successResultDrop ? body.findDropObligation(*successResultDrop)
                            : nullptr;
      const std::optional<MirPlaceId> successResultDestination =
          invocation->second->successResultDestination;
      bool exactSuccessDestination = !successResultDestination;
      if (successResultDestination && invocation->second->result) {
        const MirPlace *destination = body.findPlace(*successResultDestination);
        const MirInstruction *initialize = nullptr;
        std::size_t executableUses = 0;
        for (const MirValueUse &use :
             body.usesOf(*invocation->second->result)) {
          if (use.kind != MirValueUseKind::InstructionOperand) {
            ++executableUses;
            continue;
          }
          ++executableUses;
          const MirBlock *consumerBlock = body.findBlock(use.block);
          const auto consumer =
              consumerBlock == nullptr
                  ? std::vector<MirInstruction>::const_iterator{}
                  : std::find_if(consumerBlock->instructions.begin(),
                                 consumerBlock->instructions.end(),
                                 [&](const MirInstruction &candidate) {
                                   return candidate.id == use.instruction;
                                 });
          const MirInstruction *consumerInstruction =
              consumerBlock == nullptr ||
                      consumer == consumerBlock->instructions.end()
                  ? nullptr
                  : &*consumer;
          if (consumerInstruction != nullptr && use.operandIndex == 0 &&
              use.block == block.terminator.target &&
              consumerInstruction->kind == MirInstructionKind::Initialize &&
              consumerInstruction->destination == successResultDestination &&
              consumerInstruction->operands.size() == 1 &&
              consumerInstruction->operands.front().kind ==
                  MirOperandKind::Value &&
              consumerInstruction->operands.front().value ==
                  *invocation->second->result) {
            initialize = consumerInstruction;
          }
        }
        const MirDropObligation *destinationDrop = nullptr;
        bool duplicateDestinationDrop = false;
        for (const MirDropObligation &candidate : body.dropObligations) {
          if (candidate.kind == MirDropObligationKind::Binding &&
              candidate.place == *successResultDestination) {
            if (destinationDrop != nullptr) {
              duplicateDestinationDrop = true;
              break;
            }
            destinationDrop = &candidate;
          }
        }
        const bool exactDestinationLifecycle =
            !duplicateDestinationDrop &&
            (destinationDrop == nullptr
                 ? initialize != nullptr && initialize->lifecycle.empty()
                 : initialize != nullptr && initialize->lifecycle.size() == 1 &&
                       initialize->lifecycle.front().kind ==
                           MirLifecycleEventKind::Initialize &&
                       initialize->lifecycle.front().source == 0 &&
                       initialize->lifecycle.front().target ==
                           destinationDrop->id &&
                       !initialize->lifecycle.front().conditional &&
                       !initialize->lifecycle.front().failureCleanup);
        exactSuccessDestination =
            destination != nullptr &&
            destination->root == MirPlaceRootKind::Binding &&
            destination->projections.empty() &&
            destination->type == invocation->second->info.type &&
            executableUses == 1 && initialize != nullptr &&
            exactDestinationLifecycle;
      }
      if (!exactSuccessLifecycle || !exactSuccessDestination ||
          (successResultDrop &&
           (successResult == nullptr ||
            successResult->kind != MirDropObligationKind::Value ||
            !instructionProducesObligation(*invocation->second,
                                           *successResult)))) {
        return failure(body, owner,
                       "invoke success edge does not initialize its exact "
                       "owning result or direct destination",
                       block.id, block.terminator.invokeInstruction);
      }
      if (invocation->second->result &&
          std::any_of(body.usesOf(*invocation->second->result).begin(),
                      body.usesOf(*invocation->second->result).end(),
                      [&](const MirValueUse &use) {
                        return use.block == block.terminator.elseTarget;
                      })) {
        return failure(body, owner,
                       "invoke result is used on its failure successor",
                       block.id, block.terminator.invokeInstruction);
      }
      ++failureInvokeCounts[block.terminator.failureRecord - 1];
      ++invokedInstructions[block.terminator.invokeInstruction];
    }
    const bool emptyFailureEndpointPayload =
        block.terminator.invokeInstruction == 0 && !block.terminator.value &&
        !block.terminator.returnLoan && block.terminator.target == 0 &&
        block.terminator.elseTarget == 0 &&
        block.terminator.switchTargets.empty() &&
        block.terminator.successLifecycle.empty();
    if (block.terminator.kind == MirTerminatorKind::PropagateFailure) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      if (record == nullptr || block.activeFailure == 0 ||
          block.activeFailure != block.terminator.failureRecord ||
          body.kind == MirBodyKind::HostedStartup ||
          !emptyFailureEndpointPayload) {
        return failure(body, owner,
                       "failure propagation does not preserve its exact "
                       "fixed record",
                       block.id);
      }
      ++failureEndpointCounts[block.terminator.failureRecord - 1];
    }
    if (block.terminator.kind == MirTerminatorKind::ContainFailure) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      if (record == nullptr || body.kind != MirBodyKind::HostedStartup ||
          block.activeFailure == 0 ||
          block.activeFailure != block.terminator.failureRecord ||
          !emptyFailureEndpointPayload) {
        return failure(body, owner,
                       "hosted containment does not consume its exact active "
                       "failure",
                       block.id);
      }
      ++failureEndpointCounts[block.terminator.failureRecord - 1];
    }
    if (block.terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      const MirFailureRecord *secondary =
          body.findFailureRecord(block.terminator.failureRecord);
      const MirBlock *producer = secondary == nullptr
                                     ? nullptr
                                     : body.findBlock(secondary->producerBlock);
      if (secondary == nullptr || block.activeFailure == 0 ||
          block.failureParameter == 0 ||
          block.failureParameter != block.terminator.failureRecord ||
          block.activeFailure == block.terminator.failureRecord ||
          producer == nullptr ||
          producer->activeFailure != block.activeFailure ||
          !emptyFailureEndpointPayload) {
        return failure(body, owner,
                       "cleanup-failure termination does not preserve its "
                       "primary and first secondary records",
                       block.id);
      }
      ++failureEndpointCounts[block.terminator.failureRecord - 1];
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
  std::vector<std::size_t> failurePredecessorCounts(body.failureRecords.size(),
                                                    0);
  for (const MirBlock &block : body.blocks) {
    for (const MirBlockId successor : successors(block.terminator)) {
      const MirBlock &target = body.blocks[successor - 1];
      const MirFailureRecordId parameter = target.failureParameter;
      const bool invokeFailure =
          block.terminator.kind == MirTerminatorKind::Invoke &&
          block.terminator.elseTarget == successor;
      const MirFailureRecordId expectedActive =
          invokeFailure
              ? (block.activeFailure == 0 ? block.terminator.failureRecord
                                          : block.activeFailure)
              : block.activeFailure;
      if (target.activeFailure != expectedActive ||
          (!invokeFailure && parameter != 0)) {
        return failure(body, owner,
                       "control-flow edge does not preserve exact active "
                       "failure state",
                       successor);
      }
      if (parameter == 0) {
        continue;
      }
      const bool exactFailureEdge =
          invokeFailure && block.terminator.failureRecord == parameter;
      if (!exactFailureEdge) {
        return failure(body, owner,
                       "failure-record parameter has a non-failure "
                       "predecessor",
                       successor);
      }
      ++failurePredecessorCounts[parameter - 1];
    }
  }
  for (const MirFailureRecord &record : body.failureRecords) {
    const MirBlock *parameter = body.findBlock(record.parameterBlock);
    if (parameter == nullptr) {
      continue;
    }
    if (parameter->activeFailure != record.id) {
      if (parameter->failureParameter != record.id ||
          !parameter->instructions.empty() ||
          parameter->terminator.kind !=
              MirTerminatorKind::TerminateCleanupFailure ||
          parameter->terminator.failureRecord != record.id) {
        return failure(body, owner,
                       "cleanup secondary does not terminate immediately",
                       parameter->id);
      }
      continue;
    }

    const MirTerminatorKind expectedEndpoint =
        body.kind == MirBodyKind::HostedStartup
            ? MirTerminatorKind::ContainFailure
            : MirTerminatorKind::PropagateFailure;
    const MirBlock *cursor = parameter;
    std::unordered_set<MirBlockId> primaryChain;
    std::size_t previousConstructionOrder =
        std::numeric_limits<std::size_t>::max();
    while (true) {
      if (cursor == nullptr || cursor->activeFailure != record.id ||
          !primaryChain.insert(cursor->id).second ||
          (cursor->failureParameter != 0 &&
           cursor->failureParameter != record.id)) {
        return failure(body, owner,
                       "primary failure cleanup is cyclic or changes its "
                       "active record",
                       cursor == nullptr ? 0 : cursor->id);
      }
      for (const MirInstruction &instruction : cursor->instructions) {
        if (instruction.kind != MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1 ||
            !instruction.lifecycle.front().failureCleanup) {
          continue;
        }
        const MirDropObligation *obligation =
            body.findDropObligation(instruction.lifecycle.front().source);
        if (obligation == nullptr ||
            obligation->constructionOrder >= previousConstructionOrder) {
          return failure(body, owner,
                         "primary failure cleanup is not globally in reverse "
                         "construction order",
                         cursor->id, instruction.id);
        }
        previousConstructionOrder = obligation->constructionOrder;
      }

      if (cursor->terminator.kind == expectedEndpoint) {
        if (cursor->terminator.failureRecord != record.id) {
          return failure(body, owner,
                         "primary failure cleanup reaches the wrong terminal "
                         "record",
                         cursor->id);
        }
        break;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Goto) {
        cursor = body.findBlock(cursor->terminator.target);
        continue;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Invoke) {
        const MirBlock *normal = body.findBlock(cursor->terminator.target);
        const MirBlock *secondary =
            body.findBlock(cursor->terminator.elseTarget);
        if (secondary == nullptr || secondary->activeFailure != record.id ||
            secondary->failureParameter != cursor->terminator.failureRecord ||
            secondary->failureParameter == record.id ||
            !secondary->instructions.empty() ||
            secondary->terminator.kind !=
                MirTerminatorKind::TerminateCleanupFailure ||
            secondary->terminator.failureRecord !=
                secondary->failureParameter) {
          return failure(body, owner,
                         "fallible failure cleanup does not route its first "
                         "secondary directly to emergency termination",
                         cursor->id, cursor->terminator.invokeInstruction);
        }
        cursor = normal;
        continue;
      }
      return failure(body, owner,
                     "primary failure cleanup bypasses its exact terminal "
                     "endpoint",
                     cursor->id);
    }

    for (const MirBlock &candidate : body.blocks) {
      const bool primaryState = candidate.activeFailure == record.id &&
                                (candidate.failureParameter == 0 ||
                                 candidate.failureParameter == record.id);
      if (primaryState && !primaryChain.contains(candidate.id)) {
        return failure(body, owner,
                       "active primary failure block is disconnected from "
                       "its parameter-to-endpoint chain",
                       candidate.id);
      }
    }
  }
  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    if (failureParameterCounts[index] != 1 || failureInvokeCounts[index] != 1 ||
        failurePredecessorCounts[index] != 1 ||
        failureEndpointCounts[index] != 1) {
      return failure(body, owner,
                     "failure record must have one invoke, predecessor, "
                     "parameter, and terminal endpoint");
    }
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
  std::unordered_map<MirInstructionId, std::size_t> bindingDropCoverage;
  const auto precedingDropSequence = [&](const MirBlock &markerBlock,
                                         std::size_t markerIndex,
                                         std::size_t dropCount)
      -> std::optional<std::vector<const MirInstruction *>> {
    std::vector<const MirInstruction *> reversed;
    reversed.reserve(dropCount);
    const MirBlock *cursor = &markerBlock;
    std::size_t cursorIndex = markerIndex;
    std::unordered_set<MirBlockId> visited;
    while (reversed.size() < dropCount) {
      if (!visited.insert(cursor->id).second) {
        return std::nullopt;
      }
      while (cursorIndex != 0 && reversed.size() < dropCount) {
        const MirInstruction &candidate = cursor->instructions[cursorIndex - 1];
        if (candidate.kind != MirInstructionKind::Drop) {
          break;
        }
        reversed.push_back(&candidate);
        --cursorIndex;
      }
      if (reversed.size() == dropCount) {
        break;
      }
      if (cursorIndex != 0) {
        return std::nullopt;
      }
      const MirBlock *predecessor = nullptr;
      std::size_t predecessorCount = 0;
      for (const MirBlock &candidate : body.blocks) {
        const std::vector<MirBlockId> outgoing =
            successors(candidate.terminator);
        if (std::find(outgoing.begin(), outgoing.end(), cursor->id) ==
            outgoing.end()) {
          continue;
        }
        ++predecessorCount;
        if (candidate.terminator.kind == MirTerminatorKind::Invoke &&
            candidate.terminator.target == cursor->id &&
            candidate.activeFailure == cursor->activeFailure) {
          predecessor = &candidate;
        }
      }
      if (predecessorCount != 1 || predecessor == nullptr) {
        return std::nullopt;
      }
      cursor = predecessor;
      cursorIndex = cursor->instructions.size();
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
  };
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.cleanupBoundaryEnd == 0) {
        continue;
      }
      const MirCleanupBoundary &boundary =
          body.cleanupBoundaries[marker.cleanupBoundaryEnd - 1];
      const auto drops = precedingDropSequence(block, markerIndex,
                                               boundary.obligations.size());
      if (!drops || drops->size() != boundary.obligations.size()) {
        return failure(body, owner,
                       "cleanup sequence is not connected to its exact "
                       "boundary marker",
                       block.id, marker.id);
      }
      for (std::size_t dropIndex = 0; dropIndex < boundary.obligations.size();
           ++dropIndex) {
        const MirInstruction &drop = *(*drops)[dropIndex];
        if (drop.lifecycle.size() != 1 ||
            drop.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop.lifecycle.front().source != boundary.obligations[dropIndex] ||
            drop.lifecycle.front().failureCleanup !=
                (boundary.kind == MirCleanupBoundaryKind::Failure)) {
          return failure(body, owner,
                         "cleanup sequence is not the exact reverse "
                         "construction order",
                         block.id, drop.id);
        }
        ++bindingDropCoverage[drop.id];
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
      const bool failureCleanup = instruction.lifecycle.front().failureCleanup;
      const bool requiresBoundary =
          failureCleanup ||
          (obligation != nullptr &&
           obligation->kind == MirDropObligationKind::Binding);
      if (requiresBoundary && bindingDropCoverage[instruction.id] != 1) {
        return failure(body, owner,
                       failureCleanup
                           ? "failure drop is not covered by exactly one "
                             "failure cleanup boundary"
                           : "binding drop is not covered by exactly one "
                             "lexical cleanup boundary",
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
  for (const MirBlock &block : body.blocks) {
    const MirTerminatorProvenance &provenance = block.terminator.provenance;
    if (provenance.kind == MirTerminatorProvenanceKind::None) {
      if (provenance.foldSourceValue != 0) {
        return failure(body, owner,
                       "terminator without rewrite provenance carries a fold "
                       "source",
                       block.id);
      }
      continue;
    }
    if (provenance.kind != MirTerminatorProvenanceKind::BranchFold ||
        block.terminator.kind != MirTerminatorKind::Goto ||
        block.terminator.target == 0 || provenance.foldSourceValue == 0) {
      return failure(body, owner,
                     "terminator rewrite provenance is outside the "
                     "branch-fold contract",
                     block.id);
    }
    const MirValue *condition = body.findValue(provenance.foldSourceValue);
    bool literalCondition = false;
    if (condition != nullptr) {
      for (const MirBlock &candidate : body.blocks) {
        for (const MirInstruction &instruction : candidate.instructions) {
          if (instruction.result &&
              *instruction.result == provenance.foldSourceValue &&
              instruction.kind == MirInstructionKind::Compute &&
              instruction.operation == MirOperation::Literal &&
              instruction.literal &&
              std::holds_alternative<bool>(*instruction.literal)) {
            literalCondition = true;
          }
        }
      }
    }
    if (!literalCondition) {
      return failure(body, owner,
                     "branch fold does not retain its dominating literal "
                     "condition",
                     block.id);
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
  const auto definingInstruction = [&](MirValueId valueId) {
    const MirValue *value = body.findValue(valueId);
    if (value == nullptr) {
      return static_cast<const MirInstruction *>(nullptr);
    }
    const auto definition = instructionsById.find(value->definition);
    return definition == instructionsById.end() ? nullptr : definition->second;
  };
  for (const MirBlock &block : body.blocks) {
    const MirTerminatorProvenance &provenance = block.terminator.provenance;
    if (provenance.kind != MirTerminatorProvenanceKind::BranchFold) {
      continue;
    }
    const MirInstruction *source =
        definingInstruction(provenance.foldSourceValue);
    const auto sourceBlock = source == nullptr
                                 ? instructionBlocks.end()
                                 : instructionBlocks.find(source->id);
    if (source == nullptr || sourceBlock == instructionBlocks.end() ||
        (sourceBlock->second != block.id &&
         !dominance->dominates(sourceBlock->second, block.id))) {
      return failure(body, owner,
                     "branch fold condition does not dominate the folded "
                     "terminator",
                     block.id);
    }
  }
  const auto exactComputeFoldRewrite = [&](const MirInstruction &instruction) {
    const MirLiteralProvenance &provenance = instruction.literalProvenance;
    if (!instruction.literal || !instruction.result ||
        provenance.sourceValue != 0 || provenance.sourceValues.empty() ||
        provenance.sourceValues.size() > 2) {
      return false;
    }
    std::vector<MirComputeFoldOperand> operands;
    operands.reserve(provenance.sourceValues.size());
    for (const MirValueId sourceValue : provenance.sourceValues) {
      const MirValue *value = body.findValue(sourceValue);
      const MirInstruction *source = definingInstruction(sourceValue);
      if (value == nullptr || source == nullptr ||
          sourceValue == *instruction.result ||
          !strictlyPrecedes(*source, instruction) ||
          source->kind != MirInstructionKind::Compute ||
          source->operation != MirOperation::Literal || !source->literal) {
        return false;
      }
      operands.push_back(
          {.literal = *source->literal, .type = value->info.type});
    }
    const std::optional<Literal> folded = evaluateMirComputeFold(
        provenance.sourceOperation, operands, instruction.info.type);
    return folded && *folded == *instruction.literal;
  };
  const auto exactLiteralRewrite = [&](const MirInstruction &instruction) {
    if (!instruction.literal || !instruction.result ||
        instruction.literalProvenance.kind !=
            MirLiteralProvenanceKind::IdentityFold) {
      return false;
    }
    const SemanticType &type = instruction.info.type;
    MirValueId sourceValue = instruction.literalProvenance.sourceValue;
    const MirInstruction *source = definingInstruction(sourceValue);
    if (source == nullptr || !strictlyPrecedes(*source, instruction)) {
      return false;
    }

    std::unordered_set<MirValueId> visited;
    while (sourceValue != 0 && visited.size() <= body.values.size() &&
           visited.insert(sourceValue).second) {
      const MirValue *value = body.findValue(sourceValue);
      source = definingInstruction(sourceValue);
      if (value == nullptr || source == nullptr || value->info.type != type ||
          source->info.type != type ||
          source->kind != MirInstructionKind::Compute) {
        return false;
      }
      if (source->operation == MirOperation::Literal) {
        return source->literal && *source->literal == *instruction.literal;
      }
      if (source->operation != MirOperation::Identity ||
          source->operands.size() != 1 ||
          source->operands.front().kind != MirOperandKind::Value ||
          source->operands.front().type != type) {
        return false;
      }
      sourceValue = source->operands.front().value;
    }
    return false;
  };
  std::unordered_map<HirValueId, std::size_t>
      programConstantMaterializationCounts;
  std::unordered_map<HirValueId, std::unordered_set<MirValueId>>
      programConstantScheduleValues;
  std::unordered_set<MirInstructionId> programConstantMagnitudeInstructions;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Load &&
          expectedProgramConstants.contains(instruction.hirValue)) {
        return failure(body, owner,
                       "program-constant substitution was lowered as a "
                       "storage load",
                       block.id, instruction.id);
      }
      if (instruction.programConstantSubstitution) {
        const auto expected =
            expectedProgramConstants.find(instruction.hirValue);
        const MirValue *resultValue =
            instruction.result ? body.findValue(*instruction.result) : nullptr;
        if (expected == expectedProgramConstants.end() ||
            ++programConstantMaterializationCounts[instruction.hirValue] != 1 ||
            resultValue == nullptr ||
            resultValue->sourceValue != instruction.hirValue ||
            resultValue->info != instruction.info ||
            instruction.info.category != ValueCategory::Value ||
            instruction.info.access != AccessMode::ReadOnly ||
            !programConstantMatchesType(*expected->second,
                                        instruction.info.type)) {
          return failure(body, owner,
                         "program-constant substitution materialization "
                         "does not match its exact MIR inventory",
                         block.id, instruction.id);
        }
        programConstantScheduleValues[instruction.hirValue].insert(
            *instruction.result);
        const ConstantValue &constant = *expected->second;
        const auto *integer = std::get_if<ConstantInteger>(&constant);
        const std::optional<Literal> literal = programConstantLiteral(constant);
        bool exact = false;
        if (instruction.info.type.kind == SemanticType::Enum) {
          exact = integer != nullptr &&
                  instruction.operation == MirOperation::EnumConstant &&
                  instruction.operands.empty() && !instruction.literal &&
                  instruction.enumOwner == instruction.info.type.enumId &&
                  instruction.enumValue ==
                      std::optional<EnumConstant>{
                          EnumConstant{.negative = integer->negative,
                                       .magnitude = integer->magnitude}};
        } else if (integer != nullptr && integer->negative) {
          const MirValueId magnitudeValue =
              instruction.operands.size() == 1 &&
                      instruction.operands.front().kind == MirOperandKind::Value
                  ? instruction.operands.front().value
                  : 0;
          const MirInstruction *magnitude =
              magnitudeValue == 0 ? nullptr
                                  : definingInstruction(magnitudeValue);
          const std::vector<MirValueUse> &magnitudeUses =
              body.usesOf(magnitudeValue);
          exact = instruction.operation == MirOperation::Negate &&
                  instruction.operands.size() == 1 && magnitude != nullptr &&
                  magnitude->result == magnitudeValue &&
                  strictlyPrecedes(*magnitude, instruction) &&
                  magnitude->kind == MirInstructionKind::Compute &&
                  magnitude->hirValue == instruction.hirValue &&
                  magnitude->operation == MirOperation::Literal &&
                  magnitude->literal == literal &&
                  magnitude->literalProvenance.kind ==
                      MirLiteralProvenanceKind::Source &&
                  !magnitude->programConstantSubstitution &&
                  magnitude->info.type == instruction.info.type &&
                  magnitudeUses.size() == 1 &&
                  magnitudeUses.front().kind ==
                      MirValueUseKind::InstructionOperand &&
                  magnitudeUses.front().instruction == instruction.id &&
                  magnitudeUses.front().operandIndex == 0;
          if (exact) {
            programConstantScheduleValues[instruction.hirValue].insert(
                magnitudeValue);
            programConstantMagnitudeInstructions.insert(magnitude->id);
          }
        } else {
          exact = literal && instruction.operation == MirOperation::Literal &&
                  instruction.operands.empty() &&
                  instruction.literal == literal &&
                  instruction.literalProvenance.kind ==
                      MirLiteralProvenanceKind::Source;
        }
        if (!exact) {
          return failure(body, owner,
                         "program-constant substitution has the wrong exact "
                         "constant schedule",
                         block.id, instruction.id);
        }
      }
      if (instruction.literalProvenance.kind ==
              MirLiteralProvenanceKind::IdentityFold &&
          !exactLiteralRewrite(instruction)) {
        return failure(body, owner,
                       "literal rewrite does not retain an exact dominating "
                       "MIR identity-fold proof",
                       block.id, instruction.id);
      }
      if (instruction.literalProvenance.kind ==
              MirLiteralProvenanceKind::ComputeFold &&
          !exactComputeFoldRewrite(instruction)) {
        return failure(body, owner,
                       "literal rewrite does not retain an exact dominating "
                       "MIR compute-fold proof",
                       block.id, instruction.id);
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Compute &&
          expectedProgramConstants.contains(instruction.hirValue) &&
          !instruction.programConstantSubstitution &&
          !programConstantMagnitudeInstructions.contains(instruction.id)) {
        return failure(body, owner,
                       "program-constant substitution retains an extra "
                       "unmarked compute",
                       block.id, instruction.id);
      }
    }
  }
  // An ordered call stages its inputs, so a substituted constant used as a
  // receiver or argument keeps its exact HIR identity on the staged value as
  // well as on its one materialization. Admit only a stage that forwards an
  // already scheduled value of the same constant.
  for (bool changed = true; changed;) {
    changed = false;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::CallInput ||
            instruction.programConstantSubstitution || !instruction.result ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value) {
          continue;
        }
        const auto schedule =
            programConstantScheduleValues.find(instruction.hirValue);
        if (schedule == programConstantScheduleValues.end() ||
            !schedule->second.contains(instruction.operands.front().value)) {
          continue;
        }
        const MirValue *staged = body.findValue(*instruction.result);
        if (staged == nullptr || staged->sourceValue != instruction.hirValue) {
          continue;
        }
        changed =
            schedule->second.insert(*instruction.result).second || changed;
      }
    }
  }
  for (const MirValue &value : body.values) {
    const auto expected = expectedProgramConstants.find(value.sourceValue);
    if (expected == expectedProgramConstants.end()) {
      continue;
    }
    const auto schedule = programConstantScheduleValues.find(value.sourceValue);
    if (schedule == programConstantScheduleValues.end() ||
        !schedule->second.contains(value.id)) {
      return failure(body, owner,
                     "program-constant substitution retains an extra MIR "
                     "value schedule");
    }
  }
  for (const auto &[hirValue, _] : expectedProgramConstants) {
    if (programConstantMaterializationCounts[hirValue] != 1) {
      return failure(body, owner,
                     "program-constant substitution has no exact MIR "
                     "materialization");
    }
  }
  const auto isFullExpressionRoot = [&](const MirInstruction &instruction) {
    return std::any_of(
        body.fullExpressions.begin(), body.fullExpressions.end(),
        [&](const MirFullExpression &expression) {
          return std::find(expression.roots.begin(), expression.roots.end(),
                           instruction.hirValue) != expression.roots.end();
        });
  };
  std::unordered_map<HirValueId, std::size_t> orderedInvocationCounts;
  std::vector<std::size_t> preparedParameterCounts(body.dropObligations.size(),
                                                   0);
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
      if (instruction.preparedParameterDrop) {
        ++preparedParameterCounts[*instruction.preparedParameterDrop - 1];
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
  for (const MirDropObligation &obligation : body.dropObligations) {
    const std::size_t references = preparedParameterCounts[obligation.id - 1];
    if ((obligation.kind == MirDropObligationKind::PreparedParameter) !=
        (references == 1)) {
      return failure(body, owner,
                     "prepared-parameter obligation must belong to exactly one "
                     "call input");
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
        const bool mutableStorageInput =
            argument != nullptr && index == 0 &&
            argument->callInputKind == HirCallInputKind::MutableBorrow &&
            (parameter.kind == SemanticType::Storage ||
             parameter.kind == SemanticType::PrefixStorage) &&
            (invocation.intrinsic == IntrinsicKind::StorageConstruct ||
             invocation.intrinsic == IntrinsicKind::PrefixStorageAppend ||
             invocation.intrinsic == IntrinsicKind::PrefixStorageInsert);
        const bool exactKind =
            argument != nullptr &&
            (mutableStorageInput ||
             (parameter.kind == SemanticType::Class
                  ? (argument->callInputKind == HirCallInputKind::CopyValue ||
                     argument->callInputKind == HirCallInputKind::MoveValue)
                  : argument->callInputKind ==
                        (parameter.kind != SemanticType::Reference
                             ? HirCallInputKind::Value
                             : (parameter.referenceAccess == AccessMode::Mutable
                                    ? HirCallInputKind::MutableBorrow
                                    : HirCallInputKind::ReadBorrow))));
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
      std::unordered_set<MirDropObligationId> preparedDrops;
      for (const MirInstruction *input : inputs) {
        if ((previous != nullptr && !strictlyPrecedes(*previous, *input)) ||
            !strictlyPrecedes(*input, invocation)) {
          return failure(body, owner,
                         "ordered invocation inputs must form a strict "
                         "receiver, argument, invocation chain",
                         block.id, invocation.id);
        }
        const bool owningInput =
            input->callInputKind == HirCallInputKind::CopyValue ||
            input->callInputKind == HirCallInputKind::MoveValue;
        if (invocation.kind == MirInstructionKind::Call && owningInput) {
          if (!input->preparedParameterDrop ||
              !preparedDrops.insert(*input->preparedParameterDrop).second) {
            return failure(
                body, owner,
                "ordinary call owning inputs require distinct prepared "
                "parameter obligations",
                block.id, invocation.id);
          }
          const std::size_t transfers = static_cast<std::size_t>(std::count_if(
              invocation.lifecycle.begin(), invocation.lifecycle.end(),
              [&](const MirLifecycleEvent &event) {
                return event.kind == MirLifecycleEventKind::TransferOut &&
                       event.source == *input->preparedParameterDrop;
              }));
          if (transfers != 1) {
            return failure(
                body, owner,
                "prepared owning parameter must transfer exactly once when "
                "the callee begins",
                block.id, invocation.id);
          }
        } else if (input->preparedParameterDrop) {
          return failure(body, owner,
                         "prepared parameter obligation is attached to an "
                         "unsupported invocation input",
                         block.id, invocation.id);
        }
        previous = input;
      }
      for (const MirLifecycleEvent &event : invocation.lifecycle) {
        const MirDropObligation *source =
            event.kind == MirLifecycleEventKind::TransferOut
                ? body.findDropObligation(event.source)
                : nullptr;
        if (source != nullptr &&
            source->kind == MirDropObligationKind::PreparedParameter &&
            !preparedDrops.contains(source->id)) {
          return failure(body, owner,
                         "call transfers a prepared parameter owned by a "
                         "different invocation",
                         block.id, invocation.id);
        }
      }
    }
  }
  struct PreparedCallArgumentContext {
    std::vector<MirDropObligationId> priorPreparedDrops;
  };
  const auto preparedCallArgumentContext = [&](const MirInstruction &detector)
      -> std::optional<PreparedCallArgumentContext> {
    if (!detector.result || isFullExpressionRoot(detector)) {
      return std::nullopt;
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &invocation : block.instructions) {
        if (invocation.kind != MirInstructionKind::Call ||
            invocation.callSite == 0) {
          continue;
        }
        for (std::size_t index = 0; index < invocation.operands.size();
             ++index) {
          const MirInstruction *argument =
              callInputFor(invocation.operands[index]);
          if (argument == nullptr || argument->hirValue != detector.hirValue ||
              argument->callInputKind != HirCallInputKind::Value ||
              argument->operands.size() != 1 ||
              argument->operands.front().kind != MirOperandKind::Value ||
              argument->operands.front().value != *detector.result ||
              !strictlyPrecedes(detector, *argument)) {
            continue;
          }
          PreparedCallArgumentContext context;
          const auto addPriorStage = [&](const MirInstruction *input) {
            if (input != nullptr && input->preparedParameterDrop &&
                strictlyPrecedes(*input, detector)) {
              context.priorPreparedDrops.push_back(
                  *input->preparedParameterDrop);
            }
          };
          if (invocation.receiver) {
            addPriorStage(callInputFor(*invocation.receiver));
          }
          for (std::size_t prior = 0; prior < index; ++prior) {
            addPriorStage(callInputFor(invocation.operands[prior]));
          }
          if (!context.priorPreparedDrops.empty()) {
            return context;
          }
        }
      }
    }
    return std::nullopt;
  };
  for (const auto &[instructionId, instruction] : instructionsById) {
    const std::size_t invokeCount = invokedInstructions[instructionId];
    const bool fullExpressionRoot = isFullExpressionRoot(*instruction);
    const std::optional<PreparedCallArgumentContext> argumentContext =
        preparedCallArgumentContext(*instruction);
    const MirFailureControlFlowPosition position =
        fullExpressionRoot ? MirFailureControlFlowPosition::FullExpressionRoot
        : argumentContext.has_value()
            ? MirFailureControlFlowPosition::PreparedCallArgumentRoot
            : MirFailureControlFlowPosition::None;
    const bool requiresEdge =
        mirBodyRoutesFailureEdges(body) &&
        requiresMirFailureControlFlow(*instruction, position);
    if ((requiresEdge && invokeCount != 1) ||
        (!requiresEdge && invokeCount != 0)) {
      return failure(body, owner,
                     "failure-capable operation does not have exactly one "
                     "eligible invoke edge",
                     instructionBlocks.at(instructionId), instructionId);
    }
    if (!argumentContext || invokeCount != 1) {
      continue;
    }
    const MirBlock &producer =
        body.blocks[instructionBlocks.at(instructionId) - 1];
    // A failure-capable destructor routes its own secondary failure, so one
    // reverse cleanup edge can span a chain of blocks instead of ending in the
    // immediate failure successor. Follow the normal continuation of each
    // cleanup invoke so every staged owner is counted exactly once across the
    // whole chain.
    std::vector<const MirBlock *> cleanupChain;
    std::unordered_set<MirBlockId> visitedCleanupBlocks;
    for (MirBlockId next = producer.terminator.elseTarget;
         next != 0 && next <= body.blocks.size() &&
         visitedCleanupBlocks.insert(next).second;) {
      const MirBlock &cleanupBlock = body.blocks[next - 1];
      cleanupChain.push_back(&cleanupBlock);
      if (cleanupBlock.terminator.kind != MirTerminatorKind::Invoke) {
        break;
      }
      next = cleanupBlock.terminator.target;
    }
    for (const MirDropObligationId prepared :
         argumentContext->priorPreparedDrops) {
      std::size_t cleanupCount = 0;
      for (const MirBlock *failureBlock : cleanupChain) {
        cleanupCount += static_cast<std::size_t>(std::count_if(
            failureBlock->instructions.begin(),
            failureBlock->instructions.end(),
            [&](const MirInstruction &cleanup) {
              return cleanup.kind == MirInstructionKind::Drop &&
                     cleanup.lifecycle.size() == 1 &&
                     cleanup.lifecycle.front().kind ==
                         MirLifecycleEventKind::Drop &&
                     cleanup.lifecycle.front().source == prepared &&
                     cleanup.lifecycle.front().failureCleanup;
            }));
      }
      if (cleanupCount != 1) {
        return failure(
            body, owner,
            "nested call-argument failure cleanup must drop every earlier "
            "prepared owning parameter exactly once",
            producer.id, instructionId);
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
      std::unordered_set<MirInstructionId> expressionDropIds;
      for (const MirBlock &candidateBlock : body.blocks) {
        for (const MirInstruction &instruction : candidateBlock.instructions) {
          for (const MirLifecycleEvent &event : instruction.lifecycle) {
            const MirDropObligation *obligation =
                event.kind == MirLifecycleEventKind::Drop &&
                        !event.failureCleanup
                    ? body.findDropObligation(event.source)
                    : nullptr;
            if (obligation != nullptr &&
                obligation->fullExpression == expression.id) {
              expressionDropIds.insert(instruction.id);
            }
          }
        }
      }
      const auto drops =
          precedingDropSequence(block, markerIndex, expressionDropIds.size());
      if (!drops || drops->size() != expressionDropIds.size()) {
        return failure(body, owner,
                       "full-expression cleanup is not connected to its "
                       "boundary marker",
                       block.id, marker.id);
      }
      std::size_t previousDrop = std::numeric_limits<std::size_t>::max();
      for (const MirInstruction *drop : *drops) {
        const MirDropObligationId obligation =
            drop->lifecycle.size() == 1 ? drop->lifecycle.front().source : 0;
        const MirDropObligation *dropDescriptor =
            body.findDropObligation(obligation);
        if (drop->kind != MirInstructionKind::Drop ||
            drop->lifecycle.size() != 1 ||
            drop->lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop->lifecycle.front().failureCleanup ||
            !expressionDropIds.contains(drop->id) ||
            dropDescriptor == nullptr ||
            dropDescriptor->fullExpression != expression.id ||
            dropDescriptor->constructionOrder >= previousDrop) {
          return failure(
              body, owner,
              "full-expression drops must be contiguous and in reverse "
              "obligation order",
              block.id, drop->id);
        }
        previousDrop = dropDescriptor->constructionOrder;
      }
      const MirInstruction &firstBoundary =
          drops->empty() ? marker : *drops->front();
      for (const HirValueId root : expression.roots) {
        bool rootBeforeBoundary = false;
        bool hasConcreteValue = false;
        for (const MirValue &value : body.values) {
          if (value.sourceValue != root) {
            continue;
          }
          hasConcreteValue = true;
          const auto definition = instructionsById.find(value.definition);
          if (definition != instructionsById.end()) {
            rootBeforeBoundary =
                rootBeforeBoundary ||
                strictlyPrecedes(*definition->second, firstBoundary);
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
            if (strictlyPrecedes(definition, firstBoundary)) {
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
  if (MirVerificationResult initialization =
          verifyMirScalarInitializationFlow(body, owner);
      !initialization.valid()) {
    return initialization;
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
  std::size_t consumed = 0;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      consumed += static_cast<std::size_t>(std::count_if(
          instruction.lifecycle.begin(), instruction.lifecycle.end(),
          [&](const MirLifecycleEvent &event) {
            if (event.source != obligation) {
              return false;
            }
            if (event.kind == MirLifecycleEventKind::TransferOut) {
              return true;
            }
            // A staged owned-parameter field consumes the moved value by
            // reparenting it into the armed ConstructionRollback obligation
            // instead of transferring it out silently.
            if (event.kind != MirLifecycleEventKind::Reparent) {
              return false;
            }
            const MirDropObligation *target =
                body.findDropObligation(event.target);
            return target != nullptr &&
                   target->kind == MirDropObligationKind::ConstructionRollback;
          }));
    }
  }
  return consumed == 1;
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

[[nodiscard]] bool
exactInPlaceConstructionRoles(const std::vector<SemanticType> &arguments,
                              const std::vector<SemanticType> &parameters) {
  std::vector<SemanticType> argumentRoles;
  std::vector<SemanticType> parameterRoles;
  for (const SemanticType &type : arguments) {
    appendCanonicalParameterRoles(type, argumentRoles);
  }
  for (const SemanticType &type : parameters) {
    appendCanonicalParameterRoles(type, parameterRoles);
  }
  if (argumentRoles.size() != parameterRoles.size()) {
    return false;
  }
  for (std::size_t index = 0; index < argumentRoles.size(); ++index) {
    const SemanticType &argument = argumentRoles[index];
    const SemanticType &parameter = parameterRoles[index];
    if (argument == parameter) {
      continue;
    }
    if (parameter.kind != SemanticType::RawPointer) {
      return false;
    }
    if (argument == SemanticType::NullPtr) {
      continue;
    }
    if (argument.kind != SemanticType::RawPointer ||
        parameter.arguments.size() != 1 || argument.arguments.size() != 1 ||
        parameter.arguments.front() != argument.arguments.front() ||
        (parameter.pointerAccess == AccessMode::Mutable &&
         argument.pointerAccess != AccessMode::Mutable)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool callResultMatches(const ExpressionInfo &result,
                                     const SemanticType &returnType) {
  if (returnType.kind != SemanticType::Reference) {
    return result.type == returnType;
  }
  return returnType.arguments.size() == 1 &&
         result.type == returnType.arguments.front() &&
         result.category == ValueCategory::Place &&
         result.access == returnType.referenceAccess;
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
         callResultMatches(instruction.info, signature.returnType) &&
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
          (!callResultMatches(instruction.info, functionTarget->returnType) ||
           !exactParameterRoles(instruction.parameterTypes,
                                functionTarget->parameterTypes))) {
        return failure(body, owner,
                       "operator() invocation does not match its exact target "
                       "signature",
                       block.id, instruction.id);
      }
      if (exactLambdaCallable &&
          (!callResultMatches(instruction.info, lambdaTarget->returnType) ||
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

      for (std::size_t index = 0; index < instruction.parameterTypes.size() &&
                                  index < instruction.operands.size();
           ++index) {
        const SemanticType &parameter = instruction.parameterTypes[index];
        const MirOperand &operand = instruction.operands[index];
        if (parameter.kind != SemanticType::Reference || operand.place == 0) {
          continue;
        }
        const MirPlace *source = body.findPlace(operand.place);
        const MirOperandKind expectedKind =
            parameter.referenceAccess == AccessMode::Mutable
                ? MirOperandKind::BorrowWrite
                : MirOperandKind::BorrowRead;
        if (source == nullptr || parameter.arguments.size() != 1 ||
            operand.type != parameter || operand.kind != expectedKind ||
            source->type != parameter.arguments.front() ||
            (parameter.referenceAccess == AccessMode::Mutable &&
             source->access != AccessMode::Mutable)) {
          return failure(body, owner,
                         "direct reference argument does not match its exact "
                         "parameter pointee and access",
                         block.id, instruction.id);
        }
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
            : !target->mayRaiseDefinedFailure
                ? FailurePropagationKind::None
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
        std::optional<std::size_t> inPlaceArgumentOffset;
        SemanticType::Kind inPlaceStorageKind = SemanticType::Unknown;
        if (instruction.kind == MirInstructionKind::Call) {
          switch (instruction.intrinsic) {
          case IntrinsicKind::StorageConstruct:
            inPlaceArgumentOffset = 2;
            inPlaceStorageKind = SemanticType::Storage;
            break;
          case IntrinsicKind::PrefixStorageAppend:
            inPlaceArgumentOffset = 1;
            inPlaceStorageKind = SemanticType::PrefixStorage;
            break;
          case IntrinsicKind::PrefixStorageInsert:
            inPlaceArgumentOffset = 2;
            inPlaceStorageKind = SemanticType::PrefixStorage;
            break;
          default:
            break;
          }
        }
        if (inPlaceArgumentOffset) {
          const MirClassInstance *constructorOwner =
              program.findClassInstance(constructorTarget->owner);
          const bool hasIndex = *inPlaceArgumentOffset == 2;
          const bool exactStorageSurface =
              instruction.parameterTypes.size() >= *inPlaceArgumentOffset &&
              !instruction.parameterTypes.empty() &&
              instruction.parameterTypes.front().kind == inPlaceStorageKind &&
              instruction.parameterTypes.front().arguments.size() == 1 &&
              (!hasIndex ||
               instruction.parameterTypes[1] == SemanticType::UInt64);
          std::vector<SemanticType> constructorArguments;
          if (exactStorageSurface) {
            constructorArguments.assign(
                instruction.parameterTypes.begin() +
                    static_cast<std::ptrdiff_t>(*inPlaceArgumentOffset),
                instruction.parameterTypes.end());
          }
          if (!exactStorageSurface || constructorOwner == nullptr ||
              constructorOwner->type !=
                  instruction.parameterTypes.front().arguments.front() ||
              !exactInPlaceConstructionRoles(
                  constructorArguments, constructorTarget->parameterTypes)) {
            return failure(body, owner,
                           "in-place storage construction does not match its "
                           "exact element constructor target",
                           block.id, instruction.id);
          }
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
        const FailurePropagationKind expectedPropagation =
            constructorTarget->mayRaiseDefinedFailure
                ? FailurePropagationKind::Constructor
                : FailurePropagationKind::None;
        if (instruction.kind == MirInstructionKind::Construct &&
            instruction.definedFailure.propagation != expectedPropagation) {
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
      const bool exactSource =
          expectedSource && *expectedSource == loan->source;
      if (!exactSource && !sameCanonicalPlace(expectedPlace, actualPlace)) {
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
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  for (const MirLoan &loan : body.loans) {
    for (const HirBindingId carrier : loan.carriers) {
      bindingLoans[carrier].push_back(loan.id);
    }
  }
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
      const MirCanonicalPlace actual =
          canonicalBorrowOriginPlace(body, loan->source, bindingLoans);
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
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop) {
        continue;
      }
      const MirDropObligation *obligation =
          body.findDropObligation(instruction.lifecycle.front().source);
      const MirDestructorInstance *destructor =
          obligation != nullptr && obligation->dropType.destructor
              ? program.findDestructorInstance(*obligation->dropType.destructor)
              : nullptr;
      const FailurePropagationKind expected =
          destructor != nullptr && destructor->mayRaiseDefinedFailure
              ? FailurePropagationKind::Destructor
              : FailurePropagationKind::None;
      if (obligation == nullptr ||
          instruction.definedFailure.propagation != expected ||
          !instruction.definedFailure.localOrigins.empty()) {
        return failure(body, owner,
                       "drop propagation does not match its exact destructor "
                       "failure summary",
                       block.id, instruction.id);
      }
    }
  }
  return {};
}

struct MirCheckedIntegerFailureContract {
  bool applicable = false;
  bool validShape = true;
  std::vector<DefinedFailureOutcome> outcomes;
};

[[nodiscard]] bool integerConversionMayFail(CheckedIntegerDomain source,
                                            CheckedIntegerDomain target) {
  if (source.signedValue == target.signedValue) {
    return source.width > target.width;
  }
  if (!source.signedValue && target.signedValue) {
    return source.width >= target.width;
  }
  return true;
}

[[nodiscard]] MirCheckedIntegerFailureContract
checkedIntegerFailureContract(const MirBody &body,
                              const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute) {
    return {};
  }

  const std::optional<CheckedIntegerDomain> resultDomain =
      constantIntegerDomain(instruction.info.type);
  const auto operandDomain = [&](std::size_t index) {
    return index < instruction.operands.size()
               ? constantIntegerDomain(instruction.operands[index].type)
               : std::nullopt;
  };
  const auto sourceIntegerLiteralMagnitude =
      [&](std::size_t index) -> std::optional<std::uint64_t> {
    if (index >= instruction.operands.size() ||
        instruction.operands[index].kind != MirOperandKind::Value) {
      return std::nullopt;
    }

    MirValueId valueId = instruction.operands[index].value;
    SemanticType valueType = instruction.operands[index].type;
    std::unordered_set<MirValueId> visited;
    while (valueId != 0 && visited.size() <= body.values.size() &&
           visited.insert(valueId).second) {
      const MirValue *value = body.findValue(valueId);
      const MirBlock *definitionBlock =
          value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
      const auto definition =
          definitionBlock == nullptr
              ? std::vector<MirInstruction>::const_iterator{}
              : std::find_if(definitionBlock->instructions.begin(),
                             definitionBlock->instructions.end(),
                             [&](const MirInstruction &candidate) {
                               return candidate.id == value->definition;
                             });
      if (value == nullptr || value->info.type != valueType ||
          definitionBlock == nullptr ||
          definition == definitionBlock->instructions.end() ||
          definition->kind != MirInstructionKind::Compute ||
          definition->result != valueId || definition->info.type != valueType) {
        return std::nullopt;
      }
      if (definition->operation == MirOperation::Literal) {
        const std::uint64_t *magnitude =
            definition->literal
                ? std::get_if<std::uint64_t>(&*definition->literal)
                : nullptr;
        return definition->operands.empty() &&
                       definition->literalProvenance.kind ==
                           MirLiteralProvenanceKind::Source &&
                       definition->literalProvenance.sourceValue == 0 &&
                       magnitude != nullptr
                   ? std::optional<std::uint64_t>{*magnitude}
                   : std::nullopt;
      }
      if (definition->operation != MirOperation::Identity ||
          definition->literal ||
          definition->literalProvenance.kind !=
              MirLiteralProvenanceKind::None ||
          definition->literalProvenance.sourceValue != 0 ||
          definition->operands.size() != 1 ||
          definition->operands.front().kind != MirOperandKind::Value ||
          definition->operands.front().type != valueType) {
        return std::nullopt;
      }
      valueId = definition->operands.front().value;
    }
    return std::nullopt;
  };
  const auto operandAdmitsExecutionDomain = [&](std::size_t index,
                                                bool negativeLiteral = false) {
    const std::optional<std::uint64_t> literal =
        sourceIntegerLiteralMagnitude(index);
    if (literal) {
      return resultDomain && checkedIntegerFits({.negative = negativeLiteral,
                                                 .magnitude = *literal},
                                                *resultDomain);
    }
    const std::optional<CheckedIntegerDomain> operand = operandDomain(index);
    return resultDomain && operand &&
           (*operand == *resultDomain ||
            !integerConversionMayFail(*operand, *resultDomain));
  };
  const auto binary = [&] {
    return MirCheckedIntegerFailureContract{
        .applicable = resultDomain.has_value(),
        .validShape = resultDomain && instruction.operands.size() == 2 &&
                      operandAdmitsExecutionDomain(0) &&
                      operandAdmitsExecutionDomain(1)};
  };
  const auto shift = [&] {
    const std::optional<CheckedIntegerDomain> count = operandDomain(1);
    return MirCheckedIntegerFailureContract{
        .applicable = resultDomain.has_value(),
        .validShape = resultDomain && instruction.operands.size() == 2 &&
                      count && operandAdmitsExecutionDomain(0)};
  };

  MirCheckedIntegerFailureContract contract;
  switch (instruction.operation) {
  case MirOperation::Add:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                          .detail = DefinedFailureDetail::Addition}};
    break;
  case MirOperation::Subtract:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                          .detail = DefinedFailureDetail::Subtraction}};
    break;
  case MirOperation::Multiply:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                          .detail = DefinedFailureDetail::Multiplication}};
    break;
  case MirOperation::Divide:
    contract = binary();
    if (resultDomain && resultDomain->signedValue) {
      contract.outcomes.push_back({.code = DefinedFailureCode::IntegerOverflow,
                                   .detail = DefinedFailureDetail::Division});
    }
    contract.outcomes.push_back(
        {.code = DefinedFailureCode::DivisionByZero,
         .detail = DefinedFailureDetail::IntegerDivision});
    break;
  case MirOperation::Remainder:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::ModuloByZero,
                          .detail = DefinedFailureDetail::IntegerModulo}};
    break;
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftRight: {
    contract = shift();
    const DefinedFailureDetail detail =
        instruction.operation == MirOperation::ShiftLeft
            ? DefinedFailureDetail::LeftShift
            : DefinedFailureDetail::RightShift;
    const std::optional<CheckedIntegerDomain> countDomain = operandDomain(1);
    if (countDomain && countDomain->signedValue) {
      contract.outcomes.push_back(
          {.code = DefinedFailureCode::NegativeShiftCount, .detail = detail});
    }
    contract.outcomes.push_back(
        {.code = DefinedFailureCode::ShiftCountOutOfRange, .detail = detail});
    break;
  }
  case MirOperation::Negate: {
    const std::optional<std::uint64_t> sourceMagnitude =
        sourceIntegerLiteralMagnitude(0);
    const bool exactSignedMinimum =
        resultDomain && resultDomain->signedValue && sourceMagnitude &&
        resultDomain->width != 0 && resultDomain->width <= 64 &&
        *sourceMagnitude == (std::uint64_t{1} << (resultDomain->width - 1U));
    contract = {.applicable = resultDomain.has_value(),
                .validShape = resultDomain && resultDomain->signedValue &&
                              instruction.operands.size() == 1 &&
                              operandAdmitsExecutionDomain(0, true)};
    if (!instruction.programConstantSubstitution && !exactSignedMinimum) {
      contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                            .detail = DefinedFailureDetail::Negation}};
    }
    break;
  }
  case MirOperation::Convert: {
    if (!resultDomain || instruction.operands.size() != 1) {
      return {};
    }
    const std::optional<CheckedIntegerDomain> sourceDomain = operandDomain(0);
    if (!sourceDomain) {
      // Float-to-integer checks and the lossless char bridge remain outside
      // this bounded integer-to-integer verifier family.
      return {};
    }
    contract.applicable = true;
    if (integerConversionMayFail(*sourceDomain, *resultDomain)) {
      contract.outcomes = {
          {.code = DefinedFailureCode::NumericConversionOutOfRange,
           .detail = DefinedFailureDetail::NumericCast}};
    }
    break;
  }
  default:
    return {};
  }

  if (!contract.applicable && !instruction.definedFailure.empty()) {
    // Floating arithmetic never selects an integer-domain record. Mark the
    // represented contract applicable-but-invalid so forged metadata cannot
    // hide behind the noninteger near-miss.
    contract.applicable = true;
    contract.validShape = false;
  }
  return contract;
}

[[nodiscard]] MirVerificationResult verifyMirCheckedIntegerFailureContracts(
    const MirProgram &program, const MirBody &body, std::size_t owner) {
  const auto fullExpressionRoot = [&](const MirInstruction &instruction) {
    return std::any_of(
        body.fullExpressions.begin(), body.fullExpressions.end(),
        [&](const MirFullExpression &expression) {
          return std::find(expression.roots.begin(), expression.roots.end(),
                           instruction.hirValue) != expression.roots.end();
        });
  };
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const MirCheckedIntegerFailureContract contract =
          checkedIntegerFailureContract(body, instruction);
      if (!contract.applicable) {
        continue;
      }
      if (!contract.validShape) {
        return failure(body, owner,
                       "checked integer operation does not retain its exact "
                       "fixed-width operand and result domains",
                       block.id, instruction.id);
      }

      const bool exactLocalOperation =
          instruction.definedFailure.propagation ==
              FailurePropagationKind::None &&
          (contract.outcomes.empty()
               ? instruction.definedFailure.localOrigins.empty() &&
                     instruction.localFailureSites.empty()
               : instruction.definedFailure.localOrigins.size() == 1 &&
                     instruction.definedFailure.localOrigins.front().outcomes ==
                         contract.outcomes &&
                     instruction.localFailureSites.size() == 1 &&
                     instruction.localFailureSites.front() != 0);
      if (!exactLocalOperation) {
        return failure(body, owner,
                       "checked integer operation does not retain its exact "
                       "canonical local failure outcomes",
                       block.id, instruction.id);
      }

      std::vector<const MirFailureRecord *> records;
      for (const MirFailureRecord &record : body.failureRecords) {
        if (record.producerInstruction == instruction.id) {
          records.push_back(&record);
        }
      }
      if (contract.outcomes.empty()) {
        if (!records.empty()) {
          return failure(body, owner,
                         "non-failing integer conversion retains a stale "
                         "failure record",
                         block.id, instruction.id);
        }
        continue;
      }

      const DefinedFailureOrigin &origin =
          instruction.definedFailure.localOrigins.front();
      const FailureSiteId site = instruction.localFailureSites.front();
      const auto assignment =
          std::find_if(program.failureMetadata().assignments().begin(),
                       program.failureMetadata().assignments().end(),
                       [&](const FailureOriginAssignment &candidate) {
                         return candidate.sourceUnit == origin.sourceUnit &&
                                candidate.start == origin.start &&
                                candidate.end == origin.end;
                       });
      if (assignment == program.failureMetadata().assignments().end() ||
          assignment->line != origin.line || assignment->site != site ||
          std::any_of(contract.outcomes.begin(), contract.outcomes.end(),
                      [&](DefinedFailureOutcome outcome) {
                        return std::find(assignment->outcomes.begin(),
                                         assignment->outcomes.end(),
                                         outcome) == assignment->outcomes.end();
                      })) {
        return failure(body, owner,
                       "checked integer operation does not retain its exact "
                       "artifact-local origin and site identity",
                       block.id, instruction.id);
      }

      if ((mirBodyRoutesFailureEdges(body) &&
           fullExpressionRoot(instruction)) ||
          !records.empty()) {
        if (records.size() != 1) {
          return failure(body, owner,
                         "checked integer operation must produce exactly one "
                         "fixed failure record",
                         block.id, instruction.id);
        }
        const MirFailureRecord &record = *records.front();
        const MirBlock *producer = body.findBlock(record.producerBlock);
        const MirBlock *parameter = body.findBlock(record.parameterBlock);
        const auto endpointCount = static_cast<std::size_t>(std::count_if(
            body.blocks.begin(), body.blocks.end(),
            [&](const MirBlock &candidate) {
              const bool exactKind =
                  body.kind == MirBodyKind::HostedStartup
                      ? candidate.terminator.kind ==
                            MirTerminatorKind::ContainFailure
                      : candidate.terminator.kind ==
                            MirTerminatorKind::PropagateFailure;
              return exactKind && candidate.activeFailure == record.id &&
                     candidate.terminator.failureRecord == record.id;
            }));
        if (producer == nullptr || parameter == nullptr || producer != &block ||
            record.producerInstruction != instruction.id ||
            producer->terminator.kind != MirTerminatorKind::Invoke ||
            producer->terminator.invokeInstruction != instruction.id ||
            producer->terminator.failureRecord != record.id ||
            producer->terminator.elseTarget != parameter->id ||
            parameter->failureParameter != record.id ||
            parameter->activeFailure != record.id || endpointCount != 1) {
          return failure(body, owner,
                         "checked integer operation does not retain its exact "
                         "invoke, failure parameter, and terminal chain",
                         block.id, instruction.id);
        }
      }
    }
  }
  return {};
}

} // namespace

static std::vector<bool>
deriveMirFunctionDefinedFailureEffects(const MirProgram &program,
                                       std::vector<bool> *destructorEffects) {
  const auto scalarType = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Bool:
    case SemanticType::Char:
    case SemanticType::NullPtr:
      return true;
    default:
      return false;
    }
  };
  const auto scalarInfo = [&](const ExpressionInfo &info) {
    return scalarType(info.type) && info.traits.drop == DropKind::Trivial &&
           !info.traits.containsBorrowedState;
  };
  const auto rawPointerType = [](const SemanticType &type) {
    return type.kind == SemanticType::RawPointer &&
           type.arguments.size() == 1 &&
           type.arguments.front() != SemanticType::Void;
  };
  const auto rawPointerOffsetType = [](const SemanticType &type) {
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
  const auto passiveCAbiRecordType = [&](const SemanticType &type) {
    if (type.kind != SemanticType::Class) {
      return false;
    }
    const MirClassInstance *selected = nullptr;
    for (const MirClassInstance &candidate : program.classInstances()) {
      if (candidate.type != type) {
        continue;
      }
      if (selected != nullptr) {
        return false;
      }
      selected = &candidate;
    }
    return selected != nullptr && selected->cAbiRecord &&
           selected->cAbiLayout.has_value() &&
           !selected->unionLayout.has_value() &&
           !selected->requiresActiveDropState &&
           !selected->requiresActiveCleanup;
  };
  // Passive values cannot raise merely by being loaded, copied, or passed.
  // `[[c_abi]]` records are part of that set by their verified
  // standard-layout/trivially-copyable contract. This says nothing about
  // dereferencing a raw pointer; those operations are admitted separately
  // through exact verified MIR shapes below.
  const auto passiveType = [&](const SemanticType &type) {
    return scalarType(type) || type.kind == SemanticType::StringView ||
           type.kind == SemanticType::CString || rawPointerType(type) ||
           passiveCAbiRecordType(type);
  };
  const auto passiveInfo = [&](const ExpressionInfo &info) {
    return passiveType(info.type) && info.traits.drop == DropKind::Trivial &&
           !info.traits.containsBorrowedState;
  };
  // A C-linkage or runtime-binding target carries
  // FailurePropagationKind::None by language contract: the native surface
  // has no defined-failure channel, so the call proves failure-free without
  // recursing into a body that does not exist.
  const auto nativeNoFailureTarget = [](const MirFunctionInstance &target) {
    return target.linkage == LanguageLinkage::C ||
           target.definitionKind ==
               MirFunctionInstance::DefinitionKind::RuntimeBinding;
  };
  const auto wrappingIntrinsic = [](IntrinsicKind intrinsic) {
    switch (intrinsic) {
    case IntrinsicKind::IntegerWrappingAdd:
    case IntrinsicKind::IntegerWrappingSubtract:
    case IntrinsicKind::IntegerWrappingMultiply:
    case IntrinsicKind::IntegerSaturatingAdd:
    case IntrinsicKind::IntegerSaturatingSubtract:
    case IntrinsicKind::IntegerSaturatingMultiply:
      return true;
    default:
      return false;
    }
  };
  const auto validStaticDispatchOwner = [&](const MirInstruction &instruction) {
    if (instruction.dispatchOwner == SemanticType::Unknown) {
      return true;
    }
    const MirFunctionInstance *target =
        instruction.kind == MirInstructionKind::Call &&
                instruction.functionTarget
            ? program.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    const MirClassInstance *owner =
        target != nullptr && target->staticMember && target->owner
            ? program.findClassInstance(*target->owner)
            : nullptr;
    return instruction.dispatch == CallDispatch::Static &&
           !instruction.receiver && owner != nullptr &&
           instruction.dispatchOwner == owner->type;
  };
  const auto scalarOwnership = [](const std::optional<OwnershipEvent> &event) {
    return !event ||
           ((event->kind == OwnershipEventKind::Read ||
             event->kind == OwnershipEventKind::Reinitialize) &&
            event->before == OwnershipStateSet::Available &&
            event->after == OwnershipStateSet::Available && event->reachable);
  };
  const auto scalarOperation = [](MirOperation operation) {
    switch (operation) {
    case MirOperation::Literal:
    case MirOperation::Identity:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Positive:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
      return true;
    default:
      return false;
    }
  };
  const auto noDefinedFailureDestructor = [&](HirDestructorInstanceId id) {
    const MirDestructorInstance *destructor =
        program.findDestructorInstance(id);
    if (destructor == nullptr || destructor->id != id ||
        destructor->owner == 0 ||
        destructor->definitionKind != MirDefinitionKind::Source ||
        destructor->body.kind != MirBodyKind::Destructor ||
        destructor->body.returnType != SemanticType::Void ||
        destructor->body.blocks.empty() || !destructor->body.loans.empty() ||
        !destructor->body.dropObligations.empty() ||
        !destructor->body.cleanupBoundaries.empty() ||
        !destructor->body.failureRecords.empty() ||
        !verifyMirBody(destructor->body, destructor->id).valid()) {
      return false;
    }
    const MirBody &body = destructor->body;
    if (!std::all_of(body.places.begin(), body.places.end(),
                     [&](const MirPlace &place) {
                       const bool storage =
                           place.root == MirPlaceRootKind::Symbol &&
                           place.binding == 0 && place.symbol != 0 &&
                           place.capture == 0 && place.temporary == 0 &&
                           place.value == 0 && place.loan == 0 &&
                           place.projections.empty() && !place.key &&
                           !place.initiallyAvailable &&
                           scalarType(place.type) &&
                           place.traits.drop == DropKind::Trivial &&
                           !place.traits.containsBorrowedState;
                       // A destructor reading one of its receiver's scalar
                       // fields cannot raise; writes stay confined to storage
                       // places below.
                       const bool receiverField =
                           place.root == MirPlaceRootKind::This &&
                           place.binding == 0 && place.symbol == 0 &&
                           place.capture == 0 && place.temporary == 0 &&
                           place.value == 0 && place.loan == 0 &&
                           !place.initiallyAvailable &&
                           ((place.projections.empty() &&
                             place.type.kind == SemanticType::Class) ||
                            (place.projections.size() == 1 &&
                             place.projections.front().kind ==
                                 MirProjectionKind::Field &&
                             scalarType(place.type) &&
                             place.traits.drop == DropKind::Trivial &&
                             !place.traits.containsBorrowedState));
                       return storage || receiverField;
                     }) ||
        !std::all_of(
            body.values.begin(), body.values.end(),
            [&](const MirValue &value) { return scalarInfo(value.info); })) {
      return false;
    }
    for (const MirBlock &block : body.blocks) {
      const MirTerminator &terminator = block.terminator;
      const bool exitShape =
          block.failureParameter == 0 && terminator.invokeInstruction == 0 &&
          terminator.failureRecord == 0 && terminator.switchTargets.empty() &&
          terminator.successLifecycle.empty() &&
          ((terminator.kind == MirTerminatorKind::Goto &&
            terminator.target != 0 && !terminator.value) ||
           (terminator.kind == MirTerminatorKind::Branch && terminator.value &&
            terminator.value->kind == MirOperandKind::Value) ||
           (terminator.kind == MirTerminatorKind::Return && !terminator.value));
      if (!exitShape) {
        return false;
      }
    }
    for (const MirBlock &enclosing : body.blocks)
      for (const MirInstruction &instruction : enclosing.instructions) {
        const bool common =
            instruction.unsafeOperation == UnsafeOperationKind::None &&
            !instruction.rawMemoryAccess && instruction.callSite == 0 &&
            instruction.constructorInitializer == 0 &&
            !instruction.callInputRole && instruction.callInputIndex == 0 &&
            instruction.callInputKind == HirCallInputKind::Value &&
            !instruction.preparedParameterDrop &&
            !instruction.successResultDrop &&
            !instruction.successResultDestination && !instruction.receiver &&
            instruction.parameterTypes.empty() &&
            instruction.closureCaptureTypes.empty() &&
            instruction.closureCaptureModes.empty() && !instruction.loan &&
            instruction.borrowOrigin == BorrowOriginKind::None &&
            instruction.borrowArgument == 0 &&
            instruction.borrowAccess == AccessMode::ReadOnly &&
            !instruction.borrowPlace && !instruction.enumOwner &&
            !instruction.enumValue && !instruction.enumVariant &&
            !instruction.payloadIndex &&
            instruction.intrinsic == IntrinsicKind::None &&
            instruction.synchronization.kind ==
                SynchronizationOperationKind::None &&
            instruction.definedFailure.empty() &&
            instruction.localFailureSites.empty() &&
            instruction.dispatch == CallDispatch::Static &&
            instruction.dispatchOwner == SemanticType::Unknown &&
            !instruction.functionTarget && !instruction.constructorTarget &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            !instruction.lambdaTarget &&
            instruction.callableArguments.empty() &&
            !instruction.callableBoundary && !instruction.callableInvocation &&
            !instruction.ownership;
        if (!common) {
          return false;
        }
        if (instruction.kind == MirInstructionKind::Compute) {
          const bool literalForm =
              instruction.operation == MirOperation::Literal &&
              instruction.literal.has_value() && instruction.operands.empty();
          const bool operandForm =
              instruction.operation != MirOperation::Literal &&
              scalarOperation(instruction.operation) && !instruction.literal &&
              std::all_of(instruction.operands.begin(),
                          instruction.operands.end(),
                          [](const MirOperand &operand) {
                            return operand.kind == MirOperandKind::Value;
                          });
          if (!instruction.result || instruction.destination ||
              !scalarInfo(instruction.info) || (!literalForm && !operandForm) ||
              !instruction.lifecycle.empty() ||
              instruction.fullExpressionEnd != 0 ||
              instruction.cleanupBoundaryEnd != 0) {
            return false;
          }
          continue;
        }
        if (instruction.kind == MirInstructionKind::Load) {
          if (!instruction.result || instruction.destination ||
              !scalarInfo(instruction.info) ||
              instruction.operation != MirOperation::None ||
              instruction.literal || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != MirOperandKind::Copy ||
              !instruction.lifecycle.empty() ||
              instruction.fullExpressionEnd != 0 ||
              instruction.cleanupBoundaryEnd != 0) {
            return false;
          }
          continue;
        }
        if (instruction.kind == MirInstructionKind::Assign) {
          const MirPlace *place = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
          if (!instruction.result || place == nullptr ||
              place->root != MirPlaceRootKind::Symbol ||
              !scalarInfo(instruction.info) ||
              instruction.operation != MirOperation::Assign ||
              instruction.literal || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != MirOperandKind::Value ||
              !instruction.lifecycle.empty() ||
              instruction.fullExpressionEnd != 0 ||
              instruction.cleanupBoundaryEnd != 0) {
            return false;
          }
          continue;
        }
        if (instruction.kind != MirInstructionKind::Lifecycle ||
            instruction.result || instruction.destination ||
            !instruction.operands.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !instruction.lifecycle.empty() ||
            instruction.fullExpressionEnd == 0 ||
            instruction.cleanupBoundaryEnd != 0) {
          return false;
        }
      }
    return true;
  };
  if (destructorEffects != nullptr) {
    destructorEffects->assign(program.destructorInstances().size(), true);
    for (const MirDestructorInstance &destructor :
         program.destructorInstances()) {
      if (destructor.id != 0 && destructor.id <= destructorEffects->size() &&
          noDefinedFailureDestructor(destructor.id)) {
        (*destructorEffects)[destructor.id - 1] = false;
      }
    }
  }
  const auto cleanupClass =
      [&](const SemanticType &type) -> const MirClassInstance * {
    if (type.kind != SemanticType::Class) {
      return nullptr;
    }
    const MirClassInstance *selected = nullptr;
    for (const MirClassInstance &candidate : program.classInstances()) {
      if (candidate.type != type) {
        continue;
      }
      if (selected != nullptr) {
        return nullptr;
      }
      selected = &candidate;
    }
    return selected != nullptr && selected->id != 0 &&
                   selected->declaration != 0 && selected->bases.empty() &&
                   selected->structuralBases.empty() && !selected->abstract &&
                   !selected->polymorphic && !selected->cAbiRecord &&
                   !selected->cAbiLayout && !selected->unionLayout &&
                   selected->declaredFields.empty() &&
                   selected->fields.empty() &&
                   selected->fieldDropOrder.empty() && selected->destructor &&
                   selected->requiresActiveDropState &&
                   selected->requiresActiveCleanup &&
                   noDefinedFailureDestructor(*selected->destructor)
               ? selected
               : nullptr;
  };
  const auto classDefaultCleanupNoFailure = [&](const MirFunctionInstance
                                                    &function) {
    const MirBody &body = function.body;
    if (function.id == 0 || function.declaration == 0 ||
        function.definitionKind !=
            MirFunctionInstance::DefinitionKind::Source ||
        function.linkage != LanguageLinkage::Gti ||
        !function.externalSymbol.empty() || !function.parameterTypes.empty() ||
        !function.parameterBindings.empty() ||
        !scalarType(function.returnType) ||
        body.kind != MirBodyKind::Function ||
        body.returnType != function.returnType || body.blocks.size() != 1 ||
        !body.loans.empty() || !body.failureRecords.empty() ||
        body.cleanupBoundaries.size() != 1 || body.dropObligations.empty() ||
        body.dropObligations.size() % 2 != 0 ||
        !verifyMirBody(body, function.id).valid()) {
      return false;
    }
    const std::size_t localCount = body.dropObligations.size() / 2;
    const MirBlock &block = body.blocks.front();
    if (!block.reachable || block.failureParameter != 0 ||
        body.places.size() != localCount * 2 + 1 ||
        body.values.size() != localCount + 1 ||
        block.instructions.size() != localCount * 4 + 3 ||
        block.terminator.kind != MirTerminatorKind::Return ||
        !block.terminator.value ||
        block.terminator.value->kind != MirOperandKind::Value ||
        block.terminator.invokeInstruction != 0 ||
        block.terminator.failureRecord != 0 || block.terminator.target != 0 ||
        block.terminator.elseTarget != 0 ||
        !block.terminator.switchTargets.empty() ||
        !block.terminator.successLifecycle.empty()) {
      return false;
    }

    std::unordered_set<MirPlaceId> usedPlaces;
    std::unordered_set<MirValueId> usedValues;
    std::vector<MirDropObligationId> lexicalDrops;
    for (std::size_t index = 0; index < localCount; ++index) {
      const MirInstruction &construct = block.instructions[index * 3];
      const MirInstruction &initialize = block.instructions[index * 3 + 1];
      const MirInstruction &boundary = block.instructions[index * 3 + 2];
      const MirValue *value =
          construct.result ? body.findValue(*construct.result) : nullptr;
      const MirPlace *destination =
          initialize.destination ? body.findPlace(*initialize.destination)
                                 : nullptr;
      const MirDropObligation *valueDrop =
          construct.lifecycle.size() == 1
              ? body.findDropObligation(construct.lifecycle.front().target)
              : nullptr;
      const MirDropObligation *bindingDrop =
          initialize.lifecycle.size() == 1
              ? body.findDropObligation(initialize.lifecycle.front().target)
              : nullptr;
      const MirPlace *valuePlace =
          valueDrop == nullptr ? nullptr : body.findPlace(valueDrop->place);
      const MirClassInstance *classInstance =
          value == nullptr ? nullptr : cleanupClass(value->info.type);
      if (construct.kind != MirInstructionKind::Construct ||
          !construct.result || construct.destination ||
          !construct.operands.empty() ||
          construct.operation != MirOperation::None || construct.literal ||
          construct.constructorTarget || !construct.definedFailure.empty() ||
          !construct.localFailureSites.empty() || construct.ownership ||
          construct.fullExpressionEnd != 0 ||
          construct.cleanupBoundaryEnd != 0 ||
          construct.lifecycle.size() != 1 || value == nullptr ||
          classInstance == nullptr || valueDrop == nullptr ||
          valuePlace == nullptr ||
          construct.lifecycle.front().kind !=
              MirLifecycleEventKind::Initialize ||
          construct.lifecycle.front().source != 0 ||
          construct.lifecycle.front().conditional ||
          construct.lifecycle.front().failureCleanup ||
          valueDrop->kind != MirDropObligationKind::Value ||
          valueDrop->dropType.classInstance != classInstance->id ||
          valueDrop->dropType.destructor != classInstance->destructor ||
          !valueDrop->dropType.requiresActiveCleanup ||
          valuePlace->root != MirPlaceRootKind::Value ||
          valuePlace->value != value->id || valuePlace->key ||
          initialize.kind != MirInstructionKind::Initialize ||
          initialize.result || destination == nullptr ||
          initialize.operands.size() != 1 ||
          initialize.operands.front().kind != MirOperandKind::Value ||
          initialize.operands.front().value != value->id ||
          initialize.operation != MirOperation::None ||
          !initialize.definedFailure.empty() || initialize.ownership ||
          initialize.lifecycle.size() != 1 || bindingDrop == nullptr ||
          initialize.lifecycle.front().kind !=
              MirLifecycleEventKind::Reparent ||
          initialize.lifecycle.front().source != valueDrop->id ||
          initialize.lifecycle.front().target != bindingDrop->id ||
          initialize.lifecycle.front().conditional ||
          initialize.lifecycle.front().failureCleanup ||
          bindingDrop->kind != MirDropObligationKind::Binding ||
          bindingDrop->place != destination->id ||
          bindingDrop->dropType.classInstance != classInstance->id ||
          bindingDrop->dropType.destructor != classInstance->destructor ||
          !bindingDrop->dropType.requiresActiveCleanup ||
          destination->root != MirPlaceRootKind::Binding ||
          destination->type != value->info.type ||
          boundary.kind != MirInstructionKind::Lifecycle || boundary.result ||
          boundary.destination || !boundary.operands.empty() ||
          !boundary.definedFailure.empty() || boundary.ownership ||
          !boundary.lifecycle.empty() || boundary.fullExpressionEnd == 0 ||
          boundary.cleanupBoundaryEnd != 0 ||
          !usedPlaces.insert(valuePlace->id).second ||
          !usedPlaces.insert(destination->id).second ||
          !usedValues.insert(value->id).second) {
        return false;
      }
      lexicalDrops.push_back(bindingDrop->id);
    }

    const std::size_t returnBase = localCount * 3;
    const MirInstruction &load = block.instructions[returnBase];
    const MirInstruction &returnBoundary = block.instructions[returnBase + 1];
    const MirPlace *global = load.operands.size() == 1
                                 ? body.findPlace(load.operands.front().place)
                                 : nullptr;
    const MirValue *returned =
        load.result ? body.findValue(*load.result) : nullptr;
    if (load.kind != MirInstructionKind::Load || !load.result ||
        load.destination || load.operands.size() != 1 ||
        load.operands.front().kind != MirOperandKind::Copy ||
        load.operation != MirOperation::None || load.literal ||
        !load.definedFailure.empty() || !scalarInfo(load.info) ||
        returned == nullptr || global == nullptr ||
        global->root != MirPlaceRootKind::Symbol ||
        !global->projections.empty() || !global->key ||
        global->type != returned->info.type ||
        returnBoundary.kind != MirInstructionKind::Lifecycle ||
        returnBoundary.result || returnBoundary.destination ||
        !returnBoundary.operands.empty() ||
        !returnBoundary.definedFailure.empty() ||
        !returnBoundary.lifecycle.empty() ||
        returnBoundary.fullExpressionEnd == 0 ||
        returnBoundary.cleanupBoundaryEnd != 0 ||
        !usedPlaces.insert(global->id).second ||
        !usedValues.insert(returned->id).second ||
        block.terminator.value->value != returned->id) {
      return false;
    }

    std::size_t instructionIndex = returnBase + 2;
    for (std::size_t reverse = localCount; reverse > 0; --reverse) {
      const MirDropObligation *drop =
          body.findDropObligation(lexicalDrops[reverse - 1]);
      const MirInstruction &instruction =
          block.instructions[instructionIndex++];
      if (drop == nullptr || instruction.kind != MirInstructionKind::Drop ||
          instruction.destination != drop->place || instruction.result ||
          !instruction.operands.empty() ||
          instruction.operation != MirOperation::None ||
          !instruction.definedFailure.empty() || instruction.ownership ||
          instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
          instruction.lifecycle.front().source != drop->id ||
          instruction.lifecycle.front().target != 0 ||
          instruction.lifecycle.front().conditional ||
          instruction.lifecycle.front().failureCleanup) {
        return false;
      }
    }
    const MirInstruction &cleanup = block.instructions[instructionIndex];
    std::reverse(lexicalDrops.begin(), lexicalDrops.end());
    return cleanup.kind == MirInstructionKind::Lifecycle &&
           cleanup.result == std::nullopt && !cleanup.destination &&
           cleanup.operands.empty() && cleanup.definedFailure.empty() &&
           cleanup.lifecycle.empty() && cleanup.fullExpressionEnd == 0 &&
           cleanup.cleanupBoundaryEnd == body.cleanupBoundaries.front().id &&
           body.cleanupBoundaries.front().kind ==
               MirCleanupBoundaryKind::Normal &&
           body.cleanupBoundaries.front().obligations == lexicalDrops &&
           usedPlaces.size() == body.places.size() &&
           usedValues.size() == body.values.size();
  };

  enum class State : unsigned char { Unvisited, Visiting, Complete };
  std::vector<State> states(program.functionInstances().size(),
                            State::Unvisited);
  std::vector<bool> mayRaise(program.functionInstances().size(), true);

  const auto canProveNoFailure = [&](const auto &self,
                                     HirFunctionInstanceId id) -> bool {
    if (id == 0 || id > program.functionInstances().size()) {
      return false;
    }
    State &state = states[id - 1];
    if (state == State::Complete) {
      return !mayRaise[id - 1];
    }
    if (state == State::Visiting) {
      return false;
    }
    state = State::Visiting;

    const MirFunctionInstance &function = program.functionInstances()[id - 1];
    if (classDefaultCleanupNoFailure(function)) {
      state = State::Complete;
      mayRaise[id - 1] = false;
      return true;
    }
    const MirBody &body = function.body;
    // A raw place is a single passive lvalue projection from an exact raw
    // pointer SSA value. The operation remains unsafe, but it has no GTI
    // defined-failure channel: violating its proof obligations is outside the
    // defined-failure model rather than a recoverable language failure.
    const auto rawPlaceOperation =
        [&](const MirPlace *place) -> std::optional<UnsafeOperationKind> {
      if (place == nullptr || place->root != MirPlaceRootKind::Value ||
          place->binding != 0 || place->symbol != 0 || place->capture != 0 ||
          place->temporary != 0 || place->value == 0 || place->loan != 0 ||
          place->projections.size() != 1 || !passiveType(place->type) ||
          place->traits.drop != DropKind::Trivial ||
          place->traits.containsBorrowedState || place->initiallyAvailable) {
        return std::nullopt;
      }
      const MirValue *pointer = body.findValue(place->value);
      if (pointer == nullptr || !passiveInfo(pointer->info) ||
          !rawPointerType(pointer->info.type) ||
          pointer->info.type.arguments.front() != place->type) {
        return std::nullopt;
      }
      const MirPlaceProjection &projection = place->projections.front();
      if (projection.field != 0 || projection.constantIndex ||
          projection.selection != 0) {
        return std::nullopt;
      }
      if (projection.kind == MirProjectionKind::RawDereference &&
          projection.index == 0) {
        return UnsafeOperationKind::RawDereference;
      }
      const MirValue *index =
          projection.index == 0 ? nullptr : body.findValue(projection.index);
      if (projection.kind != MirProjectionKind::RawIndex || index == nullptr ||
          !scalarInfo(index->info) || !rawPointerOffsetType(index->info.type)) {
        return std::nullopt;
      }
      return UnsafeOperationKind::RawIndex;
    };
    const auto exactValueOperand = [&](const MirOperand &operand) {
      const MirValue *value = operand.kind == MirOperandKind::Value
                                  ? body.findValue(operand.value)
                                  : nullptr;
      return value != nullptr && operand.place == 0 && operand.loan == 0 &&
             !operand.literal && operand.type == value->info.type;
    };
    const auto exactRawCompute = [&](const MirInstruction &instruction) {
      if (instruction.kind != MirInstructionKind::Compute ||
          !instruction.result || instruction.destination ||
          instruction.rawMemoryAccess || !passiveInfo(instruction.info)) {
        return false;
      }
      if (instruction.operation == MirOperation::AddressOf) {
        const MirOperand *source = instruction.operands.size() == 1
                                       ? &instruction.operands.front()
                                       : nullptr;
        const MirPlace *place =
            source != nullptr && source->kind == MirOperandKind::Address
                ? body.findPlace(source->place)
                : nullptr;
        return instruction.unsafeOperation == UnsafeOperationKind::AddressOf &&
               source != nullptr && place != nullptr &&
               source->type == place->type &&
               rawPointerType(instruction.info.type) &&
               instruction.info.type.arguments.front() == source->type &&
               instruction.info.type.pointerAccess == place->access;
      }
      if (instruction.unsafeOperation !=
              UnsafeOperationKind::PointerArithmetic ||
          instruction.operands.size() != 2 ||
          !exactValueOperand(instruction.operands[0]) ||
          !exactValueOperand(instruction.operands[1])) {
        return false;
      }
      const auto pointerOperand = [&](const MirOperand &operand,
                                      const SemanticType &type) {
        return operand.type == type && rawPointerType(operand.type);
      };
      const auto offsetOperand = [&](const MirOperand &operand) {
        return rawPointerOffsetType(operand.type);
      };
      switch (instruction.operation) {
      case MirOperation::PointerAdd:
        return rawPointerType(instruction.info.type) &&
               ((pointerOperand(instruction.operands[0],
                                instruction.info.type) &&
                 offsetOperand(instruction.operands[1])) ||
                (offsetOperand(instruction.operands[0]) &&
                 pointerOperand(instruction.operands[1],
                                instruction.info.type)));
      case MirOperation::PointerSubtract:
        return rawPointerType(instruction.info.type) &&
               pointerOperand(instruction.operands[0], instruction.info.type) &&
               offsetOperand(instruction.operands[1]);
      case MirOperation::PointerDifference:
        return instruction.info.type == SemanticType::Int64 &&
               rawPointerType(instruction.operands[0].type) &&
               instruction.operands[0].type == instruction.operands[1].type;
      default:
        return false;
      }
    };
    const auto exactRawMemoryInstruction =
        [&](const MirInstruction &instruction) {
          const MirPlace *place = nullptr;
          if (instruction.kind == MirInstructionKind::Load &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().kind == MirOperandKind::Copy) {
            place = body.findPlace(instruction.operands.front().place);
          } else if (instruction.kind == MirInstructionKind::Assign &&
                     instruction.destination) {
            place = body.findPlace(*instruction.destination);
          }
          const std::optional<UnsafeOperationKind> operation =
              rawPlaceOperation(place);
          return operation && instruction.rawMemoryAccess &&
                 instruction.unsafeOperation == *operation;
        };
    const auto exactRawLifecycle = [](const MirInstruction &instruction) {
      return instruction.kind == MirInstructionKind::Lifecycle &&
             !instruction.rawMemoryAccess &&
             (instruction.unsafeOperation ==
                  UnsafeOperationKind::RawDereference ||
              instruction.unsafeOperation == UnsafeOperationKind::RawIndex);
    };
    const auto exactForeignPointerOperation =
        [&](const MirInstruction &instruction) {
          if (instruction.unsafeOperation !=
                  UnsafeOperationKind::ForeignPointerCall ||
              instruction.rawMemoryAccess || instruction.hirValue == 0) {
            return false;
          }
          const auto nativeCall = [&](const MirInstruction &candidate) {
            const MirFunctionInstance *target =
                candidate.kind == MirInstructionKind::Call &&
                        candidate.functionTarget
                    ? program.findFunctionInstance(*candidate.functionTarget)
                    : nullptr;
            return candidate.hirValue == instruction.hirValue &&
                   candidate.unsafeOperation ==
                       UnsafeOperationKind::ForeignPointerCall &&
                   !candidate.rawMemoryAccess && target != nullptr &&
                   target->linkage == LanguageLinkage::C;
          };
          if (instruction.kind == MirInstructionKind::Call) {
            return nativeCall(instruction);
          }
          return instruction.kind == MirInstructionKind::Lifecycle &&
                 std::any_of(body.blocks.begin(), body.blocks.end(),
                             [&](const MirBlock &candidateBlock) {
                               return std::any_of(
                                   candidateBlock.instructions.begin(),
                                   candidateBlock.instructions.end(),
                                   nativeCall);
                             });
        };
    // A reference parameter cannot raise by itself: the reference is a
    // compile-proven borrow, and what the body reads through it is
    // guarded by the place rules below.
    const auto passiveParameterType = [&](const SemanticType &type) {
      return passiveType(type) || (type.kind == SemanticType::Reference &&
                                   type.arguments.size() == 1);
    };
    bool valid =
        function.id == id && function.declaration != 0 &&
        function.definitionKind ==
            MirFunctionInstance::DefinitionKind::Source &&
        function.linkage == LanguageLinkage::Gti &&
        function.externalSymbol.empty() &&
        function.parameterTypes.size() == function.parameterBindings.size() &&
        std::all_of(function.parameterTypes.begin(),
                    function.parameterTypes.end(), passiveParameterType) &&
        (function.returnType == SemanticType::Void ||
         passiveType(function.returnType)) &&
        body.kind == MirBodyKind::Function &&
        body.returnType == function.returnType && body.loans.empty() &&
        body.dropObligations.empty() && body.cleanupBoundaries.empty() &&
        verifyMirBody(body, function.id).valid();
    for (const MirPlace &place : body.places) {
      const bool ordinaryScalar =
          (place.root == MirPlaceRootKind::Binding ||
           place.root == MirPlaceRootKind::Temporary) &&
          place.projections.empty() && passiveType(place.type) &&
          place.traits.drop == DropKind::Trivial &&
          !place.traits.containsBorrowedState && place.loan == 0;
      // Namespace globals and static fields are represented as Symbol roots.
      // Reading passive storage cannot itself raise a defined failure; any
      // failing initializer is accounted for by program initialization, not
      // by a later load from the initialized object.
      const bool passiveStaticStorage =
          place.root == MirPlaceRootKind::Symbol && place.binding == 0 &&
          place.symbol != 0 && place.capture == 0 && place.temporary == 0 &&
          place.value == 0 && place.loan == 0 && place.projections.empty() &&
          passiveType(place.type) && place.traits.drop == DropKind::Trivial &&
          !place.traits.containsBorrowedState;
      // A read-only receiver's scalar field load cannot raise a defined
      // failure: the bare receiver place is only the projection carrier and
      // the projected place reads one trivially droppable scalar field.
      const bool receiverField =
          place.root == MirPlaceRootKind::This && place.binding == 0 &&
          place.symbol == 0 && place.capture == 0 && place.temporary == 0 &&
          place.value == 0 && place.loan == 0 && !place.initiallyAvailable &&
          function.owner.has_value() &&
          ((place.projections.empty() &&
            place.type.kind == SemanticType::Class) ||
           (place.projections.size() == 1 &&
            place.projections.front().kind == MirProjectionKind::Field &&
            scalarType(place.type) && place.traits.drop == DropKind::Trivial &&
            !place.traits.containsBorrowedState));
      // A reference parameter's scalar field load cannot raise either: the
      // bare reference binding and its dereference are only projection
      // carriers, and the projected place reads one trivially droppable
      // scalar field through the compile-proven borrow.
      const bool referenceParameterField =
          place.root == MirPlaceRootKind::Binding && place.loan == 0 &&
          std::find(function.parameterBindings.begin(),
                    function.parameterBindings.end(),
                    place.binding) != function.parameterBindings.end() &&
          ((place.projections.empty() &&
            place.type.kind == SemanticType::Reference &&
            place.type.arguments.size() == 1) ||
           (!place.projections.empty() &&
            place.projections.front().kind == MirProjectionKind::Dereference &&
            ((place.projections.size() == 1 &&
              place.type.kind == SemanticType::Class) ||
             (place.projections.size() == 2 &&
              place.projections[1].kind == MirProjectionKind::Field &&
              scalarType(place.type) &&
              place.traits.drop == DropKind::Trivial &&
              !place.traits.containsBorrowedState))));
      const bool acceptedPlace = ordinaryScalar || passiveStaticStorage ||
                                 receiverField || referenceParameterField ||
                                 rawPlaceOperation(&place).has_value();
      valid = valid && acceptedPlace;
    }
    for (const MirValue &value : body.values) {
      valid = valid && passiveInfo(value.info);
    }

    std::unordered_set<MirInstructionId> exactCalls;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (!valid) {
          break;
        }
        const bool exactRawComputeInstruction = exactRawCompute(instruction);
        const bool exactRawMemory = exactRawMemoryInstruction(instruction);
        const bool exactRawBoundary = exactRawLifecycle(instruction);
        const bool exactForeignPointer =
            exactForeignPointerOperation(instruction);
        const bool admittedOperationMetadata =
            (instruction.unsafeOperation == UnsafeOperationKind::None &&
             !instruction.rawMemoryAccess) ||
            exactRawComputeInstruction || exactRawMemory || exactRawBoundary ||
            exactForeignPointer;
        const bool common =
            admittedOperationMetadata &&
            instruction.closureCaptureTypes.empty() &&
            instruction.closureCaptureModes.empty() && !instruction.loan &&
            instruction.borrowOrigin == BorrowOriginKind::None &&
            instruction.borrowArgument == 0 &&
            instruction.borrowAccess == AccessMode::ReadOnly &&
            !instruction.borrowPlace && !instruction.enumOwner &&
            !instruction.enumValue && !instruction.enumVariant &&
            !instruction.payloadIndex &&
            (instruction.intrinsic == IntrinsicKind::None ||
             (instruction.kind == MirInstructionKind::Call &&
              wrappingIntrinsic(instruction.intrinsic))) &&
            instruction.synchronization.kind ==
                SynchronizationOperationKind::None &&
            instruction.localFailureSites.empty() &&
            instruction.dispatch == CallDispatch::Static &&
            validStaticDispatchOwner(instruction) &&
            !instruction.constructorTarget &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            !instruction.lambdaTarget &&
            instruction.callableArguments.empty() &&
            !instruction.callableBoundary && !instruction.callableInvocation &&
            instruction.lifecycle.empty() &&
            !instruction.preparedParameterDrop &&
            !instruction.successResultDrop &&
            !instruction.successResultDestination &&
            instruction.cleanupBoundaryEnd == 0 &&
            scalarOwnership(instruction.ownership);
        if (!common || !instruction.definedFailure.localOrigins.empty()) {
          valid = false;
          break;
        }

        switch (instruction.kind) {
        case MirInstructionKind::Compute:
          // Literals and identity copies may carry any passive value. Other
          // ordinary operations stay scalar; raw compute shapes are checked
          // independently above.
          valid = instruction.callSite == 0 && !instruction.callInputRole &&
                  !instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && instruction.result &&
                  (exactRawComputeInstruction ||
                   ((instruction.operation == MirOperation::Literal ||
                     instruction.operation == MirOperation::Identity)
                        ? passiveInfo(instruction.info)
                        : scalarInfo(instruction.info))) &&
                  (scalarOperation(instruction.operation) ||
                   exactRawComputeInstruction) &&
                  instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd == 0 &&
                  (instruction.operation == MirOperation::Literal
                       ? instruction.operands.empty() &&
                             instruction.literal.has_value()
                       : !instruction.literal &&
                             (exactRawComputeInstruction ||
                              std::all_of(instruction.operands.begin(),
                                          instruction.operands.end(),
                                          [](const MirOperand &operand) {
                                            return operand.kind ==
                                                   MirOperandKind::Value;
                                          })));
          break;
        case MirInstructionKind::Load:
          valid = instruction.callSite == 0 && !instruction.callInputRole &&
                  !instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && instruction.result &&
                  passiveInfo(instruction.info) &&
                  instruction.operation == MirOperation::None &&
                  !instruction.literal && instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd == 0 &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().kind == MirOperandKind::Copy &&
                  (instruction.unsafeOperation == UnsafeOperationKind::None ||
                   exactRawMemory);
          break;
        case MirInstructionKind::Initialize:
          valid =
              instruction.callSite == 0 && !instruction.callInputRole &&
              instruction.destination && !instruction.receiver &&
              instruction.parameterTypes.empty() &&
              !instruction.functionTarget && !instruction.result &&
              passiveInfo(instruction.info) &&
              instruction.operation == MirOperation::None &&
              !instruction.literal && instruction.definedFailure.empty() &&
              instruction.fullExpressionEnd == 0 &&
              instruction.operands.size() == 1 &&
              (instruction.operands.front().kind == MirOperandKind::Value ||
               instruction.operands.front().kind == MirOperandKind::Constant);
          break;
        case MirInstructionKind::Assign:
          valid = instruction.callSite == 0 && !instruction.callInputRole &&
                  instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && instruction.result &&
                  passiveInfo(instruction.info) &&
                  instruction.operation == MirOperation::Assign &&
                  !instruction.literal && instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd == 0 &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().kind == MirOperandKind::Value &&
                  (instruction.unsafeOperation == UnsafeOperationKind::None ||
                   exactRawMemory);
          break;
        case MirInstructionKind::Lifecycle:
          valid = instruction.callSite == 0 && !instruction.callInputRole &&
                  !instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && !instruction.result &&
                  instruction.operation == MirOperation::None &&
                  !instruction.literal && instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd != 0 &&
                  instruction.operands.empty() &&
                  (instruction.unsafeOperation == UnsafeOperationKind::None ||
                   exactRawBoundary || exactForeignPointer);
          break;
        case MirInstructionKind::CallInput:
          valid = instruction.callSite != 0 &&
                  instruction.callInputRole == MirCallInputRole::Argument &&
                  instruction.callInputKind == HirCallInputKind::Value &&
                  !instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && instruction.result &&
                  passiveInfo(instruction.info) &&
                  instruction.operation == MirOperation::None &&
                  !instruction.literal && instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd == 0 &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().kind == MirOperandKind::Value;
          break;
        case MirInstructionKind::Call: {
          if (instruction.intrinsic != IntrinsicKind::None) {
            // A wrapping/saturating arithmetic intrinsic carries no failure
            // channel: None propagation, no sites, scalar operands.
            valid =
                wrappingIntrinsic(instruction.intrinsic) &&
                instruction.callSite != 0 && !instruction.callInputRole &&
                !instruction.destination && !instruction.receiver &&
                !instruction.functionTarget && instruction.result &&
                scalarInfo(instruction.info) &&
                instruction.definedFailure.empty() &&
                instruction.definedFailure.propagation ==
                    FailurePropagationKind::None &&
                !instruction.operands.empty() &&
                instruction.operands.size() <= 2 &&
                std::all_of(instruction.operands.begin(),
                            instruction.operands.end(),
                            [&](const MirOperand &operand) {
                              return operand.kind == MirOperandKind::Value &&
                                     scalarType(operand.type);
                            });
            if (valid) {
              exactCalls.insert(instruction.id);
            }
            break;
          }
          const MirFunctionInstance *target =
              instruction.functionTarget
                  ? program.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          valid = instruction.callSite != 0 && !instruction.callInputRole &&
                  !instruction.destination && !instruction.receiver &&
                  instruction.functionTarget && target != nullptr &&
                  instruction.parameterTypes == target->parameterTypes &&
                  instruction.operands.size() ==
                      instruction.parameterTypes.size() &&
                  std::all_of(instruction.parameterTypes.begin(),
                              instruction.parameterTypes.end(), passiveType) &&
                  instruction.operation == MirOperation::None &&
                  !instruction.literal && !instruction.ownership &&
                  (instruction.definedFailure.propagation ==
                       FailurePropagationKind::DirectCall ||
                   instruction.definedFailure.propagation ==
                       FailurePropagationKind::None) &&
                  instruction.fullExpressionEnd == 0 &&
                  instruction.info.type == target->returnType &&
                  (instruction.info.type == SemanticType::Void ||
                   passiveInfo(instruction.info)) &&
                  (instruction.info.type == SemanticType::Void
                       ? !instruction.result
                       : instruction.result.has_value()) &&
                  std::all_of(
                      instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        if (operand.kind != MirOperandKind::Value ||
                            !passiveType(operand.type)) {
                          return false;
                        }
                        const MirValue *value = body.findValue(operand.value);
                        const MirInstruction *input =
                            value == nullptr
                                ? nullptr
                                : definitionFor(body, operand.value);
                        return input != nullptr &&
                               input->kind == MirInstructionKind::CallInput &&
                               input->callSite == instruction.callSite;
                      }) &&
                  (nativeNoFailureTarget(*target)
                       ? instruction.definedFailure.propagation ==
                             FailurePropagationKind::None
                       : self(self, *instruction.functionTarget));
          if (valid) {
            exactCalls.insert(instruction.id);
          }
          break;
        }
        case MirInstructionKind::Move:
        case MirInstructionKind::Borrow:
        case MirInstructionKind::Construct:
        case MirInstructionKind::Drop:
        case MirInstructionKind::EndBorrow:
        case MirInstructionKind::Modify:
        case MirInstructionKind::CallBody:
        case MirInstructionKind::Count:
          valid = false;
          break;
        }
      }
    }

    for (const MirFailureRecord &record : body.failureRecords) {
      const MirBlock *producer = body.findBlock(record.producerBlock);
      const MirBlock *parameter = body.findBlock(record.parameterBlock);
      const auto found =
          producer == nullptr
              ? std::vector<MirInstruction>::const_iterator{}
              : std::find_if(producer->instructions.begin(),
                             producer->instructions.end(),
                             [&](const MirInstruction &instruction) {
                               return instruction.id ==
                                      record.producerInstruction;
                             });
      const MirInstruction *instruction =
          producer == nullptr || found == producer->instructions.end()
              ? nullptr
              : &*found;
      valid =
          valid && instruction != nullptr && parameter != nullptr &&
          exactCalls.contains(record.producerInstruction) &&
          producer->terminator.kind == MirTerminatorKind::Invoke &&
          producer->terminator.invokeInstruction ==
              record.producerInstruction &&
          producer->terminator.failureRecord == record.id &&
          parameter->failureParameter == record.id &&
          parameter->instructions.empty() &&
          parameter->terminator.kind == MirTerminatorKind::PropagateFailure &&
          parameter->terminator.failureRecord == record.id;
    }
    for (const MirBlock &block : body.blocks) {
      if (block.failureParameter != 0) {
        const MirFailureRecord *record =
            body.findFailureRecord(block.failureParameter);
        valid = valid && record != nullptr &&
                record->parameterBlock == block.id &&
                block.instructions.empty() &&
                block.terminator.kind == MirTerminatorKind::PropagateFailure;
        continue;
      }
      switch (block.terminator.kind) {
      case MirTerminatorKind::Goto:
      case MirTerminatorKind::Branch:
      case MirTerminatorKind::Switch:
      case MirTerminatorKind::Return:
      case MirTerminatorKind::Unreachable:
        break;
      case MirTerminatorKind::Invoke:
        valid = valid &&
                exactCalls.contains(block.terminator.invokeInstruction) &&
                block.terminator.failureRecord != 0;
        break;
      case MirTerminatorKind::PropagateFailure:
      case MirTerminatorKind::None:
      case MirTerminatorKind::Exit:
      case MirTerminatorKind::ContainFailure:
      case MirTerminatorKind::TerminateCleanupFailure:
        valid = false;
        break;
      }
    }

    state = State::Complete;
    mayRaise[id - 1] = !valid;
    return valid;
  };

  for (const MirFunctionInstance &function : program.functionInstances()) {
    (void)canProveNoFailure(canProveNoFailure, function.id);
  }
  return mayRaise;
}

namespace {

class MirOwnedFailureClosure {
public:
  MirOwnedFailureClosure(const MirProgram &program,
                         MirDefinedFailureEffects &effects)
      : program(program), effects(effects),
        functionStates(program.functionInstances().size()),
        constructorStates(program.constructorInstances().size()),
        destructorStates(program.destructorInstances().size()) {}

  void derive() {
    for (const MirDestructorInstance &destructor :
         program.destructorInstances()) {
      (void)proveDestructor(destructor.id);
    }
    for (const MirConstructorInstance &constructor :
         program.constructorInstances()) {
      (void)proveConstructor(constructor.id);
    }
    for (const MirFunctionInstance &function : program.functionInstances()) {
      (void)proveFunction(function.id);
    }
  }

private:
  enum class State : unsigned char { Unvisited, Visiting, Complete };

  [[nodiscard]] static bool scalarType(const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Bool:
    case SemanticType::Char:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] static bool scalarTraits(const SemanticTypeTraits &traits,
                                         const SemanticType &type) {
    return scalarType(type) && traits.drop == DropKind::Trivial &&
           !traits.containsBorrowedState;
  }

  [[nodiscard]] static bool
  passiveConstructorValueType(const SemanticType &type) {
    if (scalarType(type)) {
      return true;
    }
    return type.kind == SemanticType::Array && type.arguments.size() == 1 &&
           type.arrayLength != 0 && type.arrayLengthParameterId == 0 &&
           passiveConstructorValueType(type.arguments.front());
  }

  [[nodiscard]] static bool
  passiveConstructorValueTraits(const SemanticTypeTraits &traits,
                                const SemanticType &type) {
    return passiveConstructorValueType(type) &&
           traits.drop == DropKind::Trivial && !traits.containsBorrowedState;
  }

  [[nodiscard]] static bool scalarOperation(MirOperation operation) {
    switch (operation) {
    case MirOperation::Literal:
    case MirOperation::Identity:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Positive:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] const MirClassInstance *
  classForType(const SemanticType &type) const {
    if (type.kind != SemanticType::Class) {
      return nullptr;
    }
    const MirClassInstance *selected = nullptr;
    for (const MirClassInstance &candidate : program.classInstances()) {
      if (candidate.type != type) {
        continue;
      }
      if (selected != nullptr) {
        return nullptr;
      }
      selected = &candidate;
    }
    if (selected == nullptr || selected->id == 0 ||
        selected->declaration == 0 || !selected->bases.empty() ||
        !selected->structuralBases.empty() || selected->abstract ||
        selected->polymorphic || selected->cAbiRecord || selected->cAbiLayout ||
        selected->unionLayout || !selected->fields.empty() ||
        !selected->fieldDropOrder.empty() ||
        !selected->fieldInitializers.values.empty() ||
        !selected->fieldInitializers.loans.empty() ||
        !selected->fieldInitializers.dropObligations.empty() ||
        !selected->fieldInitializers.failureRecords.empty() ||
        !selected->staticFieldInitializers.places.empty() ||
        !selected->staticFieldInitializers.values.empty() ||
        !selected->staticFieldInitializers.loans.empty() ||
        !selected->staticFieldInitializers.dropObligations.empty() ||
        !selected->staticFieldInitializers.failureRecords.empty() ||
        !std::all_of(selected->declaredFields.begin(),
                     selected->declaredFields.end(),
                     [](const MirClassFieldInfo &field) {
                       return field.field != 0 && field.symbol != 0 &&
                              scalarType(field.type) &&
                              field.dropKind == DropKind::Trivial &&
                              !field.requiresActiveCleanup;
                     })) {
      return nullptr;
    }
    if (!selected->destructor || !selected->requiresActiveDropState ||
        !selected->requiresActiveCleanup) {
      return nullptr;
    }
    const MirDestructorInstance *destructor =
        program.findDestructorInstance(*selected->destructor);
    return destructor != nullptr && destructor->owner == selected->id ? selected
                                                                      : nullptr;
  }

  [[nodiscard]] bool eligibleType(const SemanticType &type) const {
    if (type == SemanticType::Void || scalarType(type) ||
        classForType(type) != nullptr) {
      return true;
    }
    return type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
           classForType(type.arguments.front()) != nullptr;
  }

  // A source constructor that initializes every passive scalar/fixed-array
  // field from a matching passive value has no hidden class lifecycle work.
  // In-class defaults are not evaluated for fields explicitly named by that
  // constructor, so they do not make this exact constructor failure-capable.
  // Keep this proof separate from classForType(): that helper admits the
  // destructor-owning closure used by function/drop analysis, while this
  // shape deliberately has no destructor or active cleanup.
  [[nodiscard]] bool
  passiveConstructorOwner(const MirClassInstance &owner) const {
    return owner.id != 0 && owner.declaration != 0 && owner.bases.empty() &&
           owner.structuralBases.empty() && !owner.abstract &&
           !owner.polymorphic && !owner.cAbiRecord && !owner.cAbiLayout &&
           !owner.unionLayout && !owner.destructor &&
           !owner.requiresActiveDropState && !owner.requiresActiveCleanup &&
           owner.fields.empty() && owner.fieldDropOrder.empty() &&
           std::all_of(owner.declaredFields.begin(), owner.declaredFields.end(),
                       [](const MirClassFieldInfo &field) {
                         return field.field != 0 && field.symbol != 0 &&
                                passiveConstructorValueType(field.type) &&
                                field.dropKind == DropKind::Trivial &&
                                !field.requiresActiveCleanup;
                       }) &&
           std::count_if(program.classInstances().begin(),
                         program.classInstances().end(),
                         [&](const MirClassInstance &candidate) {
                           return candidate.type == owner.type;
                         }) == 1;
  }

  [[nodiscard]] static bool
  commonInstruction(const MirInstruction &instruction) {
    return instruction.unsafeOperation == UnsafeOperationKind::None &&
           !instruction.rawMemoryAccess &&
           instruction.closureCaptureTypes.empty() &&
           instruction.closureCaptureModes.empty() && !instruction.loan &&
           instruction.borrowOrigin == BorrowOriginKind::None &&
           instruction.borrowArgument == 0 &&
           instruction.borrowAccess == AccessMode::ReadOnly &&
           !instruction.borrowPlace && !instruction.enumOwner &&
           !instruction.enumValue && !instruction.enumVariant &&
           !instruction.payloadIndex &&
           (instruction.intrinsic == IntrinsicKind::None ||
            (instruction.kind == MirInstructionKind::Move &&
             instruction.intrinsic == IntrinsicKind::Move)) &&
           instruction.synchronization.kind ==
               SynchronizationOperationKind::None &&
           instruction.definedFailure.localOrigins.empty() &&
           instruction.localFailureSites.empty() &&
           instruction.dispatch == CallDispatch::Static &&
           instruction.dispatchOwner == SemanticType::Unknown &&
           !instruction.lambdaTarget && instruction.callableArguments.empty() &&
           !instruction.callableBoundary && !instruction.callableInvocation;
  }

  [[nodiscard]] bool proveFunction(HirFunctionInstanceId id);
  [[nodiscard]] bool proveConstructor(HirConstructorInstanceId id);
  [[nodiscard]] bool proveDestructor(HirDestructorInstanceId id);
  [[nodiscard]] bool
  proveSingleFieldTransferConstructor(const MirConstructorInstance &constructor,
                                      const MirClassInstance &owner) const;
  [[nodiscard]] bool proveBody(const MirBody &body, MirBodyKind kind,
                               std::size_t owner,
                               const MirClassInstance *ownerClass);

  const MirProgram &program;
  MirDefinedFailureEffects &effects;
  std::vector<State> functionStates;
  std::vector<State> constructorStates;
  std::vector<State> destructorStates;
};

bool MirOwnedFailureClosure::proveBody(const MirBody &body, MirBodyKind kind,
                                       std::size_t owner,
                                       const MirClassInstance *ownerClass) {
  if (body.kind != kind || body.blocks.empty() || !body.loans.empty() ||
      !verifyMirBody(body, owner).valid()) {
    return false;
  }
  const auto eligibleBodyType = [&](const SemanticType &type) {
    return eligibleType(type) ||
           (kind == MirBodyKind::Constructor && ownerClass != nullptr &&
            passiveConstructorOwner(*ownerClass) &&
            (type == ownerClass->type || passiveConstructorValueType(type)));
  };
  for (const MirPlace &place : body.places) {
    if (!eligibleBodyType(place.type) || place.root == MirPlaceRootKind::Loan ||
        place.loan != 0 || place.capture != 0) {
      return false;
    }
    if (passiveConstructorValueType(place.type) &&
        !passiveConstructorValueTraits(place.traits, place.type)) {
      return false;
    }
    if (place.root == MirPlaceRootKind::This) {
      if (ownerClass == nullptr || place.projections.size() > 1 ||
          (!place.projections.empty() &&
           (place.projections.front().kind != MirProjectionKind::Field ||
            std::none_of(ownerClass->declaredFields.begin(),
                         ownerClass->declaredFields.end(),
                         [&](const MirClassFieldInfo &field) {
                           return field.symbol ==
                                      place.projections.front().field &&
                                  field.type == place.type;
                         })))) {
        return false;
      }
    } else if (!place.projections.empty()) {
      return false;
    }
  }
  for (const MirValue &value : body.values) {
    if (!eligibleBodyType(value.info.type) ||
        (passiveConstructorValueType(value.info.type) &&
         !passiveConstructorValueTraits(value.info.traits, value.info.type))) {
      return false;
    }
  }
  for (const MirDropObligation &drop : body.dropObligations) {
    const MirClassInstance *dropClass = classForType(drop.dropType.type);
    if (dropClass == nullptr || drop.dropType.classInstance != dropClass->id ||
        drop.dropType.destructor != dropClass->destructor ||
        !drop.dropType.requiresActiveCleanup || !dropClass->destructor ||
        !proveDestructor(*dropClass->destructor)) {
      return false;
    }
  }

  std::unordered_set<MirInstructionId> provedInvokeEdges;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!commonInstruction(instruction)) {
        return false;
      }
      switch (instruction.kind) {
      case MirInstructionKind::Compute: {
        const bool passiveAggregate =
            kind == MirBodyKind::Constructor && ownerClass != nullptr &&
            passiveConstructorOwner(*ownerClass) &&
            instruction.operation == MirOperation::Aggregate &&
            instruction.info.type.kind == SemanticType::Array &&
            instruction.info.type.arguments.size() == 1 &&
            instruction.info.type.arrayLengthParameterId == 0 &&
            instruction.operands.size() == instruction.info.type.arrayLength &&
            passiveConstructorValueTraits(instruction.info.traits,
                                          instruction.info.type) &&
            std::all_of(instruction.operands.begin(),
                        instruction.operands.end(),
                        [&](const MirOperand &operand) {
                          return operand.kind == MirOperandKind::Value &&
                                 operand.type ==
                                     instruction.info.type.arguments.front();
                        });
        const bool scalar =
            scalarTraits(instruction.info.traits, instruction.info.type) &&
            scalarOperation(instruction.operation);
        if (!instruction.result || instruction.destination ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.definedFailure.propagation !=
                FailurePropagationKind::None ||
            (!scalar && !passiveAggregate) ||
            (instruction.operation == MirOperation::Literal
                 ? !instruction.literal || !instruction.operands.empty()
                 : instruction.literal.has_value())) {
          return false;
        }
        break;
      }
      case MirInstructionKind::Load:
        if (!instruction.result || instruction.destination ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            (instruction.operands.front().kind != MirOperandKind::Copy &&
             instruction.operands.front().kind != MirOperandKind::Address &&
             instruction.operands.front().kind != MirOperandKind::Move)) {
          return false;
        }
        break;
      case MirInstructionKind::Initialize:
        if (!instruction.destination || instruction.result ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            (instruction.operands.front().kind != MirOperandKind::Value &&
             instruction.operands.front().kind != MirOperandKind::Constant)) {
          return false;
        }
        break;
      case MirInstructionKind::Assign:
        if (!instruction.destination || !instruction.result ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.operation != MirOperation::Assign ||
            instruction.literal || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value ||
            !scalarTraits(instruction.info.traits, instruction.info.type)) {
          return false;
        }
        break;
      case MirInstructionKind::Move:
        if (!instruction.result || instruction.destination ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            classForType(instruction.info.type) == nullptr ||
            instruction.lifecycle.empty()) {
          return false;
        }
        break;
      case MirInstructionKind::CallInput: {
        const bool scalar = scalarType(instruction.info.type);
        const bool owned = classForType(instruction.info.type) != nullptr;
        const MirDropObligation *prepared =
            instruction.preparedParameterDrop
                ? body.findDropObligation(*instruction.preparedParameterDrop)
                : nullptr;
        if (instruction.callSite == 0 ||
            instruction.callInputRole != MirCallInputRole::Argument ||
            !instruction.result || instruction.receiver ||
            instruction.functionTarget || instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            (scalar &&
             (instruction.callInputKind != HirCallInputKind::Value ||
              instruction.destination || instruction.preparedParameterDrop ||
              !instruction.lifecycle.empty())) ||
            (owned &&
             (instruction.callInputKind != HirCallInputKind::MoveValue ||
              !instruction.destination || prepared == nullptr ||
              prepared->place != *instruction.destination ||
              instruction.lifecycle.empty())) ||
            (!scalar && !owned)) {
          return false;
        }
        break;
      }
      case MirInstructionKind::Call: {
        const MirFunctionInstance *target =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (instruction.callSite == 0 || instruction.callInputRole ||
            instruction.destination || instruction.receiver ||
            !instruction.functionTarget || target == nullptr ||
            instruction.constructorTarget ||
            instruction.parameterTypes != target->parameterTypes ||
            instruction.operands.size() != target->parameterTypes.size() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !proveFunction(target->id) ||
            (instruction.definedFailure.propagation !=
                 FailurePropagationKind::DirectCall &&
             instruction.definedFailure.propagation !=
                 FailurePropagationKind::None)) {
          return false;
        }
        provedInvokeEdges.insert(instruction.id);
        break;
      }
      case MirInstructionKind::Construct: {
        const MirConstructorInstance *target =
            instruction.constructorTarget ? program.findConstructorInstance(
                                                *instruction.constructorTarget)
                                          : nullptr;
        const MirClassInstance *constructed =
            classForType(instruction.info.type);
        const bool generatedDefault =
            !instruction.constructorTarget && constructed != nullptr &&
            constructed->declaredFields.empty() &&
            instruction.operands.empty() && instruction.parameterTypes.empty();
        if (instruction.callInputRole || instruction.destination ||
            instruction.receiver || instruction.functionTarget ||
            instruction.constructorKind != ConstructorKind::Ordinary ||
            constructed == nullptr ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !instruction.result ||
            instruction.lifecycle.empty() ||
            (!generatedDefault &&
             (target == nullptr || target->owner != constructed->id ||
              instruction.parameterTypes != target->parameterTypes ||
              instruction.operands.size() != target->parameterTypes.size() ||
              !proveConstructor(target->id))) ||
            (instruction.definedFailure.propagation !=
                 FailurePropagationKind::Constructor &&
             instruction.definedFailure.propagation !=
                 FailurePropagationKind::None)) {
          return false;
        }
        provedInvokeEdges.insert(instruction.id);
        break;
      }
      case MirInstructionKind::Drop: {
        const MirDropObligation *drop =
            instruction.lifecycle.size() == 1
                ? body.findDropObligation(instruction.lifecycle.front().source)
                : nullptr;
        if (!instruction.destination || instruction.result ||
            !instruction.operands.empty() || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.receiver ||
            instruction.functionTarget || instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || drop == nullptr ||
            instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop->place != *instruction.destination) {
          return false;
        }
        break;
      }
      case MirInstructionKind::Lifecycle:
        if (instruction.result || instruction.destination ||
            !instruction.operands.empty() || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.receiver ||
            instruction.functionTarget || instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !instruction.lifecycle.empty() ||
            (instruction.fullExpressionEnd == 0 &&
             instruction.cleanupBoundaryEnd == 0)) {
          return false;
        }
        break;
      case MirInstructionKind::Modify:
      case MirInstructionKind::Borrow:
      case MirInstructionKind::EndBorrow:
      case MirInstructionKind::CallBody:
      case MirInstructionKind::Count:
        return false;
      }
    }
  }

  for (const MirBlock &block : body.blocks) {
    if (block.failureParameter != 0) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.failureParameter);
      if (record == nullptr || record->parameterBlock != block.id ||
          !provedInvokeEdges.contains(record->producerInstruction)) {
        return false;
      }
      continue;
    }
    switch (block.terminator.kind) {
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Branch:
    case MirTerminatorKind::Switch:
    case MirTerminatorKind::Return:
    case MirTerminatorKind::Unreachable:
      break;
    case MirTerminatorKind::Invoke:
      if (!provedInvokeEdges.contains(block.terminator.invokeInstruction) ||
          block.terminator.failureRecord == 0) {
        return false;
      }
      break;
    case MirTerminatorKind::PropagateFailure: {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      if (record == nullptr ||
          !provedInvokeEdges.contains(record->producerInstruction)) {
        return false;
      }
      break;
    }
    case MirTerminatorKind::None:
    case MirTerminatorKind::Exit:
    case MirTerminatorKind::ContainFailure:
    case MirTerminatorKind::TerminateCleanupFailure:
      return false;
    }
  }
  return true;
}

bool MirOwnedFailureClosure::proveSingleFieldTransferConstructor(
    const MirConstructorInstance &constructor,
    const MirClassInstance &owner) const {
  if (constructor.id == 0 || constructor.owner != owner.id ||
      constructor.definitionKind != MirDefinitionKind::Source ||
      constructor.borrowOrigin != BorrowOriginKind::None || owner.id == 0 ||
      owner.declaration == 0 || !owner.bases.empty() ||
      !owner.structuralBases.empty() || owner.abstract || owner.polymorphic ||
      owner.cAbiRecord || owner.cAbiLayout || owner.unionLayout ||
      owner.destructor || owner.requiresActiveDropState ||
      owner.destructorStatus != SpecialMemberStatus::Generated ||
      owner.moveConstructor != SpecialMemberStatus::Generated ||
      owner.declaredFields.size() != 1 || owner.fields.size() != 1 ||
      owner.fieldDropOrder.size() != 1 ||
      constructor.parameterTypes.size() != 1 ||
      constructor.parameterBindings.size() != 1 ||
      constructor.initializers.size() != 1) {
    return false;
  }

  const MirClassFieldInfo &field = owner.declaredFields.front();
  const bool intrinsicTransferField =
      field.type.kind == SemanticType::UniqueOwner ||
      field.type.kind == SemanticType::SharedPointer ||
      field.type.kind == SemanticType::Storage ||
      field.type.kind == SemanticType::PrefixStorage;
  const MirConstructorInitializer &initializer =
      constructor.initializers.front();
  if (!intrinsicTransferField || field.dropKind != DropKind::Lexical ||
      !field.requiresActiveCleanup ||
      constructor.parameterTypes.front() != field.type ||
      initializer.kind != ConstructorInitializerTargetKind::Field ||
      initializer.targetType != field.type ||
      initializer.field != field.symbol || initializer.base ||
      initializer.constructorTarget || initializer.arguments.size() != 1 ||
      initializer.storesReference || initializer.generatedDefault ||
      initializer.ownedParameter != 0 ||
      !mirTypeMoveIsDefinedFailureFree(program, field.type)) {
    return false;
  }

  const MirBody &body = constructor.body;
  if (body.kind != MirBodyKind::Constructor ||
      body.returnType != SemanticType::Void || body.blocks.size() != 1 ||
      body.places.size() != 3 || !body.loans.empty() ||
      body.fullExpressions.size() != 1 || body.cleanupBoundaries.size() != 1 ||
      body.dropObligations.size() != 3 || !body.failureRecords.empty() ||
      body.values.size() != 1 || !verifyMirBody(body, constructor.id).valid()) {
    return false;
  }

  const MirPlace *parameterPlace = nullptr;
  const MirPlace *temporaryPlace = nullptr;
  const MirPlace *fieldPlace = nullptr;
  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Binding &&
        place.binding == constructor.parameterBindings.front() &&
        place.projections.empty() && place.type == field.type) {
      parameterPlace = &place;
      continue;
    }
    if (place.root == MirPlaceRootKind::Value && place.projections.empty() &&
        place.type == field.type) {
      temporaryPlace = &place;
      continue;
    }
    if (place.root == MirPlaceRootKind::This && place.projections.size() == 1 &&
        place.projections.front().kind == MirProjectionKind::Field &&
        place.projections.front().field == field.symbol &&
        place.type == field.type) {
      fieldPlace = &place;
    }
  }
  if (parameterPlace == nullptr || temporaryPlace == nullptr ||
      fieldPlace == nullptr || !parameterPlace->initiallyAvailable ||
      temporaryPlace->initiallyAvailable || fieldPlace->initiallyAvailable) {
    return false;
  }

  const MirValue &value = body.values.front();
  if (value.sourceValue != initializer.arguments.front() ||
      value.info.type != field.type || temporaryPlace->value != value.id ||
      temporaryPlace->sourceValue != initializer.arguments.front()) {
    return false;
  }

  const MirDropObligation *parameterDrop = nullptr;
  const MirDropObligation *temporaryDrop = nullptr;
  const MirDropObligation *fieldDrop = nullptr;
  for (const MirDropObligation &drop : body.dropObligations) {
    if (drop.dropType.type != field.type ||
        !drop.dropType.requiresActiveCleanup || drop.dropType.classInstance ||
        drop.dropType.lambdaInstance || drop.dropType.destructor) {
      return false;
    }
    if (drop.kind == MirDropObligationKind::Binding &&
        drop.place == parameterPlace->id &&
        drop.binding == constructor.parameterBindings.front()) {
      parameterDrop = &drop;
    } else if (drop.kind == MirDropObligationKind::Value &&
               drop.place == temporaryPlace->id &&
               drop.value == initializer.arguments.front()) {
      temporaryDrop = &drop;
    } else if (drop.kind == MirDropObligationKind::ConstructionRollback &&
               drop.place == fieldPlace->id) {
      fieldDrop = &drop;
    } else {
      return false;
    }
  }
  if (parameterDrop == nullptr || temporaryDrop == nullptr ||
      fieldDrop == nullptr || !parameterDrop->initiallyActive ||
      temporaryDrop->initiallyActive || fieldDrop->initiallyActive) {
    return false;
  }

  const MirFullExpression &fullExpression = body.fullExpressions.front();
  const MirCleanupBoundary &cleanup = body.cleanupBoundaries.front();
  const MirBlock &block = body.blocks.front();
  if (fullExpression.id == 0 || fullExpression.constructorInitializer != 1 ||
      fullExpression.roots != initializer.arguments || cleanup.id == 0 ||
      cleanup.kind != MirCleanupBoundaryKind::Normal ||
      cleanup.obligations !=
          std::vector<MirDropObligationId>{parameterDrop->id} ||
      !block.reachable || block.failureParameter != 0 ||
      block.activeFailure != 0 || block.instructions.size() != 6 ||
      block.terminator.kind != MirTerminatorKind::Return ||
      block.terminator.value || block.terminator.returnLoan ||
      block.terminator.invokeInstruction != 0 ||
      block.terminator.failureRecord != 0) {
    return false;
  }

  for (const MirInstruction &instruction : block.instructions) {
    if (!commonInstruction(instruction) || instruction.callSite != 0 ||
        instruction.callInputRole || instruction.callInputIndex != 0 ||
        instruction.preparedParameterDrop || instruction.successResultDrop ||
        instruction.successResultDestination || instruction.receiver ||
        !instruction.parameterTypes.empty() || instruction.functionTarget ||
        instruction.constructorTarget || instruction.bodyTarget ||
        instruction.operation != MirOperation::None || instruction.literal ||
        instruction.definedFailure.propagation !=
            FailurePropagationKind::None) {
      return false;
    }
  }

  const MirInstruction &move = block.instructions[0];
  const MirInstruction &initialize = block.instructions[1];
  const MirInstruction &fullEnd = block.instructions[2];
  const MirInstruction &transfer = block.instructions[3];
  const MirInstruction &drop = block.instructions[4];
  const MirInstruction &cleanupEnd = block.instructions[5];
  const auto exactLifecycle =
      [](const MirInstruction &instruction, MirLifecycleEventKind kind,
         MirDropObligationId source, MirDropObligationId target) {
        return instruction.lifecycle.size() == 1 &&
               instruction.lifecycle.front().kind == kind &&
               instruction.lifecycle.front().source == source &&
               instruction.lifecycle.front().target == target &&
               !instruction.lifecycle.front().conditional &&
               !instruction.lifecycle.front().failureCleanup;
      };
  return move.kind == MirInstructionKind::Move && move.result == value.id &&
         move.hirValue == initializer.arguments.front() &&
         move.operands.size() == 1 &&
         move.operands.front().kind == MirOperandKind::Move &&
         move.operands.front().place == parameterPlace->id &&
         move.info.type == field.type &&
         exactLifecycle(move, MirLifecycleEventKind::Move, parameterDrop->id,
                        temporaryDrop->id) &&
         initialize.kind == MirInstructionKind::Initialize &&
         !initialize.result && initialize.destination == fieldPlace->id &&
         initialize.constructorInitializer == 1 &&
         initialize.hirValue == initializer.arguments.front() &&
         initialize.operands.size() == 1 &&
         initialize.operands.front().kind == MirOperandKind::Value &&
         initialize.operands.front().value == value.id &&
         initialize.info.type == field.type &&
         exactLifecycle(initialize, MirLifecycleEventKind::Reparent,
                        temporaryDrop->id, fieldDrop->id) &&
         fullEnd.kind == MirInstructionKind::Lifecycle && !fullEnd.result &&
         !fullEnd.destination && fullEnd.operands.empty() &&
         fullEnd.lifecycle.empty() &&
         fullEnd.fullExpressionEnd == fullExpression.id &&
         fullEnd.cleanupBoundaryEnd == 0 &&
         transfer.kind == MirInstructionKind::Lifecycle && !transfer.result &&
         !transfer.destination && transfer.operands.empty() &&
         exactLifecycle(transfer, MirLifecycleEventKind::TransferOut,
                        fieldDrop->id, 0) &&
         drop.kind == MirInstructionKind::Drop && !drop.result &&
         drop.destination == parameterPlace->id && drop.operands.empty() &&
         drop.info.type == field.type &&
         exactLifecycle(drop, MirLifecycleEventKind::Drop, parameterDrop->id,
                        0) &&
         cleanupEnd.kind == MirInstructionKind::Lifecycle &&
         !cleanupEnd.result && !cleanupEnd.destination &&
         cleanupEnd.operands.empty() && cleanupEnd.lifecycle.empty() &&
         cleanupEnd.fullExpressionEnd == 0 &&
         cleanupEnd.cleanupBoundaryEnd == cleanup.id;
}

bool MirOwnedFailureClosure::proveDestructor(HirDestructorInstanceId id) {
  if (id == 0 || id > program.destructorInstances().size()) {
    return false;
  }
  if (!effects.destructors[id - 1]) {
    return true;
  }
  State &state = destructorStates[id - 1];
  if (state == State::Complete) {
    return !effects.destructors[id - 1];
  }
  if (state == State::Visiting) {
    return false;
  }
  state = State::Visiting;
  const MirDestructorInstance &destructor =
      program.destructorInstances()[id - 1];
  const MirClassInstance *owner = program.findClassInstance(destructor.owner);
  const bool valid =
      destructor.id == id && owner != nullptr &&
      owner->destructor == destructor.id &&
      classForType(owner->type) == owner &&
      destructor.definitionKind == MirDefinitionKind::Source &&
      destructor.body.returnType == SemanticType::Void &&
      proveBody(destructor.body, MirBodyKind::Destructor, destructor.id, owner);
  state = State::Complete;
  effects.destructors[id - 1] = !valid;
  return valid;
}

bool MirOwnedFailureClosure::proveConstructor(HirConstructorInstanceId id) {
  if (id == 0 || id > program.constructorInstances().size()) {
    return false;
  }
  State &state = constructorStates[id - 1];
  if (state == State::Complete) {
    return !effects.constructors[id - 1];
  }
  if (state == State::Visiting) {
    return false;
  }
  state = State::Visiting;
  const MirConstructorInstance &constructor =
      program.constructorInstances()[id - 1];
  const MirClassInstance *owner = program.findClassInstance(constructor.owner);
  bool valid = owner != nullptr &&
               proveSingleFieldTransferConstructor(constructor, *owner);
  if (!valid) {
    valid = constructor.id == id && owner != nullptr &&
            (classForType(owner->type) == owner ||
             passiveConstructorOwner(*owner)) &&
            constructor.definitionKind == MirDefinitionKind::Source &&
            constructor.borrowOrigin == BorrowOriginKind::None &&
            constructor.parameterTypes.size() ==
                constructor.parameterBindings.size() &&
            std::all_of(constructor.parameterTypes.begin(),
                        constructor.parameterTypes.end(), scalarType) &&
            constructor.initializers.size() == owner->declaredFields.size() &&
            constructor.body.fullExpressions.size() ==
                constructor.initializers.size() &&
            proveBody(constructor.body, MirBodyKind::Constructor,
                      constructor.id, owner);
  }

  if (valid && owner != nullptr &&
      proveSingleFieldTransferConstructor(constructor, *owner)) {
    state = State::Complete;
    effects.constructors[id - 1] = false;
    return true;
  }

  std::unordered_set<SymbolId> fields;
  std::unordered_set<std::size_t> stages;
  std::unordered_set<std::size_t> boundaries;
  std::size_t expectedStages = 0;
  for (std::size_t index = 0; valid && index < constructor.initializers.size();
       ++index) {
    const MirConstructorInitializer &initializer =
        constructor.initializers[index];
    const MirClassFieldInfo &field = owner->declaredFields[index];
    valid = initializer.kind == ConstructorInitializerTargetKind::Field &&
            initializer.field == field.symbol &&
            fields.insert(initializer.field).second &&
            initializer.targetType == field.type && !initializer.base &&
            !initializer.constructorTarget &&
            initializer.arguments.size() == 1 && !initializer.storesReference &&
            !initializer.generatedDefault && !initializer.ownedParameter;
    if (!valid) {
      break;
    }
    const MirInstruction *stage = nullptr;
    const MirInstruction *boundary = nullptr;
    for (const MirBlock &block : constructor.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind == MirInstructionKind::Initialize &&
            instruction.constructorInitializer == index + 1) {
          if (stage != nullptr) {
            valid = false;
          }
          stage = &instruction;
        }
        if (instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.fullExpressionEnd == index + 1) {
          if (boundary != nullptr) {
            valid = false;
          }
          boundary = &instruction;
        }
      }
    }
    const MirPlace *destination =
        stage != nullptr && stage->destination
            ? constructor.body.findPlace(*stage->destination)
            : nullptr;
    const MirValue *aggregateValue = nullptr;
    for (const MirValue &value : constructor.body.values) {
      if (value.sourceValue != initializer.arguments.front() ||
          value.info.type != initializer.targetType) {
        continue;
      }
      if (aggregateValue != nullptr) {
        valid = false;
        break;
      }
      aggregateValue = &value;
    }
    const MirInstruction *aggregateDefinition = nullptr;
    if (aggregateValue != nullptr) {
      for (const MirBlock &block : constructor.body.blocks) {
        const auto found =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const MirInstruction &instruction) {
                           return instruction.id == aggregateValue->definition;
                         });
        if (found != block.instructions.end()) {
          aggregateDefinition = &*found;
          break;
        }
      }
    }
    const auto full = std::find_if(
        constructor.body.fullExpressions.begin(),
        constructor.body.fullExpressions.end(),
        [&](const auto &candidate) { return candidate.id == index + 1; });
    const bool scalarStage =
        scalarType(initializer.targetType) && stage != nullptr &&
        !stage->result && stage->destination &&
        stage->hirValue == initializer.arguments.front() &&
        stage->operation == MirOperation::None && !stage->literal &&
        stage->operands.size() == 1 &&
        (stage->operands.front().kind == MirOperandKind::Value ||
         stage->operands.front().kind == MirOperandKind::Constant) &&
        stage->operands.front().type == initializer.targetType &&
        stage->info.type == initializer.targetType && destination != nullptr &&
        destination->root == MirPlaceRootKind::This &&
        destination->projections.size() == 1 &&
        destination->projections.front().kind == MirProjectionKind::Field &&
        destination->projections.front().field == initializer.field &&
        destination->sourceValue == initializer.arguments.front() &&
        destination->type == initializer.targetType &&
        stages.insert(index + 1).second;
    const bool aggregatePublication =
        initializer.targetType.kind == SemanticType::Array &&
        stage == nullptr && aggregateValue != nullptr &&
        aggregateDefinition != nullptr &&
        aggregateDefinition->kind == MirInstructionKind::Compute &&
        aggregateDefinition->operation == MirOperation::Aggregate &&
        aggregateDefinition->result == aggregateValue->id &&
        aggregateDefinition->hirValue == initializer.arguments.front() &&
        passiveConstructorValueTraits(aggregateValue->info.traits,
                                      aggregateValue->info.type);
    if (scalarType(initializer.targetType)) {
      ++expectedStages;
    }
    valid = valid && (scalarStage || aggregatePublication) &&
            boundary != nullptr && boundaries.insert(index + 1).second &&
            full != constructor.body.fullExpressions.end() &&
            full->constructorInitializer == index + 1 &&
            full->roots == initializer.arguments;
  }
  valid = valid && fields.size() == owner->declaredFields.size() &&
          stages.size() == expectedStages &&
          boundaries.size() == constructor.initializers.size();
  state = State::Complete;
  effects.constructors[id - 1] = !valid;
  return valid;
}

bool MirOwnedFailureClosure::proveFunction(HirFunctionInstanceId id) {
  if (id == 0 || id > program.functionInstances().size()) {
    return false;
  }
  if (!effects.functions[id - 1]) {
    return true;
  }
  State &state = functionStates[id - 1];
  if (state == State::Complete) {
    return !effects.functions[id - 1];
  }
  if (state == State::Visiting) {
    return false;
  }
  state = State::Visiting;
  const MirFunctionInstance &function = program.functionInstances()[id - 1];
  const bool valid =
      function.id == id && function.declaration != 0 && !function.owner &&
      !function.staticMember && function.entryKind == ProgramEntryKind::None &&
      !function.entryArgumentAppendTarget &&
      function.definitionKind == MirDefinitionKind::Source &&
      function.linkage == LanguageLinkage::Gti &&
      function.externalSymbol.empty() && !function.virtualMethod &&
      !function.pureVirtual && !function.overrideMethod &&
      function.virtualRoots.empty() && function.callableParameters.empty() &&
      function.returnBorrowOrigin == BorrowOriginKind::None &&
      function.parameterTypes.size() == function.parameterBindings.size() &&
      std::all_of(function.parameterTypes.begin(),
                  function.parameterTypes.end(),
                  [&](const SemanticType &type) {
                    return scalarType(type) || classForType(type) != nullptr;
                  }) &&
      (function.returnType == SemanticType::Void ||
       scalarType(function.returnType)) &&
      proveBody(function.body, MirBodyKind::Function, function.id, nullptr);
  state = State::Complete;
  effects.functions[id - 1] = !valid;
  return valid;
}

[[nodiscard]] bool hasCanonicalBodylessDefinition(
    const MirBody &body, MirBodyKind kind, const SemanticType &returnType,
    const std::vector<SemanticType> &parameterTypes,
    const std::vector<HirBindingId> &parameterBindings) {
  if (body.kind != kind || body.returnType != returnType || body.entry != 1 ||
      body.blocks.size() != 1 || body.places.size() != parameterTypes.size() ||
      parameterBindings.size() != parameterTypes.size() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }

  MirBlock expectedBlock;
  expectedBlock.id = 1;
  expectedBlock.terminator.kind = returnType == SemanticType::Void
                                      ? MirTerminatorKind::Return
                                      : MirTerminatorKind::Unreachable;
  expectedBlock.reachable = true;
  if (body.blocks.front() != expectedBlock) {
    return false;
  }

  for (std::size_t index = 0; index < body.places.size(); ++index) {
    const MirPlace &place = body.places[index];
    const PlaceKey expectedKey{.domain = body.placeDomain,
                               .root = place.symbol};
    if (place.id != index + 1 || place.root != MirPlaceRootKind::Binding ||
        parameterBindings[index] == 0 ||
        place.binding != parameterBindings[index] || place.symbol == 0 ||
        place.capture != 0 || place.temporary != 0 || place.value != 0 ||
        place.loan != 0 || !place.projections.empty() ||
        place.type != parameterTypes[index] || place.sourceValue != 0 ||
        !place.key || *place.key != expectedKey || !place.initiallyAvailable) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
hasCanonicalEmptyStaticInitializerBody(const MirBody &body,
                                       const MirBody &module) {
  if (body.kind != MirBodyKind::StaticFieldInitializers ||
      body.returnType != SemanticType::Void || body.entry != 1 ||
      body.blocks.size() != 1 || !body.places.empty() || !body.loans.empty() ||
      !body.fullExpressions.empty() || !body.cleanupBoundaries.empty() ||
      !body.dropObligations.empty() || !body.failureRecords.empty() ||
      !body.programConstantSubstitutions.empty() || !body.values.empty() ||
      !body.valueUses.empty() ||
      body.placeDomain.snapshot != module.placeDomain.snapshot ||
      body.placeDomain.body == 0 || body.placeDomain == module.placeDomain) {
    return false;
  }
  MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

[[nodiscard]] MirVerificationResult
verifyMirProgramInitialization(const MirProgram &program) {
  MirVerificationResult result;
  const MirProgramInitializationPlan &plan =
      program.programInitializationPlan();
  const MirBody &module = program.module();
  const auto add = [&](std::string message, MirBlockId block = 0,
                       MirInstructionId instruction = 0) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };

  std::unordered_set<SourceUnitId> sourceUnits;
  std::vector<ProgramInitializationStepId> inventoriedSteps;
  for (const MirProgramInitializationUnit &unit : plan.units) {
    if (unit.sourceUnit == 0 || !sourceUnits.insert(unit.sourceUnit).second) {
      add("program-initialization unit inventory is invalid");
    }
    for (const ProgramInitializationStepId stepId : unit.steps) {
      const MirProgramInitializationStep *step = plan.findStep(stepId);
      if (step == nullptr || step->sourceUnit != unit.sourceUnit) {
        add("program-initialization unit names the wrong step");
      }
      inventoriedSteps.push_back(stepId);
    }
  }
  const bool syntheticSource =
      plan.units.empty() && !plan.steps.empty() &&
      std::all_of(plan.steps.begin(), plan.steps.end(),
                  [](const auto &step) { return step.sourceUnit == 0; });
  std::vector<ProgramInitializationStepId> denseSteps;
  denseSteps.reserve(plan.steps.size());
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    denseSteps.push_back(index + 1);
  }
  if ((!syntheticSource && inventoriedSteps != denseSteps) ||
      (syntheticSource && !inventoriedSteps.empty())) {
    add("program-initialization unit/step inventory is not exact and dense");
  }

  const auto canonicalEmptyModule = [&] {
    if (module.kind != MirBodyKind::Module ||
        module.returnType != SemanticType::Void || module.entry != 1 ||
        module.blocks.size() != 1 || !module.places.empty() ||
        !module.loans.empty() || !module.fullExpressions.empty() ||
        !module.cleanupBoundaries.empty() || !module.dropObligations.empty() ||
        !module.failureRecords.empty() ||
        !module.programConstantSubstitutions.empty() ||
        !module.values.empty() || !module.valueUses.empty()) {
      return false;
    }
    MirBlock expected;
    expected.id = 1;
    expected.terminator.kind = MirTerminatorKind::Exit;
    expected.reachable = true;
    return module.blocks.front() == expected;
  };
  if (plan.steps.empty()) {
    if (!canonicalEmptyModule()) {
      add("empty program-initialization plan has a noncanonical module body");
    }
    return result;
  }
  if (module.kind != MirBodyKind::Module ||
      module.returnType != SemanticType::Void || module.entry == 0 ||
      module.entry > module.blocks.size()) {
    add("program-initialization plan has no exact Module/0 body");
    return result;
  }

  struct LocatedInstruction {
    const MirBlock *block = nullptr;
    const MirInstruction *instruction = nullptr;
    std::size_t position = 0;
  };
  std::unordered_map<MirInstructionId, LocatedInstruction> instructions;
  std::unordered_map<HirFullExpressionId, std::vector<LocatedInstruction>>
      fullExpressionMarkers;
  std::vector<std::vector<const MirBlock *>> stepBlocks(plan.steps.size());
  ProgramInitializationStepId previousTag = 0;
  for (const MirBlock &block : module.blocks) {
    if (block.programInitializationStep == 0 ||
        block.programInitializationStep > plan.steps.size() ||
        block.programInitializationStep < previousTag ||
        block.programInitializationStep > previousTag + 1) {
      add("module block has a non-dense program-initialization tag", block.id);
    } else {
      previousTag = block.programInitializationStep;
      stepBlocks[block.programInitializationStep - 1].push_back(&block);
    }
    for (std::size_t position = 0; position < block.instructions.size();
         ++position) {
      const MirInstruction &instruction = block.instructions[position];
      instructions.emplace(instruction.id,
                           LocatedInstruction{.block = &block,
                                              .instruction = &instruction,
                                              .position = position});
      if (instruction.fullExpressionEnd != 0) {
        fullExpressionMarkers[instruction.fullExpressionEnd].push_back(
            {.block = &block,
             .instruction = &instruction,
             .position = position});
      }
    }
  }
  if (previousTag != plan.steps.size()) {
    add("module block tags do not cover every initialization step");
  }
  std::unordered_map<HirBindingId, ProgramInitializationStepId> bindingSteps;
  std::unordered_map<SymbolId, ProgramInitializationStepId> symbolSteps;
  std::unordered_set<MirPlaceId> storagePlaces;
  std::unordered_set<MirInstructionId> storageInitializations;
  std::unordered_set<HirStatementId> statements;
  std::unordered_set<HirValueId> initializerValues;
  std::unordered_set<HirFullExpressionId> fullExpressions;
  std::unordered_map<MirBlockId, std::vector<MirBlockId>> predecessors;
  for (const MirBlock &block : module.blocks) {
    for (const MirBlockId successor : successors(block.terminator)) {
      if (successor != 0 && successor <= module.blocks.size()) {
        predecessors[successor].push_back(block.id);
      }
    }
  }
  const std::optional<MirDominanceInfo> dominance = computeMirDominance(module);
  if (!dominance) {
    add("program-initialization CFG has no usable dominance relation");
  }

  const auto instructionDominates = [&](const LocatedInstruction &earlier,
                                        const LocatedInstruction &later) {
    if (earlier.block == nullptr || later.block == nullptr || !dominance) {
      return false;
    }
    return earlier.block->id == later.block->id
               ? earlier.position < later.position
               : dominance->dominates(earlier.block->id, later.block->id);
  };

  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    const MirProgramInitializationStep &step = plan.steps[index];
    const MirPlace *storage = module.findPlace(step.storagePlace);
    const LocatedInstruction located =
        instructions.contains(step.storageInitialization)
            ? instructions.at(step.storageInitialization)
            : LocatedInstruction{};
    const MirInstruction *initialize = located.instruction;
    const MirBlock *entry = module.findBlock(step.entryBlock);
    const PlaceKey expectedKey{.domain = module.placeDomain,
                               .root = step.symbol};
    const bool realSource = !plan.units.empty();
    if (step.id != index + 1 ||
        (realSource ? !sourceUnits.contains(step.sourceUnit)
                    : step.sourceUnit != 0) ||
        step.symbol == 0 || step.binding == 0 ||
        !bindingSteps.emplace(step.binding, step.id).second ||
        !symbolSteps.emplace(step.symbol, step.id).second ||
        !storagePlaces.insert(step.storagePlace).second ||
        !storageInitializations.insert(step.storageInitialization).second ||
        step.requiresActiveCleanup || storage == nullptr ||
        storage->root != MirPlaceRootKind::Binding ||
        storage->binding != step.binding || storage->symbol != step.symbol ||
        storage->capture != 0 || storage->temporary != 0 ||
        storage->value != 0 || storage->loan != 0 ||
        !storage->projections.empty() ||
        storage->type == SemanticType::Unknown || storage->sourceValue != 0 ||
        !storage->key || *storage->key != expectedKey ||
        storage->initiallyAvailable || entry == nullptr ||
        entry->programInitializationStep != step.id ||
        stepBlocks[index].empty() ||
        stepBlocks[index].front()->id != step.entryBlock ||
        (index == 0 ? module.entry != step.entryBlock
                    : plan.steps[index - 1].entryBlock >= step.entryBlock)) {
      add("program-initialization step has invalid identity or storage",
          step.entryBlock, step.storageInitialization);
    }
    if (step.storageKind == ProgramStorageKind::NamespaceGlobal) {
      if (step.ownerClass != 0) {
        add("namespace-global initialization step has a class owner");
      }
    } else if (step.storageKind == ProgramStorageKind::StaticField) {
      if (step.ownerClass == 0 ||
          program.findClassInstance(step.ownerClass) == nullptr) {
        add("static-field initialization step has an invalid class owner");
      }
    } else {
      add("program-initialization step has an invalid storage kind");
    }

    const ExpressionInfo expectedInfo =
        storage == nullptr ? ExpressionInfo{}
                           : ExpressionInfo{.type = storage->type,
                                            .category = ValueCategory::Place,
                                            .access = storage->access,
                                            .traits = storage->traits};
    MirInstruction canonicalInitialize;
    canonicalInitialize.id = step.storageInitialization;
    canonicalInitialize.kind = MirInstructionKind::Initialize;
    canonicalInitialize.destination = step.storagePlace;
    canonicalInitialize.info = expectedInfo;
    const bool commonInitialize =
        initialize != nullptr && located.block != nullptr &&
        located.block->programInitializationStep == step.id;
    const std::vector<LocatedInstruction> &markers =
        fullExpressionMarkers[step.fullExpression];
    if (step.role == ProgramInitializationStepRole::DataOnly) {
      const bool implicitZero =
          step.dataInitialization ==
              MirProgramDataInitializationKind::ImplicitZero &&
          !step.dataConstant;
      const bool constant =
          step.dataInitialization ==
              MirProgramDataInitializationKind::Constant &&
          step.dataConstant && storage != nullptr &&
          programConstantMatchesType(*step.dataConstant, storage->type);
      if ((!implicitZero && !constant) || step.statement != 0 ||
          step.initializer != 0 || step.fullExpression != 0 ||
          !commonInitialize || *initialize != canonicalInitialize ||
          stepBlocks[index].size() != 1 ||
          stepBlocks[index].front()->instructions.size() != 1 ||
          stepBlocks[index].front()->instructions.front().id !=
              step.storageInitialization) {
        add("data-only initialization step is not its exact zero/constant "
            "storage stage",
            step.entryBlock, step.storageInitialization);
      }
    } else if (step.role == ProgramInitializationStepRole::Initializer) {
      const MirFullExpression *full =
          step.fullExpression == 0 ||
                  step.fullExpression > module.fullExpressions.size()
              ? nullptr
              : &module.fullExpressions[step.fullExpression - 1];
      const auto operandMatchesInitializer = [&] {
        if (initialize == nullptr || initialize->operands.size() != 1) {
          return false;
        }
        const MirOperand &operand = initialize->operands.front();
        if (operand.kind == MirOperandKind::Value) {
          const MirValue *value = module.findValue(operand.value);
          return value != nullptr && value->sourceValue == step.initializer;
        }
        if (operand.kind == MirOperandKind::Loan) {
          const MirLoan *loan = module.findLoan(operand.loan);
          const MirPlace *source =
              loan == nullptr ? nullptr : module.findPlace(loan->source);
          return loan != nullptr && (loan->producedBy == step.initializer ||
                                     (source != nullptr &&
                                      source->sourceValue == step.initializer));
        }
        const MirPlace *place = module.findPlace(operand.place);
        return place != nullptr && place->sourceValue == step.initializer;
      };
      if (initialize != nullptr) {
        canonicalInitialize.hirStatement = step.statement;
        if (initialize->operands.size() == 1) {
          const MirOperand &operand = initialize->operands.front();
          MirOperand canonicalOperand;
          canonicalOperand.kind = operand.kind;
          canonicalOperand.type = operand.type;
          switch (operand.kind) {
          case MirOperandKind::Value:
            canonicalOperand.value = operand.value;
            break;
          case MirOperandKind::Constant:
            canonicalOperand.literal = operand.literal;
            break;
          case MirOperandKind::Address:
          case MirOperandKind::Copy:
          case MirOperandKind::Move:
          case MirOperandKind::BorrowRead:
          case MirOperandKind::BorrowWrite:
            canonicalOperand.place = operand.place;
            break;
          case MirOperandKind::Loan:
            canonicalOperand.loan = operand.loan;
            break;
          }
          canonicalInitialize.operands.push_back(std::move(canonicalOperand));
        }
      }
      MirInstruction canonicalMarker;
      if (markers.size() == 1) {
        canonicalMarker.id = markers.front().instruction->id;
        canonicalMarker.kind = MirInstructionKind::Lifecycle;
        canonicalMarker.hirValue = step.initializer;
        canonicalMarker.hirStatement = step.statement;
        canonicalMarker.fullExpressionEnd = step.fullExpression;
      }
      if (step.dataInitialization != MirProgramDataInitializationKind::None ||
          step.dataConstant || step.statement == 0 || step.initializer == 0 ||
          step.fullExpression == 0 ||
          !statements.insert(step.statement).second ||
          !initializerValues.insert(step.initializer).second ||
          !fullExpressions.insert(step.fullExpression).second ||
          !commonInitialize || !operandMatchesInitializer() ||
          *initialize != canonicalInitialize || full == nullptr ||
          full->id != step.fullExpression ||
          full->statement != step.statement ||
          full->constructorInitializer != 0 ||
          full->roots != std::vector<HirValueId>{step.initializer} ||
          markers.size() != 1 ||
          markers.front().block->programInitializationStep != step.id ||
          *markers.front().instruction != canonicalMarker ||
          !instructionDominates(located, markers.front())) {
        add("dynamic initialization step lacks its exact statement, root, "
            "storage Initialize, or ordered full-expression marker",
            step.entryBlock, step.storageInitialization);
      }
    } else {
      add("program-initialization step has an invalid role");
    }

    const std::vector<MirBlockId> &incoming = predecessors[step.entryBlock];
    const MirBlock *boundary = nullptr;
    if (index == 0) {
      if (!incoming.empty()) {
        add("first program-initialization entry has a predecessor",
            step.entryBlock);
      }
    } else {
      const MirBlock *predecessor =
          incoming.size() == 1 ? module.findBlock(incoming.front()) : nullptr;
      MirTerminator expectedTransition;
      expectedTransition.kind = MirTerminatorKind::Goto;
      expectedTransition.target = step.entryBlock;
      if (predecessor == nullptr ||
          predecessor->programInitializationStep != step.id - 1 ||
          predecessor->terminator != expectedTransition) {
        add("dense program-initialization steps lack one explicit Goto split",
            step.entryBlock);
      } else {
        boundary = predecessor;
      }
    }

    if (index + 1 == plan.steps.size()) {
      const auto finalExit = std::find_if(
          stepBlocks[index].begin(), stepBlocks[index].end(),
          [](const MirBlock *block) {
            return block->terminator.kind == MirTerminatorKind::Exit;
          });
      boundary = finalExit == stepBlocks[index].end() ? nullptr : *finalExit;
    } else {
      const std::vector<MirBlockId> &nextIncoming =
          predecessors[plan.steps[index + 1].entryBlock];
      boundary = nextIncoming.size() == 1
                     ? module.findBlock(nextIncoming.front())
                     : nullptr;
    }

    MirTerminator expectedBoundary;
    if (index + 1 == plan.steps.size()) {
      expectedBoundary.kind = MirTerminatorKind::Exit;
    } else {
      expectedBoundary.kind = MirTerminatorKind::Goto;
      expectedBoundary.target = plan.steps[index + 1].entryBlock;
    }
    if (boundary == nullptr || boundary->terminator != expectedBoundary) {
      add("program-initialization boundary is not the exact dense Goto or "
          "final Exit",
          boundary == nullptr ? step.entryBlock : boundary->id);
    }

    std::unordered_set<MirBlockId> forwardReachable;
    std::vector<MirBlockId> worklist{step.entryBlock};
    while (!worklist.empty()) {
      const MirBlockId blockId = worklist.back();
      worklist.pop_back();
      const MirBlock *block = module.findBlock(blockId);
      if (block == nullptr || block->programInitializationStep != step.id ||
          !forwardReachable.insert(blockId).second) {
        continue;
      }
      for (const MirBlockId successor : successors(block->terminator)) {
        const MirBlock *target = module.findBlock(successor);
        if (target != nullptr && target->programInitializationStep == step.id) {
          worklist.push_back(successor);
        }
      }
    }
    if (forwardReachable.size() != stepBlocks[index].size()) {
      add("program-initialization step contains a block unreachable from its "
          "entry",
          step.entryBlock);
    }
    const auto reentersPublishedBlock = [&](MirBlockId published) {
      const MirBlock *publishedBlock = module.findBlock(published);
      if (publishedBlock == nullptr) {
        return true;
      }
      std::unordered_set<MirBlockId> visited;
      std::vector<MirBlockId> pending;
      for (const MirBlockId successor :
           successors(publishedBlock->terminator)) {
        const MirBlock *target = module.findBlock(successor);
        if (target != nullptr && target->programInitializationStep == step.id) {
          pending.push_back(successor);
        }
      }
      while (!pending.empty()) {
        const MirBlockId blockId = pending.back();
        pending.pop_back();
        if (blockId == published) {
          return true;
        }
        const MirBlock *block = module.findBlock(blockId);
        if (block == nullptr || !visited.insert(blockId).second) {
          continue;
        }
        for (const MirBlockId successor : successors(block->terminator)) {
          const MirBlock *target = module.findBlock(successor);
          if (target != nullptr &&
              target->programInitializationStep == step.id) {
            pending.push_back(successor);
          }
        }
      }
      return false;
    };
    if (located.block != nullptr && reentersPublishedBlock(located.block->id)) {
      add("program-initialization CFG can execute storage Initialize more "
          "than once",
          located.block->id, step.storageInitialization);
    }
    if (step.role == ProgramInitializationStepRole::Initializer &&
        markers.size() == 1 &&
        reentersPublishedBlock(markers.front().block->id)) {
      add("program-initialization CFG can publish a full expression more "
          "than once",
          markers.front().block->id, markers.front().instruction->id);
    }
    if (boundary == nullptr || located.block == nullptr || !dominance ||
        !dominance->dominates(located.block->id, boundary->id)) {
      add("program storage Initialize does not dominate the dense transition "
          "or final Exit",
          step.entryBlock, step.storageInitialization);
    }
    if (step.role == ProgramInitializationStepRole::Initializer &&
        (markers.size() != 1 || boundary == nullptr || !dominance ||
         !dominance->dominates(markers.front().block->id, boundary->id))) {
      add("dynamic full-expression marker does not dominate the dense "
          "transition or final Exit",
          step.entryBlock,
          markers.empty() ? 0 : markers.front().instruction->id);
    }
  }

  if (module.fullExpressions.size() != fullExpressions.size()) {
    add("module full-expression inventory is not exactly the dynamic steps");
  }
  const auto placeIsUsed = [&](MirPlaceId id) {
    if (std::any_of(module.loans.begin(), module.loans.end(),
                    [&](const MirLoan &loan) { return loan.source == id; }) ||
        std::any_of(
            module.dropObligations.begin(), module.dropObligations.end(),
            [&](const MirDropObligation &drop) { return drop.place == id; })) {
      return true;
    }
    for (const MirBlock &block : module.blocks) {
      if (block.terminator.value && block.terminator.value->place == id) {
        return true;
      }
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.destination == id ||
            (instruction.receiver && instruction.receiver->place == id) ||
            std::any_of(instruction.operands.begin(),
                        instruction.operands.end(),
                        [&](const MirOperand &operand) {
                          return operand.place == id;
                        })) {
          return true;
        }
      }
    }
    return false;
  };
  for (const MirPlace &place : module.places) {
    if (place.root == MirPlaceRootKind::This) {
      add("Module program initialization contains a This-root place");
      continue;
    }
    const bool bindingRoot = place.root == MirPlaceRootKind::Binding;
    const bool symbolRoot = place.root == MirPlaceRootKind::Symbol;
    const bool rootAxesExact =
        (bindingRoot || symbolRoot)
            ? place.capture == 0 && place.temporary == 0 && place.value == 0 &&
                  place.loan == 0 && (bindingRoot || place.binding == 0)
            : place.binding == 0 && place.symbol == 0 && place.capture == 0 &&
                  (place.root == MirPlaceRootKind::Temporary
                       ? place.value == 0 && place.loan == 0
                       : place.temporary == 0) &&
                  (place.root == MirPlaceRootKind::Value ? place.loan == 0
                                                         : place.value == 0) &&
                  (place.root == MirPlaceRootKind::Loan ? place.value == 0
                                                        : place.loan == 0);
    if (!rootAxesExact) {
      add("Module place retains identities outside its exact root kind");
    }
    if (place.root == MirPlaceRootKind::Temporary &&
        (place.sourceValue == 0 || !placeIsUsed(place.id))) {
      add("Module contains an orphan program-initialization temporary place");
    }
    if (!bindingRoot && !symbolRoot) {
      continue;
    }
    std::optional<ProgramInitializationStepId> identity;
    bool exact = true;
    const auto requireIdentity = [&](ProgramInitializationStepId candidate) {
      exact = exact && candidate != 0 && (!identity || *identity == candidate);
      if (candidate != 0 && !identity) {
        identity = candidate;
      }
    };
    if (place.root == MirPlaceRootKind::Binding) {
      const auto binding = bindingSteps.find(place.binding);
      requireIdentity(binding == bindingSteps.end() ? 0 : binding->second);
    }
    const auto symbol = symbolSteps.find(place.symbol);
    requireIdentity(symbol == symbolSteps.end() ? 0 : symbol->second);
    if (place.key) {
      const auto keyed = symbolSteps.find(place.key->root);
      requireIdentity(keyed == symbolSteps.end() ? 0 : keyed->second);
    }
    if (!exact) {
      add("module binding/symbol-root place does not name one exact planned "
          "storage step");
    }
  }

  const auto stepForPlace = [&](MirPlaceId placeId, const auto &resolve,
                                std::unordered_set<MirPlaceId> &visiting)
      -> std::optional<ProgramInitializationStepId> {
    const MirPlace *place = module.findPlace(placeId);
    if (place == nullptr || !visiting.insert(placeId).second) {
      return ProgramInitializationStepId{0};
    }
    std::optional<ProgramInitializationStepId> selected;
    const auto select = [&](ProgramInitializationStepId candidate) {
      if (candidate == 0) {
        return true;
      }
      if (selected && *selected != candidate) {
        return false;
      }
      selected = candidate;
      return true;
    };
    if (const auto found = bindingSteps.find(place->binding);
        found != bindingSteps.end() && !select(found->second)) {
      return ProgramInitializationStepId{0};
    }
    if (const auto found = symbolSteps.find(place->symbol);
        found != symbolSteps.end() && !select(found->second)) {
      return ProgramInitializationStepId{0};
    }
    if (place->key) {
      if (const auto found = symbolSteps.find(place->key->root);
          found != symbolSteps.end() && !select(found->second)) {
        return ProgramInitializationStepId{0};
      }
    }
    if (place->root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = module.findLoan(place->loan);
      if (loan == nullptr) {
        return ProgramInitializationStepId{0};
      }
      const std::optional<ProgramInitializationStepId> source =
          resolve(loan->source, resolve, visiting);
      if (source && (*source == 0 || !select(*source))) {
        return ProgramInitializationStepId{0};
      }
    }
    visiting.erase(placeId);
    return selected;
  };
  const auto resolvePlaceStep = [&](MirPlaceId placeId) {
    std::unordered_set<MirPlaceId> visiting;
    return stepForPlace(placeId, stepForPlace, visiting);
  };
  const auto resolveLoanStep = [&](MirLoanId loanId) {
    const MirLoan *loan = module.findLoan(loanId);
    return loan == nullptr
               ? std::optional<
                     ProgramInitializationStepId>{ProgramInitializationStepId{
                     0}}
               : resolvePlaceStep(loan->source);
  };
  const auto legalPriorStep =
      [&](std::optional<ProgramInitializationStepId> target,
          ProgramInitializationStepId current, bool sameStepInitialization) {
        if (!target) {
          return true;
        }
        if (*target == 0) {
          return false;
        }
        return *target < current ||
               (sameStepInitialization && *target == current);
      };
  const auto legalPriorAccess = [&](MirPlaceId placeId,
                                    ProgramInitializationStepId current,
                                    bool sameStepInitialization) {
    return legalPriorStep(resolvePlaceStep(placeId), current,
                          sameStepInitialization);
  };

  std::size_t exits = 0;
  for (const MirBlock &block : module.blocks) {
    const ProgramInitializationStepId current = block.programInitializationStep;
    const MirProgramInitializationStep *step = plan.findStep(current);
    for (const MirInstruction &instruction : block.instructions) {
      const bool exactInitialization =
          step != nullptr && instruction.id == step->storageInitialization &&
          instruction.kind == MirInstructionKind::Initialize &&
          instruction.destination == step->storagePlace;
      if (instruction.destination &&
          !legalPriorAccess(*instruction.destination, current,
                            exactInitialization)) {
        add("program-storage destination is not initialized/accessed in step "
            "order",
            block.id, instruction.id);
      }
      const auto checkOperand = [&](const MirOperand &operand) {
        if (operand.place != 0 &&
            !legalPriorAccess(operand.place, current, false)) {
          add("program-storage operand/receiver is not accessed strictly "
              "after its step",
              block.id, instruction.id);
        }
        if (operand.loan != 0 &&
            !legalPriorStep(resolveLoanStep(operand.loan), current, false)) {
          add("loan operand/receiver accesses program storage before its "
              "step",
              block.id, instruction.id);
        }
      };
      if (instruction.receiver) {
        checkOperand(*instruction.receiver);
      }
      for (const MirOperand &operand : instruction.operands) {
        checkOperand(operand);
      }
      if (instruction.programConstantSubstitution &&
          (step == nullptr ||
           step->role != ProgramInitializationStepRole::Initializer ||
           instruction.kind != MirInstructionKind::Compute)) {
        add("program-constant substitution is outside an exact dynamic "
            "materialization step",
            block.id, instruction.id);
      }
    }
    if (block.terminator.value && block.terminator.value->place != 0 &&
        !legalPriorAccess(block.terminator.value->place, current, false)) {
      add("terminator accesses program storage before its step", block.id);
    }
    if (block.terminator.value && block.terminator.value->loan != 0 &&
        !legalPriorStep(resolveLoanStep(block.terminator.value->loan), current,
                        false)) {
      add("terminator loan accesses program storage before its step", block.id);
    }
    for (const MirBlockId successor : successors(block.terminator)) {
      const MirBlock *target = module.findBlock(successor);
      if (target == nullptr) {
        continue;
      }
      const bool sameStep = target->programInitializationStep == current;
      const bool failureEdge =
          block.terminator.kind == MirTerminatorKind::Invoke &&
          block.terminator.elseTarget == successor;
      const bool exactNext = current < plan.steps.size() &&
                             target->programInitializationStep == current + 1 &&
                             target->id == plan.steps[current].entryBlock;
      if ((failureEdge && !sameStep) ||
          (!failureEdge && !sameStep && !exactNext)) {
        add("normal program-initialization edge skips or escapes its dense "
            "step",
            block.id);
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Exit) {
      ++exits;
      if (current != plan.steps.size()) {
        add("program initialization exits before the final step", block.id);
      }
    } else if (block.terminator.kind == MirTerminatorKind::PropagateFailure ||
               block.terminator.kind ==
                   MirTerminatorKind::TerminateCleanupFailure) {
      if (block.activeFailure == 0) {
        add("program-initialization failure endpoint has no active failure",
            block.id);
      }
    } else if (block.terminator.kind == MirTerminatorKind::Return ||
               block.terminator.kind == MirTerminatorKind::Unreachable ||
               block.terminator.kind == MirTerminatorKind::ContainFailure ||
               block.terminator.kind == MirTerminatorKind::None) {
      add("program-initialization block has a non-normal terminator", block.id);
    }
  }
  if (exits != 1) {
    add("final program-initialization step must reach exactly one Exit");
  }

  for (std::size_t index = 0; index < stepBlocks.size(); ++index) {
    const ProgramInitializationStepId stepId = index + 1;
    const MirBlockId nextEntry =
        index + 1 < plan.steps.size() ? plan.steps[index + 1].entryBlock : 0;
    std::unordered_set<MirBlockId> canReachBoundary;
    bool changed = true;
    while (changed) {
      changed = false;
      for (const MirBlock *block : stepBlocks[index]) {
        if (block->activeFailure != 0 || canReachBoundary.contains(block->id)) {
          continue;
        }
        std::vector<MirBlockId> outgoing = successors(block->terminator);
        if (block->terminator.kind == MirTerminatorKind::Invoke) {
          outgoing = {block->terminator.target};
        }
        const bool boundary =
            (nextEntry != 0 && std::find(outgoing.begin(), outgoing.end(),
                                         nextEntry) != outgoing.end()) ||
            (nextEntry == 0 &&
             block->terminator.kind == MirTerminatorKind::Exit);
        const bool reachesKnown = std::any_of(
            outgoing.begin(), outgoing.end(), [&](MirBlockId successor) {
              const MirBlock *target = module.findBlock(successor);
              return target != nullptr && target->activeFailure == 0 &&
                     target->programInitializationStep == stepId &&
                     canReachBoundary.contains(successor);
            });
        if (boundary || reachesKnown) {
          canReachBoundary.insert(block->id);
          changed = true;
        }
      }
    }
    const std::size_t normalBlocks = static_cast<std::size_t>(std::count_if(
        stepBlocks[index].begin(), stepBlocks[index].end(),
        [](const MirBlock *block) { return block->activeFailure == 0; }));
    if (canReachBoundary.size() != normalBlocks) {
      add("program-initialization step has a block that cannot reach its "
          "dense successor or final Exit",
          plan.steps[index].entryBlock);
    }
  }
  return result;
}

[[nodiscard]] MirVerificationResult
verifyMirHostedStartup(const MirProgram &program) {
  MirVerificationResult result;
  const std::optional<MirHostedStartupPlan> &optionalPlan =
      program.hostedStartupPlan();
  const MirBody *body = program.hostedStartup();
  const auto add = [&](std::string message, MirBlockId block = 0,
                       MirInstructionId instruction = 0) {
    result.errors.push_back({.bodyKind = MirBodyKind::HostedStartup,
                             .owner = optionalPlan ? optionalPlan->entry : 0,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };

  std::vector<const MirFunctionInstance *> entries;
  for (const MirFunctionInstance &function : program.functionInstances()) {
    if (function.entryKind != ProgramEntryKind::None) {
      entries.push_back(&function);
    }
  }
  if (entries.empty()) {
    if (optionalPlan || body != nullptr) {
      add("hosted-startup authority exists without a program entry");
    }
    return result;
  }
  if (entries.size() != 1) {
    add("hosted-startup authority requires one exact program entry");
    return result;
  }
  if (!optionalPlan || body == nullptr) {
    add("program entry is missing hosted-startup MIR authority");
    return result;
  }
  const MirHostedStartupPlan &plan = *optionalPlan;
  const MirFunctionInstance &entry = *entries.front();
  if (plan.entry == 0 || plan.entry != entry.id ||
      plan.kind != entry.entryKind || plan.sourceAnchor.sourceUnit == 0 ||
      plan.sourceAnchor.end <= plan.sourceAnchor.start ||
      plan.sourceAnchor.line < 1 ||
      plan.programInitializationTarget !=
          MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0} ||
      plan.exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70 ||
      entry.owner || entry.staticMember ||
      entry.linkage != LanguageLinkage::Gti ||
      entry.returnType != SemanticType::Int32 ||
      entry.definitionKind != MirDefinitionKind::Source ||
      !entry.externalSymbol.empty() || entry.constexprFunction ||
      entry.virtualMethod || entry.pureVirtual || entry.overrideMethod ||
      !entry.virtualRoots.empty() || entry.overloadedOperator ||
      !entry.callableParameters.empty() ||
      entry.receiverMutability != ReceiverMutability::ReadOnly ||
      entry.returnBorrowOrigin != BorrowOriginKind::None ||
      entry.returnBorrowParameter != 0 ||
      entry.returnBorrowAccess != AccessMode::ReadOnly ||
      entry.returnBorrowPlace || body->kind != MirBodyKind::HostedStartup ||
      findMirBody(program, {.kind = MirBodyKind::HostedStartup,
                            .owner = plan.entry}) != body) {
    add("hosted-startup plan identity, source anchor, or entry is invalid");
  }
  std::size_t maximumSourceBodyDomain = 0;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    if (address.kind == MirBodyKind::HostedStartup) {
      continue;
    }
    if (const MirBody *sourceBody = findMirBody(program, address)) {
      maximumSourceBodyDomain =
          std::max(maximumSourceBodyDomain, sourceBody->placeDomain.body);
    }
  }
  if (body->placeDomain.snapshot != program.module().placeDomain.snapshot ||
      body->placeDomain.body != maximumSourceBodyDomain + 1 ||
      body->placeDomain.revision != 0 || body->entry != 1) {
    add("hosted-startup place domain is not the exact final generated domain");
  }

  const bool callsProgramInitialization = std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
  const MirDefinedFailureEffects derivedFailureEffects =
      deriveMirDefinedFailureEffects(program);
  const auto exactFunctionFailureSummary =
      [&](const MirFunctionInstance *item) {
        return item != nullptr && item->id != 0 &&
               item->id <= derivedFailureEffects.functions.size() &&
               item->mayRaiseDefinedFailure ==
                   derivedFailureEffects.functions[item->id - 1];
      };
  const auto exactConstructorFailureSummary =
      [&](const MirConstructorInstance *item) {
        return item != nullptr && item->id != 0 &&
               item->id <= derivedFailureEffects.constructors.size() &&
               item->mayRaiseDefinedFailure ==
                   derivedFailureEffects.constructors[item->id - 1];
      };
  if (!exactFunctionFailureSummary(&entry)) {
    add("hosted entry defined-failure summary is not exactly MIR-derived");
  }
  const bool moduleMayRaise = std::any_of(
      program.module().blocks.begin(), program.module().blocks.end(),
      [](const MirBlock &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [](const MirInstruction &instruction) {
                             return !instruction.definedFailure.empty();
                           });
      });
  const auto propagation = [](FailurePropagationKind kind, bool mayRaise) {
    DefinedFailureOperation failure;
    failure.propagation = mayRaise ? kind : FailurePropagationKind::None;
    return failure;
  };
  const auto failureBehavior = [](bool mayRaise) {
    return mayRaise ? MirHostedStartupFailureBehavior::Propagate
                    : MirHostedStartupFailureBehavior::None;
  };
  const auto generatedInfo = [](const SemanticType &type,
                                ValueCategory category = ValueCategory::Value,
                                AccessMode access = AccessMode::ReadOnly) {
    return ExpressionInfo{.type = type,
                          .category = category,
                          .access = access,
                          .traits = semanticTraits(type)};
  };
  const auto instructionAt = [&](MirBlockId blockId, MirInstructionId id) {
    const MirBlock *block = body->findBlock(blockId);
    if (block == nullptr) {
      return static_cast<const MirInstruction *>(nullptr);
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };

  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    const MirHostedStartupOperation &operation = plan.operations[index];
    const bool instruction = operation.instruction != 0;
    if (operation.id != index + 1 ||
        operation.kind >= MirHostedStartupOperationKind::Count ||
        operation.failureBehavior >= MirHostedStartupFailureBehavior::Count ||
        operation.block == 0 || operation.block > body->blocks.size() ||
        instruction == operation.terminator ||
        (operation.place != 0 &&
         (operation.place > body->places.size() ||
          body->places[operation.place - 1].hostedStartupOperation !=
              operation.id)) ||
        (operation.value != 0 &&
         (operation.value > body->values.size() ||
          body->values[operation.value - 1].hostedStartupOperation !=
              operation.id)) ||
        (operation.dropObligation != 0 &&
         (operation.dropObligation > body->dropObligations.size() ||
          body->dropObligations[operation.dropObligation - 1]
                  .hostedStartupOperation != operation.id)) ||
        (operation.failureRecord != 0 &&
         (operation.failureRecord > body->failureRecords.size() ||
          body->failureRecords[operation.failureRecord - 1]
                  .hostedStartupOperation != operation.id)) ||
        (operation.cleanupBoundary != 0 &&
         (operation.cleanupBoundary > body->cleanupBoundaries.size() ||
          body->cleanupBoundaries[operation.cleanupBoundary - 1]
                  .hostedStartupOperation != operation.id))) {
      add("hosted-startup operation row has an invalid dense identity or "
          "entity reference",
          operation.block, operation.instruction);
      continue;
    }
    if (instruction) {
      const MirInstruction *record =
          instructionAt(operation.block, operation.instruction);
      if (record == nullptr || record->hostedStartupOperation != operation.id) {
        add("hosted-startup operation does not own its exact instruction",
            operation.block, operation.instruction);
      }
    } else {
      const MirBlock *block = body->findBlock(operation.block);
      if (block == nullptr ||
          block->terminator.hostedStartupOperation != operation.id) {
        add("hosted-startup operation does not own its exact terminator",
            operation.block);
      }
    }
  }
  for (const MirPlace &place : body->places) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(place.hostedStartupOperation);
    if (operation == nullptr || operation->place != place.id) {
      add("generated hosted-startup place is orphaned or multiply owned");
    }
  }
  for (const MirValue &value : body->values) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(value.hostedStartupOperation);
    if (operation == nullptr || operation->value != value.id) {
      add("generated hosted-startup value is orphaned or multiply owned");
    }
  }
  for (const MirDropObligation &drop : body->dropObligations) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(drop.hostedStartupOperation);
    if (operation == nullptr || operation->dropObligation != drop.id) {
      add("generated hosted-startup drop is orphaned or multiply owned");
    }
  }
  for (const MirFailureRecord &record : body->failureRecords) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(record.hostedStartupOperation);
    if (operation == nullptr || operation->failureRecord != record.id) {
      add("generated hosted-startup failure record is orphaned or multiply "
          "owned");
    }
  }
  for (const MirCleanupBoundary &boundary : body->cleanupBoundaries) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(boundary.hostedStartupOperation);
    if (operation == nullptr || operation->cleanupBoundary != boundary.id) {
      add("generated hosted-startup cleanup boundary is orphaned or multiply "
          "owned");
    }
  }
  for (const MirBlock &block : body->blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const MirHostedStartupOperation *operation =
          plan.findOperation(instruction.hostedStartupOperation);
      if (operation == nullptr || operation->instruction != instruction.id ||
          operation->block != block.id) {
        add("generated hosted-startup instruction is orphaned or multiply "
            "owned",
            block.id, instruction.id);
      }
      for (const DefinedFailureOrigin &origin :
           instruction.definedFailure.localOrigins) {
        if (std::any_of(origin.outcomes.begin(), origin.outcomes.end(),
                        [](const DefinedFailureOutcome &outcome) {
                          return outcome.detail ==
                                 DefinedFailureDetail::HostedArguments;
                        })) {
          add("hosted-startup operation produces the reserved hosted_arguments "
              "failure detail",
              block.id, instruction.id);
        }
      }
    }
    const MirHostedStartupOperation *operation =
        plan.findOperation(block.terminator.hostedStartupOperation);
    if (operation == nullptr || !operation->terminator ||
        operation->block != block.id) {
      add("generated hosted-startup terminator is orphaned or multiply owned",
          block.id);
    }
  }

  struct TakenOperation {
    const MirHostedStartupOperation *actual = nullptr;
    MirHostedStartupOperation expected;
  };
  std::size_t nextOperation = 1;
  MirInstructionId nextInstruction = 1;
  MirPlaceId nextPlace = 1;
  MirValueId nextValue = 1;
  MirDropObligationId nextDrop = 1;
  MirFailureRecordId nextFailureRecord = 1;
  std::size_t nextCleanupBoundary = 1;
  const auto take = [&](MirHostedStartupOperationKind kind,
                        MirHostedStartupFailureBehavior behavior,
                        MirBlockId block, bool instruction, bool place,
                        bool value, bool drop, bool failureRecord = false,
                        bool cleanupBoundary = false) {
    TakenOperation taken;
    taken.expected.id = nextOperation++;
    taken.expected.kind = kind;
    taken.expected.failureBehavior = behavior;
    taken.expected.block = block;
    if (instruction) {
      taken.expected.instruction = nextInstruction++;
    } else {
      taken.expected.terminator = true;
    }
    if (place) {
      taken.expected.place = nextPlace++;
    }
    if (value) {
      taken.expected.value = nextValue++;
    }
    if (drop) {
      taken.expected.dropObligation = nextDrop++;
    }
    if (failureRecord) {
      taken.expected.failureRecord = nextFailureRecord++;
    }
    if (cleanupBoundary) {
      taken.expected.cleanupBoundary = nextCleanupBoundary++;
    }
    taken.actual = plan.findOperation(taken.expected.id);
    if (taken.actual == nullptr || *taken.actual != taken.expected) {
      add("hosted-startup dense operation sequence or per-kind entity union "
          "is not exact",
          block, taken.expected.instruction);
    }
    return taken;
  };
  const auto exactValue = [&](const TakenOperation &operation,
                              const ExpressionInfo &info) {
    if (operation.expected.value == 0 ||
        operation.expected.value > body->values.size()) {
      return;
    }
    const MirValue expected{.id = operation.expected.value,
                            .hostedStartupOperation = operation.expected.id,
                            .sourceValue = 0,
                            .info = info,
                            .definitionBlock = operation.expected.block,
                            .definition = operation.expected.instruction};
    if (body->values[operation.expected.value - 1] != expected) {
      add("hosted-startup generated value is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactPlace = [&](const TakenOperation &operation,
                              MirPlace expected) {
    if (operation.expected.place == 0 ||
        operation.expected.place > body->places.size()) {
      return;
    }
    expected.id = operation.expected.place;
    expected.hostedStartupOperation = operation.expected.id;
    if (body->places[operation.expected.place - 1] != expected) {
      add("hosted-startup generated place is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactDrop = [&](const TakenOperation &operation,
                             MirDropObligation expected) {
    if (operation.expected.dropObligation == 0 ||
        operation.expected.dropObligation > body->dropObligations.size()) {
      return;
    }
    expected.id = operation.expected.dropObligation;
    expected.hostedStartupOperation = operation.expected.id;
    expected.constructionOrder = operation.expected.dropObligation;
    if (body->dropObligations[operation.expected.dropObligation - 1] !=
        expected) {
      add("hosted-startup generated drop is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactInstruction = [&](const TakenOperation &operation,
                                    MirInstruction expected) {
    const MirInstruction *actual =
        instructionAt(operation.expected.block, operation.expected.instruction);
    expected.id = operation.expected.instruction;
    expected.hostedStartupOperation = operation.expected.id;
    if (actual == nullptr || *actual != expected) {
      add("hosted-startup generated instruction is not its exact canonical "
          "stage",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactTerminator = [&](const TakenOperation &operation,
                                   MirTerminator expected) {
    const MirBlock *block = body->findBlock(operation.expected.block);
    expected.hostedStartupOperation = operation.expected.id;
    if (block == nullptr || block->terminator != expected) {
      add("hosted-startup generated terminator is not its exact canonical "
          "stage",
          operation.expected.block);
    }
  };
  const auto exactFailureRecord = [&](const TakenOperation &operation,
                                      MirFailureRecord expected) {
    if (operation.expected.failureRecord == 0 ||
        operation.expected.failureRecord > body->failureRecords.size()) {
      return;
    }
    expected.id = operation.expected.failureRecord;
    expected.hostedStartupOperation = operation.expected.id;
    if (body->failureRecords[operation.expected.failureRecord - 1] !=
        expected) {
      add("hosted-startup generated failure record is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactCleanupBoundary =
      [&](const TakenOperation &operation,
          std::vector<MirDropObligationId> obligations) {
        if (operation.expected.cleanupBoundary == 0 ||
            operation.expected.cleanupBoundary >
                body->cleanupBoundaries.size()) {
          return;
        }
        const MirCleanupBoundary expected{
            .id = operation.expected.cleanupBoundary,
            .hostedStartupOperation = operation.expected.id,
            .kind = MirCleanupBoundaryKind::Failure,
            .obligations = std::move(obligations)};
        if (body->cleanupBoundaries[operation.expected.cleanupBoundary - 1] !=
            expected) {
          add("hosted-startup generated cleanup boundary is not exact",
              operation.expected.block, operation.expected.instruction);
        }
      };
  MirBlockId nextGeneratedBlock = 2;
  const auto destructorMayRaise = [&](MirDropObligationId obligation) {
    const MirDropObligation *drop = body->findDropObligation(obligation);
    const MirDestructorInstance *destructor =
        drop != nullptr && drop->dropType.destructor
            ? program.findDestructorInstance(*drop->dropType.destructor)
            : nullptr;
    return destructor != nullptr && destructor->mayRaiseDefinedFailure;
  };
  const auto takeFailureRoute = [&](MirBlockId producerBlock,
                                    MirInstructionId producerInstruction,
                                    const std::vector<MirDropObligationId>
                                        &activeDrops,
                                    MirDropObligationId successDrop = 0) {
    const MirBlockId normalBlock = nextGeneratedBlock++;
    const MirBlockId failureBlock = nextGeneratedBlock++;
    const TakenOperation route =
        take(MirHostedStartupOperationKind::RouteOperationFailure,
             MirHostedStartupFailureBehavior::None, producerBlock, false, false,
             false, false, true);
    exactFailureRecord(route, {.producerBlock = producerBlock,
                               .producerInstruction = producerInstruction,
                               .parameterBlock = failureBlock});
    MirTerminator invoke{.kind = MirTerminatorKind::Invoke,
                         .invokeInstruction = producerInstruction,
                         .failureRecord = route.expected.failureRecord,
                         .target = normalBlock,
                         .elseTarget = failureBlock};
    if (successDrop != 0) {
      invoke.successLifecycle = {
          {.kind = MirLifecycleEventKind::Initialize, .target = successDrop}};
    }
    exactTerminator(route, std::move(invoke));

    MirBlockId cleanupBlock = failureBlock;
    std::vector<MirDropObligationId> boundaryDrops;
    boundaryDrops.reserve(activeDrops.size());
    for (auto candidate = activeDrops.rbegin(); candidate != activeDrops.rend();
         ++candidate) {
      const MirDropObligation *drop = body->findDropObligation(*candidate);
      const MirPlace *place =
          drop == nullptr ? nullptr : body->findPlace(drop->place);
      const bool mayRaise = destructorMayRaise(*candidate);
      const TakenOperation cleanupDrop = take(
          MirHostedStartupOperationKind::DropFailureCleanup,
          failureBehavior(mayRaise), cleanupBlock, true, false, false, false);
      if (drop != nullptr && place != nullptr) {
        exactInstruction(
            cleanupDrop,
            {.kind = MirInstructionKind::Drop,
             .destination = drop->place,
             .definedFailure =
                 propagation(FailurePropagationKind::Destructor, mayRaise),
             .info = generatedInfo(drop->dropType.type, ValueCategory::Place,
                                   AccessMode::Mutable),
             .lifecycle = {{.kind = MirLifecycleEventKind::Drop,
                            .source = *candidate,
                            .failureCleanup = true}}});
      }
      boundaryDrops.push_back(*candidate);
      if (!mayRaise) {
        continue;
      }
      const MirBlockId cleanupNormal = nextGeneratedBlock++;
      const MirBlockId secondaryBlock = nextGeneratedBlock++;
      const TakenOperation cleanupRoute =
          take(MirHostedStartupOperationKind::RouteCleanupFailure,
               MirHostedStartupFailureBehavior::None, cleanupBlock, false,
               false, false, false, true);
      exactFailureRecord(cleanupRoute, {.producerBlock = cleanupBlock,
                                        .producerInstruction =
                                            cleanupDrop.expected.instruction,
                                        .parameterBlock = secondaryBlock});
      exactTerminator(cleanupRoute,
                      {.kind = MirTerminatorKind::Invoke,
                       .invokeInstruction = cleanupDrop.expected.instruction,
                       .failureRecord = cleanupRoute.expected.failureRecord,
                       .target = cleanupNormal,
                       .elseTarget = secondaryBlock});
      const TakenOperation terminate =
          take(MirHostedStartupOperationKind::TerminateCleanupFailure,
               MirHostedStartupFailureBehavior::None, secondaryBlock, false,
               false, false, false);
      exactTerminator(terminate,
                      {.kind = MirTerminatorKind::TerminateCleanupFailure,
                       .failureRecord = cleanupRoute.expected.failureRecord});
      cleanupBlock = cleanupNormal;
    }
    if (!boundaryDrops.empty()) {
      const TakenOperation end =
          take(MirHostedStartupOperationKind::EndFailureCleanup,
               MirHostedStartupFailureBehavior::None, cleanupBlock, true, false,
               false, false, false, true);
      exactCleanupBoundary(end, boundaryDrops);
      exactInstruction(end,
                       {.kind = MirInstructionKind::Lifecycle,
                        .cleanupBoundaryEnd = end.expected.cleanupBoundary});
    }
    const TakenOperation contain =
        take(MirHostedStartupOperationKind::ContainFailure,
             MirHostedStartupFailureBehavior::None, cleanupBlock, false, false,
             false, false);
    exactTerminator(contain, {.kind = MirTerminatorKind::ContainFailure,
                              .failureRecord = route.expected.failureRecord});
    return normalBlock;
  };

  if (plan.kind == ProgramEntryKind::NoArguments) {
    MirBlockId currentBlock = 1;
    if (!entry.parameterTypes.empty() || plan.appendFunction != 0 ||
        plan.vectorConstructor != 0 || plan.stringConstructor != 0 ||
        plan.argumentIndexPlace != 0 || plan.argumentVectorPlace != 0 ||
        plan.stabilizedCount != 0 || plan.argumentVector != 0 ||
        !body->places.empty() || !body->dropObligations.empty()) {
      add("no-argument hosted-startup plan retains owned-argument state");
    }
    if (callsProgramInitialization) {
      const TakenOperation callBody =
          take(MirHostedStartupOperationKind::CallProgramInitialization,
               failureBehavior(moduleMayRaise), currentBlock, true, false,
               false, false);
      exactInstruction(callBody,
                       {.kind = MirInstructionKind::CallBody,
                        .definedFailure = propagation(
                            FailurePropagationKind::BodyCall, moduleMayRaise),
                        .bodyTarget = plan.programInitializationTarget,
                        .info = generatedInfo(SemanticType::Void)});
      if (moduleMayRaise) {
        currentBlock =
            takeFailureRoute(currentBlock, callBody.expected.instruction, {});
      }
    }
    const TakenOperation callEntry =
        take(MirHostedStartupOperationKind::CallEntry,
             failureBehavior(entry.mayRaiseDefinedFailure), currentBlock, true,
             false, true, false);
    exactValue(callEntry, generatedInfo(SemanticType::Int32));
    exactInstruction(callEntry, {.kind = MirInstructionKind::Call,
                                 .result = callEntry.expected.value,
                                 .definedFailure = propagation(
                                     FailurePropagationKind::DirectCall,
                                     entry.mayRaiseDefinedFailure),
                                 .functionTarget = entry.id,
                                 .info = generatedInfo(SemanticType::Int32)});
    if (entry.mayRaiseDefinedFailure) {
      currentBlock =
          takeFailureRoute(currentBlock, callEntry.expected.instruction, {});
    }
    const TakenOperation returnEntry =
        take(MirHostedStartupOperationKind::ReturnEntry,
             MirHostedStartupFailureBehavior::None, currentBlock, false, false,
             false, false);
    exactTerminator(returnEntry,
                    {.kind = MirTerminatorKind::Return,
                     .value = MirOperand{.kind = MirOperandKind::Value,
                                         .value = callEntry.expected.value,
                                         .type = SemanticType::Int32}});
    if (plan.entryResult != callEntry.expected.value) {
      add("no-argument hosted-startup result identity is invalid");
    }
  } else if (plan.kind == ProgramEntryKind::OwnedArguments) {
    MirBlockId currentBlock = 1;
    const bool entryShape =
        entry.parameterTypes.size() == 2 &&
        entry.parameterTypes[0] == SemanticType::Int32 &&
        entry.parameterTypes[1].kind == SemanticType::Class &&
        entry.parameterTypes[1].arguments.size() == 1 &&
        entry.parameterTypes[1].arguments.front().kind == SemanticType::Class &&
        entry.entryArgumentAppendTarget ==
            std::optional<HirFunctionInstanceId>{plan.appendFunction};
    const SemanticType vectorType =
        entryShape ? entry.parameterTypes[1] : SemanticType::Unknown;
    const SemanticType stringType =
        entryShape ? vectorType.arguments.front() : SemanticType::Unknown;
    const MirFunctionInstance *append =
        program.findFunctionInstance(plan.appendFunction);
    const MirConstructorInstance *vectorConstructor =
        program.findConstructorInstance(plan.vectorConstructor);
    const MirConstructorInstance *stringConstructor =
        program.findConstructorInstance(plan.stringConstructor);
    const MirClassInstance *vectorClass =
        vectorConstructor == nullptr
            ? nullptr
            : program.findClassInstance(vectorConstructor->owner);
    const MirClassInstance *stringClass =
        stringConstructor == nullptr
            ? nullptr
            : program.findClassInstance(stringConstructor->owner);
    const bool exactTargets =
        entryShape && append != nullptr && append->owner &&
        vectorClass != nullptr && stringClass != nullptr &&
        *append->owner == vectorClass->id && vectorClass->type == vectorType &&
        stringClass->type == stringType &&
        append->returnType == SemanticType::Void &&
        append->parameterTypes == std::vector<SemanticType>{stringType} &&
        !append->staticMember && append->entryKind == ProgramEntryKind::None &&
        append->linkage == LanguageLinkage::Gti &&
        append->externalSymbol.empty() && !append->constexprFunction &&
        append->receiverMutability == ReceiverMutability::Mutable &&
        !append->virtualMethod && !append->pureVirtual &&
        !append->overrideMethod && append->virtualRoots.empty() &&
        !append->overloadedOperator && append->callableParameters.empty() &&
        append->returnBorrowOrigin == BorrowOriginKind::None &&
        append->returnBorrowParameter == 0 &&
        append->returnBorrowAccess == AccessMode::ReadOnly &&
        !append->returnBorrowPlace &&
        append->definitionKind == MirDefinitionKind::Source &&
        exactFunctionFailureSummary(append) &&
        vectorConstructor->owner == vectorClass->id &&
        vectorConstructor->parameterTypes.empty() &&
        vectorConstructor->borrowOrigin == BorrowOriginKind::None &&
        vectorConstructor->borrowParameter == 0 &&
        vectorConstructor->borrowAccess == AccessMode::ReadOnly &&
        vectorConstructor->definitionKind == MirDefinitionKind::Source &&
        exactConstructorFailureSummary(vectorConstructor) &&
        stringConstructor->owner == stringClass->id &&
        stringConstructor->parameterTypes ==
            std::vector<SemanticType>{SemanticType::StringView} &&
        stringConstructor->borrowOrigin == BorrowOriginKind::None &&
        stringConstructor->borrowParameter == 0 &&
        stringConstructor->borrowAccess == AccessMode::ReadOnly &&
        stringConstructor->definitionKind == MirDefinitionKind::Source &&
        exactConstructorFailureSummary(stringConstructor) &&
        vectorClass->requiresActiveCleanup &&
        stringClass->requiresActiveCleanup;
    if (!exactTargets) {
      add("owned-argument hosted-startup targets, signatures, or CFG shape "
          "are invalid");
    }
    const MirDropType vectorDropType{
        .type = vectorType,
        .classInstance = vectorClass == nullptr
                             ? std::nullopt
                             : std::optional{vectorClass->id},
        .destructor =
            vectorClass == nullptr ? std::nullopt : vectorClass->destructor,
        .requiresActiveCleanup =
            vectorClass != nullptr && vectorClass->requiresActiveCleanup};
    const MirDropType stringDropType{
        .type = stringType,
        .classInstance = stringClass == nullptr
                             ? std::nullopt
                             : std::optional{stringClass->id},
        .destructor =
            stringClass == nullptr ? std::nullopt : stringClass->destructor,
        .requiresActiveCleanup =
            stringClass != nullptr && stringClass->requiresActiveCleanup};

    const DefinedFailureOrigin validateOrigin{
        .outcomes = {{.code = DefinedFailureCode::HostedRuntimeContractFailure,
                      .detail = DefinedFailureDetail::NegativeArgumentCount}},
        .sourceUnit = plan.sourceAnchor.sourceUnit,
        .start = plan.sourceAnchor.start,
        .end = plan.sourceAnchor.end,
        .line = plan.sourceAnchor.line};
    const DefinedFailureOrigin convertOrigin{
        .outcomes = {{.code = DefinedFailureCode::NumericConversionOutOfRange,
                      .detail = DefinedFailureDetail::HostedArgumentCount}},
        .sourceUnit = plan.sourceAnchor.sourceUnit,
        .start = plan.sourceAnchor.start,
        .end = plan.sourceAnchor.end,
        .line = plan.sourceAnchor.line};
    const std::optional<FailureSiteId> validateSite =
        program.failureMetadata().siteFor(validateOrigin);
    const std::optional<FailureSiteId> convertSite =
        program.failureMetadata().siteFor(convertOrigin);
    if (!validateSite || !convertSite || *validateSite == 0 ||
        *validateSite != *convertSite) {
      add("owned-argument hosted failures do not share one exact nonzero site");
    }
    const FailureSiteId hostedSite = validateSite.value_or(0);
    const FailureSiteDescriptor *hostedDescriptor =
        program.failureMetadata().findSite(hostedSite);
    const std::vector<DefinedFailureOutcome> expectedHostedOutcomes{
        {.code = DefinedFailureCode::NumericConversionOutOfRange,
         .detail = DefinedFailureDetail::HostedArgumentCount},
        {.code = DefinedFailureCode::HostedRuntimeContractFailure,
         .detail = DefinedFailureDetail::NegativeArgumentCount}};
    if (hostedDescriptor == nullptr ||
        hostedDescriptor->line != plan.sourceAnchor.line ||
        hostedDescriptor->start != plan.sourceAnchor.start ||
        hostedDescriptor->end != plan.sourceAnchor.end ||
        hostedDescriptor->outcomes != expectedHostedOutcomes) {
      add("owned-argument hosted failure site is not the exact two-outcome "
          "shared anchor");
    }
    const TakenOperation validate =
        take(MirHostedStartupOperationKind::ValidateArgumentCount,
             MirHostedStartupFailureBehavior::Detect, currentBlock, true, false,
             true, false);
    exactValue(validate, generatedInfo(SemanticType::Int64));
    exactInstruction(validate,
                     {.kind = MirInstructionKind::Load,
                      .result = validate.expected.value,
                      .definedFailure = {.localOrigins = {validateOrigin}},
                      .localFailureSites = {hostedSite},
                      .info = generatedInfo(SemanticType::Int64)});
    currentBlock =
        takeFailureRoute(currentBlock, validate.expected.instruction, {});

    const TakenOperation convert =
        take(MirHostedStartupOperationKind::ConvertArgumentCount,
             MirHostedStartupFailureBehavior::Detect, currentBlock, true, false,
             true, false);
    exactValue(convert, generatedInfo(SemanticType::Int32));
    exactInstruction(convert,
                     {.kind = MirInstructionKind::Compute,
                      .result = convert.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = validate.expected.value,
                                    .type = SemanticType::Int64}},
                      .operation = MirOperation::None,
                      .definedFailure = {.localOrigins = {convertOrigin}},
                      .localFailureSites = {hostedSite},
                      .info = generatedInfo(SemanticType::Int32)});
    currentBlock =
        takeFailureRoute(currentBlock, convert.expected.instruction, {});
    if (callsProgramInitialization) {
      const TakenOperation callBody =
          take(MirHostedStartupOperationKind::CallProgramInitialization,
               failureBehavior(moduleMayRaise), currentBlock, true, false,
               false, false);
      exactInstruction(callBody,
                       {.kind = MirInstructionKind::CallBody,
                        .definedFailure = propagation(
                            FailurePropagationKind::BodyCall, moduleMayRaise),
                        .bodyTarget = plan.programInitializationTarget,
                        .info = generatedInfo(SemanticType::Void)});
      if (moduleMayRaise) {
        currentBlock =
            takeFailureRoute(currentBlock, callBody.expected.instruction, {});
      }
    }

    const TakenOperation vector =
        take(MirHostedStartupOperationKind::ConstructArgumentVector,
             failureBehavior(vectorConstructor != nullptr &&
                             vectorConstructor->mayRaiseDefinedFailure),
             currentBlock, true, true, true, true);
    exactValue(vector, generatedInfo(vectorType));
    exactPlace(vector, {.root = MirPlaceRootKind::Value,
                        .value = vector.expected.value,
                        .type = vectorType,
                        .access = AccessMode::Mutable,
                        .traits = semanticTraits(vectorType)});
    exactDrop(vector, {.kind = MirDropObligationKind::Value,
                       .place = vector.expected.place,
                       .generatedValue = vector.expected.value,
                       .dropType = vectorDropType});
    const bool vectorMayRaise = vectorConstructor != nullptr &&
                                vectorConstructor->mayRaiseDefinedFailure;
    MirInstruction expectedVector{
        .kind = MirInstructionKind::Construct,
        .result = vector.expected.value,
        .definedFailure =
            propagation(FailurePropagationKind::Constructor, vectorMayRaise),
        .constructorTarget = plan.vectorConstructor,
        .info = generatedInfo(vectorType)};
    if (vectorMayRaise) {
      expectedVector.successResultDrop = vector.expected.dropObligation;
    } else {
      expectedVector.lifecycle = {{.kind = MirLifecycleEventKind::Initialize,
                                   .target = vector.expected.dropObligation}};
    }
    exactInstruction(vector, std::move(expectedVector));
    if (vectorMayRaise) {
      currentBlock = takeFailureRoute(currentBlock, vector.expected.instruction,
                                      {}, vector.expected.dropObligation);
    }

    const TakenOperation initializeIndex =
        take(MirHostedStartupOperationKind::InitializeArgumentIndex,
             MirHostedStartupFailureBehavior::None, currentBlock, true, true,
             false, false);
    exactPlace(initializeIndex,
               {.root = MirPlaceRootKind::Temporary,
                .temporary = 1,
                .type = SemanticType::Int32,
                .access = AccessMode::Mutable,
                .traits = semanticTraits(SemanticType::Int32)});
    exactInstruction(
        initializeIndex,
        {.kind = MirInstructionKind::Initialize,
         .destination = initializeIndex.expected.place,
         .operands = {{.kind = MirOperandKind::Constant,
                       .literal = Literal{std::uint64_t{0}},
                       .type = SemanticType::Int32}},
         .info = generatedInfo(SemanticType::Int32, ValueCategory::Place,
                               AccessMode::Mutable)});
    const MirBlockId loopHeaderBlock = nextGeneratedBlock++;
    const TakenOperation enterLoop =
        take(MirHostedStartupOperationKind::EnterArgumentLoop,
             MirHostedStartupFailureBehavior::None, currentBlock, false, false,
             false, false);
    exactTerminator(enterLoop, {.kind = MirTerminatorKind::Goto,
                                .target = loopHeaderBlock});

    const TakenOperation loadIndex =
        take(MirHostedStartupOperationKind::LoadArgumentIndex,
             MirHostedStartupFailureBehavior::None, loopHeaderBlock, true,
             false, true, false);
    exactValue(loadIndex, generatedInfo(SemanticType::Int32));
    exactInstruction(loadIndex,
                     {.kind = MirInstructionKind::Load,
                      .result = loadIndex.expected.value,
                      .operands = {{.kind = MirOperandKind::Copy,
                                    .place = initializeIndex.expected.place,
                                    .type = SemanticType::Int32}},
                      .info = generatedInfo(SemanticType::Int32)});
    const TakenOperation testIndex =
        take(MirHostedStartupOperationKind::TestArgumentIndex,
             MirHostedStartupFailureBehavior::None, loopHeaderBlock, true,
             false, true, false);
    exactValue(testIndex, generatedInfo(SemanticType::Bool));
    exactInstruction(testIndex,
                     {.kind = MirInstructionKind::Compute,
                      .result = testIndex.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = loadIndex.expected.value,
                                    .type = SemanticType::Int32},
                                   {.kind = MirOperandKind::Value,
                                    .value = convert.expected.value,
                                    .type = SemanticType::Int32}},
                      .operation = MirOperation::Less,
                      .info = generatedInfo(SemanticType::Bool)});
    const MirBlockId loopBodyBlock = nextGeneratedBlock++;
    const MirBlockId entryCallBlock = nextGeneratedBlock++;
    const TakenOperation branchLoop =
        take(MirHostedStartupOperationKind::BranchArgumentLoop,
             MirHostedStartupFailureBehavior::None, loopHeaderBlock, false,
             false, false, false);
    exactTerminator(branchLoop,
                    {.kind = MirTerminatorKind::Branch,
                     .value = MirOperand{.kind = MirOperandKind::Value,
                                         .value = testIndex.expected.value,
                                         .type = SemanticType::Bool},
                     .target = loopBodyBlock,
                     .elseTarget = entryCallBlock});

    MirBlockId iterationBlock = loopBodyBlock;

    const TakenOperation readView =
        take(MirHostedStartupOperationKind::ReadArgumentView,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(readView, generatedInfo(SemanticType::StringView));
    exactInstruction(readView,
                     {.kind = MirInstructionKind::Compute,
                      .result = readView.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = loadIndex.expected.value,
                                    .type = SemanticType::Int32}},
                      .operation = MirOperation::None,
                      .info = generatedInfo(SemanticType::StringView)});
    const TakenOperation stringInput =
        take(MirHostedStartupOperationKind::PrepareStringConstructorArgument,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(stringInput, generatedInfo(SemanticType::StringView));
    exactInstruction(stringInput,
                     {.kind = MirInstructionKind::CallInput,
                      .callInputRole = MirCallInputRole::Argument,
                      .callInputKind = HirCallInputKind::Value,
                      .result = stringInput.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = readView.expected.value,
                                    .type = SemanticType::StringView}},
                      .info = generatedInfo(SemanticType::StringView)});
    const bool stringMayRaise = stringConstructor != nullptr &&
                                stringConstructor->mayRaiseDefinedFailure;
    const TakenOperation string =
        take(MirHostedStartupOperationKind::ConstructArgumentString,
             failureBehavior(stringMayRaise), iterationBlock, true, true, true,
             true);
    exactValue(string, generatedInfo(stringType));
    exactPlace(string, {.root = MirPlaceRootKind::Value,
                        .value = string.expected.value,
                        .type = stringType,
                        .access = AccessMode::Mutable,
                        .traits = semanticTraits(stringType)});
    exactDrop(string, {.kind = MirDropObligationKind::Value,
                       .place = string.expected.place,
                       .generatedValue = string.expected.value,
                       .dropType = stringDropType});
    MirInstruction expectedString{
        .kind = MirInstructionKind::Construct,
        .result = string.expected.value,
        .operands = {{.kind = MirOperandKind::Value,
                      .value = stringInput.expected.value,
                      .type = SemanticType::StringView}},
        .parameterTypes = {SemanticType::StringView},
        .definedFailure =
            propagation(FailurePropagationKind::Constructor, stringMayRaise),
        .constructorTarget = plan.stringConstructor,
        .info = generatedInfo(stringType)};
    if (stringMayRaise) {
      expectedString.successResultDrop = string.expected.dropObligation;
    } else {
      expectedString.lifecycle = {{.kind = MirLifecycleEventKind::Initialize,
                                   .target = string.expected.dropObligation}};
    }
    exactInstruction(string, std::move(expectedString));
    if (stringMayRaise) {
      iterationBlock = takeFailureRoute(
          iterationBlock, string.expected.instruction,
          {vector.expected.dropObligation}, string.expected.dropObligation);
    }

    const TakenOperation appendReceiver =
        take(MirHostedStartupOperationKind::PrepareAppendReceiver,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(appendReceiver, generatedInfo(vectorType));
    exactInstruction(appendReceiver,
                     {.kind = MirInstructionKind::CallInput,
                      .callInputRole = MirCallInputRole::Receiver,
                      .callInputKind = HirCallInputKind::MutableBorrow,
                      .result = appendReceiver.expected.value,
                      .operands = {{.kind = MirOperandKind::BorrowWrite,
                                    .place = vector.expected.place,
                                    .type = vectorType}},
                      .info = generatedInfo(vectorType)});
    const TakenOperation appendArgument =
        take(MirHostedStartupOperationKind::PrepareAppendArgumentMove,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, true,
             true, true);
    exactValue(appendArgument, generatedInfo(stringType));
    exactPlace(appendArgument, {.root = MirPlaceRootKind::Temporary,
                                .temporary = 2,
                                .type = stringType,
                                .access = AccessMode::Mutable,
                                .traits = semanticTraits(stringType)});
    exactDrop(appendArgument, {.kind = MirDropObligationKind::PreparedParameter,
                               .place = appendArgument.expected.place,
                               .generatedValue = appendArgument.expected.value,
                               .dropType = stringDropType});
    exactInstruction(
        appendArgument,
        {.kind = MirInstructionKind::CallInput,
         .callInputRole = MirCallInputRole::Argument,
         .callInputKind = HirCallInputKind::MoveValue,
         .preparedParameterDrop = appendArgument.expected.dropObligation,
         .result = appendArgument.expected.value,
         .destination = appendArgument.expected.place,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = string.expected.value,
                       .type = stringType}},
         .info = generatedInfo(stringType),
         .lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                        .source = string.expected.dropObligation,
                        .target = appendArgument.expected.dropObligation}}});
    const bool appendMayRaise =
        append != nullptr && append->mayRaiseDefinedFailure;
    const TakenOperation appendCall =
        take(MirHostedStartupOperationKind::CallAppend,
             failureBehavior(appendMayRaise), iterationBlock, true, false,
             false, false);
    exactInstruction(
        appendCall,
        {.kind = MirInstructionKind::Call,
         .receiver = MirOperand{.kind = MirOperandKind::Value,
                                .value = appendReceiver.expected.value,
                                .type = vectorType},
         .operands = {{.kind = MirOperandKind::Value,
                       .value = appendArgument.expected.value,
                       .type = stringType}},
         .parameterTypes = {stringType},
         .definedFailure =
             propagation(FailurePropagationKind::DirectCall, appendMayRaise),
         .functionTarget = plan.appendFunction,
         .info = generatedInfo(SemanticType::Void),
         .lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                        .source = appendArgument.expected.dropObligation}}});
    if (appendMayRaise) {
      iterationBlock =
          takeFailureRoute(iterationBlock, appendCall.expected.instruction,
                           {vector.expected.dropObligation});
    }
    const TakenOperation advance =
        take(MirHostedStartupOperationKind::AdvanceArgumentIndex,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(advance, generatedInfo(SemanticType::Int32, ValueCategory::Place,
                                      AccessMode::Mutable));
    exactInstruction(advance, {.kind = MirInstructionKind::Modify,
                               .result = advance.expected.value,
                               .destination = initializeIndex.expected.place,
                               .operation = MirOperation::PreIncrement,
                               .info = generatedInfo(SemanticType::Int32,
                                                     ValueCategory::Place,
                                                     AccessMode::Mutable)});
    const TakenOperation continueLoop =
        take(MirHostedStartupOperationKind::ContinueArgumentLoop,
             MirHostedStartupFailureBehavior::None, iterationBlock, false,
             false, false, false);
    exactTerminator(continueLoop, {.kind = MirTerminatorKind::Goto,
                                   .target = loopHeaderBlock});

    MirBlockId finalBlock = entryCallBlock;

    const TakenOperation entryCount =
        take(MirHostedStartupOperationKind::PrepareEntryCount,
             MirHostedStartupFailureBehavior::None, finalBlock, true, false,
             true, false);
    exactValue(entryCount, generatedInfo(SemanticType::Int32));
    exactInstruction(entryCount, {.kind = MirInstructionKind::CallInput,
                                  .callInputRole = MirCallInputRole::Argument,
                                  .callInputKind = HirCallInputKind::Value,
                                  .result = entryCount.expected.value,
                                  .operands = {{.kind = MirOperandKind::Value,
                                                .value = convert.expected.value,
                                                .type = SemanticType::Int32}},
                                  .info = generatedInfo(SemanticType::Int32)});
    const TakenOperation entryArguments =
        take(MirHostedStartupOperationKind::PrepareEntryArgumentsMove,
             MirHostedStartupFailureBehavior::None, finalBlock, true, true,
             true, true);
    exactValue(entryArguments, generatedInfo(vectorType));
    exactPlace(entryArguments, {.root = MirPlaceRootKind::Temporary,
                                .temporary = 3,
                                .type = vectorType,
                                .access = AccessMode::Mutable,
                                .traits = semanticTraits(vectorType)});
    exactDrop(entryArguments, {.kind = MirDropObligationKind::PreparedParameter,
                               .place = entryArguments.expected.place,
                               .generatedValue = entryArguments.expected.value,
                               .dropType = vectorDropType});
    exactInstruction(
        entryArguments,
        {.kind = MirInstructionKind::CallInput,
         .callInputRole = MirCallInputRole::Argument,
         .callInputIndex = 1,
         .callInputKind = HirCallInputKind::MoveValue,
         .preparedParameterDrop = entryArguments.expected.dropObligation,
         .result = entryArguments.expected.value,
         .destination = entryArguments.expected.place,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = vector.expected.value,
                       .type = vectorType}},
         .info = generatedInfo(vectorType),
         .lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                        .source = vector.expected.dropObligation,
                        .target = entryArguments.expected.dropObligation}}});
    const TakenOperation callEntry =
        take(MirHostedStartupOperationKind::CallEntry,
             failureBehavior(entry.mayRaiseDefinedFailure), finalBlock, true,
             false, true, false);
    exactValue(callEntry, generatedInfo(SemanticType::Int32));
    exactInstruction(
        callEntry,
        {.kind = MirInstructionKind::Call,
         .result = callEntry.expected.value,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = entryCount.expected.value,
                       .type = SemanticType::Int32},
                      {.kind = MirOperandKind::Value,
                       .value = entryArguments.expected.value,
                       .type = vectorType}},
         .parameterTypes = {SemanticType::Int32, vectorType},
         .definedFailure = propagation(FailurePropagationKind::DirectCall,
                                       entry.mayRaiseDefinedFailure),
         .functionTarget = entry.id,
         .info = generatedInfo(SemanticType::Int32),
         .lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                        .source = entryArguments.expected.dropObligation}}});
    if (entry.mayRaiseDefinedFailure) {
      finalBlock =
          takeFailureRoute(finalBlock, callEntry.expected.instruction, {});
    }
    const TakenOperation returnEntry =
        take(MirHostedStartupOperationKind::ReturnEntry,
             MirHostedStartupFailureBehavior::None, finalBlock, false, false,
             false, false);
    exactTerminator(returnEntry,
                    {.kind = MirTerminatorKind::Return,
                     .value = MirOperand{.kind = MirOperandKind::Value,
                                         .value = callEntry.expected.value,
                                         .type = SemanticType::Int32}});

    if (plan.argumentIndexPlace != initializeIndex.expected.place ||
        plan.argumentVectorPlace != vector.expected.place ||
        plan.stabilizedCount != convert.expected.value ||
        plan.argumentVector != vector.expected.value ||
        plan.entryResult != callEntry.expected.value) {
      add("owned-argument hosted-startup summary identities are invalid");
    }
  } else {
    add("hosted-startup plan has an invalid entry kind");
  }

  if (nextOperation != plan.operations.size() + 1 ||
      nextInstruction != body->instructionCount() + 1 ||
      nextPlace != body->places.size() + 1 ||
      nextValue != body->values.size() + 1 ||
      nextDrop != body->dropObligations.size() + 1) {
    add("hosted-startup plan or body retains an extra generated entity");
  }
  return result;
}

} // namespace

MirDefinedFailureEffects
deriveMirDefinedFailureEffects(const MirProgram &program) {
  MirDefinedFailureEffects effects;
  effects.functions =
      deriveMirFunctionDefinedFailureEffects(program, &effects.destructors);
  effects.constructors.assign(program.constructorInstances().size(), true);
  MirOwnedFailureClosure(program, effects).derive();
  return effects;
}

bool mirTypeMoveIsDefinedFailureFree(const MirProgram &program,
                                     const SemanticType &type) {
  const auto closedType = [](const auto &self,
                             const SemanticType &candidate) -> bool {
    if (candidate.kind == SemanticType::Unknown ||
        candidate.kind == SemanticType::TypeParameter ||
        candidate.kind == SemanticType::TypePack ||
        candidate.kind == SemanticType::TypeName ||
        std::any_of(candidate.valueArguments.begin(),
                    candidate.valueArguments.end(),
                    [](const CompileTimeValue &value) {
                      return value.kind == CompileTimeValue::Parameter;
                    })) {
      return false;
    }
    return std::all_of(
        candidate.arguments.begin(), candidate.arguments.end(),
        [&](const SemanticType &argument) { return self(self, argument); });
  };
  if (!closedType(closedType, type)) {
    return false;
  }

  std::unordered_set<HirClassInstanceId> visiting;
  const auto prove = [&](const auto &self,
                         const SemanticType &candidate) -> bool {
    switch (candidate.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::Bool:
    case SemanticType::Char:
    case SemanticType::StringView:
    case SemanticType::CString:
    case SemanticType::NullPtr:
    case SemanticType::RawPointer:
    case SemanticType::Enum:
    case SemanticType::Reference:
      return true;
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
    case SemanticType::PrefixStorage:
      // These compiler capabilities move only their control/storage handle;
      // they do not move or destroy the represented element.
      return candidate.arguments.size() == 1;
    case SemanticType::Array:
      return candidate.arguments.size() == 1 &&
             candidate.arrayLengthParameterId == 0 &&
             candidate.arrayLength != 0 &&
             self(self, candidate.arguments.front());
    case SemanticType::Expected:
      return candidate.arguments.size() == 2 &&
             (candidate.arguments.front() == SemanticType::Void ||
              self(self, candidate.arguments.front())) &&
             self(self, candidate.arguments.back());
    case SemanticType::Unexpected:
      return candidate.arguments.size() == 1 &&
             self(self, candidate.arguments.front());
    case SemanticType::Class: {
      const MirClassInstance *instance = nullptr;
      for (const MirClassInstance &item : program.classInstances()) {
        if (item.type != candidate) {
          continue;
        }
        if (instance != nullptr) {
          return false;
        }
        instance = &item;
      }
      if (instance == nullptr || instance->id == 0 ||
          instance->moveConstructor != SpecialMemberStatus::Generated ||
          instance->destructorStatus != SpecialMemberStatus::Generated ||
          instance->destructor || instance->requiresActiveDropState ||
          instance->abstract || instance->polymorphic ||
          instance->unionLayout || !instance->bases.empty() ||
          !instance->structuralBases.empty() ||
          !visiting.insert(instance->id).second) {
        return false;
      }
      const bool fields = std::all_of(instance->declaredFields.begin(),
                                      instance->declaredFields.end(),
                                      [&](const MirClassFieldInfo &field) {
                                        return field.field != 0 &&
                                               field.symbol != 0 &&
                                               self(self, field.type);
                                      });
      visiting.erase(instance->id);
      return fields;
    }
    case SemanticType::Unknown:
    case SemanticType::Void:
    case SemanticType::TypeParameter:
    case SemanticType::TypePack:
    case SemanticType::TypeName:
    case SemanticType::Function:
    case SemanticType::Lambda:
      return false;
    }
    return false;
  };
  return prove(prove, type);
}

std::vector<bool>
deriveMirScalarDefinedFailureEffects(const MirProgram &program) {
  return deriveMirDefinedFailureEffects(program).functions;
}

MirVerificationResult verifyMirProgram(const MirProgram &program) {
  MirVerificationResult result;
  if (!program.valid()) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .message = "MIR program is marked invalid"});
  }
  const FailureMetadataVerificationResult failureMetadata =
      verifyFailureMetadata(program.failureMetadata());
  for (const std::string &error : failureMetadata.errors) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .message = "invalid failure metadata: " + error});
  }

  const auto verifyBody = [&](const MirBody &body, std::size_t owner) {
    append(result, verifyMirBody(body, owner));
    append(result,
           verifyMirCheckedIntegerFailureContracts(program, body, owner));
    for (const MirPlace &place : body.places) {
      const bool classPackElement =
          place.type.kind == SemanticType::Class &&
          place.projections.size() == 1 &&
          place.projections.front().kind == MirProjectionKind::PackElement;
      if (!classPackElement) {
        continue;
      }
      const auto instance = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const MirClassInstance &candidate) {
            return candidate.type == place.type;
          });
      if (instance == program.classInstances().end() ||
          place.traits != instance->traits) {
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .message = "concrete class pack element traits do not match its "
                        "class instance"});
      }
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        for (std::size_t originIndex = 0;
             originIndex < instruction.definedFailure.localOrigins.size();
             ++originIndex) {
          const std::optional<FailureSiteId> expected =
              program.failureMetadata().siteFor(
                  instruction.definedFailure.localOrigins[originIndex]);
          if (!expected ||
              originIndex >= instruction.localFailureSites.size() ||
              instruction.localFailureSites[originIndex] != *expected) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .block = block.id,
                 .instruction = instruction.id,
                 .message = "defined-failure origin does not retain its exact "
                            "artifact-local site"});
          }
        }
      }
    }
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

  append(result, verifyMirProgramInitialization(program));
  append(result, verifyMirHostedStartup(program));
  if (program.hostedStartupPlan() && program.hostedStartup() != nullptr) {
    append(result, verifyMirBody(*program.hostedStartup(),
                                 program.hostedStartupPlan()->entry));
  }
  verifyBody(program.module(), 0);
  const auto typeRequiresActiveCleanup =
      [&](const SemanticType &type, const auto &query) -> std::optional<bool> {
    switch (type.kind) {
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
    case SemanticType::PrefixStorage:
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
    const auto validSpecialMemberStatus = [](SpecialMemberStatus status) {
      return status == SpecialMemberStatus::Declared ||
             status == SpecialMemberStatus::Generated ||
             status == SpecialMemberStatus::Deleted;
    };
    if (!validSpecialMemberStatus(instance.defaultConstructor) ||
        !validSpecialMemberStatus(instance.copyConstructor) ||
        !validSpecialMemberStatus(instance.moveConstructor) ||
        !validSpecialMemberStatus(instance.copyAssignment) ||
        !validSpecialMemberStatus(instance.moveAssignment) ||
        !validSpecialMemberStatus(instance.destructorStatus) ||
        ((instance.destructorStatus == SpecialMemberStatus::Declared) !=
         instance.destructor.has_value())) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class special-member metadata is invalid or does not "
                      "match its declared destructor"});
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
    const bool hasMergedStaticInitialization = std::any_of(
        program.programInitializationPlan().steps.begin(),
        program.programInitializationPlan().steps.end(),
        [&](const MirProgramInitializationStep &step) {
          return step.storageKind == ProgramStorageKind::StaticField &&
                 step.ownerClass == instance.id;
        });
    if (hasMergedStaticInitialization &&
        !hasCanonicalEmptyStaticInitializerBody(
            instance.staticFieldInitializers, program.module())) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::StaticFieldInitializers,
           .owner = instance.id,
           .message = "legacy static-field initializer body is not the "
                      "canonical empty migration shell"});
    }
  }
  std::size_t entryPoints = 0;
  for (std::size_t index = 0; index < program.functionInstances().size();
       ++index) {
    const MirFunctionInstance &instance = program.functionInstances()[index];
    if (instance.id != index + 1 || instance.declaration == 0) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "function instance identity or declaration is invalid"});
    }
    switch (instance.definitionKind) {
    case MirFunctionInstance::DefinitionKind::Source:
    case MirFunctionInstance::DefinitionKind::RuntimeBinding:
    case MirFunctionInstance::DefinitionKind::Declaration:
      break;
    default:
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "function definition kind is invalid"});
      break;
    }
    if (instance.definitionKind != MirDefinitionKind::Source &&
        !hasCanonicalBodylessDefinition(
            instance.body, MirBodyKind::Function, instance.returnType,
            instance.parameterTypes, instance.parameterBindings)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "bodyless function declaration or runtime binding has "
                      "noncanonical MIR body"});
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
  const MirDefinedFailureEffects derivedFailureEffects =
      deriveMirDefinedFailureEffects(program);
  if (derivedFailureEffects.functions.size() !=
          program.functionInstances().size() ||
      derivedFailureEffects.constructors.size() !=
          program.constructorInstances().size() ||
      derivedFailureEffects.destructors.size() !=
          program.destructorInstances().size()) {
    result.errors.push_back(
        {.bodyKind = MirBodyKind::Module,
         .message = "defined-failure effect summary has the wrong size"});
  } else {
    for (const MirFunctionInstance &function : program.functionInstances()) {
      if (function.id == 0 ||
          function.id > derivedFailureEffects.functions.size() ||
          function.mayRaiseDefinedFailure ||
          !derivedFailureEffects.functions[function.id - 1]) {
        continue;
      }
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = function.id,
           .message = "failure-free function summary is not proved by an "
                      "acyclic closed scalar MIR call graph"});
    }
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
    if (instance.definitionKind != MirDefinitionKind::Source &&
        instance.definitionKind != MirDefinitionKind::Declaration) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor definition kind is invalid"});
    }
    if (instance.definitionKind == MirDefinitionKind::Declaration &&
        !hasCanonicalBodylessDefinition(
            instance.body, MirBodyKind::Constructor, SemanticType::Void,
            instance.parameterTypes, instance.parameterBindings)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "bodyless constructor declaration has noncanonical MIR "
                      "body"});
    }
    if (instance.id != 0 &&
        instance.id <= derivedFailureEffects.constructors.size() &&
        !instance.mayRaiseDefinedFailure &&
        derivedFailureEffects.constructors[instance.id - 1]) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "failure-free constructor summary is not proved by "
                      "bounded MIR construction effects"});
    }
    const MirClassInstance *constructorOwner =
        program.findClassInstance(instance.owner);
    const auto scalarInitializerType = [](const SemanticType &type) {
      switch (type.kind) {
      case SemanticType::Int8:
      case SemanticType::Int16:
      case SemanticType::Int32:
      case SemanticType::Int64:
      case SemanticType::UInt8:
      case SemanticType::UInt16:
      case SemanticType::UInt32:
      case SemanticType::UInt64:
      case SemanticType::Bool:
      case SemanticType::Char:
        return true;
      default:
        return false;
      }
    };
    std::vector<std::size_t> initializerStageCounts(
        instance.initializers.size());
    std::vector<std::size_t> initializerBoundaryCounts(
        instance.initializers.size());
    bool exactInitializerStages = constructorOwner != nullptr;
    for (const MirBlock &block : instance.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.constructorInitializer == 0) {
          continue;
        }
        const std::size_t stage = instruction.constructorInitializer;
        if (stage > instance.initializers.size()) {
          exactInitializerStages = false;
          continue;
        }
        ++initializerStageCounts[stage - 1];
        const MirConstructorInitializer &initializer =
            instance.initializers[stage - 1];
        const MirClassFieldInfo *field =
            constructorOwner == nullptr ? nullptr
                                        : [&]() -> const MirClassFieldInfo * {
          const auto found =
              std::find_if(constructorOwner->declaredFields.begin(),
                           constructorOwner->declaredFields.end(),
                           [&](const MirClassFieldInfo &candidate) {
                             return candidate.symbol == initializer.field;
                           });
          return found == constructorOwner->declaredFields.end() ? nullptr
                                                                 : &*found;
        }();
        const MirPlace *destination =
            instruction.destination
                ? instance.body.findPlace(*instruction.destination)
                : nullptr;
        const MirValue *sourceValue =
            instruction.operands.size() == 1 &&
                    instruction.operands.front().kind == MirOperandKind::Value
                ? instance.body.findValue(instruction.operands.front().value)
                : nullptr;
        exactInitializerStages =
            exactInitializerStages &&
            initializer.kind == ConstructorInitializerTargetKind::Field &&
            initializer.arguments.size() == 1 && field != nullptr &&
            field->type == initializer.targetType &&
            instruction.kind == MirInstructionKind::Initialize &&
            !instruction.result && instruction.destination &&
            instruction.hirValue == initializer.arguments.front() &&
            instruction.operands.size() == 1 && sourceValue != nullptr &&
            sourceValue->sourceValue == initializer.arguments.front() &&
            instruction.operands.front().type == initializer.targetType &&
            instruction.info.type == initializer.targetType &&
            destination != nullptr &&
            destination->root == MirPlaceRootKind::This &&
            destination->projections.size() == 1 &&
            destination->projections.front().kind == MirProjectionKind::Field &&
            destination->projections.front().field == initializer.field &&
            destination->sourceValue == initializer.arguments.front() &&
            destination->type == initializer.targetType;
      }
    }
    for (const MirFullExpression &full : instance.body.fullExpressions) {
      if (full.constructorInitializer == 0) {
        continue;
      }
      if (full.constructorInitializer > instance.initializers.size()) {
        exactInitializerStages = false;
        continue;
      }
      ++initializerBoundaryCounts[full.constructorInitializer - 1];
      exactInitializerStages =
          exactInitializerStages &&
          full.roots ==
              instance.initializers[full.constructorInitializer - 1].arguments;
    }
    for (std::size_t initializerIndex = 0;
         initializerIndex < instance.initializers.size(); ++initializerIndex) {
      const MirConstructorInitializer &initializer =
          instance.initializers[initializerIndex];
      const auto field =
          constructorOwner == nullptr
              ? std::vector<MirClassFieldInfo>::const_iterator{}
              : std::find_if(constructorOwner->declaredFields.begin(),
                             constructorOwner->declaredFields.end(),
                             [&](const MirClassFieldInfo &candidate) {
                               return candidate.symbol == initializer.field;
                             });
      const bool explicitScalarField =
          constructorOwner != nullptr &&
          field != constructorOwner->declaredFields.end() &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          !initializer.storesReference && !initializer.constructorTarget &&
          !initializer.generatedDefault && initializer.arguments.size() == 1 &&
          field->type == initializer.targetType &&
          scalarInitializerType(initializer.targetType) &&
          field->dropKind == DropKind::Trivial && !field->requiresActiveCleanup;
      // A lexical field completed from one owning argument may publish an
      // explicit rollback-armed stage; whether it does depends on the
      // argument's lowering-time ownership, so its stage count is at most
      // one rather than exactly one. This is deliberately based on the
      // field's lifecycle contract rather than compiler-known type kinds:
      // storage and source-defined owning classes obey the same rule.
      const bool explicitOwningField =
          constructorOwner != nullptr &&
          field != constructorOwner->declaredFields.end() &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          !initializer.storesReference && !initializer.generatedDefault &&
          initializer.arguments.size() == 1 &&
          field->type == initializer.targetType &&
          field->dropKind == DropKind::Lexical;
      // Constructor full-expression boundaries are shared lifecycle facts,
      // not scalar-stage markers: reference, nested-construction, and
      // generated-default initializers can legitimately publish one. Only the
      // bounded scalar form requires a matching Initialize stage.
      if ((explicitScalarField &&
           (initializerStageCounts[initializerIndex] != 1 ||
            initializerBoundaryCounts[initializerIndex] != 1)) ||
          (!explicitScalarField && explicitOwningField &&
           (initializerStageCounts[initializerIndex] > 1 ||
            initializerBoundaryCounts[initializerIndex] > 1)) ||
          (!explicitScalarField && !explicitOwningField &&
           (initializerStageCounts[initializerIndex] != 0 ||
            initializerBoundaryCounts[initializerIndex] > 1))) {
        exactInitializerStages = false;
      }
    }
    if (!exactInitializerStages) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor initializer stages do not exactly cover "
                      "their scalar or owning field initializers"});
    }
    for (const MirConstructorInitializer &initializer : instance.initializers) {
      if (!initializer.ownedParameter) {
        continue;
      }
      const std::size_t parameter = *initializer.ownedParameter;
      const bool exactField =
          constructorOwner != nullptr &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          initializer.field != 0 && initializer.arguments.size() == 1 &&
          parameter < instance.parameterTypes.size() &&
          parameter < instance.parameterBindings.size() &&
          initializer.targetType == instance.parameterTypes[parameter] &&
          std::any_of(
              constructorOwner->declaredFields.begin(),
              constructorOwner->declaredFields.end(),
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
    if (instance.definitionKind != MirDefinitionKind::Source &&
        instance.definitionKind != MirDefinitionKind::Declaration) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "destructor definition kind is invalid"});
    }
    if (instance.definitionKind == MirDefinitionKind::Declaration &&
        !hasCanonicalBodylessDefinition(instance.body, MirBodyKind::Destructor,
                                        SemanticType::Void, {}, {})) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "bodyless destructor declaration has noncanonical MIR "
                      "body"});
    }
    if (instance.id != 0 &&
        instance.id <= derivedFailureEffects.destructors.size() &&
        !instance.mayRaiseDefinedFailure &&
        derivedFailureEffects.destructors[instance.id - 1]) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "failure-free destructor summary is not proved by "
                      "bounded MIR cleanup effects"});
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
  if (const MirBody *hosted = program.hostedStartup()) {
    append(result, verifyMirDropTargets(program, *hosted,
                                        program.hostedStartupPlan()->entry,
                                        exactActiveCleanup));
  }
  return result;
}

MirVerificationResult
verifyMirOptimizationCoherence(const MirProgram &source,
                               const MirProgram &optimized) {
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

  if (source.executionProfile() != optimized.executionProfile() ||
      source.classInstances().size() != optimized.classInstances().size() ||
      source.functionInstances().size() !=
          optimized.functionInstances().size() ||
      source.constructorInstances().size() !=
          optimized.constructorInstances().size() ||
      source.destructorInstances().size() !=
          optimized.destructorInstances().size() ||
      source.lambdaInstances().size() != optimized.lambdaInstances().size()) {
    reject({}, "optimized MIR changed program profile or concrete instance "
               "cardinality");
    return result;
  }

  MirProgram candidate = source;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(source)) {
    const MirBody *sourceBody = findMirBody(source, address);
    const MirBody *optimizedBody = findMirBody(optimized, address);
    MirBody *candidateBody = findMirBody(candidate, address);
    if (sourceBody == nullptr || optimizedBody == nullptr ||
        candidateBody == nullptr ||
        sourceBody->blocks.size() != optimizedBody->blocks.size()) {
      reject(address,
             "optimized MIR changed a source body or its block schedule");
      return result;
    }
    for (std::size_t blockIndex = 0; blockIndex < sourceBody->blocks.size();
         ++blockIndex) {
      const MirBlock &sourceBlock = sourceBody->blocks[blockIndex];
      const MirBlock &optimizedBlock = optimizedBody->blocks[blockIndex];
      MirBlock &candidateBlock = candidateBody->blocks[blockIndex];
      if (sourceBlock.terminator.provenance.kind !=
          MirTerminatorProvenanceKind::None) {
        reject(address,
               "source MIR already contains optimizer terminator provenance",
               sourceBlock.id);
        return result;
      }
      if (optimizedBlock.terminator.provenance.kind ==
          MirTerminatorProvenanceKind::BranchFold) {
        const MirTerminatorProvenance &provenance =
            optimizedBlock.terminator.provenance;
        const bool sourceBranch =
            sourceBlock.terminator.kind == MirTerminatorKind::Branch &&
            sourceBlock.terminator.value &&
            sourceBlock.terminator.value->kind == MirOperandKind::Value &&
            sourceBlock.terminator.value->value == provenance.foldSourceValue;
        const bool *condition = nullptr;
        for (const MirBlock &candidate : optimizedBody->blocks) {
          for (const MirInstruction &instruction : candidate.instructions) {
            if (instruction.result &&
                *instruction.result == provenance.foldSourceValue &&
                instruction.kind == MirInstructionKind::Compute &&
                instruction.operation == MirOperation::Literal &&
                instruction.literal) {
              condition = std::get_if<bool>(&*instruction.literal);
            }
          }
        }
        const MirBlockId takenTarget =
            condition == nullptr
                ? 0
                : (*condition ? sourceBlock.terminator.target
                              : sourceBlock.terminator.elseTarget);
        MirTerminator expected = sourceBlock.terminator;
        expected.kind = MirTerminatorKind::Goto;
        expected.value.reset();
        expected.target = takenTarget;
        expected.elseTarget = 0;
        expected.provenance = provenance;
        if (!sourceBranch || condition == nullptr || takenTarget == 0 ||
            optimizedBlock.terminator != expected) {
          reject(address,
                 "optimized MIR branch fold does not replay from its "
                 "retained condition",
                 sourceBlock.id);
          return result;
        }
        candidateBlock.terminator = expected;
      } else if (optimizedBlock.terminator.provenance.kind !=
                 MirTerminatorProvenanceKind::None) {
        reject(address,
               "terminator rewrite provenance appears outside an authorized "
               "branch fold",
               sourceBlock.id);
        return result;
      }
      if (sourceBlock.id != optimizedBlock.id ||
          sourceBlock.instructions.size() !=
              optimizedBlock.instructions.size()) {
        reject(address, "optimized MIR changed a source block schedule",
               sourceBlock.id);
        return result;
      }
      for (std::size_t instructionIndex = 0;
           instructionIndex < sourceBlock.instructions.size();
           ++instructionIndex) {
        const MirInstruction &sourceInstruction =
            sourceBlock.instructions[instructionIndex];
        const MirInstruction &optimizedInstruction =
            optimizedBlock.instructions[instructionIndex];
        if (sourceInstruction.literalProvenance.kind !=
                MirLiteralProvenanceKind::None &&
            sourceInstruction.literalProvenance.kind !=
                MirLiteralProvenanceKind::Source) {
          reject(address,
                 "source MIR already contains optimizer rewrite provenance",
                 sourceBlock.id, sourceInstruction.id);
          return result;
        }
        if (optimizedInstruction.literalProvenance.kind ==
            MirLiteralProvenanceKind::ComputeFold) {
          // A compute fold replaces the exact source computation whose
          // operation and value operands the provenance retains; the
          // folded literal must replay through the single evaluation
          // authority over the source operands' literals.
          const MirLiteralProvenance &provenance =
              optimizedInstruction.literalProvenance;
          bool operandsMatch = sourceInstruction.operands.size() ==
                                   provenance.sourceValues.size() &&
                               !provenance.sourceValues.empty();
          for (std::size_t operandIndex = 0;
               operandsMatch &&
               operandIndex < sourceInstruction.operands.size();
               ++operandIndex) {
            const MirOperand &operand =
                sourceInstruction.operands[operandIndex];
            operandsMatch =
                operand.kind == MirOperandKind::Value &&
                operand.value == provenance.sourceValues[operandIndex];
          }
          if (sourceInstruction.id != optimizedInstruction.id ||
              sourceInstruction.kind != MirInstructionKind::Compute ||
              sourceInstruction.operation != provenance.sourceOperation ||
              !operandsMatch || sourceInstruction.literal ||
              sourceInstruction.literalProvenance.kind !=
                  MirLiteralProvenanceKind::None ||
              optimizedInstruction.kind != MirInstructionKind::Compute ||
              optimizedInstruction.operation != MirOperation::Literal ||
              !optimizedInstruction.literal) {
            reject(address,
                   "optimized MIR compute fold does not match an exact "
                   "source computation",
                   sourceBlock.id, optimizedInstruction.id);
            return result;
          }
          MirInstruction expected = sourceInstruction;
          expected.operation = MirOperation::Literal;
          expected.operands.clear();
          expected.literal = optimizedInstruction.literal;
          expected.literalProvenance = provenance;
          if (optimizedInstruction != expected) {
            reject(address,
                   "optimized MIR compute fold changed fields outside its "
                   "exact rewrite allowlist",
                   sourceBlock.id, optimizedInstruction.id);
            return result;
          }
          candidateBlock.instructions[instructionIndex] = std::move(expected);
          continue;
        }
        if (optimizedInstruction.literalProvenance.kind !=
            MirLiteralProvenanceKind::IdentityFold) {
          continue;
        }
        if (sourceInstruction.id != optimizedInstruction.id ||
            sourceInstruction.kind != MirInstructionKind::Compute ||
            sourceInstruction.operation != MirOperation::Identity ||
            sourceInstruction.operands.size() != 1 ||
            sourceInstruction.operands.front().kind != MirOperandKind::Value ||
            sourceInstruction.operands.front().value == 0 ||
            sourceInstruction.literal ||
            sourceInstruction.literalProvenance.kind !=
                MirLiteralProvenanceKind::None ||
            sourceInstruction.literalProvenance.sourceValue != 0 ||
            optimizedInstruction.kind != MirInstructionKind::Compute ||
            optimizedInstruction.operation != MirOperation::Literal ||
            !optimizedInstruction.literal) {
          reject(address,
                 "optimized MIR rewrite does not match an exact source "
                 "identity computation",
                 sourceBlock.id, optimizedInstruction.id);
          return result;
        }
        MirInstruction expected = sourceInstruction;
        expected.operation = MirOperation::Literal;
        expected.operands.clear();
        expected.literal = optimizedInstruction.literal;
        expected.literalProvenance = {
            .kind = MirLiteralProvenanceKind::IdentityFold,
            .sourceValue = sourceInstruction.operands.front().value};
        if (optimizedInstruction != expected) {
          reject(address,
                 "optimized MIR identity fold changed fields outside its "
                 "exact rewrite allowlist",
                 sourceBlock.id, optimizedInstruction.id);
          return result;
        }
        candidateBlock.instructions[instructionIndex] = std::move(expected);
      }
    }
    if (!rebuildMirValueUses(*candidateBody)) {
      reject(address,
             "authorized MIR rewrites could not rebuild exact value uses");
      return result;
    }
    // An authorized branch fold changes successors, so the candidate's
    // reachability recomputes deterministically before the exact compare.
    rebuildMirReachability(*candidateBody);
  }

  const MirVerificationResult candidateVerification =
      verifyMirProgram(candidate);
  if (!candidateVerification.valid()) {
    return candidateVerification;
  }
  if (candidate != optimized) {
    reject({}, "optimized MIR contains a change not authorized by MIR rewrite "
               "provenance");
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
