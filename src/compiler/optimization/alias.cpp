#include "gti/optimization/alias.h"

#include "gti/mir_effects.h"
#include "gti/optimization/coverage.h"

#include <algorithm>
#include <optional>
#include <set>

namespace lang {
namespace {

[[nodiscard]] bool rawProjection(const MirPlace &place) {
  return std::any_of(place.projections.begin(), place.projections.end(),
                     [](const MirPlaceProjection &projection) {
                       return projection.kind == MirProjectionKind::RawIndex ||
                              projection.kind ==
                                  MirProjectionKind::RawDereference;
                     });
}

[[nodiscard]] bool indirectProjection(const MirPlace &place) {
  return std::any_of(
      place.projections.begin(), place.projections.end(),
      [](const MirPlaceProjection &projection) {
        return projection.kind == MirProjectionKind::Dereference ||
               projection.kind == MirProjectionKind::RawDereference;
      });
}

[[nodiscard]] bool sameRoot(const MirPlace &left, const MirPlace &right) {
  if (left.root != right.root ||
      left.hostedStartupOperation != right.hostedStartupOperation) {
    return false;
  }
  switch (left.root) {
  case MirPlaceRootKind::Binding:
    return left.binding != 0 && left.binding == right.binding;
  case MirPlaceRootKind::Symbol:
    return left.symbol != 0 && left.symbol == right.symbol &&
           left.capture == right.capture;
  case MirPlaceRootKind::This:
    return true;
  case MirPlaceRootKind::Temporary:
    return left.temporary != 0 && left.temporary == right.temporary;
  case MirPlaceRootKind::Value:
    return left.value != 0 && left.value == right.value;
  case MirPlaceRootKind::Loan:
    return left.loan != 0 && left.loan == right.loan;
  }
  return false;
}

[[nodiscard]] bool preciseRoot(const MirPlace &place) {
  switch (place.root) {
  case MirPlaceRootKind::Binding:
    return place.binding != 0;
  case MirPlaceRootKind::Symbol:
    return place.symbol != 0;
  case MirPlaceRootKind::This:
    return true;
  case MirPlaceRootKind::Temporary:
    return place.temporary != 0;
  case MirPlaceRootKind::Value:
  case MirPlaceRootKind::Loan:
    return false;
  }
  return false;
}

[[nodiscard]] std::optional<bool>
overlappingFields(const MirProgram &program, SymbolId left, SymbolId right) {
  bool foundOwner = false;
  for (const MirClassInstance &instance : program.classInstances()) {
    const auto contains = [&](SymbolId field) {
      return std::any_of(instance.declaredFields.begin(),
                         instance.declaredFields.end(),
                         [&](const MirClassFieldInfo &candidate) {
                           return candidate.symbol == field;
                         });
    };
    if (!contains(left) || !contains(right)) {
      continue;
    }
    foundOwner = true;
    if (instance.kind == ClassKind::Union) {
      return true;
    }
  }
  return foundOwner ? std::optional<bool>{false} : std::nullopt;
}

[[nodiscard]] MirAliasRelation projectionRelation(const MirProgram &program,
                                                  const MirPlace &left,
                                                  const MirPlace &right) {
  const std::size_t common =
      std::min(left.projections.size(), right.projections.size());
  for (std::size_t index = 0; index < common; ++index) {
    const MirPlaceProjection &lhs = left.projections[index];
    const MirPlaceProjection &rhs = right.projections[index];
    if (lhs == rhs) {
      continue;
    }
    if (lhs.kind == MirProjectionKind::Field &&
        rhs.kind == MirProjectionKind::Field && lhs.field != 0 &&
        rhs.field != 0 && lhs.field != rhs.field) {
      const std::optional<bool> overlap =
          overlappingFields(program, lhs.field, rhs.field);
      return overlap && !*overlap ? MirAliasRelation::NoAlias
                                  : MirAliasRelation::MayAlias;
    }
    if ((lhs.kind == MirProjectionKind::Index ||
         lhs.kind == MirProjectionKind::PackElement) &&
        lhs.kind == rhs.kind && lhs.constantIndex && rhs.constantIndex &&
        lhs.constantIndex != rhs.constantIndex) {
      return MirAliasRelation::NoAlias;
    }
    if (lhs.kind == MirProjectionKind::PackElement &&
        rhs.kind == MirProjectionKind::PackElement &&
        lhs.selection != rhs.selection) {
      return MirAliasRelation::NoAlias;
    }
    return MirAliasRelation::MayAlias;
  }
  return left.projections.size() == right.projections.size()
             ? MirAliasRelation::MustAlias
             : MirAliasRelation::MayAlias;
}

[[nodiscard]] bool carriesPlaceIdentity(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Reference:
  case SemanticType::RawPointer:
  case SemanticType::UniqueOwner:
  case SemanticType::SharedPointer:
  case SemanticType::Storage:
  case SemanticType::PrefixStorage:
  case SemanticType::Class:
    return true;
  default:
    return false;
  }
}

void addReason(std::vector<MirPlaceEscapeFact> &facts, MirPlaceId place,
               MirPlaceEscapeReason reason) {
  if (place == 0 || place > facts.size()) {
    return;
  }
  facts[place - 1].reasons |= static_cast<std::uint8_t>(reason);
}

void addOrigins(std::vector<MirPlaceEscapeFact> &facts,
                const std::vector<std::set<MirPlaceId>> &origins,
                const MirOperand &operand, MirPlaceEscapeReason reason) {
  if (operand.place != 0) {
    addReason(facts, operand.place, reason);
  }
  if (operand.value == 0 || operand.value > origins.size()) {
    return;
  }
  for (const MirPlaceId place : origins[operand.value - 1]) {
    addReason(facts, place, reason);
  }
}

[[nodiscard]] bool nonLocalDestination(const MirPlace &place) {
  return place.root == MirPlaceRootKind::Symbol ||
         place.root == MirPlaceRootKind::This ||
         place.root == MirPlaceRootKind::Loan || indirectProjection(place);
}

} // namespace

MirAliasRelation MirPlaceAliasEscapeAnalysis::relation(MirPlaceId left,
                                                       MirPlaceId right) const {
  if (left == 0 || right == 0 || left > placeCount || right > placeCount ||
      relations.size() != placeCount * placeCount) {
    return MirAliasRelation::MayAlias;
  }
  return relations[(left - 1) * placeCount + (right - 1)];
}

const MirPlaceEscapeFact *
MirPlaceAliasEscapeAnalysis::escape(MirPlaceId place) const {
  return place == 0 || place > escapes.size() ? nullptr : &escapes[place - 1];
}

MirAliasRelation mirPlaceRelation(const MirProgram &program,
                                  const MirPlace &left, const MirPlace &right) {
  const auto classified = [](const MirPlace &place) {
    return optimizationCoverage(place.root).classified() &&
           std::all_of(
               place.projections.begin(), place.projections.end(),
               [](const MirPlaceProjection &projection) {
                 return optimizationCoverage(projection.kind).classified();
               });
  };
  if (!classified(left) || !classified(right)) {
    return MirAliasRelation::MayAlias;
  }
  if (left.id != 0 && left.id == right.id) {
    return MirAliasRelation::MustAlias;
  }
  if (rawProjection(left) || rawProjection(right)) {
    return MirAliasRelation::MayAlias;
  }
  if (indirectProjection(left) || indirectProjection(right)) {
    return MirAliasRelation::MayAlias;
  }
  // MIR projections are the body-local storage shape. Some construction-time
  // semantic keys intentionally identify only the owner root, so an equal key
  // must not collapse two explicit sibling fields into one place.
  if (sameRoot(left, right)) {
    return projectionRelation(program, left, right);
  }
  if (left.key && right.key) {
    const PlaceRelationResult relation = placeRelation(*left.key, *right.key);
    if (!relation.compatibleDomain) {
      return MirAliasRelation::MayAlias;
    }
    switch (relation.relation) {
    case PlaceRelation::Equal:
      return MirAliasRelation::MustAlias;
    case PlaceRelation::Disjoint:
      return MirAliasRelation::NoAlias;
    case PlaceRelation::LeftStrictPrefix:
    case PlaceRelation::RightStrictPrefix:
    case PlaceRelation::MayAlias:
      return MirAliasRelation::MayAlias;
    }
  }
  return preciseRoot(left) && preciseRoot(right) ? MirAliasRelation::NoAlias
                                                 : MirAliasRelation::MayAlias;
}

MirPlaceAliasEscapeAnalysis
analyzeMirPlaceAliasEscape(const MirProgram &program, MirBodyAddress address) {
  MirPlaceAliasEscapeAnalysis result;
  const MirBody *body = findMirBody(program, address);
  if (body == nullptr || !verifyMirOptimizationVocabulary(*body).valid()) {
    return result;
  }
  result.placeCount = body->places.size();
  result.relations.resize(result.placeCount * result.placeCount,
                          MirAliasRelation::MayAlias);
  result.escapes.resize(result.placeCount);
  for (std::size_t index = 0; index < result.placeCount; ++index) {
    result.escapes[index].place = index + 1;
    for (std::size_t other = 0; other < result.placeCount; ++other) {
      result.relations[index * result.placeCount + other] =
          mirPlaceRelation(program, body->places[index], body->places[other]);
    }
    if (rawProjection(body->places[index])) {
      addReason(result.escapes, index + 1, MirPlaceEscapeReason::RawMemory);
    }
  }

  std::vector<std::set<MirPlaceId>> origins(body->values.size());
  for (std::size_t round = 0; round <= body->values.size(); ++round) {
    bool changed = false;
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.tombstone || !instruction.result ||
            *instruction.result == 0 || *instruction.result > origins.size() ||
            !carriesPlaceIdentity(instruction.info.type)) {
          continue;
        }
        std::set<MirPlaceId> next = origins[*instruction.result - 1];
        if (instruction.destination) {
          next.insert(*instruction.destination);
        }
        if (instruction.receiver) {
          if (instruction.receiver->place != 0) {
            next.insert(instruction.receiver->place);
          }
          if (instruction.receiver->value != 0 &&
              instruction.receiver->value <= origins.size()) {
            next.insert(origins[instruction.receiver->value - 1].begin(),
                        origins[instruction.receiver->value - 1].end());
          }
        }
        for (const MirOperand &operand : instruction.operands) {
          if (operand.place != 0) {
            next.insert(operand.place);
          }
          if (operand.value != 0 && operand.value <= origins.size()) {
            next.insert(origins[operand.value - 1].begin(),
                        origins[operand.value - 1].end());
          }
        }
        if (next != origins[*instruction.result - 1]) {
          origins[*instruction.result - 1] = std::move(next);
          changed = true;
        }
      }
    }
    if (!changed) {
      break;
    }
  }

  for (const MirLoan &loan : body->loans) {
    if (loan.escapes) {
      addReason(result.escapes, loan.source,
                MirPlaceEscapeReason::RetainedBorrow);
      for (const MirPlaceId source : loan.additionalSources) {
        addReason(result.escapes, source, MirPlaceEscapeReason::RetainedBorrow);
      }
    }
  }
  for (const MirBlock &block : body->blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone) {
        continue;
      }
      if (instruction.operation == MirOperation::AddressOf) {
        for (const MirOperand &operand : instruction.operands) {
          addOrigins(result.escapes, origins, operand,
                     MirPlaceEscapeReason::AddressTaken);
        }
      }
      if (instruction.destination &&
          *instruction.destination <= body->places.size() &&
          nonLocalDestination(body->places[*instruction.destination - 1])) {
        for (const MirOperand &operand : instruction.operands) {
          if (operand.kind == MirOperandKind::Address ||
              operand.kind == MirOperandKind::BorrowRead ||
              operand.kind == MirOperandKind::BorrowWrite ||
              operand.kind == MirOperandKind::Loan ||
              (operand.value != 0 && operand.value <= body->values.size() &&
               carriesPlaceIdentity(
                   body->values[operand.value - 1].info.type))) {
            addOrigins(result.escapes, origins, operand,
                       MirPlaceEscapeReason::StoredOutsideLocal);
          }
        }
      }

      const bool invocation =
          instruction.kind == MirInstructionKind::Call ||
          instruction.kind == MirInstructionKind::Construct ||
          instruction.kind == MirInstructionKind::CallBody;
      if (!invocation || instruction.intrinsic != IntrinsicKind::None ||
          instruction.synchronization.kind !=
              SynchronizationOperationKind::None) {
        continue;
      }
      const std::optional<MirEffectSummary> summary =
          exactMirInvocationSummary(program, *body, instruction);
      if (!summary) {
        if (instruction.receiver) {
          addOrigins(result.escapes, origins, *instruction.receiver,
                     MirPlaceEscapeReason::UnknownCall);
        }
        for (const MirOperand &operand : instruction.operands) {
          addOrigins(result.escapes, origins, operand,
                     MirPlaceEscapeReason::UnknownCall);
        }
        continue;
      }
      if (summary->receiverEscapes && instruction.receiver) {
        addOrigins(result.escapes, origins, *instruction.receiver,
                   MirPlaceEscapeReason::ExactCall);
      }
      if (summary->argumentsEscape.size() != instruction.operands.size()) {
        for (const MirOperand &operand : instruction.operands) {
          addOrigins(result.escapes, origins, operand,
                     MirPlaceEscapeReason::UnknownCall);
        }
        continue;
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (summary->argumentsEscape[index]) {
          addOrigins(result.escapes, origins, instruction.operands[index],
                     MirPlaceEscapeReason::ExactCall);
        }
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Return &&
        block.terminator.value) {
      addOrigins(result.escapes, origins, *block.terminator.value,
                 MirPlaceEscapeReason::Returned);
    }
  }

  // Equal MIR places denote one storage identity and therefore share escape
  // facts. May-alias alone is deliberately insufficient to assert escape.
  for (std::size_t left = 0; left < result.placeCount; ++left) {
    for (std::size_t right = 0; right < result.placeCount; ++right) {
      if (result.relation(left + 1, right + 1) == MirAliasRelation::MustAlias) {
        result.escapes[left].reasons |= result.escapes[right].reasons;
      }
    }
  }
  return result;
}

} // namespace lang
