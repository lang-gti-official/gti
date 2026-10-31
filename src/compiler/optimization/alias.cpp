#include "gti/optimization/alias.h"

#include "gti/mir_effects.h"
#include "gti/optimization/coverage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace lang {
namespace {

// A distinct element type prevents linker coalescing with uninstrumented
// integer deque operations in prebuilt LLVM libraries.
struct CaptureWorkItem {
  std::size_t value;
};

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

void addLoanOrigins(const MirBody &body, const MirOperand &operand,
                    std::set<MirPlaceId> &origins) {
  const auto retain = [&](MirLoanId id) {
    const auto sources = mirLoanProtectedSources(body, id);
    origins.insert(sources.begin(), sources.end());
  };
  if (operand.loan != 0)
    retain(operand.loan);
  const auto *place = body.findPlace(operand.place);
  if (place == nullptr)
    return;
  if (place->root == MirPlaceRootKind::Loan)
    retain(place->loan);
  else if (place->root == MirPlaceRootKind::Binding)
    for (const auto &loan : body.loans)
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    place->binding) != loan.carriers.end())
        retain(loan.id);
}

void addOrigins(const MirBody &body, std::vector<MirPlaceEscapeFact> &facts,
                const std::vector<std::set<MirPlaceId>> &origins,
                const MirOperand &operand, MirPlaceEscapeReason reason) {
  if (operand.place != 0) {
    addReason(facts, operand.place, reason);
  }
  std::set<MirPlaceId> protectedSources;
  addLoanOrigins(body, operand, protectedSources);
  for (const MirPlaceId place : protectedSources)
    addReason(facts, place, reason);
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
          addLoanOrigins(*body, *instruction.receiver, next);
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
          addLoanOrigins(*body, operand, next);
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
      for (const MirPlaceId source : mirLoanProtectedSources(*body, loan.id)) {
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
          addOrigins(*body, result.escapes, origins, operand,
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
            addOrigins(*body, result.escapes, origins, operand,
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
          addOrigins(*body, result.escapes, origins, *instruction.receiver,
                     MirPlaceEscapeReason::UnknownCall);
        }
        for (const MirOperand &operand : instruction.operands) {
          addOrigins(*body, result.escapes, origins, operand,
                     MirPlaceEscapeReason::UnknownCall);
        }
        continue;
      }
      if (summary->receiverEscapes && instruction.receiver) {
        addOrigins(*body, result.escapes, origins, *instruction.receiver,
                   MirPlaceEscapeReason::ExactCall);
      }
      if (summary->argumentsEscape.size() != instruction.operands.size()) {
        for (const MirOperand &operand : instruction.operands) {
          addOrigins(*body, result.escapes, origins, operand,
                     MirPlaceEscapeReason::UnknownCall);
        }
        continue;
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (summary->argumentsEscape[index]) {
          addOrigins(*body, result.escapes, origins,
                     instruction.operands[index],
                     MirPlaceEscapeReason::ExactCall);
        }
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Return &&
        block.terminator.value) {
      addOrigins(*body, result.escapes, origins, *block.terminator.value,
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

  // A dereference, raw access or loan referent designates storage some
  // identity was formed for: a loan's protected source, an escaping or
  // address-taken place, or storage that existed before this body. A local
  // binding with none of these in the body is disjoint from all of them.
  std::set<decltype(MirPlace::binding)> referenced;
  const auto reference = [&](MirPlaceId id) {
    if (id != 0 && id <= body->places.size() &&
        body->places[id - 1].root == MirPlaceRootKind::Binding)
      referenced.insert(body->places[id - 1].binding);
  };
  for (const MirPlace &place : body->places)
    if (result.escapes[place.id - 1].reasons != 0)
      reference(place.id);
  for (const MirLoan &loan : body->loans) {
    reference(loan.source);
    for (const MirPlaceId source : mirLoanProtectedSources(*body, loan.id))
      reference(source);
  }
  for (const MirBlock &block : body->blocks)
    for (const MirInstruction &instruction : block.instructions) {
      const auto identity = [&](const MirOperand &operand) {
        if (operand.kind == MirOperandKind::Address ||
            operand.kind == MirOperandKind::BorrowRead ||
            operand.kind == MirOperandKind::BorrowWrite ||
            operand.kind == MirOperandKind::Loan)
          reference(operand.place);
      };
      if (instruction.receiver)
        identity(*instruction.receiver);
      for (const MirOperand &operand : instruction.operands)
        identity(operand);
    }
  const auto designatesOther = [](const MirPlace &place) {
    return indirectProjection(place) || rawProjection(place) ||
           place.root == MirPlaceRootKind::Loan;
  };
  const auto unreferencedLocal = [&](const MirPlace &place) {
    return place.root == MirPlaceRootKind::Binding && place.binding != 0 &&
           !designatesOther(place) && !referenced.contains(place.binding);
  };
  for (std::size_t left = 0; left < result.placeCount; ++left)
    for (std::size_t right = 0; right < result.placeCount; ++right) {
      const MirPlace &first = body->places[left];
      const MirPlace &second = body->places[right];
      if ((designatesOther(first) && unreferencedLocal(second)) ||
          (designatesOther(second) && unreferencedLocal(first)))
        result.relations[left * result.placeCount + right] =
            MirAliasRelation::NoAlias;
    }
  return result;
}

namespace {

// Capture keys name tracked storage: the receiver's own storage, or one
// binding (a local, or the referents of a reference or borrow-carrying
// parameter). Each body numbers its keys densely and works with bit sets.
using CaptureKey = std::size_t;
constexpr CaptureKey kReceiverKey = 0;

[[nodiscard]] std::optional<CaptureKey> captureKey(const MirPlace &place) {
  if (place.root == MirPlaceRootKind::This)
    return kReceiverKey;
  if (place.root == MirPlaceRootKind::Binding && place.binding != 0)
    return static_cast<CaptureKey>(place.binding);
  return std::nullopt;
}

class KeySet {
public:
  KeySet() = default;
  explicit KeySet(std::size_t size) : words((size + 63) / 64, 0) {}

  void insert(std::size_t index) {
    words[index / 64] |= std::uint64_t{1} << (index % 64);
  }
  [[nodiscard]] bool contains(std::size_t index) const {
    return index / 64 < words.size() &&
           ((words[index / 64] >> (index % 64)) & 1U) != 0;
  }
  // Returns whether any bit was added.
  bool merge(const KeySet &other) {
    bool changed = false;
    for (std::size_t word = 0; word < words.size() && word < other.words.size();
         ++word) {
      const std::uint64_t before = words[word];
      words[word] |= other.words[word];
      changed = changed || words[word] != before;
    }
    return changed;
  }
  void fill() {
    for (auto &word : words)
      word = ~std::uint64_t{0};
  }
  template <typename Visit> void forEach(Visit visit) const {
    for (std::size_t word = 0; word < words.size(); ++word)
      for (std::uint64_t bits = words[word]; bits != 0; bits &= bits - 1)
        visit(word * 64 + static_cast<std::size_t>(std::countr_zero(bits)));
  }

private:
  std::vector<std::uint64_t> words;
};

[[nodiscard]] bool addressOperand(const MirOperand &operand) {
  return operand.kind == MirOperandKind::Address ||
         operand.kind == MirOperandKind::BorrowRead ||
         operand.kind == MirOperandKind::BorrowWrite ||
         operand.kind == MirOperandKind::Loan;
}

[[nodiscard]] MirCaptureClass widest(MirCaptureClass left,
                                     MirCaptureClass right) {
  return static_cast<std::uint8_t>(left) >= static_cast<std::uint8_t>(right)
             ? left
             : right;
}

[[nodiscard]] bool bodyLess(MirBodyAddress left, MirBodyAddress right) {
  return std::pair{static_cast<int>(left.kind), left.owner} <
         std::pair{static_cast<int>(right.kind), right.owner};
}

[[nodiscard]] bool invocationKind(MirInstructionKind kind) {
  return kind == MirInstructionKind::Call ||
         kind == MirInstructionKind::Construct ||
         kind == MirInstructionKind::CallBody ||
         kind == MirInstructionKind::InitializeBase ||
         kind == MirInstructionKind::Drop ||
         kind == MirInstructionKind::CleanupBeforeAssign ||
         kind == MirInstructionKind::AllocateObject;
}

[[nodiscard]] bool intrinsicCall(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Call &&
         instruction.intrinsic != IntrinsicKind::None &&
         !(instruction.intrinsic == IntrinsicKind::ObjectConstruct &&
           instruction.constructorTarget) &&
         !instruction.elementCleanup;
}

// Destruction and cleanup run their destructors with the destination (or a
// cleaned element) as the receiver.
[[nodiscard]] bool cleanupInvocation(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Drop ||
         instruction.kind == MirInstructionKind::CleanupBeforeAssign ||
         instruction.elementCleanup.has_value();
}

struct InputClasses {
  bool unknown = false;
  MirCaptureClass receiver = MirCaptureClass::None;
  MirCaptureClass destination = MirCaptureClass::None;
  std::vector<MirCaptureClass> operands;
  bool capturesResult = false;
};

// Which edge of an instruction a capture applies to. An invocation's
// failure edge also sees what its callee captured while failing.
enum class CaptureEdge : std::uint8_t { Success, Failure };

class CaptureScan {
public:
  CaptureScan(const MirProgram &program, const MirBody &body,
              const MirProgramCaptureSummaries &summaries)
      : program(program), body(body), summaries(summaries),
        producers(body.values.size(), nullptr),
        parameterValues(body.values.size(), false) {
    for (const MirPlace &place : body.places)
      if (const auto key = captureKey(place);
          key && keyIndex.emplace(*key, keys.size()).second)
        keys.push_back(*key);
    loanKeys.reserve(body.loans.size());
    for (const MirLoan &loan : body.loans) {
      KeySet sources = empty();
      for (const MirPlaceId source : mirLoanProtectedSources(body, loan.id))
        if (const MirPlace *place = body.findPlace(source))
          if (const auto key = captureKey(*place))
            sources.insert(keyIndex.at(*key));
      loanKeys.push_back(sources);
      for (const HirBindingId carrier : loan.carriers)
        carrierKeys.try_emplace(carrier, empty()).first->second.merge(sources);
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirBlockParameter &parameter : block.parameters)
        if (parameter.value != 0 && parameter.value <= parameterValues.size())
          parameterValues[parameter.value - 1] = true;
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.tombstone)
          continue;
        if (instruction.result && *instruction.result != 0 &&
            *instruction.result <= producers.size())
          producers[*instruction.result - 1] = &instruction;
        if (invocationKind(instruction.kind)) {
          targets.emplace(&instruction,
                          mirInvocationTargets(program, body, instruction));
          kinds.emplace(&instruction, classify(instruction));
        }
      }
    }
    everyKey = empty();
    everyKey.fill();
    origins.assign(body.values.size(), empty());
    computeOrigins();
  }

  [[nodiscard]] KeySet empty() const { return KeySet(keys.size()); }
  [[nodiscard]] std::optional<std::size_t> indexOf(CaptureKey key) const {
    const auto found = keyIndex.find(key);
    return found == keyIndex.end() ? std::nullopt
                                   : std::optional<std::size_t>{found->second};
  }
  [[nodiscard]] CaptureKey keyAt(std::size_t index) const {
    return keys[index];
  }
  [[nodiscard]] std::size_t keyCount() const { return keys.size(); }

  [[nodiscard]] const KeySet &keysOfLoan(MirLoanId loan) const {
    return loan != 0 && loan <= loanKeys.size() ? loanKeys[loan - 1] : noKeys();
  }

  // The tracked identities an operand may carry. An address-forming operand
  // or a receiver carries its place's storage; a reference or borrow-carrying
  // value carries the referents its place stands for. A scalar or raw pointer
  // read from storage carries none: storing an address into any place is a
  // capture, so a loaded value never names uncaptured storage.
  [[nodiscard]] KeySet carried(const MirOperand &operand, bool receiver) const {
    KeySet keys = empty();
    if (const MirPlace *place = body.findPlace(operand.place)) {
      if (const auto key = captureKey(*place);
          key && (receiver || addressOperand(operand) ||
                  place->type.kind == SemanticType::Reference ||
                  place->traits.containsBorrowedState))
        keys.insert(keyIndex.at(*key));
      if (place->root == MirPlaceRootKind::Loan)
        keys.merge(keysOfLoan(place->loan));
      else if (place->root == MirPlaceRootKind::Binding)
        if (const auto found = carrierKeys.find(place->binding);
            found != carrierKeys.end())
          keys.merge(found->second);
    }
    keys.merge(keysOfLoan(operand.loan));
    if (operand.value != 0 && operand.value <= origins.size())
      keys.merge(origins[operand.value - 1]);
    return keys;
  }

  // Intrinsics with no exact body. One that writes no memory, runs no code
  // and does not synchronize only inspects or reborrows its operands: their
  // identities reach at most its result. Any other intrinsic, and every
  // synchronization operation, captures what it receives.
  [[nodiscard]] bool
  inspectingIntrinsic(const MirInstruction &instruction) const {
    const auto found = kinds.find(&instruction);
    return found != kinds.end() && found->second == InvocationKind::Inspecting;
  }

  [[nodiscard]] bool opaqueOperation(const MirInstruction &instruction) const {
    const auto found = kinds.find(&instruction);
    return found != kinds.end() && found->second == InvocationKind::Opaque;
  }

  [[nodiscard]] const InputClasses &
  inputClasses(const MirInstruction &instruction, CaptureEdge edge) const {
    auto &slot = classCache[&instruction][static_cast<std::size_t>(edge)];
    if (!slot)
      slot = computeInputClasses(instruction, edge);
    return *slot;
  }

  [[nodiscard]] InputClasses
  computeInputClasses(const MirInstruction &instruction,
                      CaptureEdge edge) const {
    InputClasses result;
    result.operands.assign(instruction.operands.size(), MirCaptureClass::None);
    const auto cached = targets.find(&instruction);
    if (cached == targets.end() || cached->second.boundary ||
        cached->second.addresses.empty()) {
      result.unknown = true;
      return result;
    }
    // Final-address construction passes the vacant pointee first. Forming a
    // raw address of tracked storage is itself a capture, so it is untracked.
    const std::size_t offset =
        instruction.kind == MirInstructionKind::Call &&
                instruction.intrinsic == IntrinsicKind::ObjectConstruct
            ? 1
            : 0;
    for (const MirBodyAddress target : cached->second.addresses) {
      const MirBodyCaptureSummary *summary = summaries.find(target);
      if (summary == nullptr) {
        result.unknown = true;
        return result;
      }
      const bool failure = edge == CaptureEdge::Failure;
      const MirCaptureClass receiver =
          failure ? summary->failureReceiver : summary->receiver;
      const auto &parameters =
          failure ? summary->failureParameters : summary->parameters;
      result.capturesResult = result.capturesResult || summary->capturesResult;
      if (cleanupInvocation(instruction)) {
        result.destination = widest(result.destination, receiver);
        result.receiver = widest(result.receiver, receiver);
        for (auto &operand : result.operands)
          operand = widest(operand, receiver);
        continue;
      }
      if (instruction.kind == MirInstructionKind::InitializeBase)
        result.destination = widest(result.destination, receiver);
      if (instruction.receiver)
        result.receiver = widest(result.receiver, receiver);
      for (std::size_t index = offset; index < instruction.operands.size();
           ++index) {
        const std::size_t parameter = index - offset;
        result.operands[index] =
            widest(result.operands[index], parameter < parameters.size()
                                               ? parameters[parameter]
                                               : MirCaptureClass::Captured);
      }
    }
    return result;
  }

  // Whether an instruction receives a value whose provenance this scan
  // cannot follow: an undefined value, or a staging or transfer chain that
  // is empty or cyclic. Verified MIR has none; malformed MIR fails closed.
  [[nodiscard]] bool unresolvedInputs(const MirInstruction &instruction) const {
    const auto unresolved = [&](MirValueId value) {
      std::set<MirValueId> seen;
      while (value != 0) {
        if (value > producers.size() || !seen.insert(value).second)
          return true;
        const MirInstruction *producer = producers[value - 1];
        if (producer == nullptr)
          return !parameterValues[value - 1];
        if (producer->kind != MirInstructionKind::CallInput &&
            producer->kind != MirInstructionKind::Move)
          return false;
        if (producer->operands.size() != 1)
          return true;
        value = producer->operands.front().value;
      }
      return false;
    };
    if (instruction.receiver && unresolved(instruction.receiver->value))
      return true;
    return std::any_of(
        instruction.operands.begin(), instruction.operands.end(),
        [&](const MirOperand &operand) { return unresolved(operand.value); });
  }

  // Every tracked identity an instruction receives, by any operand kind.
  [[nodiscard]] KeySet involved(const MirInstruction &instruction) const {
    if (unresolvedInputs(instruction))
      return everyKey;
    KeySet keys = inputs(instruction);
    const auto own = [&](MirPlaceId id) {
      if (const MirPlace *place = body.findPlace(id))
        if (const auto key = captureKey(*place))
          keys.insert(keyIndex.at(*key));
    };
    if (instruction.receiver)
      own(instruction.receiver->place);
    for (const MirOperand &operand : instruction.operands)
      own(operand.place);
    if (instruction.destination)
      own(*instruction.destination);
    if (instruction.loan)
      keys.merge(keysOfLoan(*instruction.loan));
    return keys;
  }

  // The tracked identities this instruction may make reachable from code or
  // storage outside the body's view, on the given edge.
  [[nodiscard]] const KeySet &captures(const MirInstruction &instruction,
                                       CaptureEdge edge) const {
    auto &slot = captureCache[&instruction][static_cast<std::size_t>(edge)];
    if (!slot)
      slot = computeCaptures(instruction, edge);
    return *slot;
  }

  [[nodiscard]] KeySet computeCaptures(const MirInstruction &instruction,
                                       CaptureEdge edge) const {
    KeySet result = empty();
    if (instruction.tombstone)
      return result;
    if (unresolvedInputs(instruction))
      return everyKey;
    const KeySet all = inputs(instruction);
    KeySet destinationKeys = empty();
    const MirPlace *destination = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
    if (destination)
      if (const auto key = captureKey(*destination))
        destinationKeys.insert(keyIndex.at(*key));

    if (!instruction.closureCaptureTypes.empty())
      result.merge(all);
    if (instruction.operation == MirOperation::AddressOf)
      for (const MirOperand &operand : instruction.operands) {
        if (const MirPlace *place = body.findPlace(operand.place))
          if (const auto key = captureKey(*place))
            result.insert(keyIndex.at(*key));
        result.merge(carried(operand, false));
      }

    KeySet stored = empty();
    bool adoptsCapturedResult = false;
    if (invocationKind(instruction.kind)) {
      const bool objectResult = carriesPlaceIdentity(instruction.info.type);
      if (inspectingIntrinsic(instruction)) {
        stored = all;
      } else if (opaqueOperation(instruction)) {
        result.merge(all);
        result.merge(destinationKeys);
      } else if (kinds.at(&instruction) != InvocationKind::StructuralDrop) {
        const InputClasses &classes = inputClasses(instruction, edge);
        if (classes.unknown) {
          result.merge(all);
          result.merge(destinationKeys);
          stored = all;
          adoptsCapturedResult = objectResult;
        } else {
          if (instruction.receiver) {
            const KeySet keys = carried(*instruction.receiver, true);
            if (classes.receiver == MirCaptureClass::Captured)
              result.merge(keys);
            if (classes.receiver != MirCaptureClass::None)
              stored.merge(keys);
          }
          for (std::size_t index = 0; index < instruction.operands.size();
               ++index) {
            if (classes.operands[index] == MirCaptureClass::None)
              continue;
            const KeySet keys = carried(instruction.operands[index], false);
            if (classes.operands[index] == MirCaptureClass::Captured)
              result.merge(keys);
            stored.merge(keys);
          }
          if (classes.destination == MirCaptureClass::Captured)
            result.merge(destinationKeys);
          adoptsCapturedResult = objectResult && classes.capturesResult;
        }
      }
      if (cleanupInvocation(instruction) ||
          instruction.kind == MirInstructionKind::InitializeBase)
        stored = empty();
    } else if (instruction.kind == MirInstructionKind::Initialize ||
               instruction.kind == MirInstructionKind::Assign ||
               instruction.kind == MirInstructionKind::Modify ||
               instruction.kind == MirInstructionKind::Move ||
               instruction.kind == MirInstructionKind::CallInput) {
      if (destination)
        for (const MirOperand &operand : instruction.operands) {
          stored.merge(carried(operand, false));
          adoptsCapturedResult =
              adoptsCapturedResult || producedCapturedResult(operand.value);
        }
    } else if (instruction.kind != MirInstructionKind::Load &&
               instruction.kind != MirInstructionKind::Compute &&
               instruction.kind != MirInstructionKind::Borrow &&
               instruction.kind != MirInstructionKind::ActivateBorrow &&
               instruction.kind != MirInstructionKind::EndBorrow &&
               instruction.kind != MirInstructionKind::Lifecycle) {
      result.merge(all);
    }

    // Storing an identity into any place makes it reachable through that
    // place, except a binding that MIR records as the carrier of a loan of
    // the same storage: every use of a carrier still names the loan.
    if (destination) {
      const KeySet *carriedLoans = nullptr;
      if (destination->root == MirPlaceRootKind::Binding &&
          destination->binding != 0 && destination->projections.empty())
        if (const auto found = carrierKeys.find(destination->binding);
            found != carrierKeys.end())
          carriedLoans = &found->second;
      stored.forEach([&](std::size_t index) {
        if (carriedLoans == nullptr || !carriedLoans->contains(index))
          result.insert(index);
      });
      if (adoptsCapturedResult)
        result.merge(destinationKeys);
    }
    return result;
  }

  [[nodiscard]] KeySet returned(const MirTerminator &terminator) const {
    KeySet keys = empty();
    if (terminator.kind != MirTerminatorKind::Return)
      return keys;
    if (terminator.value)
      keys = carried(*terminator.value, false);
    if (terminator.returnLoan)
      keys.merge(keysOfLoan(*terminator.returnLoan));
    return keys;
  }

  [[nodiscard]] KeySet escapingLoanSources() const {
    KeySet keys = empty();
    for (const MirLoan &loan : body.loans)
      if (loan.escapes)
        keys.merge(keysOfLoan(loan.id));
    return keys;
  }

  // Whether a value was built by an invocation whose constructed or returned
  // object may already have its own address captured.
  [[nodiscard]] bool producedCapturedResult(MirValueId value) const {
    std::set<MirValueId> seen;
    while (value != 0 && value <= producers.size() &&
           seen.insert(value).second) {
      const MirInstruction *producer = producers[value - 1];
      if (producer == nullptr)
        return false;
      if (invocationKind(producer->kind) && !intrinsicCall(*producer) &&
          producer->synchronization.kind ==
              SynchronizationOperationKind::None) {
        if (!carriesPlaceIdentity(producer->info.type))
          return false;
        const InputClasses &classes =
            inputClasses(*producer, CaptureEdge::Success);
        return classes.unknown || classes.capturesResult;
      }
      if ((producer->kind != MirInstructionKind::CallInput &&
           producer->kind != MirInstructionKind::Move) ||
          producer->operands.size() != 1)
        return false;
      value = producer->operands.front().value;
    }
    return false;
  }

private:
  // How an invocation instruction is treated, fixed for one scan.
  enum class InvocationKind : std::uint8_t {
    Exact,
    StructuralDrop,
    Inspecting,
    Opaque,
  };

  [[nodiscard]] InvocationKind
  classify(const MirInstruction &instruction) const {
    if (instruction.synchronization.kind !=
            SynchronizationOperationKind::None ||
        instruction.kind == MirInstructionKind::AllocateObject)
      return InvocationKind::Opaque;
    if (instruction.kind == MirInstructionKind::Drop &&
        isMirStructuralValueDrop(program, instruction))
      return InvocationKind::StructuralDrop;
    if (!intrinsicCall(instruction))
      return InvocationKind::Exact;
    const MirEffectTraits effect = effects(program, body, instruction);
    const bool inspecting =
        !effect.writesPlace && !effect.writesUnknownMemory &&
        !effect.invokesUserCode && !effect.invokesRuntime &&
        !effect.maySynchronize && !effect.allocates && !effect.movesValue &&
        !effect.dropsValue && !effect.initializesValue;
    return inspecting ? InvocationKind::Inspecting : InvocationKind::Opaque;
  }

  [[nodiscard]] const KeySet &noKeys() const { return noKeySet; }

  [[nodiscard]] KeySet inputs(const MirInstruction &instruction) const {
    KeySet keys = empty();
    if (instruction.receiver)
      keys.merge(carried(*instruction.receiver, true));
    for (const MirOperand &operand : instruction.operands)
      keys.merge(carried(operand, false));
    return keys;
  }

  void computeOrigins() {
    const auto flow = [&](MirBlockId target,
                          const std::vector<MirOperand> &arguments,
                          bool &changed) {
      const MirBlock *successor = body.findBlock(target);
      if (successor == nullptr)
        return;
      for (std::size_t index = 0;
           index < arguments.size() && index < successor->parameters.size();
           ++index) {
        const MirValueId value = successor->parameters[index].value;
        if (value != 0 && value <= origins.size())
          changed =
              origins[value - 1].merge(carried(arguments[index], false)) ||
              changed;
      }
    };
    for (std::size_t round = 0; round <= origins.size(); ++round) {
      bool changed = false;
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          if (instruction.tombstone || !instruction.result ||
              *instruction.result == 0 ||
              *instruction.result > origins.size() ||
              !carriesPlaceIdentity(instruction.info.type))
            continue;
          KeySet next = empty();
          if (instruction.loan)
            next.merge(keysOfLoan(*instruction.loan));
          if (invocationKind(instruction.kind) &&
              !opaqueOperation(instruction) &&
              !inspectingIntrinsic(instruction)) {
            const InputClasses &classes =
                inputClasses(instruction, CaptureEdge::Success);
            if (instruction.receiver &&
                (classes.unknown || classes.receiver != MirCaptureClass::None))
              next.merge(carried(*instruction.receiver, true));
            for (std::size_t index = 0; index < instruction.operands.size();
                 ++index)
              if (classes.unknown ||
                  classes.operands[index] != MirCaptureClass::None)
                next.merge(carried(instruction.operands[index], false));
          } else {
            next.merge(inputs(instruction));
          }
          changed = origins[*instruction.result - 1].merge(next) || changed;
        }
        const MirTerminator &terminator = block.terminator;
        flow(terminator.target, terminator.targetArguments, changed);
        flow(terminator.elseTarget, terminator.elseArguments, changed);
        for (const MirSwitchTarget &target : terminator.switchTargets)
          flow(target.target, target.arguments, changed);
      }
      if (!changed)
        break;
    }
  }

  const MirProgram &program;
  const MirBody &body;
  const MirProgramCaptureSummaries &summaries;
  std::unordered_map<CaptureKey, std::size_t> keyIndex;
  std::vector<CaptureKey> keys;
  std::vector<KeySet> loanKeys;
  std::unordered_map<HirBindingId, KeySet> carrierKeys;
  std::vector<KeySet> origins;
  std::vector<const MirInstruction *> producers;
  std::vector<bool> parameterValues;
  KeySet everyKey;
  KeySet noKeySet;
  std::unordered_map<const MirInstruction *, MirInvocationTargets> targets;
  std::unordered_map<const MirInstruction *, InvocationKind> kinds;
  // Origins are complete once constructed, so captures and classes are
  // fixed for the scan's lifetime.
  mutable std::unordered_map<const MirInstruction *,
                             std::array<std::optional<InputClasses>, 2>>
      classCache;
  mutable std::unordered_map<const MirInstruction *,
                             std::array<std::optional<KeySet>, 2>>
      captureCache;
};

// Forward may-capture dataflow over one body. A block ending in Invoke
// passes its call's success-path captures to the success target and its
// any-path captures to the failure target; every other edge sees both.
struct CaptureFlow {
  std::vector<std::pair<MirInstructionId, KeySet>> before;
  KeySet atReturn;
  KeySet anywhere;
};

[[nodiscard]] CaptureFlow flowCaptures(const CaptureScan &scan,
                                       const MirBody &body,
                                       const KeySet &initial) {
  CaptureFlow result{
      .before = {}, .atReturn = scan.empty(), .anywhere = initial};
  if (body.findBlock(body.entry) == nullptr)
    return result;
  std::vector<std::optional<KeySet>> entering(body.blocks.size());
  entering[body.entry - 1] = initial;
  std::deque<CaptureWorkItem> work{{body.entry}};
  std::map<MirInstructionId, KeySet> before;
  const auto propagate = [&](MirBlockId successor, const KeySet &state) {
    if (successor == 0 || successor > entering.size())
      return;
    auto &next = entering[successor - 1];
    bool changed = false;
    if (!next) {
      next = state;
      changed = true;
    } else {
      changed = next->merge(state);
    }
    if (changed)
      work.push_back({successor});
  };
  while (!work.empty()) {
    const MirBlockId id = work.front().value;
    work.pop_front();
    const MirBlock *block = body.findBlock(id);
    if (block == nullptr || !entering[id - 1])
      continue;
    KeySet state = *entering[id - 1];
    KeySet failureState = state;
    const MirTerminator &terminator = block->terminator;
    const bool invoke = terminator.kind == MirTerminatorKind::Invoke;
    for (const MirInstruction &instruction : block->instructions) {
      before.insert_or_assign(instruction.id, state);
      if (invoke && instruction.id == terminator.invokeInstruction) {
        failureState = state;
        failureState.merge(scan.captures(instruction, CaptureEdge::Failure));
        state.merge(scan.captures(instruction, CaptureEdge::Success));
      } else {
        // Without an Invoke edge the callee cannot fail out of this block.
        state.merge(scan.captures(instruction, CaptureEdge::Failure));
        failureState = state;
      }
    }
    result.anywhere.merge(state);
    result.anywhere.merge(failureState);
    if (terminator.kind == MirTerminatorKind::Return)
      result.atReturn.merge(state);
    if (invoke) {
      propagate(terminator.target, state);
      propagate(terminator.elseTarget, failureState);
      continue;
    }
    if (terminator.target != 0)
      propagate(terminator.target, state);
    if (terminator.elseTarget != 0)
      propagate(terminator.elseTarget, state);
    for (const MirSwitchTarget &target : terminator.switchTargets)
      propagate(target.target, state);
  }
  result.before.assign(before.begin(), before.end());
  return result;
}

struct BodyInputs {
  bool receiver = false;
  // Parameter binding, or zero when the parameter carries no caller
  // identity (a scalar, raw address or self-contained value).
  std::vector<HirBindingId> parameters;
};

[[nodiscard]] BodyInputs bodyInputs(const MirProgram &program,
                                    MirBodyAddress address,
                                    const MirBody &body) {
  BodyInputs inputs;
  const std::vector<HirBindingId> *bindings = nullptr;
  switch (address.kind) {
  case MirBodyKind::Function:
    if (const auto *function = program.findFunctionInstance(address.owner)) {
      inputs.receiver = function->owner.has_value() && !function->staticMember;
      bindings = &function->parameterBindings;
    }
    break;
  case MirBodyKind::Constructor:
    if (const auto *constructor =
            program.findConstructorInstance(address.owner)) {
      inputs.receiver = true;
      bindings = &constructor->parameterBindings;
    }
    break;
  case MirBodyKind::Destructor:
  case MirBodyKind::FieldInitializers:
    inputs.receiver = true;
    break;
  default:
    break;
  }
  if (bindings == nullptr)
    return inputs;
  for (const HirBindingId binding : *bindings) {
    const auto place = std::find_if(
        body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
          return candidate.root == MirPlaceRootKind::Binding &&
                 candidate.binding == binding && candidate.projections.empty();
        });
    const bool tracked = place == body.places.end() ||
                         place->type.kind == SemanticType::Reference ||
                         place->traits.containsBorrowedState;
    inputs.parameters.push_back(tracked ? binding : 0);
  }
  return inputs;
}

[[nodiscard]] bool summarizedBody(const MirProgram &program,
                                  MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Function: {
    const auto *function = program.findFunctionInstance(address.owner);
    return function != nullptr &&
           function->definitionKind == MirDefinitionKind::Source;
  }
  case MirBodyKind::Constructor: {
    const auto *constructor = program.findConstructorInstance(address.owner);
    return constructor != nullptr &&
           constructor->definitionKind == MirDefinitionKind::Source;
  }
  case MirBodyKind::Destructor: {
    const auto *destructor = program.findDestructorInstance(address.owner);
    return destructor != nullptr &&
           destructor->definitionKind == MirDefinitionKind::Source;
  }
  case MirBodyKind::FieldInitializers:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] MirBodyCaptureSummary
summarizeCaptures(const MirProgram &program, MirBodyAddress address,
                  const MirBody &body,
                  const MirProgramCaptureSummaries &summaries) {
  const BodyInputs inputs = bodyInputs(program, address, body);
  const CaptureScan scan(program, body, summaries);
  const CaptureFlow flow = flowCaptures(scan, body, scan.empty());
  KeySet returned = scan.escapingLoanSources();
  bool capturesResult = false;
  for (const MirBlock &block : body.blocks) {
    returned.merge(scan.returned(block.terminator));
    if (block.terminator.kind == MirTerminatorKind::Return &&
        block.terminator.value)
      capturesResult = capturesResult || scan.producedCapturedResult(
                                             block.terminator.value->value);
  }
  const auto classify = [&](CaptureKey key, const KeySet &captured) {
    const auto index = scan.indexOf(key);
    if (!index)
      return MirCaptureClass::None;
    return captured.contains(*index)   ? MirCaptureClass::Captured
           : returned.contains(*index) ? MirCaptureClass::ReturnOnly
                                       : MirCaptureClass::None;
  };
  MirBodyCaptureSummary summary;
  if (inputs.receiver) {
    summary.receiver = classify(kReceiverKey, flow.atReturn);
    summary.failureReceiver = classify(kReceiverKey, flow.anywhere);
  }
  for (const HirBindingId binding : inputs.parameters) {
    summary.parameters.push_back(binding == 0
                                     ? MirCaptureClass::None
                                     : classify(binding, flow.atReturn));
    summary.failureParameters.push_back(binding == 0
                                            ? MirCaptureClass::None
                                            : classify(binding, flow.anywhere));
  }
  if (address.kind == MirBodyKind::Constructor) {
    // Field initializers run first on the same receiver.
    if (const auto *constructor =
            program.findConstructorInstance(address.owner))
      if (const MirBodyCaptureSummary *initializers = summaries.find(
              {.kind = MirBodyKind::FieldInitializers,
               .owner = static_cast<std::size_t>(constructor->owner)})) {
        summary.receiver = widest(summary.receiver, initializers->receiver);
        summary.failureReceiver =
            widest(summary.failureReceiver, initializers->failureReceiver);
      }
    capturesResult = summary.receiver == MirCaptureClass::Captured;
  } else if (address.kind == MirBodyKind::FieldInitializers) {
    capturesResult = summary.receiver == MirCaptureClass::Captured;
  } else if (address.kind != MirBodyKind::Function) {
    capturesResult = false;
  }
  summary.capturesResult = capturesResult;
  return summary;
}

template <typename Entries>
[[nodiscard]] const std::vector<HirBindingId> *
findBindings(const Entries &entries, MirInstructionId instruction) {
  const auto found = std::lower_bound(
      entries.begin(), entries.end(), instruction,
      [](const auto &entry, MirInstructionId id) { return entry.first < id; });
  return found == entries.end() || found->first != instruction ? nullptr
                                                               : &found->second;
}

} // namespace

const MirBodyCaptureSummary *
MirProgramCaptureSummaries::find(MirBodyAddress body) const {
  const auto found = std::lower_bound(bodies.begin(), bodies.end(), body,
                                      [](const auto &entry, auto address) {
                                        return bodyLess(entry.first, address);
                                      });
  return found == bodies.end() || !(found->first == body) ? nullptr
                                                          : &found->second;
}

MirProgramCaptureSummaries
deriveMirProgramCaptureSummaries(const MirProgram &program) {
  MirProgramCaptureSummaries result;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr || !summarizedBody(program, address))
      continue;
    MirBodyCaptureSummary initial;
    const std::size_t parameters =
        bodyInputs(program, address, *body).parameters.size();
    initial.parameters.assign(parameters, MirCaptureClass::None);
    initial.failureParameters.assign(parameters, MirCaptureClass::None);
    result.bodies.emplace_back(address, std::move(initial));
  }
  std::sort(result.bodies.begin(), result.bodies.end(),
            [](const auto &left, const auto &right) {
              return bodyLess(left.first, right.first);
            });
  // Classes only grow, so a caller is revisited only when a callee's
  // summary changes. The least fixpoint is reached within a bounded number
  // of visits; exhausting the bound falls back to capturing every input
  // rather than trusting a partial summary.
  const auto indexOf =
      [&](MirBodyAddress address) -> std::optional<std::size_t> {
    const auto found = std::lower_bound(
        result.bodies.begin(), result.bodies.end(), address,
        [](const auto &entry, auto key) { return bodyLess(entry.first, key); });
    if (found == result.bodies.end() || !(found->first == address))
      return std::nullopt;
    return static_cast<std::size_t>(found - result.bodies.begin());
  };
  std::vector<std::vector<std::size_t>> callers(result.bodies.size());
  for (std::size_t index = 0; index < result.bodies.size(); ++index) {
    const MirBodyAddress address = result.bodies[index].first;
    const MirBody &body = *findMirBody(program, address);
    for (const MirBlock &block : body.blocks)
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.tombstone || !invocationKind(instruction.kind))
          continue;
        for (const MirBodyAddress target :
             mirInvocationTargets(program, body, instruction).addresses)
          if (const auto callee = indexOf(target))
            callers[*callee].push_back(index);
      }
    if (address.kind == MirBodyKind::Constructor)
      if (const auto *constructor =
              program.findConstructorInstance(address.owner))
        if (const auto initializers = indexOf(
                {.kind = MirBodyKind::FieldInitializers,
                 .owner = static_cast<std::size_t>(constructor->owner)}))
          callers[*initializers].push_back(index);
  }
  for (auto &list : callers) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
  }
  std::deque<CaptureWorkItem> work;
  std::vector<bool> queued(result.bodies.size(), true);
  for (std::size_t index = 0; index < result.bodies.size(); ++index)
    work.push_back({index});
  std::size_t visits = 0;
  const std::size_t budget = 16 * result.bodies.size() + 16;
  bool converged = true;
  while (!work.empty()) {
    if (++visits > budget) {
      converged = false;
      break;
    }
    const std::size_t index = work.front().value;
    work.pop_front();
    queued[index] = false;
    auto &[address, summary] = result.bodies[index];
    MirBodyCaptureSummary next = summarizeCaptures(
        program, address, *findMirBody(program, address), result);
    if (next == summary)
      continue;
    summary = std::move(next);
    for (const std::size_t caller : callers[index])
      if (!queued[caller]) {
        queued[caller] = true;
        work.push_back({caller});
      }
  }
  if (!converged)
    for (auto &[address, summary] : result.bodies) {
      (void)address;
      summary.receiver = summary.failureReceiver = MirCaptureClass::Captured;
      std::fill(summary.parameters.begin(), summary.parameters.end(),
                MirCaptureClass::Captured);
      std::fill(summary.failureParameters.begin(),
                summary.failureParameters.end(), MirCaptureClass::Captured);
      summary.capturesResult = true;
    }
  return result;
}

bool MirLocalCaptureAnalysis::mayBeCapturedBefore(
    HirBindingId binding, MirInstructionId instruction) const {
  if (!valid)
    return true;
  const auto *bindings = findBindings(capturedBefore, instruction);
  return bindings == nullptr ||
         std::binary_search(bindings->begin(), bindings->end(), binding);
}

bool MirLocalCaptureAnalysis::involves(HirBindingId binding,
                                       MirInstructionId instruction) const {
  if (!valid)
    return true;
  const auto *bindings = findBindings(involved, instruction);
  return bindings == nullptr ||
         std::binary_search(bindings->begin(), bindings->end(), binding);
}

MirLocalCaptureAnalysis
analyzeMirLocalCaptures(const MirProgram &program, MirBodyAddress address,
                        const MirProgramCaptureSummaries &summaries) {
  MirLocalCaptureAnalysis result;
  const MirBody *body = findMirBody(program, address);
  if (body == nullptr || body->findBlock(body->entry) == nullptr)
    return result;
  const CaptureScan scan(program, *body, summaries);
  // An escaping loan makes its source reachable after the body returns; treat
  // it as captured from entry.
  const CaptureFlow flow =
      flowCaptures(scan, *body, scan.escapingLoanSources());
  const auto bindingsOf = [&](const KeySet &keys) {
    std::vector<HirBindingId> bindings;
    keys.forEach([&](std::size_t index) {
      if (index < scan.keyCount() && scan.keyAt(index) != kReceiverKey)
        bindings.push_back(static_cast<HirBindingId>(scan.keyAt(index)));
    });
    std::sort(bindings.begin(), bindings.end());
    return bindings;
  };
  for (const auto &[instruction, keys] : flow.before)
    result.capturedBefore.emplace_back(instruction, bindingsOf(keys));
  for (const MirBlock &block : body->blocks)
    for (const MirInstruction &instruction : block.instructions)
      result.involved.emplace_back(instruction.id,
                                   bindingsOf(scan.involved(instruction)));
  std::sort(result.involved.begin(), result.involved.end());
  result.valid = true;
  return result;
}

} // namespace lang
