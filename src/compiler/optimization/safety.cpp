#include "gti/optimization/safety.h"
#include "gti/optimization/coverage.h"

#include "gti/checked_integer.h"
#include "gti/mir_dominance.h"
#include "gti/mir_effects.h"
#include "gti/optimization/alias.h"
#include "gti/optimization/dataflow.h"
#include "gti/optimization/loops.h"

#include <algorithm>
#include <compare>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace lang {
namespace {

struct Evidence {
  std::vector<MirBlockId> blocks;
  std::vector<MirInstructionId> instructions;
  std::vector<MirValueId> values;
  std::vector<MirPlaceId> places;

  friend bool operator==(const Evidence &, const Evidence &) = default;
};

struct IntegerFact {
  CheckedIntegerDomain domain;
  CheckedIntegerValue lower;
  CheckedIntegerValue upper;
  // A result-domain bound that remains valid when loop joins widen the
  // observed interval. Only operations whose semantics impose the bound
  // independently of a changing operand may provide this envelope.
  std::optional<std::pair<CheckedIntegerValue, CheckedIntegerValue>>
      wideningEnvelope;
  Evidence evidence;

  friend bool operator==(const IntegerFact &, const IntegerFact &) = default;
};

using FieldPath = std::vector<SymbolId>;
using IntegerFieldFacts = std::map<FieldPath, IntegerFact>;

struct ObjectFact {
  std::map<FieldPath, IntegerFact> integers;
  std::optional<IntegerFact> elements;
  IntegerFieldFacts elementIntegers;

  friend bool operator==(const ObjectFact &, const ObjectFact &) = default;
};

// These are transfer-rule barriers, not replacement MIR effects. A vacant
// scalar construction remains opaque, but has a narrower supported transfer
// than an unhandled operation which might write an already live object.
enum class LiveMemoryBarrier { None, VacantScalarConstruction, Unknown };

[[nodiscard]] bool mergeObject(ObjectFact &target, const ObjectFact &incoming,
                               bool widen);

struct AbstractState {
  bool reachable = false;
  // Unknown effects on a failure-only path must not poison a successful
  // result summary, but an opaque effect on any successful path must survive
  // joins even when all scalar result facts later become constant.
  bool hasOpaqueEffects = false;
  LiveMemoryBarrier liveMemoryBarrier = LiveMemoryBarrier::None;
  std::map<MirValueId, IntegerFact> integers;
  std::map<MirPlaceId, IntegerFact> integerPlaces;
  std::map<MirValueId, IntegerFact> arrayValues;
  std::map<MirPlaceId, IntegerFact> arrayElements;
  std::map<MirValueId, MirPlaceId> loadedOrigins;
  std::map<MirPlaceId, IntegerFact> aggregateElements;
  std::map<MirPlaceId, IntegerFieldFacts> aggregateElementIntegers;
  // Key zero is an analysis-only implicit This root. It carries ordinary
  // receiver fields when MIR needs projected fields but no whole-object place;
  // it is never published as a proof's place identity.
  std::map<MirPlaceId, ObjectFact> objects;
  std::map<MirValueId, MirPlaceId> aggregateOrigins;
  std::map<MirValueId, ObjectFact> objectValues;

  friend bool operator==(const AbstractState &,
                         const AbstractState &) = default;
};

template <typename T> struct ObservedFact {
  bool observed = false;
  std::optional<T> fact;

  friend bool operator==(const ObservedFact &, const ObservedFact &) = default;
};

struct ClosedCallContext {
  bool reached = false;
  ObservedFact<ObjectFact> receiver;
  std::vector<ObservedFact<IntegerFact>> arguments;
  std::vector<ObservedFact<ObjectFact>> argumentObjects;

  friend bool operator==(const ClosedCallContext &,
                         const ClosedCallContext &) = default;
};

struct ClosedCallSummary {
  bool complete = false;
  // Result/postcondition completeness is independent of effect closure.
  // A known scalar result may survive an opaque cleanup or runtime operation,
  // but callers must invalidate memory and cannot use that call as a closed
  // recurrence edge.
  bool hasOpaqueEffects = false;
  LiveMemoryBarrier liveMemoryBarrier = LiveMemoryBarrier::None;
  ObservedFact<IntegerFact> result;
  ObservedFact<ObjectFact> receiver;
  std::vector<ObservedFact<ObjectFact>> argumentObjects;
  ObservedFact<ObjectFact> constructed;

  friend bool operator==(const ClosedCallSummary &,
                         const ClosedCallSummary &) = default;
};

struct MirBodyAddressLess {
  bool operator()(MirBodyAddress left, MirBodyAddress right) const {
    return std::tuple{static_cast<std::size_t>(left.kind), left.owner} <
           std::tuple{static_cast<std::size_t>(right.kind), right.owner};
  }
};

struct ClosedCallRound {
  const MirProgram &program;
  const std::map<MirBodyAddress, ClosedCallSummary, MirBodyAddressLess>
      &summaries;
  const std::set<MirBodyAddress, MirBodyAddressLess> &rejected;
  std::vector<std::pair<MirBodyAddress, ClosedCallContext>> discovered;
  std::set<MirBodyAddress, MirBodyAddressLess> incompleteBodies;
};

using PlaceThresholds = std::map<MirPlaceId, IntegerFact>;

struct BlockTransfer {
  AbstractState output;
  std::map<MirInstructionId, AbstractState> before;
};

[[nodiscard]] int compare(CheckedIntegerValue left, CheckedIntegerValue right) {
  left = normalizeCheckedInteger(left);
  right = normalizeCheckedInteger(right);
  if (left.negative != right.negative) {
    return left.negative ? -1 : 1;
  }
  if (left.magnitude == right.magnitude) {
    return 0;
  }
  if (left.negative) {
    return left.magnitude > right.magnitude ? -1 : 1;
  }
  return left.magnitude < right.magnitude ? -1 : 1;
}

[[nodiscard]] CheckedIntegerValue minimumValue(CheckedIntegerDomain domain) {
  return domain.signedValue
             ? CheckedIntegerValue{.negative = true,
                                   .magnitude = std::uint64_t{1}
                                                << (domain.width - 1)}
             : CheckedIntegerValue{};
}

[[nodiscard]] CheckedIntegerValue maximumValue(CheckedIntegerDomain domain) {
  if (!domain.signedValue) {
    return {.magnitude = checkedIntegerMask(domain)};
  }
  return {.magnitude =
              (std::uint64_t{1} << (domain.width - 1)) - std::uint64_t{1}};
}

template <typename T>
void appendUnique(std::vector<T> &target, const std::vector<T> &source) {
  target.insert(target.end(), source.begin(), source.end());
  std::sort(target.begin(), target.end());
  target.erase(std::unique(target.begin(), target.end()), target.end());
}

void add(Evidence &evidence, MirBlockId block) {
  if (block != 0) {
    evidence.blocks.push_back(block);
    std::sort(evidence.blocks.begin(), evidence.blocks.end());
    evidence.blocks.erase(
        std::unique(evidence.blocks.begin(), evidence.blocks.end()),
        evidence.blocks.end());
  }
}

void addInstruction(Evidence &evidence, MirInstructionId instruction) {
  if (instruction != 0) {
    evidence.instructions.push_back(instruction);
    std::sort(evidence.instructions.begin(), evidence.instructions.end());
    evidence.instructions.erase(
        std::unique(evidence.instructions.begin(), evidence.instructions.end()),
        evidence.instructions.end());
  }
}

void addValue(Evidence &evidence, MirValueId value) {
  if (value != 0) {
    evidence.values.push_back(value);
    std::sort(evidence.values.begin(), evidence.values.end());
    evidence.values.erase(
        std::unique(evidence.values.begin(), evidence.values.end()),
        evidence.values.end());
  }
}

void addPlace(Evidence &evidence, MirPlaceId place) {
  if (place != 0) {
    evidence.places.push_back(place);
    std::sort(evidence.places.begin(), evidence.places.end());
    evidence.places.erase(
        std::unique(evidence.places.begin(), evidence.places.end()),
        evidence.places.end());
  }
}

[[nodiscard]] Evidence merged(Evidence left, const Evidence &right) {
  appendUnique(left.blocks, right.blocks);
  appendUnique(left.instructions, right.instructions);
  appendUnique(left.values, right.values);
  appendUnique(left.places, right.places);
  return left;
}

[[nodiscard]] IntegerFact fullIntegerFact(CheckedIntegerDomain domain,
                                          Evidence evidence = {}) {
  return {.domain = domain,
          .lower = minimumValue(domain),
          .upper = maximumValue(domain),
          .evidence = std::move(evidence)};
}

[[nodiscard]] std::optional<IntegerFact>
literalIntegerFact(const Literal &literal, const SemanticType &type,
                   Evidence evidence = {}) {
  const std::optional<CheckedIntegerDomain> domain =
      constantIntegerDomain(type);
  if (!domain) {
    return std::nullopt;
  }
  const ConstantEvaluation evaluated = evaluateConstantLiteral(literal, domain);
  const ConstantInteger *integer =
      evaluated.value ? std::get_if<ConstantInteger>(&*evaluated.value)
                      : nullptr;
  if (integer == nullptr || integer->domain != *domain) {
    return std::nullopt;
  }
  const CheckedIntegerValue value = checkedIntegerValue(*integer);
  return IntegerFact{.domain = *domain,
                     .lower = value,
                     .upper = value,
                     .evidence = std::move(evidence)};
}

[[nodiscard]] std::optional<CheckedIntegerValue>
evaluatedValue(const std::optional<CheckedIntegerOutcome> &outcome) {
  if (!outcome) {
    return std::nullopt;
  }
  const CheckedIntegerValue *value =
      std::get_if<CheckedIntegerValue>(&*outcome);
  return value == nullptr ? std::nullopt
                          : std::optional<CheckedIntegerValue>{*value};
}

[[nodiscard]] CheckedIntegerValue lesser(CheckedIntegerValue left,
                                         CheckedIntegerValue right) {
  return compare(left, right) <= 0 ? left : right;
}

[[nodiscard]] std::optional<IntegerFact>
remainderFact(const IntegerFact &left, const IntegerFact &right) {
  if (left.domain != right.domain || right.lower != right.upper) {
    return std::nullopt;
  }
  const CheckedIntegerValue divisor = normalizeCheckedInteger(right.lower);
  if (divisor.magnitude == 0) {
    return std::nullopt;
  }

  const CheckedIntegerValue zero;
  const CheckedIntegerValue positiveLimit{.magnitude = divisor.magnitude -
                                                       std::uint64_t{1}};
  const CheckedIntegerValue negativeLimit = normalizeCheckedInteger(
      {.negative = true, .magnitude = positiveLimit.magnitude});
  IntegerFact result{
      .domain = left.domain,
      .lower = zero,
      .upper = positiveLimit,
      .wideningEnvelope =
          std::pair{left.domain.signedValue ? negativeLimit : zero,
                    positiveLimit},
      .evidence = merged(left.evidence, right.evidence)};

  if (!left.domain.signedValue || compare(left.lower, zero) >= 0) {
    if (compare(left.upper,
                CheckedIntegerValue{.magnitude = divisor.magnitude}) < 0) {
      result.lower = left.lower;
      result.upper = left.upper;
    } else {
      result.upper = lesser(left.upper, positiveLimit);
    }
    return result;
  }

  if (compare(left.upper, zero) <= 0) {
    if (left.lower.magnitude < divisor.magnitude) {
      result.lower = left.lower;
      result.upper = left.upper;
    } else {
      result.lower = normalizeCheckedInteger(
          {.negative = true,
           .magnitude =
               std::min(left.lower.magnitude, positiveLimit.magnitude)});
      result.upper = zero;
    }
    return result;
  }

  result.lower = normalizeCheckedInteger(
      {.negative = true,
       .magnitude = std::min(left.lower.magnitude, positiveLimit.magnitude)});
  result.upper = lesser(left.upper, positiveLimit);
  return result;
}

[[nodiscard]] std::optional<IntegerFact>
bitwiseAndFact(const IntegerFact &left, const IntegerFact &right) {
  if (left.domain != right.domain) {
    return std::nullopt;
  }
  const IntegerFact *mask = nullptr;
  if (left.lower == left.upper &&
      !normalizeCheckedInteger(left.lower).negative) {
    mask = &left;
  } else if (right.lower == right.upper &&
             !normalizeCheckedInteger(right.lower).negative) {
    mask = &right;
  }
  if (mask == nullptr) {
    return std::nullopt;
  }
  const CheckedIntegerValue zero;
  const CheckedIntegerValue upper = normalizeCheckedInteger(mask->lower);
  return IntegerFact{.domain = left.domain,
                     .lower = zero,
                     .upper = upper,
                     .wideningEnvelope = std::pair{zero, upper},
                     .evidence = merged(left.evidence, right.evidence)};
}

[[nodiscard]] std::optional<IntegerFact>
binaryFact(CheckedIntegerOperation operation, const IntegerFact &left,
           const IntegerFact &right) {
  if (left.domain != right.domain) {
    return std::nullopt;
  }
  std::vector<CheckedIntegerValue> results;
  const auto evaluate = [&](CheckedIntegerValue lhs, CheckedIntegerValue rhs) {
    const std::optional<CheckedIntegerValue> value = evaluatedValue(
        evaluateCheckedIntegerBinary(operation, lhs, rhs, left.domain));
    if (value) {
      results.push_back(*value);
    }
    return value.has_value();
  };
  bool safe = true;
  switch (operation) {
  case CheckedIntegerOperation::Remainder:
    return remainderFact(left, right);
  case CheckedIntegerOperation::BitwiseAnd:
    return bitwiseAndFact(left, right);
  case CheckedIntegerOperation::Add:
    safe =
        evaluate(left.lower, right.lower) && evaluate(left.upper, right.upper);
    break;
  case CheckedIntegerOperation::Subtract:
    safe =
        evaluate(left.lower, right.upper) && evaluate(left.upper, right.lower);
    break;
  case CheckedIntegerOperation::Multiply:
    safe = evaluate(left.lower, right.lower) &&
           evaluate(left.lower, right.upper) &&
           evaluate(left.upper, right.lower) &&
           evaluate(left.upper, right.upper);
    break;
  default:
    return std::nullopt;
  }
  if (!safe || results.empty()) {
    return std::nullopt;
  }
  const auto less = [](CheckedIntegerValue lhs, CheckedIntegerValue rhs) {
    return compare(lhs, rhs) < 0;
  };
  IntegerFact result{
      .domain = left.domain,
      .lower = *std::min_element(results.begin(), results.end(), less),
      .upper = *std::max_element(results.begin(), results.end(), less),
      .evidence = merged(left.evidence, right.evidence)};
  const auto stableBounds = [](const IntegerFact &fact) {
    if (fact.wideningEnvelope) {
      return fact.wideningEnvelope;
    }
    return fact.lower == fact.upper
               ? std::optional<std::pair<CheckedIntegerValue,
                                         CheckedIntegerValue>>{std::pair{
                     fact.lower, fact.upper}}
               : std::nullopt;
  };
  const auto leftBounds = stableBounds(left);
  const auto rightBounds = stableBounds(right);
  if (!leftBounds || !rightBounds) {
    return result;
  }
  std::vector<CheckedIntegerValue> envelopeValues;
  const auto evaluateEnvelope = [&](CheckedIntegerValue lhs,
                                    CheckedIntegerValue rhs) {
    const std::optional<CheckedIntegerValue> value = evaluatedValue(
        evaluateCheckedIntegerBinary(operation, lhs, rhs, left.domain));
    if (value) {
      envelopeValues.push_back(*value);
    }
    return value.has_value();
  };
  bool stable = true;
  switch (operation) {
  case CheckedIntegerOperation::Add:
    stable = (left.lower == left.upper || right.lower == right.upper) &&
             evaluateEnvelope(leftBounds->first, rightBounds->first) &&
             evaluateEnvelope(leftBounds->second, rightBounds->second);
    break;
  case CheckedIntegerOperation::Subtract:
    stable = (left.lower == left.upper || right.lower == right.upper) &&
             evaluateEnvelope(leftBounds->first, rightBounds->second) &&
             evaluateEnvelope(leftBounds->second, rightBounds->first);
    break;
  case CheckedIntegerOperation::Multiply:
    stable = evaluateEnvelope(leftBounds->first, rightBounds->first) &&
             evaluateEnvelope(leftBounds->first, rightBounds->second) &&
             evaluateEnvelope(leftBounds->second, rightBounds->first) &&
             evaluateEnvelope(leftBounds->second, rightBounds->second);
    break;
  default:
    stable = false;
    break;
  }
  if (stable && !envelopeValues.empty()) {
    result.wideningEnvelope = std::pair{
        *std::min_element(envelopeValues.begin(), envelopeValues.end(), less),
        *std::max_element(envelopeValues.begin(), envelopeValues.end(), less)};
  }
  return result;
}

[[nodiscard]] std::optional<IntegerFact>
negatedFact(const IntegerFact &operand) {
  const std::optional<CheckedIntegerValue> lower =
      evaluatedValue(evaluateCheckedIntegerUnary(
          CheckedIntegerOperation::Negate, operand.upper, operand.domain));
  const std::optional<CheckedIntegerValue> upper =
      evaluatedValue(evaluateCheckedIntegerUnary(
          CheckedIntegerOperation::Negate, operand.lower, operand.domain));
  if (!lower || !upper) {
    return std::nullopt;
  }
  return IntegerFact{.domain = operand.domain,
                     .lower = *lower,
                     .upper = *upper,
                     .evidence = operand.evidence};
}

[[nodiscard]] std::optional<IntegerFact>
convertedIntegerFact(const IntegerFact &source, CheckedIntegerDomain target) {
  if (!checkedIntegerFits(source.lower, target) ||
      !checkedIntegerFits(source.upper, target)) {
    return std::nullopt;
  }
  std::optional<std::pair<CheckedIntegerValue, CheckedIntegerValue>> envelope;
  if (source.wideningEnvelope &&
      checkedIntegerFits(source.wideningEnvelope->first, target) &&
      checkedIntegerFits(source.wideningEnvelope->second, target)) {
    envelope = source.wideningEnvelope;
  }
  return IntegerFact{.domain = target,
                     .lower = source.lower,
                     .upper = source.upper,
                     .wideningEnvelope = std::move(envelope),
                     .evidence = source.evidence};
}

[[nodiscard]] std::optional<IntegerFact>
integerForValue(const MirBody &body, const AbstractState &state,
                MirValueId value) {
  const auto known = state.integers.find(value);
  if (known != state.integers.end()) {
    return known->second;
  }
  const MirValue *mirValue = body.findValue(value);
  const std::optional<CheckedIntegerDomain> domain =
      mirValue == nullptr ? std::nullopt
                          : constantIntegerDomain(mirValue->info.type);
  if (!domain) {
    return std::nullopt;
  }
  Evidence evidence;
  addValue(evidence, value);
  return fullIntegerFact(*domain, std::move(evidence));
}

[[nodiscard]] std::optional<IntegerFact>
integerForPlace(const MirBody &body, const AbstractState &state,
                MirPlaceId place) {
  const auto known = state.integerPlaces.find(place);
  if (known != state.integerPlaces.end()) {
    return known->second;
  }
  const MirPlace *mirPlace = body.findPlace(place);
  const std::optional<CheckedIntegerDomain> domain =
      mirPlace == nullptr ? std::nullopt
                          : constantIntegerDomain(mirPlace->type);
  if (!domain) {
    return std::nullopt;
  }
  Evidence evidence;
  addPlace(evidence, place);
  return fullIntegerFact(*domain, std::move(evidence));
}

[[nodiscard]] MirPlaceId
canonicalPlace(const MirPlaceAliasEscapeAnalysis &aliases, MirPlaceId place) {
  if (place == 0 || place > aliases.placeCount) {
    return place;
  }
  for (MirPlaceId candidate = 1; candidate <= aliases.placeCount; ++candidate) {
    if (aliases.relation(place, candidate) == MirAliasRelation::MustAlias) {
      return candidate;
    }
  }
  return place;
}

[[nodiscard]] bool immutableScalarBinding(const MirBody &body, MirPlaceId id) {
  const auto *place = body.findPlace(id);
  return place && place->root == MirPlaceRootKind::Binding &&
         place->binding != 0 && place->projections.empty() &&
         place->access == AccessMode::ReadOnly &&
         !place->traits.containsBorrowedState &&
         constantIntegerDomain(place->type).has_value();
}

[[nodiscard]] std::optional<IntegerFact>
integerForOperand(const MirBody &body, const AbstractState &state,
                  const MirOperand &operand) {
  if (operand.literal) {
    return literalIntegerFact(*operand.literal, operand.type);
  }
  if (operand.kind == MirOperandKind::Value && operand.value != 0) {
    return integerForValue(body, state, operand.value);
  }
  if (operand.place != 0) {
    return integerForPlace(body, state, operand.place);
  }
  return std::nullopt;
}

[[nodiscard]] bool singleton(const IntegerFact &fact) {
  return fact.lower == fact.upper;
}

void stabilize(IntegerFact &fact) {
  fact.wideningEnvelope = std::pair{fact.lower, fact.upper};
}

void stabilize(ObjectFact &object) {
  for (auto &[path, fact] : object.integers) {
    (void)path;
    stabilize(fact);
  }
  if (object.elements) {
    stabilize(*object.elements);
  }
  for (auto &[path, fact] : object.elementIntegers) {
    (void)path;
    stabilize(fact);
  }
}

[[nodiscard]] IntegerFact joinedInteger(const IntegerFact &left,
                                        const IntegerFact &right, bool widen);

[[nodiscard]] IntegerFact joinedElementFact(const IntegerFact &existing,
                                            const IntegerFact &incoming) {
  IntegerFact result = joinedInteger(existing, incoming, false);
  const std::optional<std::pair<CheckedIntegerValue, CheckedIntegerValue>>
      incomingEnvelope =
          incoming.wideningEnvelope
              ? incoming.wideningEnvelope
              : (singleton(incoming)
                     ? std::optional<std::pair<CheckedIntegerValue,
                                               CheckedIntegerValue>>{std::pair{
                           incoming.lower, incoming.upper}}
                     : std::nullopt);
  if (!incomingEnvelope || existing.domain != incoming.domain) {
    return result;
  }
  const auto existingEnvelope = existing.wideningEnvelope.value_or(
      std::pair{existing.lower, existing.upper});
  result.wideningEnvelope =
      std::pair{compare(existingEnvelope.first, incomingEnvelope->first) <= 0
                    ? existingEnvelope.first
                    : incomingEnvelope->first,
                compare(existingEnvelope.second, incomingEnvelope->second) >= 0
                    ? existingEnvelope.second
                    : incomingEnvelope->second};
  return result;
}

[[nodiscard]] std::optional<CheckedIntegerValue>
adjust(CheckedIntegerValue value, int delta, CheckedIntegerDomain domain) {
  const CheckedIntegerValue one{.magnitude = 1};
  return evaluatedValue(
      evaluateCheckedIntegerBinary(delta < 0 ? CheckedIntegerOperation::Subtract
                                             : CheckedIntegerOperation::Add,
                                   value, one, domain));
}

[[nodiscard]] bool intersect(IntegerFact &fact,
                             std::optional<CheckedIntegerValue> lower,
                             std::optional<CheckedIntegerValue> upper,
                             Evidence evidence) {
  if (lower && compare(*lower, fact.lower) > 0) {
    fact.lower = *lower;
  }
  if (upper && compare(*upper, fact.upper) < 0) {
    fact.upper = *upper;
  }
  if (compare(fact.lower, fact.upper) > 0) {
    return false;
  }
  fact.evidence = merged(std::move(fact.evidence), evidence);
  return true;
}

[[nodiscard]] const MirInstruction *definingInstruction(const MirBody &body,
                                                        MirValueId value) {
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
}

[[nodiscard]] MirOperation invertedComparison(MirOperation operation) {
  switch (operation) {
  case MirOperation::Less:
    return MirOperation::Greater;
  case MirOperation::LessEqual:
    return MirOperation::GreaterEqual;
  case MirOperation::Greater:
    return MirOperation::Less;
  case MirOperation::GreaterEqual:
    return MirOperation::LessEqual;
  default:
    return operation;
  }
}

void refineCondition(const MirBody &body, MirBlockId block,
                     const MirOperand &condition, AbstractState &state,
                     bool branchTaken, Evidence conditionEvidence = {}) {
  if (!state.reachable || condition.kind != MirOperandKind::Value) {
    return;
  }
  const MirInstruction *comparison = definingInstruction(body, condition.value);
  // A detector consumes an exactly staged boolean value. Follow only inert
  // value staging/boolean negation, with a hard bound; arbitrary source calls
  // and loaded booleans do not supply a hidden predicate contract.
  for (std::size_t depth = 0; comparison && depth < 32; ++depth) {
    if (comparison->operands.size() != 1 ||
        comparison->operands.front().kind != MirOperandKind::Value ||
        comparison->info.type != SemanticType::Bool)
      break;
    const bool staged = comparison->kind == MirInstructionKind::CallInput &&
                        comparison->callInputKind == HirCallInputKind::Value;
    const bool negated = comparison->kind == MirInstructionKind::Compute &&
                         comparison->operation == MirOperation::LogicalNot;
    const bool identity = comparison->kind == MirInstructionKind::Compute &&
                          comparison->operation == MirOperation::Identity;
    if (!staged && !negated && !identity)
      break;
    const auto effect = effects(*comparison);
    if (comparison->rawMemoryAccess ||
        comparison->unsafeOperation != UnsafeOperationKind::None ||
        effect.writesPlace || effect.writesUnknownMemory ||
        effect.invokesUserCode || effect.maySynchronize)
      return;
    addInstruction(conditionEvidence, comparison->id);
    addValue(conditionEvidence, *comparison->result);
    if (negated)
      branchTaken = !branchTaken;
    comparison = definingInstruction(body, comparison->operands.front().value);
  }
  if (comparison == nullptr ||
      comparison->kind != MirInstructionKind::Compute ||
      comparison->operands.size() != 2) {
    return;
  }
  MirOperation operation = comparison->operation;
  if (operation != MirOperation::Less && operation != MirOperation::LessEqual &&
      operation != MirOperation::Greater &&
      operation != MirOperation::GreaterEqual &&
      operation != MirOperation::Equal && operation != MirOperation::NotEqual) {
    return;
  }
  const MirOperand *variable = &comparison->operands[0];
  const MirOperand *constant = &comparison->operands[1];
  std::optional<IntegerFact> variableFact =
      integerForOperand(body, state, *variable);
  std::optional<IntegerFact> constantFact =
      integerForOperand(body, state, *constant);
  if (!variableFact || !constantFact || !singleton(*constantFact)) {
    variable = &comparison->operands[1];
    constant = &comparison->operands[0];
    variableFact = integerForOperand(body, state, *variable);
    constantFact = integerForOperand(body, state, *constant);
    operation = invertedComparison(operation);
  }
  if (!variableFact || !constantFact || !singleton(*constantFact) ||
      variableFact->domain != constantFact->domain || variable->value == 0) {
    return;
  }

  std::optional<CheckedIntegerValue> lower;
  std::optional<CheckedIntegerValue> upper;
  const CheckedIntegerValue boundary = constantFact->lower;
  switch (operation) {
  case MirOperation::Less:
    if (branchTaken) {
      upper = adjust(boundary, -1, variableFact->domain);
    } else {
      lower = boundary;
    }
    break;
  case MirOperation::LessEqual:
    if (branchTaken) {
      upper = boundary;
    } else {
      lower = adjust(boundary, 1, variableFact->domain);
    }
    break;
  case MirOperation::Greater:
    if (branchTaken) {
      lower = adjust(boundary, 1, variableFact->domain);
    } else {
      upper = boundary;
    }
    break;
  case MirOperation::GreaterEqual:
    if (branchTaken) {
      lower = boundary;
    } else {
      upper = adjust(boundary, -1, variableFact->domain);
    }
    break;
  case MirOperation::Equal:
    if (branchTaken) {
      lower = boundary;
      upper = boundary;
    }
    break;
  case MirOperation::NotEqual:
    if (!branchTaken) {
      lower = boundary;
      upper = boundary;
    }
    break;
  default:
    return;
  }
  // An adjustment beyond the integer domain denotes an unreachable edge. It
  // is intentionally left unrefined here; no proof is emitted from that edge.
  if ((operation == MirOperation::Less && branchTaken && !upper) ||
      (operation == MirOperation::LessEqual && !branchTaken && !lower) ||
      (operation == MirOperation::Greater && branchTaken && !lower) ||
      (operation == MirOperation::GreaterEqual && !branchTaken && !upper)) {
    return;
  }
  Evidence evidence = merged(variableFact->evidence, constantFact->evidence);
  evidence = merged(std::move(evidence), conditionEvidence);
  add(evidence, block);
  addInstruction(evidence, comparison->id);
  addValue(evidence, condition.value);
  if (!intersect(*variableFact, lower, upper, evidence)) {
    state.reachable = false;
    return;
  }
  auto branchEnvelope = variableFact->wideningEnvelope.value_or(std::pair{
      minimumValue(variableFact->domain), maximumValue(variableFact->domain)});
  if (lower && compare(*lower, branchEnvelope.first) > 0) {
    branchEnvelope.first = *lower;
  }
  if (upper && compare(*upper, branchEnvelope.second) < 0) {
    branchEnvelope.second = *upper;
  }
  if (compare(branchEnvelope.first, branchEnvelope.second) <= 0) {
    variableFact->wideningEnvelope = branchEnvelope;
  }
  state.integers[variable->value] = *variableFact;
  const auto loadedOrigin = state.loadedOrigins.find(variable->value);
  if (loadedOrigin != state.loadedOrigins.end()) {
    const MirPlaceId place = loadedOrigin->second;
    IntegerFact placeFact =
        integerForPlace(body, state, place).value_or(*variableFact);
    if (!intersect(placeFact, lower, upper, evidence)) {
      state.reachable = false;
      return;
    }
    placeFact.wideningEnvelope = variableFact->wideningEnvelope;
    state.integerPlaces[place] = std::move(placeFact);
  }
}

void refineBranch(const MirBody &body, const MirBlock &block,
                  AbstractState &state, bool branchTaken) {
  if (block.terminator.kind == MirTerminatorKind::Branch &&
      block.terminator.value)
    refineCondition(body, block.id, *block.terminator.value, state,
                    branchTaken);
}

[[nodiscard]] std::optional<IntegerFact>
computeIntegerFact(const MirBody &body, const AbstractState &state,
                   const MirInstruction &instruction) {
  if (!instruction.result ||
      !constantIntegerDomain(instruction.info.type).has_value()) {
    return std::nullopt;
  }
  Evidence evidence;
  addInstruction(evidence, instruction.id);
  addValue(evidence, *instruction.result);
  if (instruction.operation == MirOperation::Literal && instruction.literal) {
    return literalIntegerFact(*instruction.literal, instruction.info.type,
                              std::move(evidence));
  }
  if (instruction.operands.empty()) {
    return std::nullopt;
  }
  const std::optional<IntegerFact> left =
      integerForOperand(body, state, instruction.operands[0]);
  if (!left) {
    return std::nullopt;
  }
  std::optional<IntegerFact> result;
  switch (instruction.operation) {
  case MirOperation::Identity:
  case MirOperation::Positive:
    result = left;
    break;
  case MirOperation::Convert: {
    const CheckedIntegerDomain target =
        *constantIntegerDomain(instruction.info.type);
    result = convertedIntegerFact(*left, target);
    break;
  }
  case MirOperation::Negate:
    result = negatedFact(*left);
    break;
  case MirOperation::Add:
  case MirOperation::Subtract:
  case MirOperation::Multiply:
  case MirOperation::Remainder:
  case MirOperation::BitwiseAnd: {
    if (instruction.operands.size() != 2) {
      break;
    }
    const std::optional<IntegerFact> right =
        integerForOperand(body, state, instruction.operands[1]);
    if (!right) {
      break;
    }
    CheckedIntegerOperation operation = CheckedIntegerOperation::BitwiseAnd;
    switch (instruction.operation) {
    case MirOperation::Add:
      operation = CheckedIntegerOperation::Add;
      break;
    case MirOperation::Subtract:
      operation = CheckedIntegerOperation::Subtract;
      break;
    case MirOperation::Multiply:
      operation = CheckedIntegerOperation::Multiply;
      break;
    case MirOperation::Remainder:
      operation = CheckedIntegerOperation::Remainder;
      break;
    case MirOperation::BitwiseAnd:
      break;
    default:
      break;
    }
    result = binaryFact(operation, *left, *right);
    break;
  }
  default:
    break;
  }
  if (result) {
    result->evidence = merged(std::move(result->evidence), evidence);
  }
  return result;
}

[[nodiscard]] bool samePlacePrefix(const MirPlace &candidate,
                                   const MirPlace &projected) {
  if (candidate.root != projected.root ||
      candidate.hostedStartupOperation != projected.hostedStartupOperation ||
      candidate.binding != projected.binding ||
      candidate.symbol != projected.symbol ||
      candidate.capture != projected.capture ||
      candidate.temporary != projected.temporary ||
      candidate.value != projected.value || candidate.loan != projected.loan ||
      candidate.projections.size() + 1 != projected.projections.size()) {
    return false;
  }
  return std::equal(candidate.projections.begin(), candidate.projections.end(),
                    projected.projections.begin());
}

struct FixedArrayProjection {
  MirPlaceId arrayPlace = 0;
  MirPlaceId elementPlace = 0;
  std::uint64_t length = 0;
  MirPlaceProjection projection;
};

[[nodiscard]] IntegerFact joinedInteger(const IntegerFact &left,
                                        const IntegerFact &right, bool widen);

// The sibling array place one Index projection above `projected`, whatever
// its element type. A nonterminal dimension of a chained element access has
// an array element type, so it stays outside fixedArrayProjection()'s
// scalar-element shape while still owning a concrete bound.
[[nodiscard]] const MirPlace *fixedArrayParentPlace(const MirBody &body,
                                                    const MirPlace &projected) {
  const auto parent = std::find_if(
      body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
        return candidate.type.kind == SemanticType::Array &&
               candidate.type.arrayLength != 0 &&
               candidate.type.arguments.size() == 1 &&
               samePlacePrefix(candidate, projected);
      });
  return parent == body.places.end() ? nullptr : &*parent;
}

[[nodiscard]] FixedArrayProjection
fixedArrayProjection(const MirBody &body, MirPlaceId projectedId) {
  const MirPlace *projected = body.findPlace(projectedId);
  if (projected == nullptr || projected->projections.empty()) {
    return {};
  }
  const MirPlaceProjection &projection = projected->projections.back();
  if (projection.kind != MirProjectionKind::Index) {
    return {};
  }
  const auto parent = std::find_if(
      body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
        return candidate.type.kind == SemanticType::Array &&
               candidate.type.arrayLength != 0 &&
               candidate.type.arguments.size() == 1 &&
               constantIntegerDomain(candidate.type.arguments.front()) &&
               samePlacePrefix(candidate, *projected);
      });
  if (parent == body.places.end()) {
    return {};
  }
  return {.arrayPlace = parent->id,
          .elementPlace = projectedId,
          .length = parent->type.arrayLength,
          .projection = projection};
}

[[nodiscard]] bool samePlaceRoot(const MirPlace &left, const MirPlace &right) {
  return left.hostedStartupOperation == right.hostedStartupOperation &&
         left.root == right.root && left.binding == right.binding &&
         left.symbol == right.symbol && left.capture == right.capture &&
         left.temporary == right.temporary && left.value == right.value &&
         left.loan == right.loan;
}

[[nodiscard]] bool transparentRootProjection(MirProjectionKind kind) {
  return kind == MirProjectionKind::Dereference ||
         kind == MirProjectionKind::RawDereference;
}

[[nodiscard]] MirPlaceId
aggregateRoot(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
              MirPlaceId placeId) {
  placeId = canonicalPlace(aliases, placeId);
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return 0;
  }
  const MirPlace *best = nullptr;
  for (const MirPlace &candidate : body.places) {
    if (!samePlaceRoot(candidate, *place) ||
        candidate.projections.size() > place->projections.size() ||
        !std::equal(candidate.projections.begin(), candidate.projections.end(),
                    place->projections.begin())) {
      continue;
    }
    const SemanticType *type = &candidate.type;
    if (type->kind == SemanticType::Reference && type->arguments.size() == 1) {
      type = &type->arguments.front();
    }
    if (type->kind != SemanticType::Class) {
      continue;
    }
    if (best == nullptr ||
        candidate.projections.size() > best->projections.size()) {
      best = &candidate;
    }
  }
  return best == nullptr ? 0 : canonicalPlace(aliases, best->id);
}

[[nodiscard]] std::optional<FieldPath> fieldPath(const MirPlace &root,
                                                 const MirPlace &place,
                                                 bool includeRoot = false) {
  if (!samePlaceRoot(root, place) ||
      root.projections.size() > place.projections.size() ||
      !std::equal(root.projections.begin(), root.projections.end(),
                  place.projections.begin())) {
    return std::nullopt;
  }
  FieldPath result;
  for (auto iterator = place.projections.begin() + root.projections.size();
       iterator != place.projections.end(); ++iterator) {
    const MirPlaceProjection &projection = *iterator;
    if (transparentRootProjection(projection.kind)) {
      continue;
    }
    if (projection.kind != MirProjectionKind::Field || projection.field == 0) {
      return std::nullopt;
    }
    result.push_back(projection.field);
  }
  return result.empty() && !includeRoot
             ? std::nullopt
             : std::optional<FieldPath>{std::move(result)};
}

[[nodiscard]] std::optional<FieldPath> fieldPath(const MirBody &body,
                                                 MirPlaceId rootId,
                                                 MirPlaceId placeId,
                                                 bool includeRoot = false) {
  if (rootId == 0) {
    const MirPlace receiver{.root = MirPlaceRootKind::This};
    const MirPlace *place = body.findPlace(placeId);
    return place == nullptr ? std::nullopt
                            : fieldPath(receiver, *place, includeRoot);
  }
  const MirPlace *root = body.findPlace(rootId);
  const MirPlace *place = body.findPlace(placeId);
  return root == nullptr || place == nullptr
             ? std::nullopt
             : fieldPath(*root, *place, includeRoot);
}

struct IndexedAggregateAccess {
  MirPlaceId root = 0;
  FieldPath field;
};

[[nodiscard]] std::optional<IndexedAggregateAccess>
indexedAggregateAccess(const MirProgram *program, const MirBody &body,
                       const MirPlaceAliasEscapeAnalysis &aliases,
                       MirPlaceId placeId);

[[nodiscard]] IntegerFact rebased(IntegerFact fact, MirBlockId block = 0,
                                  MirInstructionId instruction = 0,
                                  MirValueId value = 0, MirPlaceId place = 0) {
  fact.evidence = {};
  add(fact.evidence, block);
  addInstruction(fact.evidence, instruction);
  addValue(fact.evidence, value);
  addPlace(fact.evidence, place);
  return fact;
}

void seedObjectFact(const MirBody &body,
                    const MirPlaceAliasEscapeAnalysis &aliases, MirPlaceId root,
                    const ObjectFact &object, AbstractState &state) {
  root = canonicalPlace(aliases, root);
  state.objects.insert_or_assign(root, object);
  for (const MirPlace &place : body.places) {
    const std::optional<FieldPath> path = fieldPath(body, root, place.id);
    if (!path) {
      continue;
    }
    const MirPlaceId destination = canonicalPlace(aliases, place.id);
    if (const auto integer = object.integers.find(*path);
        integer != object.integers.end()) {
      state.integerPlaces[destination] =
          rebased(integer->second, 0, 0, 0, destination);
    }
  }
  if (object.elements) {
    state.aggregateElements[root] = rebased(*object.elements, 0, 0, 0, root);
  } else {
    state.aggregateElements.erase(root);
  }
  if (!object.elementIntegers.empty()) {
    IntegerFieldFacts fields = object.elementIntegers;
    for (auto &[path, fact] : fields) {
      (void)path;
      fact = rebased(std::move(fact), 0, 0, 0, root);
    }
    state.aggregateElementIntegers.insert_or_assign(root, std::move(fields));
  } else {
    state.aggregateElementIntegers.erase(root);
  }
}

[[nodiscard]] std::optional<ObjectFact>
extractObjectFact(const MirBody &body,
                  const MirPlaceAliasEscapeAnalysis &aliases, MirPlaceId root,
                  const AbstractState &state) {
  root = canonicalPlace(aliases, root);
  ObjectFact result;
  if (const auto object = state.objects.find(root);
      object != state.objects.end()) {
    result = object->second;
  }
  // Whole-object summaries use paths relative to that object's exact place.
  // Project a retained ancestor's fields when an ordinary call selects a
  // class subobject for which MIR has no descendant scalar places yet.
  for (const auto &[parent, object] : state.objects) {
    if (parent == root)
      continue;
    const auto prefix = fieldPath(body, parent, root, true);
    if (!prefix)
      continue;
    for (const auto &[path, fact] : object.integers) {
      if (path.size() <= prefix->size() ||
          !std::equal(prefix->begin(), prefix->end(), path.begin()))
        continue;
      FieldPath suffix(path.begin() + prefix->size(), path.end());
      result.integers.insert_or_assign(std::move(suffix), fact);
    }
  }
  bool observedElements = false;
  std::optional<IntegerFact> derivedElements;
  bool observedElementIntegers = false;
  IntegerFieldFacts derivedElementIntegers;
  const auto mergeElementIntegers = [&](const IntegerFieldFacts &incoming) {
    if (!observedElementIntegers) {
      derivedElementIntegers = incoming;
      observedElementIntegers = true;
      return;
    }
    for (auto iterator = derivedElementIntegers.begin();
         iterator != derivedElementIntegers.end();) {
      const auto found = incoming.find(iterator->first);
      if (found == incoming.end()) {
        iterator = derivedElementIntegers.erase(iterator);
        continue;
      }
      iterator->second = joinedElementFact(iterator->second, found->second);
      ++iterator;
    }
  };
  for (const MirPlace &place : body.places) {
    const std::optional<FieldPath> path = fieldPath(body, root, place.id);
    if (!path) {
      continue;
    }
    const MirPlaceId source = canonicalPlace(aliases, place.id);
    if (const auto integer = state.integerPlaces.find(source);
        integer != state.integerPlaces.end()) {
      result.integers.insert_or_assign(*path, integer->second);
    }
    if (const auto nested = state.objects.find(source);
        source != root && nested != state.objects.end()) {
      for (const auto &[suffix, fact] : nested->second.integers) {
        FieldPath combined = *path;
        combined.insert(combined.end(), suffix.begin(), suffix.end());
        result.integers.insert_or_assign(std::move(combined), fact);
      }
    }
  }
  if (const auto aggregateElements = state.aggregateElements.find(root);
      aggregateElements != state.aggregateElements.end()) {
    observedElements = true;
    derivedElements =
        derivedElements ? std::optional<IntegerFact>{joinedElementFact(
                              *derivedElements, aggregateElements->second)}
                        : std::optional<IntegerFact>{aggregateElements->second};
  }
  if (const auto aggregateFields = state.aggregateElementIntegers.find(root);
      aggregateFields != state.aggregateElementIntegers.end()) {
    observedElements = true;
    mergeElementIntegers(aggregateFields->second);
  }
  if (observedElements) {
    result.elements = std::move(derivedElements);
    result.elementIntegers = std::move(derivedElementIntegers);
  }
  if (result.integers.empty() && !result.elements &&
      result.elementIntegers.empty()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] MirPlaceId
objectOriginForOperand(const MirBody &body,
                       const MirPlaceAliasEscapeAnalysis &aliases,
                       const AbstractState &state, const MirOperand &operand) {
  if (operand.place != 0) {
    return aggregateRoot(body, aliases, operand.place);
  }
  const auto origin = state.aggregateOrigins.find(operand.value);
  return origin == state.aggregateOrigins.end() ? 0 : origin->second;
}

[[nodiscard]] std::optional<ObjectFact>
objectForOperand(const MirBody &body,
                 const MirPlaceAliasEscapeAnalysis &aliases,
                 const AbstractState &state, const MirOperand &operand) {
  if (const auto value = state.objectValues.find(operand.value);
      value != state.objectValues.end()) {
    return value->second;
  }
  const MirPlaceId origin =
      objectOriginForOperand(body, aliases, state, operand);
  return origin == 0 ? std::nullopt
                     : extractObjectFact(body, aliases, origin, state);
}

void applyObjectToOperand(const MirBody &body,
                          const MirPlaceAliasEscapeAnalysis &aliases,
                          const MirOperand &operand, const ObjectFact &object,
                          AbstractState &state) {
  const MirPlaceId origin =
      objectOriginForOperand(body, aliases, state, operand);
  if (origin != 0) {
    seedObjectFact(body, aliases, origin, object, state);
  } else if (operand.value != 0) {
    state.objectValues.insert_or_assign(operand.value, object);
  }
}

[[nodiscard]] std::optional<CheckedIntegerOperation>
compoundCheckedOperation(MirOperation operation) {
  switch (operation) {
  case MirOperation::AddAssign:
    return CheckedIntegerOperation::Add;
  case MirOperation::SubtractAssign:
    return CheckedIntegerOperation::Subtract;
  case MirOperation::MultiplyAssign:
    return CheckedIntegerOperation::Multiply;
  case MirOperation::RemainderAssign:
    return CheckedIntegerOperation::Remainder;
  case MirOperation::BitwiseAndAssign:
    return CheckedIntegerOperation::BitwiseAnd;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<IntegerFact>
compoundIntegerFact(const MirBody &body, const MirProgram *program,
                    const MirPlaceAliasEscapeAnalysis &aliases,
                    const AbstractState &state,
                    const MirInstruction &instruction) {
  if (!instruction.destination || instruction.operands.size() != 1) {
    return std::nullopt;
  }
  const std::optional<CheckedIntegerOperation> operation =
      compoundCheckedOperation(instruction.operation);
  const std::optional<IntegerFact> right =
      integerForOperand(body, state, instruction.operands.front());
  if (!operation || !right) {
    return std::nullopt;
  }
  std::optional<IntegerFact> left;
  if (const std::optional<IndexedAggregateAccess> indexed =
          indexedAggregateAccess(program, body, aliases,
                                 *instruction.destination)) {
    if (indexed->field.empty()) {
      if (const auto elements = state.aggregateElements.find(indexed->root);
          elements != state.aggregateElements.end()) {
        left = elements->second;
      }
    } else if (const auto fields =
                   state.aggregateElementIntegers.find(indexed->root);
               fields != state.aggregateElementIntegers.end()) {
      if (const auto fact = fields->second.find(indexed->field);
          fact != fields->second.end()) {
        left = fact->second;
      }
    }
  }
  if (!left) {
    const FixedArrayProjection projected =
        fixedArrayProjection(body, *instruction.destination);
    const auto elements =
        state.arrayElements.find(canonicalPlace(aliases, projected.arrayPlace));
    if (projected.arrayPlace != 0 && elements != state.arrayElements.end()) {
      left = elements->second;
    }
  }
  if (!left) {
    left = integerForPlace(body, state,
                           canonicalPlace(aliases, *instruction.destination));
  }
  return left ? binaryFact(*operation, *left, *right) : std::nullopt;
}

[[nodiscard]] bool
trackableArrayRoot(const MirPlaceAliasEscapeAnalysis &aliases,
                   MirPlaceId place) {
  const MirPlaceEscapeFact *escape = aliases.escape(place);
  return escape == nullptr || !escape->escapesBody();
}

[[nodiscard]] std::optional<IntegerFact>
arrayForOperand(const AbstractState &state,
                const MirPlaceAliasEscapeAnalysis &aliases,
                const MirOperand &operand) {
  if (operand.value != 0) {
    const auto value = state.arrayValues.find(operand.value);
    if (value != state.arrayValues.end()) {
      return value->second;
    }
  }
  if (operand.place != 0) {
    const auto elements =
        state.arrayElements.find(canonicalPlace(aliases, operand.place));
    if (elements != state.arrayElements.end()) {
      return elements->second;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<IntegerFact>
aggregateArrayFact(const MirBody &body, const AbstractState &state,
                   const MirInstruction &instruction) {
  if (!instruction.result || instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::Aggregate ||
      instruction.info.type.kind != SemanticType::Array ||
      instruction.info.type.arrayLength == 0 ||
      instruction.info.type.arguments.size() != 1) {
    return std::nullopt;
  }
  const std::optional<CheckedIntegerDomain> domain =
      constantIntegerDomain(instruction.info.type.arguments.front());
  if (!domain) {
    return std::nullopt;
  }
  Evidence evidence;
  addInstruction(evidence, instruction.id);
  addValue(evidence, *instruction.result);
  IntegerFact elements{.domain = *domain,
                       .lower = {},
                       .upper = {},
                       .evidence = std::move(evidence)};
  bool initialized = false;
  for (const MirOperand &operand : instruction.operands) {
    const std::optional<IntegerFact> fact =
        integerForOperand(body, state, operand);
    if (!fact || fact->domain != *domain) {
      return std::nullopt;
    }
    elements = initialized ? joinedInteger(elements, *fact, false) : *fact;
    initialized = true;
  }
  if (!initialized ||
      instruction.operands.size() < instruction.info.type.arrayLength) {
    IntegerFact zero{.domain = *domain, .lower = {}, .upper = {}};
    elements = initialized ? joinedInteger(elements, zero, false) : zero;
  }
  addInstruction(elements.evidence, instruction.id);
  addValue(elements.evidence, *instruction.result);
  return elements;
}

void invalidatePlaceRoot(const MirBody &body,
                         const MirPlaceAliasEscapeAnalysis &aliases,
                         AbstractState &state, MirPlaceId destination) {
  const MirPlace *written = body.findPlace(destination);
  if (written == nullptr) {
    std::erase_if(state.integerPlaces, [&](const auto &fact) {
      return !immutableScalarBinding(body, fact.first);
    });
    state.arrayElements.clear();
    state.aggregateElements.clear();
    state.aggregateElementIntegers.clear();
    state.objects.clear();
    state.loadedOrigins.clear();
    state.aggregateOrigins.clear();
    state.objectValues.clear();
    return;
  }
  const auto overlaps = [&](MirPlaceId place) {
    return aliases.relation(destination, place) != MirAliasRelation::NoAlias;
  };
  // Zero is an analysis-only implicit receiver root, not a MIR identity.
  // A projected This write invalidates overlapping field paths; other writes
  // have no alias proof for an unmaterialized root and conservatively kill it.
  std::optional<ObjectFact> implicitReceiver;
  const MirPlace receiver{.root = MirPlaceRootKind::This};
  if (const auto path = fieldPath(receiver, *written)) {
    if (const auto found = state.objects.find(0);
        found != state.objects.end()) {
      implicitReceiver = found->second;
      std::erase_if(implicitReceiver->integers, [&](const auto &field) {
        const auto prefix = std::min(path->size(), field.first.size());
        return std::equal(path->begin(), path->begin() + prefix,
                          field.first.begin());
      });
      implicitReceiver->elements.reset();
      implicitReceiver->elementIntegers.clear();
    }
  }
  std::erase_if(state.integerPlaces, [&](const auto &entry) {
    const auto relation = aliases.relation(destination, entry.first);
    return relation != MirAliasRelation::NoAlias &&
           (relation == MirAliasRelation::MustAlias ||
            !immutableScalarBinding(body, entry.first));
  });
  std::erase_if(state.arrayElements,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.aggregateElements,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.aggregateElementIntegers,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.objects,
                [&](const auto &entry) { return overlaps(entry.first); });
  if (implicitReceiver)
    state.objects.insert_or_assign(0, std::move(*implicitReceiver));
  std::erase_if(state.aggregateOrigins,
                [&](const auto &entry) { return overlaps(entry.second); });
  std::erase_if(state.loadedOrigins,
                [&](const auto &entry) { return overlaps(entry.second); });
}

[[nodiscard]] bool
initializesReferenceCarrier(const MirBody &body,
                            const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Initialize ||
      !instruction.destination || instruction.operands.size() != 1) {
    return false;
  }
  const MirPlace *destination = body.findPlace(*instruction.destination);
  const MirOperand &source = instruction.operands.front();
  if (destination == nullptr ||
      destination->root != MirPlaceRootKind::Binding ||
      destination->binding == 0 ||
      destination->type.kind != SemanticType::Reference ||
      source.kind != MirOperandKind::Loan || source.place == 0 ||
      source.loan == 0) {
    return false;
  }
  const MirLoan *loan = body.findLoan(source.loan);
  const MirPlace *address = body.findPlace(source.place);
  return loan != nullptr && address != nullptr && !loan->entry &&
         !loan->escapes &&
         (loan->kind == MirLoanKind::Local ||
          loan->kind == MirLoanKind::CallResult) &&
         loan->carriers.size() == 1 &&
         loan->carriers.front() == destination->binding &&
         ((address->root == MirPlaceRootKind::Loan &&
           address->loan == loan->id) ||
          loan->source == address->id);
}

struct FixedArrayIndex {
  MirPlaceId arrayPlace = 0;
  MirPlaceId elementPlace = 0;
  std::uint64_t length = 0;
  std::optional<IntegerFact> index;
  // A multidimensional element access folds every dimension into one place
  // and one bounds site, and the emitted text checks each dimension in turn.
  // The site is therefore dischargeable only when every earlier dimension is
  // also proven; an unresolved or unproven parent leaves this false.
  bool parentsProven = false;
};

[[nodiscard]] std::optional<IntegerFact>
fixedArrayDimensionIndex(const MirBody &body, const AbstractState &state,
                         MirPlaceId elementPlace,
                         const MirPlaceProjection &projection) {
  if (projection.constantIndex) {
    const std::optional<CheckedIntegerDomain> domain =
        constantIntegerDomain(SemanticType::UInt64);
    if (!domain) {
      return std::nullopt;
    }
    Evidence evidence;
    addPlace(evidence, elementPlace);
    return IntegerFact{.domain = *domain,
                       .lower = {.magnitude = *projection.constantIndex},
                       .upper = {.magnitude = *projection.constantIndex},
                       .evidence = std::move(evidence)};
  }
  if (projection.index == 0) {
    return std::nullopt;
  }
  return integerForValue(body, state, projection.index);
}

[[nodiscard]] bool fixedArrayDimensionWithin(const IntegerFact &index,
                                             std::uint64_t length) {
  return !normalizeCheckedInteger(index.lower).negative &&
         compare(index.upper, CheckedIntegerValue{.magnitude = length}) < 0;
}

// Walks the nonterminal Index dimensions above `element` and reports whether
// each one is provably in bounds of its own sibling array place.
[[nodiscard]] bool fixedArrayParentsProven(const MirBody &body,
                                           const AbstractState &state,
                                           MirPlaceId arrayPlace) {
  const MirPlace *parent = body.findPlace(arrayPlace);
  while (parent != nullptr && !parent->projections.empty() &&
         parent->projections.back().kind == MirProjectionKind::Index) {
    const MirPlace *grandparent = fixedArrayParentPlace(body, *parent);
    if (grandparent == nullptr) {
      return false;
    }
    const std::optional<IntegerFact> index = fixedArrayDimensionIndex(
        body, state, parent->id, parent->projections.back());
    if (!index ||
        !fixedArrayDimensionWithin(*index, grandparent->type.arrayLength)) {
      return false;
    }
    parent = grandparent;
  }
  return parent != nullptr;
}

[[nodiscard]] FixedArrayIndex
fixedArrayIndex(const MirBody &body, const AbstractState &state,
                const MirInstruction &instruction) {
  MirPlaceId projectedId = 0;
  if (instruction.kind == MirInstructionKind::Load && instruction.result &&
      instruction.operands.size() == 1) {
    projectedId = instruction.operands.front().place;
  } else if (instruction.kind == MirInstructionKind::Assign &&
             instruction.destination && instruction.operands.size() == 1) {
    projectedId = *instruction.destination;
  }
  if (projectedId == 0 || !isMirBlockParameterType(instruction.info.type) ||
      instruction.info.traits.drop != DropKind::Trivial ||
      instruction.info.traits.containsBorrowedState) {
    return {};
  }
  const FixedArrayProjection projected =
      fixedArrayProjection(body, projectedId);
  if (projected.arrayPlace == 0) {
    return {};
  }
  std::optional<IntegerFact> index =
      fixedArrayDimensionIndex(body, state, projectedId, projected.projection);
  return {.arrayPlace = projected.arrayPlace,
          .elementPlace = projectedId,
          .length = projected.length,
          .index = std::move(index),
          .parentsProven =
              fixedArrayParentsProven(body, state, projected.arrayPlace)};
}

[[nodiscard]] bool within(const IntegerFact &index,
                          const IntegerFact &capacity) {
  return !normalizeCheckedInteger(index.lower).negative &&
         !normalizeCheckedInteger(capacity.lower).negative &&
         compare(index.lower, CheckedIntegerValue{}) >= 0 &&
         compare(index.upper, capacity.lower) < 0;
}

[[nodiscard]] std::optional<MirBodyAddress>
closedCallTarget(const MirProgram &program, const MirInstruction &instruction) {
  if (instruction.dispatch != CallDispatch::Static ||
      instruction.nativeCallbackAdapter || instruction.threadTaskAdapter ||
      instruction.lambdaTarget) {
    return std::nullopt;
  }
  if (instruction.kind == MirInstructionKind::Call &&
      instruction.functionTarget) {
    const MirFunctionInstance *target =
        program.findFunctionInstance(*instruction.functionTarget);
    if (target == nullptr ||
        target->definitionKind != MirDefinitionKind::Source ||
        target->linkage != LanguageLinkage::Gti || target->virtualMethod ||
        target->pureVirtual) {
      return std::nullopt;
    }
    return MirBodyAddress{.kind = MirBodyKind::Function,
                          .owner = *instruction.functionTarget};
  }
  if (instruction.kind == MirInstructionKind::Construct &&
      instruction.constructorTarget) {
    const MirConstructorInstance *target =
        program.findConstructorInstance(*instruction.constructorTarget);
    if (target == nullptr ||
        target->definitionKind != MirDefinitionKind::Source) {
      return std::nullopt;
    }
    return MirBodyAddress{.kind = MirBodyKind::Constructor,
                          .owner = *instruction.constructorTarget};
  }
  return std::nullopt;
}

[[nodiscard]] bool owningCallOperand(const MirBody &body,
                                     const MirOperand &operand) {
  const MirInstruction *input = definingInstruction(body, operand.value);
  if (input == nullptr || input->kind != MirInstructionKind::CallInput) {
    return false;
  }
  const bool owningType = operand.type.kind == SemanticType::Class ||
                          operand.type.kind == SemanticType::UniqueOwner ||
                          operand.type.kind == SemanticType::SharedPointer ||
                          operand.type.kind == SemanticType::Expected;
  return owningType &&
         (input->callInputKind == HirCallInputKind::CopyValue ||
          input->callInputKind == HirCallInputKind::MoveValue ||
          input->callInputKind == HirCallInputKind::MoveBorrowedTemporary);
}

[[nodiscard]] ClosedCallContext
contextForCall(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
               const MirInstruction &instruction, const AbstractState &state) {
  ClosedCallContext result;
  result.reached = true;
  if (instruction.receiver) {
    result.receiver.observed = true;
    result.receiver.fact =
        objectForOperand(body, aliases, state, *instruction.receiver);
  }
  result.arguments.resize(instruction.operands.size());
  result.argumentObjects.resize(instruction.operands.size());
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    result.arguments[index].observed = true;
    result.arguments[index].fact =
        integerForOperand(body, state, instruction.operands[index]);
    result.argumentObjects[index].observed = true;
    result.argumentObjects[index].fact =
        objectForOperand(body, aliases, state, instruction.operands[index]);
  }
  return result;
}

void invalidateObjectOperand(const MirBody &body,
                             const MirPlaceAliasEscapeAnalysis &aliases,
                             const MirOperand &operand, AbstractState &state) {
  const MirPlaceId origin =
      objectOriginForOperand(body, aliases, state, operand);
  if (origin != 0) {
    invalidatePlaceRoot(body, aliases, state, origin);
  } else if (operand.value != 0) {
    state.objectValues.erase(operand.value);
  }
}

[[nodiscard]] bool parameterAllowsMutation(const SemanticType &type);
[[nodiscard]] std::optional<std::vector<MirPlaceId>>
operandProtectedPlaces(const MirBody &body, const MirOperand &operand);
[[nodiscard]] bool
operandMayAliasPlace(const MirBody &body,
                     const MirPlaceAliasEscapeAnalysis &aliases,
                     const MirOperand &operand, MirPlaceId tracked);
void killUnknownMemory(const MirBody &body, AbstractState &state);
void preserveLiveScalarFactsForConstruction(const MirProgram *program,
                                            MirBodyAddress address,
                                            const MirBody &body,
                                            AbstractState &state);

void invalidateClosedCallInputs(const MirProgram &program, const MirBody &body,
                                const MirPlaceAliasEscapeAnalysis &aliases,
                                const MirInstruction &instruction,
                                MirBodyAddress address, MirBodyAddress target,
                                bool opaqueEffects,
                                LiveMemoryBarrier liveMemoryBarrier,
                                AbstractState &state) {
  const auto summary = exactMirInvocationSummary(program, body, instruction);
  const bool scalarConstructionOnly =
      opaqueEffects &&
      liveMemoryBarrier == LiveMemoryBarrier::VacantScalarConstruction;
  if (!summary || (opaqueEffects && (summary->traits.maySynchronize ||
                                     (summary->traits.writesUnknownMemory &&
                                      !scalarConstructionOnly)))) {
    killUnknownMemory(body, state);
    return;
  }
  if (scalarConstructionOnly)
    preserveLiveScalarFactsForConstruction(&program, address, body, state);
  const auto *function = target.kind == MirBodyKind::Function
                             ? program.findFunctionInstance(target.owner)
                             : nullptr;
  const auto *constructor = target.kind == MirBodyKind::Constructor
                                ? program.findConstructorInstance(target.owner)
                                : nullptr;
  const auto *parameters = function      ? &function->parameterTypes
                           : constructor ? &constructor->parameterTypes
                                         : nullptr;
  if (parameters == nullptr ||
      parameters->size() != instruction.operands.size()) {
    killUnknownMemory(body, state);
    return;
  }

  // Scalar/object return summaries do not describe every post-call referent.
  // Invalidate the exact staged inputs first; independently proven object
  // summaries may subsequently republish their supported field facts. A shell
  // reference also protects the external owners in its stored dependency.
  const auto invalidate = [&](const MirOperand &operand) {
    const auto sources = operandProtectedPlaces(body, operand);
    if (!sources) {
      killUnknownMemory(body, state);
      return;
    }
    // MayAlias is not transitive: invalidate from actual sources, never from
    // all places which might alias a source and then expand aliasing again.
    for (const auto source : *sources)
      invalidatePlaceRoot(body, aliases, state, source);
  };
  if (summary->traits.writesPlace)
    for (const auto &place : body.places)
      if (place.root == MirPlaceRootKind::Symbol)
        invalidatePlaceRoot(body, aliases, state, place.id);
  if (function && instruction.receiver &&
      receiverAllowsMutation(function->receiverMutability) &&
      (summary->traits.writesPlace || summary->receiverEscapes ||
       function->receiverMutability == ReceiverMutability::Consuming))
    invalidate(*instruction.receiver);
  for (std::size_t index = 0; index < parameters->size(); ++index) {
    // Returning/forwarding a raw address does not overwrite another already
    // live object. Only the fully interpreted vacant-scalar profile may use
    // that contract; actual raw writes, destruction, or an unknown call mark
    // the successful path Unknown instead. Checked references and ordinary
    // receiver/global writes still invalidate their exact protected inputs.
    if (scalarConstructionOnly &&
        (*parameters)[index].kind == SemanticType::RawPointer)
      continue;
    const bool escapes = index >= summary->argumentsEscape.size() ||
                         summary->argumentsEscape[index];
    if (parameterAllowsMutation((*parameters)[index]) &&
        (summary->traits.writesPlace || escapes))
      invalidate(instruction.operands[index]);
  }
}

[[nodiscard]] bool
applyClosedCall(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
                const MirBlock &block, const MirInstruction &instruction,
                AbstractState &state, ClosedCallRound *closedCalls,
                MirBodyAddress address) {
  if (closedCalls == nullptr ||
      (instruction.kind != MirInstructionKind::Call &&
       instruction.kind != MirInstructionKind::Construct) ||
      instruction.intrinsic != IntrinsicKind::None) {
    return false;
  }
  const std::optional<MirBodyAddress> target =
      closedCallTarget(closedCalls->program, instruction);
  const bool owningArgument =
      std::any_of(instruction.operands.begin(), instruction.operands.end(),
                  [&](const MirOperand &operand) {
                    return owningCallOperand(body, operand);
                  });
  if (!target || closedCalls->rejected.contains(*target) || owningArgument) {
    // A permanently unsupported edge is an opaque transfer, not a pending
    // source summary. The caller still has an ordinary successful continuation
    // after the barrier below, although that edge supplies no result or
    // post-call memory facts. Only an admitted, not-yet-available source
    // summary postpones the continuation and retries the caller.
    return false;
  }

  ClosedCallContext discovered =
      contextForCall(body, aliases, instruction, state);
  closedCalls->discovered.emplace_back(*target, std::move(discovered));
  const auto found = closedCalls->summaries.find(*target);
  if (found == closedCalls->summaries.end() || !found->second.complete) {
    closedCalls->incompleteBodies.insert(address);
    // Do not turn a temporarily unavailable predecessor summary into an
    // observed top context for later calls. A subsequent program round
    // replays this body once the exact predecessor summary exists.
    state.reachable = false;
    return true;
  }
  const ClosedCallSummary &summary = found->second;
  const MirPlaceId receiverOrigin =
      instruction.receiver
          ? objectOriginForOperand(body, aliases, state, *instruction.receiver)
          : 0;
  std::vector<MirPlaceId> argumentOrigins;
  argumentOrigins.reserve(instruction.operands.size());
  for (const auto &operand : instruction.operands)
    argumentOrigins.push_back(
        objectOriginForOperand(body, aliases, state, operand));
  invalidateClosedCallInputs(closedCalls->program, body, aliases, instruction,
                             address, *target, summary.hasOpaqueEffects,
                             summary.liveMemoryBarrier, state);
  state.liveMemoryBarrier =
      std::max(state.liveMemoryBarrier, summary.liveMemoryBarrier);
  if (summary.hasOpaqueEffects) {
    // Opacity blocks recurrence invariance; it is not independent write
    // authority. The exact effect summary above governs caller invalidation.
    state.hasOpaqueEffects = true;
  }
  if (instruction.result) {
    if (summary.result.observed && summary.result.fact) {
      IntegerFact fact = *summary.result.fact;
      stabilize(fact);
      state.integers[*instruction.result] = rebased(
          std::move(fact), block.id, instruction.id, *instruction.result);
    } else {
      state.integers.erase(*instruction.result);
    }
    if (summary.constructed.observed && summary.constructed.fact) {
      ObjectFact object = *summary.constructed.fact;
      stabilize(object);
      state.objectValues[*instruction.result] = std::move(object);
    }
  }
  if (instruction.kind == MirInstructionKind::Construct &&
      instruction.destination && summary.constructed.observed &&
      summary.constructed.fact) {
    ObjectFact object = *summary.constructed.fact;
    stabilize(object);
    invalidatePlaceRoot(body, aliases, state, *instruction.destination);
    seedObjectFact(body, aliases, *instruction.destination, object, state);
  }
  if (instruction.loan && summary.result.observed) {
    for (const MirPlace &place : body.places) {
      if (place.root != MirPlaceRootKind::Loan ||
          place.loan != *instruction.loan) {
        continue;
      }
      const MirPlaceId destination = canonicalPlace(aliases, place.id);
      if (summary.result.fact) {
        IntegerFact fact = *summary.result.fact;
        stabilize(fact);
        state.integerPlaces[destination] =
            rebased(std::move(fact), block.id, instruction.id, 0, destination);
      } else {
        state.integerPlaces.erase(destination);
      }
    }
  }
  if (instruction.receiver && summary.receiver.observed) {
    if (summary.receiver.fact) {
      ObjectFact object = *summary.receiver.fact;
      stabilize(object);
      if (receiverOrigin != 0)
        seedObjectFact(body, aliases, receiverOrigin, object, state);
      else
        applyObjectToOperand(body, aliases, *instruction.receiver, object,
                             state);
    } else {
      invalidateObjectOperand(body, aliases, *instruction.receiver, state);
    }
  }
  const std::size_t count =
      std::min(instruction.operands.size(), summary.argumentObjects.size());
  for (std::size_t index = 0; index < count; ++index) {
    if (!summary.argumentObjects[index].observed) {
      continue;
    }
    if (summary.argumentObjects[index].fact) {
      ObjectFact object = *summary.argumentObjects[index].fact;
      stabilize(object);
      if (argumentOrigins[index] != 0)
        seedObjectFact(body, aliases, argumentOrigins[index], object, state);
      else
        applyObjectToOperand(body, aliases, instruction.operands[index], object,
                             state);
    } else {
      invalidateObjectOperand(body, aliases, instruction.operands[index],
                              state);
    }
  }
  return true;
}

void killUnknownMemory(const MirBody &body, AbstractState &state) {
  state.liveMemoryBarrier = LiveMemoryBarrier::Unknown;
  // Readonly scalar copies are actual immutable values, not readonly
  // references to mutable storage. Valid unknown/raw effects cannot change
  // them; exact reinitialization still invalidates their own place above.
  std::erase_if(state.integerPlaces, [&](const auto &fact) {
    return !immutableScalarBinding(body, fact.first);
  });
  state.arrayElements.clear();
  state.aggregateElements.clear();
  state.aggregateElementIntegers.clear();
  state.objects.clear();
  state.loadedOrigins.clear();
  state.aggregateOrigins.clear();
  state.objectValues.clear();
}

[[nodiscard]] bool inertRawAddressOperation(const MirInstruction &instruction) {
  if (instruction.rawMemoryAccess || !instruction.result ||
      instruction.destination || instruction.receiver)
    return false;
  const bool pointerComputation =
      instruction.kind == MirInstructionKind::Compute &&
      instruction.unsafeOperation == UnsafeOperationKind::PointerArithmetic &&
      (instruction.operation == MirOperation::PointerAdd ||
       instruction.operation == MirOperation::PointerSubtract ||
       instruction.operation == MirOperation::PointerDifference);
  const bool pointerConversion =
      instruction.kind == MirInstructionKind::Call &&
      instruction.intrinsic == IntrinsicKind::RawPointerConversion &&
      instruction.operands.size() == 1 &&
      instruction.operands.front().type.kind == SemanticType::RawPointer &&
      instruction.info.type.kind == SemanticType::RawPointer;
  const auto effect = effects(instruction);
  return (pointerComputation || pointerConversion) && !effect.writesPlace &&
         !effect.readsUnknownMemory && !effect.writesUnknownMemory &&
         !effect.invokesUserCode && !effect.invokesRuntime &&
         !effect.maySynchronize;
}

[[nodiscard]] bool inertLifecycleMarker(const MirInstruction &instruction) {
  return instruction.kind == MirInstructionKind::Lifecycle &&
         instruction.lifecycle.empty() && !instruction.rawMemoryAccess &&
         (instruction.fullExpressionEnd ||
          instruction.earlyExitFullExpressionEnd ||
          instruction.cleanupBoundaryEnd) &&
         !instruction.result && !instruction.destination &&
         !instruction.receiver && instruction.operands.empty() &&
         !instruction.loan && !instruction.functionTarget &&
         !instruction.constructorTarget && !instruction.bodyTarget &&
         !instruction.nativeCallbackAdapter && !instruction.threadTaskAdapter &&
         instruction.synchronization.kind ==
             SynchronizationOperationKind::None &&
         instruction.operation == MirOperation::None &&
         instruction.intrinsic == IntrinsicKind::None;
}

[[nodiscard]] const SemanticType *
implicitReceiverType(const MirProgram *program, MirBodyAddress address) {
  if (!program)
    return nullptr;
  HirClassInstanceId owner = 0;
  if (address.kind == MirBodyKind::Function) {
    const auto *function = program->findFunctionInstance(address.owner);
    owner = function && function->owner ? *function->owner : 0;
  } else if (address.kind == MirBodyKind::Constructor) {
    const auto *constructor = program->findConstructorInstance(address.owner);
    owner = constructor ? constructor->owner : 0;
  } else if (address.kind == MirBodyKind::FieldInitializers) {
    owner = address.owner;
  }
  const auto *record = program->findClassInstance(owner);
  return record ? &record->type : nullptr;
}

[[nodiscard]] const SemanticType *ordinaryFieldType(const MirProgram *program,
                                                    const SemanticType *type,
                                                    SymbolId field) {
  if (!program || !type || type->kind != SemanticType::Class)
    return nullptr;
  const auto record = std::find_if(
      program->classInstances().begin(), program->classInstances().end(),
      [&](const auto &candidate) { return candidate.type == *type; });
  if (record == program->classInstances().end() ||
      record->kind == ClassKind::Union ||
      record->compilerCapability != CompilerCapabilityTypeKind::None)
    return nullptr;
  const auto member = std::find_if(
      record->declaredFields.begin(), record->declaredFields.end(),
      [&](const auto &candidate) { return candidate.symbol == field; });
  return member == record->declaredFields.end() ? nullptr : &member->type;
}

[[nodiscard]] bool liveScalarPath(const MirProgram *program,
                                  MirBodyAddress address, const MirBody &body,
                                  MirPlaceId root,
                                  const FieldPath &suffix = {}) {
  const MirPlace *place = root ? body.findPlace(root) : nullptr;
  const SemanticType *type = nullptr;
  if (!root || (place && place->root == MirPlaceRootKind::This)) {
    type = implicitReceiverType(program, address);
  } else if (place && (place->root == MirPlaceRootKind::Binding ||
                       place->root == MirPlaceRootKind::Symbol ||
                       place->root == MirPlaceRootKind::Temporary)) {
    const auto base = std::find_if(body.places.begin(), body.places.end(),
                                   [&](const auto &candidate) {
                                     return samePlaceRoot(candidate, *place) &&
                                            candidate.projections.empty();
                                   });
    if (base != body.places.end())
      type = &base->type;
  }
  // Only the outer checked reference may be transparent. A reference stored
  // inside an object is a different live owner, not an owned field path.
  bool outerReference = type && type->kind == SemanticType::Reference &&
                        type->arguments.size() == 1;
  if (outerReference)
    type = &type->arguments.front();
  bool fieldSeen = false;
  if (place)
    for (const auto &projection : place->projections) {
      if (projection.kind == MirProjectionKind::Dereference && outerReference &&
          !fieldSeen) {
        outerReference = false;
        continue;
      }
      if (projection.kind != MirProjectionKind::Field)
        return false;
      fieldSeen = true;
      type = ordinaryFieldType(program, type, projection.field);
      if (!type)
        return false;
    }
  for (const auto field : suffix) {
    type = ordinaryFieldType(program, type, field);
    if (!type)
      return false;
  }
  const auto domain = type ? constantIntegerDomain(*type) : std::nullopt;
  return domain && domain->width > 8;
}

void preserveLiveScalarFactsForConstruction(const MirProgram *program,
                                            MirBodyAddress address,
                                            const MirBody &body,
                                            AbstractState &state) {
  // The sealed operation requires vacant, nonconflicting final storage. Its
  // scalar form invokes no constructor/user code, so it cannot overwrite an
  // already live non-byte scalar place in a valid program. Byte/array storage
  // may provide an object's backing representation and is not preserved.
  std::erase_if(state.integerPlaces, [&](const auto &entry) {
    return !liveScalarPath(program, address, body, entry.first);
  });
  for (auto &[root, object] : state.objects) {
    std::erase_if(object.integers, [&](const auto &entry) {
      return !liveScalarPath(program, address, body, root, entry.first);
    });
    object.elements.reset();
    object.elementIntegers.clear();
  }
  state.arrayElements.clear();
  state.aggregateElements.clear();
  state.aggregateElementIntegers.clear();
  state.loadedOrigins.clear();
  state.aggregateOrigins.clear();
  state.objectValues.clear();
  state.hasOpaqueEffects = true;
  state.liveMemoryBarrier = std::max(
      state.liveMemoryBarrier, LiveMemoryBarrier::VacantScalarConstruction);
}

void applyInstruction(const MirBody &body,
                      const MirPlaceAliasEscapeAnalysis &aliases,
                      const MirBlock &block, const MirInstruction &instruction,
                      AbstractState &state,
                      const PlaceThresholds *thresholds = nullptr,
                      ClosedCallRound *closedCalls = nullptr,
                      MirBodyAddress address = {}) {
  if (!state.reachable || instruction.tombstone) {
    return;
  }
  const MirProgram *program =
      closedCalls == nullptr ? nullptr : &closedCalls->program;
  const auto unknownEffectBarrier = [&] {
    killUnknownMemory(body, state);
    state.hasOpaqueEffects = true;
  };
  // The empty full-expression marker can inherit the source expression's
  // unsafe classification. It does not repeat that expression's operation;
  // its actual Load/Call/Compute instruction already carries the effects.
  if (inertLifecycleMarker(instruction))
    return;
  if (isMirScalarFinalAddressConstruction(instruction)) {
    preserveLiveScalarFactsForConstruction(program, address, body, state);
    return;
  }
  if (inertRawAddressOperation(instruction)) {
    // Address arithmetic/conversion neither observes nor modifies a pointee.
    // Preserve unrelated memory facts, but infer no address, extent, lifetime
    // or initialized-element fact from this operation.
    state.integers.erase(*instruction.result);
    state.arrayValues.erase(*instruction.result);
    state.objectValues.erase(*instruction.result);
    return;
  }
  if (instruction.unsafeOperation == UnsafeOperationKind::DeclaredUnsafeCall &&
      !instruction.rawMemoryAccess &&
      applyClosedCall(body, aliases, block, instruction, state, closedCalls,
                      address)) {
    // An unsafe declaration requires caller obligations; it does not replace
    // the exact source body's execution/effect authority. Its actual raw
    // operations still hit the per-instruction barrier in that body.
    return;
  }
  if (instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None) {
    // Contextual facts before raw access remain useful, but no raw operation
    // is interpreted as a scalar load/store or an effect-closed invocation.
    // Handle this before the ordinary Load/Assign transfer paths as well as
    // Compute. Immutable SSA facts established earlier remain immutable.
    unknownEffectBarrier();
    if (instruction.result) {
      state.integers.erase(*instruction.result);
      state.arrayValues.erase(*instruction.result);
      state.objectValues.erase(*instruction.result);
    }
    return;
  }
  if (instruction.kind == MirInstructionKind::Compute) {
    // Compute is a representation kind, not a purity promise. An extended
    // operation must not carry old place facts across newly classified memory,
    // lifecycle, or synchronization effects just because no scalar transfer
    // function recognizes it yet.
    const MirEffectTraits effect = effects(instruction);
    if (effect.writesPlace || effect.writesUnknownMemory ||
        effect.invokesUserCode || effect.maySynchronize) {
      unknownEffectBarrier();
      if (instruction.result) {
        state.integers.erase(*instruction.result);
        state.arrayValues.erase(*instruction.result);
      }
      return;
    }
    if (const std::optional<IntegerFact> fact =
            computeIntegerFact(body, state, instruction)) {
      state.integers[*instruction.result] = *fact;
    }
    if (const std::optional<IntegerFact> elements =
            aggregateArrayFact(body, state, instruction)) {
      state.arrayValues[*instruction.result] = *elements;
    }
    if (instruction.result && instruction.operation == MirOperation::Identity &&
        instruction.operands.size() == 1 &&
        instruction.operands.front().kind == MirOperandKind::Value) {
      const auto origin =
          state.loadedOrigins.find(instruction.operands.front().value);
      if (origin != state.loadedOrigins.end()) {
        state.loadedOrigins[*instruction.result] = origin->second;
      }
      const auto aggregateOrigin =
          state.aggregateOrigins.find(instruction.operands.front().value);
      if (aggregateOrigin != state.aggregateOrigins.end()) {
        state.aggregateOrigins[*instruction.result] = aggregateOrigin->second;
      }
      const auto object =
          state.objectValues.find(instruction.operands.front().value);
      if (object != state.objectValues.end()) {
        state.objectValues[*instruction.result] = object->second;
      }
    }
    return;
  }
  if (instruction.kind == MirInstructionKind::CallInput && instruction.result &&
      instruction.operands.size() == 1) {
    const MirOperand &source = instruction.operands.front();
    if (const std::optional<IntegerFact> fact =
            integerForOperand(body, state, source)) {
      state.integers[*instruction.result] = rebased(
          *fact, block.id, instruction.id, *instruction.result, source.place);
    }
    if (source.value != 0) {
      if (const auto loaded = state.loadedOrigins.find(source.value);
          loaded != state.loadedOrigins.end()) {
        state.loadedOrigins[*instruction.result] = loaded->second;
      }
    } else if (source.place != 0) {
      state.loadedOrigins[*instruction.result] =
          canonicalPlace(aliases, source.place);
    }
    if (const MirPlaceId origin =
            objectOriginForOperand(body, aliases, state, source);
        origin != 0) {
      state.aggregateOrigins[*instruction.result] = origin;
    }
    if (const std::optional<ObjectFact> object =
            objectForOperand(body, aliases, state, source)) {
      state.objectValues[*instruction.result] = *object;
    }
    return;
  }
  if (instruction.kind == MirInstructionKind::Load && instruction.result &&
      instruction.operands.size() == 1) {
    const MirOperand &source = instruction.operands.front();
    if (source.place != 0) {
      const MirPlaceId sourcePlace = canonicalPlace(aliases, source.place);
      const FixedArrayProjection projected =
          fixedArrayProjection(body, source.place);
      const MirPlaceId arrayPlace =
          canonicalPlace(aliases, projected.arrayPlace);
      const std::optional<IndexedAggregateAccess> indexed =
          indexedAggregateAccess(program, body, aliases, source.place);
      const auto arrayElements = state.arrayElements.find(arrayPlace);
      std::optional<IntegerFact> fact;
      if (indexed) {
        if (indexed->field.empty()) {
          if (const auto elements = state.aggregateElements.find(indexed->root);
              elements != state.aggregateElements.end()) {
            fact = elements->second;
          }
        } else if (const auto fields =
                       state.aggregateElementIntegers.find(indexed->root);
                   fields != state.aggregateElementIntegers.end()) {
          if (const auto element = fields->second.find(indexed->field);
              element != fields->second.end()) {
            fact = element->second;
          }
        }
      }
      if (!fact && projected.arrayPlace != 0 &&
          arrayElements != state.arrayElements.end()) {
        fact = arrayElements->second;
      }
      if (!fact) {
        fact = integerForPlace(body, state, sourcePlace);
      }
      if (fact) {
        IntegerFact loaded = *fact;
        add(loaded.evidence, block.id);
        addInstruction(loaded.evidence, instruction.id);
        addValue(loaded.evidence, *instruction.result);
        addPlace(loaded.evidence, source.place);
        addPlace(loaded.evidence, projected.arrayPlace);
        state.integers[*instruction.result] = std::move(loaded);
        state.loadedOrigins[*instruction.result] = sourcePlace;
      }
      if (const std::optional<IntegerFact> elements =
              arrayForOperand(state, aliases, source)) {
        IntegerFact loaded = *elements;
        add(loaded.evidence, block.id);
        addInstruction(loaded.evidence, instruction.id);
        addValue(loaded.evidence, *instruction.result);
        addPlace(loaded.evidence, source.place);
        state.arrayValues[*instruction.result] = std::move(loaded);
      }
      const MirPlaceId objectRoot = aggregateRoot(body, aliases, source.place);
      if (objectRoot != 0) {
        state.aggregateOrigins[*instruction.result] = objectRoot;
        if (const std::optional<ObjectFact> object =
                extractObjectFact(body, aliases, objectRoot, state)) {
          state.objectValues[*instruction.result] = *object;
        }
      }
    }
    return;
  }
  if ((instruction.kind == MirInstructionKind::Initialize ||
       instruction.kind == MirInstructionKind::Assign ||
       instruction.kind == MirInstructionKind::Move) &&
      instruction.destination && !instruction.operands.empty()) {
    const bool referenceCarrierInitialization =
        initializesReferenceCarrier(body, instruction);
    const MirPlaceId destination =
        canonicalPlace(aliases, *instruction.destination);
    const FixedArrayProjection projected =
        fixedArrayProjection(body, *instruction.destination);
    const MirPlaceId arrayPlace = canonicalPlace(aliases, projected.arrayPlace);
    const std::optional<IndexedAggregateAccess> indexed =
        indexedAggregateAccess(program, body, aliases,
                               *instruction.destination);
    const auto previousArray = state.arrayElements.find(arrayPlace);
    const std::optional<IntegerFact> previousElements =
        projected.arrayPlace != 0 && previousArray != state.arrayElements.end()
            ? std::optional<IntegerFact>{previousArray->second}
            : std::nullopt;
    const auto previousAggregate =
        indexed ? state.aggregateElements.find(indexed->root)
                : state.aggregateElements.end();
    const std::optional<IntegerFact> previousAggregateElements =
        indexed && indexed->field.empty() &&
                previousAggregate != state.aggregateElements.end()
            ? std::optional<IntegerFact>{previousAggregate->second}
            : std::nullopt;
    std::optional<IntegerFact> previousAggregateField;
    if (indexed && !indexed->field.empty()) {
      if (const auto fields =
              state.aggregateElementIntegers.find(indexed->root);
          fields != state.aggregateElementIntegers.end()) {
        if (const auto previous = fields->second.find(indexed->field);
            previous != fields->second.end()) {
          previousAggregateField = previous->second;
        }
      }
    }
    const MirOperand &source = instruction.operands.front();
    std::optional<IntegerFact> fact =
        source.place != 0
            ? integerForPlace(body, state,
                              canonicalPlace(aliases, source.place))
            : integerForOperand(body, state, source);
    if (compoundCheckedOperation(instruction.operation)) {
      fact = compoundIntegerFact(body, program, aliases, state, instruction);
    }
    const std::optional<IntegerFact> sourceArray =
        arrayForOperand(state, aliases, source);
    const std::optional<ObjectFact> sourceObject =
        objectForOperand(body, aliases, state, source);
    if (!referenceCarrierInitialization &&
        (!indexed || (!previousAggregateElements && !previousAggregateField))) {
      invalidatePlaceRoot(body, aliases, state, destination);
    }
    if (fact && projected.arrayPlace == 0) {
      IntegerFact stored = *fact;
      add(stored.evidence, block.id);
      addInstruction(stored.evidence, instruction.id);
      addPlace(stored.evidence, destination);
      const auto threshold = thresholds == nullptr
                                 ? PlaceThresholds::const_iterator{}
                                 : thresholds->find(destination);
      bool valid = true;
      if (thresholds != nullptr && threshold != thresholds->end()) {
        valid = intersect(stored, threshold->second.lower,
                          threshold->second.upper, threshold->second.evidence);
        if (valid) {
          stored.wideningEnvelope =
              std::pair{threshold->second.lower, threshold->second.upper};
        }
      }
      if (valid) {
        state.integerPlaces[destination] = std::move(stored);
      } else {
        state.integerPlaces.erase(destination);
      }
    } else {
      state.integerPlaces.erase(destination);
    }
    if (projected.arrayPlace != 0 && fact &&
        trackableArrayRoot(aliases, arrayPlace) &&
        (previousElements || projected.length == 1)) {
      IntegerFact elements = previousElements
                                 ? joinedElementFact(*previousElements, *fact)
                                 : *fact;
      add(elements.evidence, block.id);
      addInstruction(elements.evidence, instruction.id);
      addPlace(elements.evidence, projected.arrayPlace);
      addPlace(elements.evidence, projected.elementPlace);
      state.arrayElements[arrayPlace] = std::move(elements);
    } else if (const MirPlace *place = body.findPlace(destination);
               place != nullptr && place->type.kind == SemanticType::Array &&
               sourceArray && trackableArrayRoot(aliases, destination)) {
      IntegerFact elements = *sourceArray;
      add(elements.evidence, block.id);
      addInstruction(elements.evidence, instruction.id);
      addPlace(elements.evidence, destination);
      state.arrayElements[destination] = std::move(elements);
    }
    if (indexed && fact) {
      IntegerFact elements =
          indexed->field.empty()
              ? (previousAggregateElements
                     ? joinedElementFact(*previousAggregateElements, *fact)
                     : *fact)
              : (previousAggregateField
                     ? joinedElementFact(*previousAggregateField, *fact)
                     : *fact);
      add(elements.evidence, block.id);
      addInstruction(elements.evidence, instruction.id);
      addPlace(elements.evidence, indexed->root);
      addPlace(elements.evidence, *instruction.destination);
      ObjectFact &object = state.objects[indexed->root];
      if (indexed->field.empty()) {
        state.aggregateElements[indexed->root] = std::move(elements);
        object.elements = state.aggregateElements[indexed->root];
      } else {
        state.aggregateElementIntegers[indexed->root].insert_or_assign(
            indexed->field, std::move(elements));
        object.elementIntegers.insert_or_assign(
            indexed->field,
            state.aggregateElementIntegers[indexed->root].at(indexed->field));
      }
    }
    if (sourceObject) {
      seedObjectFact(body, aliases, destination, *sourceObject, state);
    }
    return;
  }
  if (instruction.kind == MirInstructionKind::Construct &&
      applyClosedCall(body, aliases, block, instruction, state, closedCalls,
                      address)) {
    return;
  }
  if (instruction.kind == MirInstructionKind::Drop && instruction.destination) {
    const MirEffectTraits effect = effects(instruction);
    if (effect.writesUnknownMemory || effect.invokesUserCode ||
        effect.maySynchronize) {
      unknownEffectBarrier();
    } else {
      invalidatePlaceRoot(body, aliases, state, *instruction.destination);
    }
    return;
  }
  if (instruction.kind != MirInstructionKind::Call) {
    const MirEffectTraits effect = effects(instruction);
    const bool opaque =
        instruction.rawMemoryAccess ||
        instruction.unsafeOperation != UnsafeOperationKind::None ||
        effect.writesUnknownMemory || effect.invokesUserCode ||
        effect.maySynchronize;
    if (opaque) {
      unknownEffectBarrier();
    } else if (effect.writesPlace) {
      // A default Initialize with no value operands still writes its exact
      // destination, but does not itself introduce unknown execution effects.
      killUnknownMemory(body, state);
    }
    return;
  }

  if ((instruction.intrinsic == IntrinsicKind::NumericTypeParameterConversion ||
       instruction.intrinsic == IntrinsicKind::NumericAliasConversion) &&
      instruction.result && instruction.operands.size() == 1) {
    const std::optional<IntegerFact> source =
        integerForOperand(body, state, instruction.operands.front());
    const std::optional<CheckedIntegerDomain> target =
        constantIntegerDomain(instruction.info.type);
    const std::optional<IntegerFact> converted =
        source && target ? convertedIntegerFact(*source, *target)
                         : std::nullopt;
    if (converted) {
      state.integers[*instruction.result] =
          rebased(*converted, block.id, instruction.id, *instruction.result);
    }
    return;
  }

  if (instruction.intrinsic == IntrinsicKind::StorageBoundsCheck &&
      instruction.info.type == SemanticType::Void && !instruction.result &&
      !instruction.receiver && instruction.operands.size() == 2 &&
      instruction.operands[0].type == SemanticType::UInt64 &&
      instruction.operands[1].type == SemanticType::UInt64) {
    // The general scalar bounds detector reads its staged integer inputs;
    // it neither mutates memory nor invokes source code. Its Invoke already
    // carries the exact failure/pre-instruction state. Do not turn this
    // primitive into an opaque memory barrier when a source owner uses it.
    return;
  }
  if (intrinsicBooleanFailureOutcome(instruction.intrinsic) &&
      instruction.info.type == SemanticType::Void && !instruction.result &&
      !instruction.receiver && instruction.operands.size() == 1 &&
      instruction.operands.front().type == SemanticType::Bool) {
    // These primitives test a source-computed condition and may fail; they
    // neither discover a container invariant nor perform a memory transfer.
    return;
  }

  if (applyClosedCall(body, aliases, block, instruction, state, closedCalls,
                      address)) {
    return;
  }

  // Calls outside the admitted closed-call vocabulary may mutate any
  // reachable place through a reference or opaque host boundary. Value facts
  // remain valid because MIR values are immutable.
  unknownEffectBarrier();
}

[[nodiscard]] BlockTransfer
transferBlock(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
              const MirBlock &block, AbstractState state,
              const PlaceThresholds *thresholds = nullptr,
              ClosedCallRound *closedCalls = nullptr,
              MirBodyAddress address = {}) {
  BlockTransfer result;
  for (const MirInstruction &instruction : block.instructions) {
    // Only Invoke's failure edge observes a pre-instruction state. Retaining
    // every intermediate state copies all scalar/place facts (and proof
    // evidence) for each instruction on every fixpoint iteration.
    if (block.terminator.kind == MirTerminatorKind::Invoke &&
        block.terminator.invokeInstruction == instruction.id) {
      result.before.insert_or_assign(instruction.id, state);
    }
    applyInstruction(body, aliases, block, instruction, state, thresholds,
                     closedCalls, address);
  }
  result.output = std::move(state);
  return result;
}

[[nodiscard]] IntegerFact joinedInteger(const IntegerFact &left,
                                        const IntegerFact &right, bool widen) {
  if (left.domain != right.domain) {
    return fullIntegerFact(left.domain, merged(left.evidence, right.evidence));
  }
  const bool lowerExpanded = compare(right.lower, left.lower) < 0;
  const bool upperExpanded = compare(right.upper, left.upper) > 0;
  const CheckedIntegerValue joinedLower =
      lowerExpanded ? right.lower : left.lower;
  const CheckedIntegerValue joinedUpper =
      upperExpanded ? right.upper : left.upper;
  const auto contains = [&](const auto &envelope, const IntegerFact &fact) {
    return compare(envelope.first, fact.lower) <= 0 &&
           compare(envelope.second, fact.upper) >= 0;
  };
  std::optional<std::pair<CheckedIntegerValue, CheckedIntegerValue>> envelope;
  if (left.wideningEnvelope && right.wideningEnvelope) {
    envelope = std::pair{compare(left.wideningEnvelope->first,
                                 right.wideningEnvelope->first) <= 0
                             ? left.wideningEnvelope->first
                             : right.wideningEnvelope->first,
                         compare(left.wideningEnvelope->second,
                                 right.wideningEnvelope->second) >= 0
                             ? left.wideningEnvelope->second
                             : right.wideningEnvelope->second};
  } else if (left.wideningEnvelope && contains(*left.wideningEnvelope, right)) {
    envelope = left.wideningEnvelope;
  } else if (right.wideningEnvelope &&
             contains(*right.wideningEnvelope, left)) {
    envelope = right.wideningEnvelope;
  }
  const bool envelopeContainsJoin =
      envelope && compare(envelope->first, joinedLower) <= 0 &&
      compare(envelope->second, joinedUpper) >= 0;
  return {.domain = left.domain,
          .lower = widen && lowerExpanded
                       ? (envelopeContainsJoin ? envelope->first
                                               : minimumValue(left.domain))
                       : (lowerExpanded ? right.lower : left.lower),
          .upper = widen && upperExpanded
                       ? (envelopeContainsJoin ? envelope->second
                                               : maximumValue(left.domain))
                       : (upperExpanded ? right.upper : left.upper),
          .wideningEnvelope = envelopeContainsJoin ? envelope : std::nullopt,
          .evidence = merged(left.evidence, right.evidence)};
}

template <typename Key>
void joinIntegers(std::map<Key, IntegerFact> &target,
                  const std::map<Key, IntegerFact> &incoming, bool widen) {
  for (auto iterator = target.begin(); iterator != target.end();) {
    const auto found = incoming.find(iterator->first);
    if (found == incoming.end()) {
      iterator = target.erase(iterator);
      continue;
    }
    iterator->second = joinedInteger(iterator->second, found->second, widen);
    ++iterator;
  }
}

void joinOptionalInteger(std::optional<IntegerFact> &target,
                         const std::optional<IntegerFact> &incoming,
                         bool widen) {
  if (!target || !incoming) {
    target.reset();
    return;
  }
  target = joinedInteger(*target, *incoming, widen);
}

void joinObjectFacts(std::map<MirPlaceId, ObjectFact> &target,
                     const std::map<MirPlaceId, ObjectFact> &incoming,
                     bool widen) {
  for (auto iterator = target.begin(); iterator != target.end();) {
    const auto found = incoming.find(iterator->first);
    if (found == incoming.end()) {
      iterator = target.erase(iterator);
      continue;
    }
    (void)mergeObject(iterator->second, found->second, widen);
    ++iterator;
  }
}

void joinAggregateElementIntegers(
    std::map<MirPlaceId, IntegerFieldFacts> &target,
    const std::map<MirPlaceId, IntegerFieldFacts> &incoming, bool widen) {
  for (auto iterator = target.begin(); iterator != target.end();) {
    const auto found = incoming.find(iterator->first);
    if (found == incoming.end()) {
      iterator = target.erase(iterator);
      continue;
    }
    joinIntegers(iterator->second, found->second, widen);
    ++iterator;
  }
}

[[nodiscard]] bool mergeInto(AbstractState &target,
                             const AbstractState &incoming, bool widen) {
  if (!incoming.reachable) {
    return false;
  }
  if (!target.reachable) {
    target = incoming;
    return true;
  }
  const AbstractState before = target;
  target.hasOpaqueEffects |= incoming.hasOpaqueEffects;
  target.liveMemoryBarrier =
      std::max(target.liveMemoryBarrier, incoming.liveMemoryBarrier);
  joinIntegers(target.integers, incoming.integers, widen);
  joinIntegers(target.integerPlaces, incoming.integerPlaces, widen);
  joinIntegers(target.arrayValues, incoming.arrayValues, widen);
  joinIntegers(target.arrayElements, incoming.arrayElements, widen);
  joinIntegers(target.aggregateElements, incoming.aggregateElements, widen);
  joinAggregateElementIntegers(target.aggregateElementIntegers,
                               incoming.aggregateElementIntegers, widen);
  joinObjectFacts(target.objects, incoming.objects, widen);
  for (auto iterator = target.loadedOrigins.begin();
       iterator != target.loadedOrigins.end();) {
    const auto found = incoming.loadedOrigins.find(iterator->first);
    if (found == incoming.loadedOrigins.end() ||
        found->second != iterator->second) {
      iterator = target.loadedOrigins.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (auto iterator = target.aggregateOrigins.begin();
       iterator != target.aggregateOrigins.end();) {
    const auto found = incoming.aggregateOrigins.find(iterator->first);
    if (found == incoming.aggregateOrigins.end() ||
        found->second != iterator->second) {
      iterator = target.aggregateOrigins.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (auto iterator = target.objectValues.begin();
       iterator != target.objectValues.end();) {
    const auto found = incoming.objectValues.find(iterator->first);
    if (found == incoming.objectValues.end() ||
        found->second != iterator->second) {
      iterator = target.objectValues.erase(iterator);
    } else {
      ++iterator;
    }
  }
  return target != before;
}

void bindEdgeArguments(const MirBody &body, MirBlockId target,
                       const std::vector<MirOperand> &arguments,
                       AbstractState &state) {
  const MirBlock *block = body.findBlock(target);
  if (block == nullptr || block->parameters.size() != arguments.size()) {
    return;
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const MirValueId parameter = block->parameters[index].value;
    const MirOperand &argument = arguments[index];
    if (const std::optional<IntegerFact> fact =
            integerForOperand(body, state, argument)) {
      IntegerFact bound = *fact;
      addValue(bound.evidence, parameter);
      state.integers[parameter] = std::move(bound);
    } else {
      state.integers.erase(parameter);
    }
    const auto loadedOrigin = argument.kind == MirOperandKind::Value
                                  ? state.loadedOrigins.find(argument.value)
                                  : state.loadedOrigins.end();
    if (loadedOrigin != state.loadedOrigins.end()) {
      state.loadedOrigins[parameter] = loadedOrigin->second;
    } else {
      state.loadedOrigins.erase(parameter);
    }
  }
}

struct Edge {
  MirBlockId target = 0;
  std::size_t ordinal = 0;
  AbstractState state;
};

[[nodiscard]] std::vector<Edge> outgoingEdges(const MirBody &body,
                                              const MirBlock &block,
                                              const BlockTransfer &transfer) {
  const MirTerminator &terminator = block.terminator;
  std::vector<Edge> result;
  const auto append = [&](MirBlockId target,
                          const std::vector<MirOperand> &arguments,
                          std::size_t ordinal, AbstractState state, bool refine,
                          bool branchTaken) {
    if (target == 0 || !state.reachable) {
      return;
    }
    if (refine) {
      refineBranch(body, block, state, branchTaken);
    }
    if (!state.reachable) {
      return;
    }
    bindEdgeArguments(body, target, arguments, state);
    result.push_back(
        {.target = target, .ordinal = ordinal, .state = std::move(state)});
  };
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    append(terminator.target, terminator.targetArguments, 0, transfer.output,
           false, false);
    break;
  case MirTerminatorKind::Branch:
    append(terminator.target, terminator.targetArguments, 0, transfer.output,
           true, true);
    append(terminator.elseTarget, terminator.elseArguments, 1, transfer.output,
           true, false);
    break;
  case MirTerminatorKind::Switch:
    append(terminator.target, terminator.targetArguments, 0, transfer.output,
           false, false);
    for (std::size_t index = 0; index < terminator.switchTargets.size();
         ++index) {
      const MirSwitchTarget &target = terminator.switchTargets[index];
      append(target.target, target.arguments, index + 1, transfer.output, false,
             false);
    }
    break;
  case MirTerminatorKind::Invoke: {
    AbstractState success = transfer.output;
    const auto detector =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const auto &instruction) {
                       return instruction.id == terminator.invokeInstruction;
                     });
    if (detector != block.instructions.end() &&
        intrinsicBooleanFailureOutcome(detector->intrinsic) &&
        detector->kind == MirInstructionKind::Call && !detector->result &&
        !detector->receiver && detector->info.type == SemanticType::Void &&
        detector->operands.size() == 1 &&
        detector->operands.front().type == SemanticType::Bool) {
      Evidence evidence;
      addInstruction(evidence, detector->id);
      refineCondition(body, block.id, detector->operands.front(), success, true,
                      std::move(evidence));
    }
    append(terminator.target, terminator.targetArguments, 0, std::move(success),
           false, false);
    const auto before = transfer.before.find(terminator.invokeInstruction);
    append(terminator.elseTarget, terminator.elseArguments, 1,
           before == transfer.before.end() ? transfer.output : before->second,
           false, false);
    break;
  }
  default:
    break;
  }
  return result;
}

struct InstructionSite {
  const MirBlock *block = nullptr;
  const MirInstruction *instruction = nullptr;
};

[[nodiscard]] InstructionSite definitionSite(const MirBody &body,
                                             MirValueId value) {
  const MirValue *mirValue = body.findValue(value);
  const MirBlock *block =
      mirValue == nullptr ? nullptr : body.findBlock(mirValue->definitionBlock);
  const MirInstruction *instruction = definingInstruction(body, value);
  return {.block = block, .instruction = instruction};
}

[[nodiscard]] std::optional<IntegerFact>
literalOperandFact(const MirBody &body, const MirOperand &operand) {
  if (operand.literal) {
    return literalIntegerFact(*operand.literal, operand.type);
  }
  if (operand.kind != MirOperandKind::Value || operand.value == 0) {
    return std::nullopt;
  }
  const MirInstruction *definition = definingInstruction(body, operand.value);
  if (definition == nullptr ||
      definition->kind != MirInstructionKind::Compute ||
      definition->operation != MirOperation::Literal || !definition->literal) {
    return std::nullopt;
  }
  return literalIntegerFact(*definition->literal, definition->info.type);
}

[[nodiscard]] std::optional<MirPlaceId>
loadedPlace(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
            const MirOperand &operand, std::size_t remaining = 4) {
  if (remaining == 0 || operand.kind != MirOperandKind::Value ||
      operand.value == 0) {
    return std::nullopt;
  }
  const MirInstruction *definition = definingInstruction(body, operand.value);
  if (definition == nullptr) {
    return std::nullopt;
  }
  if (definition->kind == MirInstructionKind::Load &&
      definition->operands.size() == 1 &&
      definition->operands.front().place != 0) {
    return canonicalPlace(aliases, definition->operands.front().place);
  }
  if (definition->kind == MirInstructionKind::Compute &&
      definition->operation == MirOperation::Identity &&
      definition->operands.size() == 1) {
    return loadedPlace(body, aliases, definition->operands.front(),
                       remaining - 1);
  }
  return std::nullopt;
}

[[nodiscard]] bool typeCarriesPlaceIdentity(const SemanticType &type) {
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

[[nodiscard]] bool parameterAllowsMutation(const SemanticType &type) {
  return (type.kind == SemanticType::Reference &&
          type.referenceAccess == AccessMode::Mutable) ||
         (type.kind == SemanticType::RawPointer &&
          type.pointerAccess == AccessMode::Mutable);
}

[[nodiscard]] std::optional<std::vector<MirPlaceId>>
operandProtectedPlaces(const MirBody &body, const MirOperand &operand) {
  const bool identityOperand = operand.kind == MirOperandKind::Address ||
                               operand.kind == MirOperandKind::BorrowRead ||
                               operand.kind == MirOperandKind::BorrowWrite ||
                               operand.kind == MirOperandKind::Loan ||
                               typeCarriesPlaceIdentity(operand.type);
  if (!identityOperand) {
    return std::vector<MirPlaceId>{};
  }
  std::vector<MirPlaceId> result;
  const auto retainLoan = [&](MirLoanId id) {
    const auto sources = mirLoanProtectedSources(body, id);
    if (sources.empty())
      return false;
    appendUnique(result, sources);
    return true;
  };
  const MirOperand *source = &operand;
  std::set<MirValueId> visited;
  for (;;) {
    if (source->loan != 0 && !retainLoan(source->loan))
      return std::nullopt;
    if (source->place != 0) {
      const auto *place = body.findPlace(source->place);
      if (place == nullptr)
        return std::nullopt;
      bool protectedReference = place->root == MirPlaceRootKind::Loan;
      if (place->root == MirPlaceRootKind::Loan && !retainLoan(place->loan))
        return std::nullopt;
      for (const auto &loan : body.loans) {
        if (place->root == MirPlaceRootKind::Binding &&
            std::find(loan.carriers.begin(), loan.carriers.end(),
                      place->binding) != loan.carriers.end()) {
          if (!retainLoan(loan.id))
            return std::nullopt;
          protectedReference |=
              !place->projections.empty() &&
              place->projections.front().kind == MirProjectionKind::Dereference;
        }
      }
      // A loan's complete protected sources already bound any projection
      // through that reference. Do not add its pointer spelling as a second,
      // unknown alias root. An actual carrier shell remains a distinct source.
      if (!protectedReference)
        appendUnique(result, std::vector<MirPlaceId>{place->id});
      return result;
    }
    if (source->value == 0)
      return result.empty() ? std::nullopt : std::optional{std::move(result)};
    if (!visited.insert(source->value).second)
      return std::nullopt;
    const auto *definition = definingInstruction(body, source->value);
    if (definition == nullptr)
      return std::nullopt;
    if (definition->loan)
      return retainLoan(*definition->loan) ? std::optional{std::move(result)}
                                           : std::nullopt;
    const bool forwardsSource =
        definition->kind == MirInstructionKind::CallInput ||
        definition->kind == MirInstructionKind::Load ||
        (definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Identity);
    if (!forwardsSource || definition->operands.size() != 1)
      return std::nullopt;
    // Staging changes the SSA identity, not its referent. Inspect both the
    // physical source and its stored owner dependency, including for Load.
    source = &definition->operands.front();
  }
}

[[nodiscard]] bool
operandMayAliasPlace(const MirBody &body,
                     const MirPlaceAliasEscapeAnalysis &aliases,
                     const MirOperand &operand, MirPlaceId tracked) {
  const auto sources = operandProtectedPlaces(body, operand);
  return !sources ||
         std::any_of(sources->begin(), sources->end(), [&](MirPlaceId source) {
           return aliases.relation(tracked, source) !=
                  MirAliasRelation::NoAlias;
         });
}

[[nodiscard]] bool
mayMutateTrackedPlace(const MirBody &body,
                      const MirPlaceAliasEscapeAnalysis &aliases,
                      const MirInstruction &instruction, MirPlaceId tracked,
                      ClosedCallRound *closedCalls) {
  const MirEffectTraits effect = effects(instruction);
  if (inertLifecycleMarker(instruction))
    return false;
  if ((instruction.rawMemoryAccess ||
       instruction.unsafeOperation != UnsafeOperationKind::None) &&
      !inertRawAddressOperation(instruction)) {
    return true;
  }
  if (instruction.kind == MirInstructionKind::Construct) {
    return true;
  }
  if (instruction.kind != MirInstructionKind::Call) {
    return effect.writesUnknownMemory || effect.invokesUserCode ||
           effect.maySynchronize;
  }
  if (closedCalls == nullptr || instruction.intrinsic != IntrinsicKind::None ||
      instruction.synchronization.kind != SynchronizationOperationKind::None) {
    return true;
  }
  const std::optional<MirBodyAddress> target =
      closedCallTarget(closedCalls->program, instruction);
  const auto summary = target ? closedCalls->summaries.find(*target)
                              : closedCalls->summaries.end();
  const std::optional<MirEffectSummary> effects =
      exactMirInvocationSummary(closedCalls->program, body, instruction);
  if (!target || closedCalls->rejected.contains(*target) ||
      summary == closedCalls->summaries.end() || !summary->second.complete ||
      summary->second.hasOpaqueEffects || !effects ||
      effects->traits.writesUnknownMemory || effects->traits.allocates ||
      effects->traits.invokesUserCode || effects->traits.targetDependent) {
    return true;
  }
  // Runtime effects conservatively carry maySynchronize even for the exact
  // prefix-length/read transfers interpreted by this analysis. Effect
  // closure above is a separate, successful-path proof: explicit atomic
  // operations reject the body, and every unsupported runtime effect marks
  // the summary opaque (including through joins and nested calls). Do not
  // replace that proof by treating every runtime-backed read as unknown.
  if (const auto *place = body.findPlace(tracked);
      place && place->root == MirPlaceRootKind::Symbol &&
      effects->traits.writesPlace)
    return true;
  if (target->kind != MirBodyKind::Function) {
    return true;
  }
  const MirFunctionInstance *function =
      closedCalls->program.findFunctionInstance(target->owner);
  if (function == nullptr ||
      function->parameterTypes.size() != instruction.operands.size()) {
    return true;
  }

  const auto aliasesTracked = [&](const MirOperand &operand) {
    return operandMayAliasPlace(body, aliases, operand, tracked);
  };
  if (instruction.receiver && aliasesTracked(*instruction.receiver) &&
      receiverAllowsMutation(function->receiverMutability) &&
      (effects->traits.writesPlace || effects->receiverEscapes ||
       function->receiverMutability == ReceiverMutability::Consuming)) {
    return true;
  }
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    const bool escapes = index >= effects->argumentsEscape.size() ||
                         effects->argumentsEscape[index];
    if (aliasesTracked(instruction.operands[index]) &&
        parameterAllowsMutation(function->parameterTypes[index]) &&
        (effects->traits.writesPlace || escapes)) {
      return true;
    }
  }
  return false;
}

struct FiniteLoopBound {
  MirPlaceId counter = 0;
  std::uint64_t maximumIterations = 0;
  Evidence evidence;
};

[[nodiscard]] std::optional<AbstractState> stateBeforeInstruction(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const std::vector<AbstractState> &incoming, InstructionSite site,
    ClosedCallRound *closedCalls, MirBodyAddress address);

[[nodiscard]] std::vector<std::optional<FiniteLoopBound>> finiteLoopBounds(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const MirDominanceInfo &dominance, const MirLoopAnalysis &loops,
    const std::vector<AbstractState> &incoming, ClosedCallRound *closedCalls,
    MirBodyAddress address) {
  std::vector<std::optional<FiniteLoopBound>> result(loops.loops.size());
  if (loops.hasIrreducibleControlFlow) {
    return result;
  }
  for (std::size_t loopIndex = 0; loopIndex < loops.loops.size(); ++loopIndex) {
    const MirNaturalLoop &loop = loops.loops[loopIndex];
    const MirBlock *header = body.findBlock(loop.header);
    const MirBlock *condition = nullptr;
    for (const MirBlock &candidate : body.blocks) {
      if (!loop.contains(candidate.id) ||
          candidate.terminator.kind != MirTerminatorKind::Branch ||
          !loop.contains(candidate.terminator.target) ||
          loop.contains(candidate.terminator.elseTarget)) {
        continue;
      }
      if (condition != nullptr) {
        condition = nullptr;
        break;
      }
      condition = &candidate;
    }
    if (header == nullptr || condition == nullptr || loop.latches.size() != 1 ||
        !condition->terminator.value ||
        condition->terminator.value->kind != MirOperandKind::Value ||
        !dominance.dominates(condition->id, loop.latches.front())) {
      continue;
    }
    const MirInstruction *comparison =
        definingInstruction(body, condition->terminator.value->value);
    if (comparison == nullptr ||
        comparison->kind != MirInstructionKind::Compute ||
        comparison->operands.size() != 2) {
      continue;
    }
    MirOperation relation = comparison->operation;
    const MirOperand *variable = &comparison->operands[0];
    const MirOperand *bound = &comparison->operands[1];
    std::optional<MirPlaceId> counter = loadedPlace(body, aliases, *variable);
    if (!counter) {
      variable = &comparison->operands[1];
      bound = &comparison->operands[0];
      counter = loadedPlace(body, aliases, *variable);
      relation = invertedComparison(relation);
    }
    const InstructionSite comparisonSite{.block = condition,
                                         .instruction = comparison};
    const std::optional<AbstractState> beforeComparison =
        stateBeforeInstruction(body, aliases, incoming, comparisonSite,
                               closedCalls, address);
    const std::optional<IntegerFact> boundFact =
        beforeComparison ? integerForOperand(body, *beforeComparison, *bound)
                         : std::nullopt;
    if (!counter || relation != MirOperation::Less || !boundFact ||
        normalizeCheckedInteger(boundFact->lower).negative) {
      continue;
    }
    const MirPlaceEscapeFact *escape = aliases.escape(*counter);
    if (escape != nullptr && escape->escapesBody()) {
      continue;
    }

    std::vector<InstructionSite> insideWrites;
    std::vector<InstructionSite> outsideWrites;
    bool opaque = false;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (loop.contains(block.id) &&
            mayMutateTrackedPlace(body, aliases, instruction, *counter,
                                  closedCalls)) {
          opaque = true;
        }
        if (!instruction.destination ||
            aliases.relation(*counter, *instruction.destination) ==
                MirAliasRelation::NoAlias) {
          continue;
        }
        (loop.contains(block.id) ? insideWrites : outsideWrites)
            .push_back({.block = &block, .instruction = &instruction});
      }
    }
    if (opaque || insideWrites.size() != 1 || outsideWrites.size() != 1) {
      continue;
    }
    const InstructionSite &initialization = outsideWrites.front();
    const InstructionSite &update = insideWrites.front();
    if (initialization.block == nullptr ||
        initialization.instruction == nullptr || update.block == nullptr ||
        update.instruction == nullptr ||
        !dominance.dominates(initialization.block->id, loop.header) ||
        !dominance.dominates(update.block->id, loop.latches.front()) ||
        (initialization.instruction->kind != MirInstructionKind::Initialize &&
         initialization.instruction->kind != MirInstructionKind::Assign) ||
        update.instruction->kind != MirInstructionKind::Assign ||
        initialization.instruction->operands.size() != 1 ||
        update.instruction->operands.size() != 1) {
      continue;
    }
    const std::optional<IntegerFact> initial =
        literalOperandFact(body, initialization.instruction->operands.front());
    const MirOperand &updatedValue = update.instruction->operands.front();
    const MirInstruction *addition =
        updatedValue.kind == MirOperandKind::Value
            ? definingInstruction(body, updatedValue.value)
            : nullptr;
    if (!initial || !singleton(*initial) ||
        normalizeCheckedInteger(initial->lower).negative ||
        initial->domain != boundFact->domain || addition == nullptr ||
        addition->kind != MirInstructionKind::Compute ||
        addition->operation != MirOperation::Add ||
        addition->operands.size() != 2) {
      continue;
    }
    const MirOperand *stepOperand = nullptr;
    if (loadedPlace(body, aliases, addition->operands[0]) == counter) {
      stepOperand = &addition->operands[1];
    } else if (loadedPlace(body, aliases, addition->operands[1]) == counter) {
      stepOperand = &addition->operands[0];
    }
    const std::optional<IntegerFact> step =
        stepOperand == nullptr ? std::nullopt
                               : literalOperandFact(body, *stepOperand);
    if (!step || !singleton(*step) || step->domain != initial->domain ||
        normalizeCheckedInteger(step->lower).negative ||
        step->lower.magnitude == 0) {
      continue;
    }
    const std::uint64_t initialValue = initial->lower.magnitude;
    const std::uint64_t boundValue = boundFact->upper.magnitude;
    const std::uint64_t stepValue = step->lower.magnitude;
    const std::uint64_t distance =
        initialValue >= boundValue ? 0 : boundValue - initialValue;
    const std::uint64_t maximumIterations =
        distance == 0 ? 0 : 1 + (distance - 1) / stepValue;
    Evidence evidence = merged(initial->evidence, boundFact->evidence);
    evidence = merged(std::move(evidence), step->evidence);
    add(evidence, loop.header);
    add(evidence, condition->id);
    add(evidence, loop.latches.front());
    addInstruction(evidence, comparison->id);
    addInstruction(evidence, initialization.instruction->id);
    addInstruction(evidence, addition->id);
    addInstruction(evidence, update.instruction->id);
    addPlace(evidence, *counter);
    result[loopIndex] = FiniteLoopBound{.counter = *counter,
                                        .maximumIterations = maximumIterations,
                                        .evidence = std::move(evidence)};
  }
  return result;
}

[[nodiscard]] std::optional<AbstractState> stateBeforeInstruction(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const std::vector<AbstractState> &incoming, InstructionSite site,
    ClosedCallRound *closedCalls, MirBodyAddress address) {
  if (site.block == nullptr || site.instruction == nullptr ||
      site.block->id == 0 || site.block->id > incoming.size() ||
      !incoming[site.block->id - 1].reachable) {
    return std::nullopt;
  }
  AbstractState state = incoming[site.block->id - 1];
  for (const MirInstruction &instruction : site.block->instructions) {
    if (instruction.id == site.instruction->id) {
      return state;
    }
    applyInstruction(body, aliases, *site.block, instruction, state, nullptr,
                     closedCalls, address);
  }
  return std::nullopt;
}

[[nodiscard]] PlaceThresholds recurrenceThresholds(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const MirDominanceInfo &dominance, const MirLoopAnalysis &loops,
    const std::vector<AbstractState> &incoming, ClosedCallRound *closedCalls,
    MirBodyAddress address) {
  PlaceThresholds result;
  if (loops.hasIrreducibleControlFlow) {
    return result;
  }
  const std::vector<std::optional<FiniteLoopBound>> bounds = finiteLoopBounds(
      body, aliases, dominance, loops, incoming, closedCalls, address);
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &assignment : block.instructions) {
      if (assignment.kind != MirInstructionKind::Assign ||
          !assignment.destination || assignment.operands.size() != 1) {
        continue;
      }
      const MirPlaceId destination =
          canonicalPlace(aliases, *assignment.destination);
      if (destination == 0 || result.contains(destination)) {
        continue;
      }
      const MirPlace *place = body.findPlace(destination);
      const std::optional<CheckedIntegerDomain> domain =
          place == nullptr ? std::nullopt : constantIntegerDomain(place->type);
      const MirOperand &source = assignment.operands.front();
      const InstructionSite addition = source.kind == MirOperandKind::Value
                                           ? definitionSite(body, source.value)
                                           : InstructionSite{};
      if (!domain || addition.instruction == nullptr ||
          addition.instruction->kind != MirInstructionKind::Compute ||
          addition.instruction->operation != MirOperation::Add ||
          addition.instruction->operands.size() != 2) {
        continue;
      }
      const MirOperand *deltaOperand = nullptr;
      if (loadedPlace(body, aliases, addition.instruction->operands[0]) ==
          std::optional<MirPlaceId>{destination}) {
        deltaOperand = &addition.instruction->operands[1];
      } else if (loadedPlace(body, aliases,
                             addition.instruction->operands[1]) ==
                 std::optional<MirPlaceId>{destination}) {
        deltaOperand = &addition.instruction->operands[0];
      }
      if (deltaOperand == nullptr) {
        continue;
      }

      std::optional<std::size_t> innerLoop;
      for (std::size_t index = 0; index < loops.loops.size(); ++index) {
        if (loops.loops[index].contains(block.id) && bounds[index] &&
            (!innerLoop ||
             loops.loops[index].depth > loops.loops[*innerLoop].depth)) {
          innerLoop = index;
        }
      }
      if (!innerLoop) {
        continue;
      }
      std::vector<std::size_t> containingLoops;
      bool exact = true;
      std::uint64_t iterations = 1;
      for (std::optional<std::size_t> index = innerLoop; index;
           index = loops.loops[*index].parent) {
        if (!bounds[*index] || loops.loops[*index].latches.size() != 1 ||
            (bounds[*index]->maximumIterations != 0 &&
             iterations > std::numeric_limits<std::uint64_t>::max() /
                              bounds[*index]->maximumIterations)) {
          exact = false;
          break;
        }
        iterations *= bounds[*index]->maximumIterations;
        containingLoops.push_back(*index);
      }
      if (!exact || containingLoops.empty()) {
        continue;
      }
      const MirNaturalLoop &outer = loops.loops[containingLoops.back()];
      const MirPlaceEscapeFact *escape = aliases.escape(destination);
      if (escape != nullptr && escape->escapesBody()) {
        continue;
      }

      InstructionSite initialization;
      bool invalidWrites = false;
      for (const MirBlock &candidateBlock : body.blocks) {
        for (const MirInstruction &candidate : candidateBlock.instructions) {
          if (outer.contains(candidateBlock.id) &&
              mayMutateTrackedPlace(body, aliases, candidate, destination,
                                    closedCalls)) {
            invalidWrites = true;
          }
          if (!candidate.destination ||
              aliases.relation(destination, *candidate.destination) ==
                  MirAliasRelation::NoAlias) {
            continue;
          }
          if (candidate.id == assignment.id) {
            continue;
          }
          if (outer.contains(candidateBlock.id) ||
              initialization.instruction != nullptr ||
              (candidate.kind != MirInstructionKind::Initialize &&
               candidate.kind != MirInstructionKind::Assign) ||
              candidate.operands.size() != 1 ||
              !dominance.dominates(candidateBlock.id, outer.header)) {
            invalidWrites = true;
            continue;
          }
          initialization = {.block = &candidateBlock,
                            .instruction = &candidate};
        }
      }
      if (invalidWrites || initialization.instruction == nullptr) {
        continue;
      }
      const std::optional<IntegerFact> initial = literalOperandFact(
          body, initialization.instruction->operands.front());
      const std::optional<AbstractState> beforeAddition =
          stateBeforeInstruction(body, aliases, incoming, addition, closedCalls,
                                 address);
      const std::optional<IntegerFact> delta =
          beforeAddition
              ? integerForOperand(body, *beforeAddition, *deltaOperand)
              : std::nullopt;
      if (!initial || !singleton(*initial) || initial->domain != *domain ||
          !delta || delta->domain != *domain ||
          !checkedIntegerFits(CheckedIntegerValue{.magnitude = iterations},
                              *domain)) {
        continue;
      }
      const CheckedIntegerValue zero;
      const CheckedIntegerValue lowerDelta =
          compare(delta->lower, zero) < 0 ? delta->lower : zero;
      const CheckedIntegerValue upperDelta =
          compare(delta->upper, zero) > 0 ? delta->upper : zero;
      const CheckedIntegerValue count{.magnitude = iterations};
      const std::optional<CheckedIntegerValue> lowerContribution =
          evaluatedValue(evaluateCheckedIntegerBinary(
              CheckedIntegerOperation::Multiply, lowerDelta, count, *domain));
      const std::optional<CheckedIntegerValue> upperContribution =
          evaluatedValue(evaluateCheckedIntegerBinary(
              CheckedIntegerOperation::Multiply, upperDelta, count, *domain));
      const std::optional<CheckedIntegerValue> lower =
          lowerContribution ? evaluatedValue(evaluateCheckedIntegerBinary(
                                  CheckedIntegerOperation::Add, initial->lower,
                                  *lowerContribution, *domain))
                            : std::nullopt;
      const std::optional<CheckedIntegerValue> upper =
          upperContribution ? evaluatedValue(evaluateCheckedIntegerBinary(
                                  CheckedIntegerOperation::Add, initial->upper,
                                  *upperContribution, *domain))
                            : std::nullopt;
      if (!lower || !upper || compare(*lower, *upper) > 0) {
        continue;
      }
      Evidence evidence = merged(initial->evidence, delta->evidence);
      add(evidence, initialization.block->id);
      add(evidence, block.id);
      addInstruction(evidence, initialization.instruction->id);
      addInstruction(evidence, addition.instruction->id);
      addInstruction(evidence, assignment.id);
      addPlace(evidence, destination);
      for (const std::size_t index : containingLoops) {
        evidence = merged(std::move(evidence), bounds[index]->evidence);
      }
      result[destination] =
          IntegerFact{.domain = *domain,
                      .lower = *lower,
                      .upper = *upper,
                      .wideningEnvelope = std::pair{*lower, *upper},
                      .evidence = std::move(evidence)};
    }
  }
  return result;
}

[[nodiscard]] std::vector<AbstractState>
solveStatesOnce(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
                const PlaceThresholds *thresholds = nullptr,
                const AbstractState *entryState = nullptr,
                ClosedCallRound *closedCalls = nullptr,
                MirBodyAddress address = {}) {
  AbstractState entry = entryState == nullptr ? AbstractState{} : *entryState;
  entry.reachable = true;
  const MirForwardDataflowResult<AbstractState> solved = runMirForwardDataflow(
      body, std::move(entry),
      [&](const MirBlock &block, const AbstractState &incoming) {
        const BlockTransfer transfer = transferBlock(
            body, aliases, block, incoming, thresholds, closedCalls, address);
        std::vector<MirForwardDataflowEdge<AbstractState>> result;
        for (Edge &edge : outgoingEdges(body, block, transfer)) {
          result.push_back({.target = edge.target,
                            .ordinal = edge.ordinal,
                            .state = std::move(edge.state)});
        }
        return result;
      },
      [](AbstractState &target, const AbstractState &incoming,
         MirDataflowMergeContext context) {
        return mergeInto(target, incoming, context.widen);
      });
  if (!solved.converged()) {
    return {};
  }
  std::vector<AbstractState> incoming(body.blocks.size());
  for (std::size_t index = 0; index < solved.incoming.size(); ++index) {
    if (solved.incoming[index]) {
      incoming[index] = *solved.incoming[index];
    }
  }
  return incoming;
}

[[nodiscard]] std::vector<AbstractState>
solveStates(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
            const AbstractState *entryState = nullptr,
            ClosedCallRound *closedCalls = nullptr,
            MirBodyAddress address = {}) {
  const std::vector<AbstractState> initial =
      solveStatesOnce(body, aliases, nullptr, entryState, closedCalls, address);
  if (initial.empty()) {
    return {};
  }
  const std::optional<MirDominanceInfo> dominance = computeMirDominance(body);
  if (!dominance) {
    return initial;
  }
  const MirLoopAnalysis loops = analyzeMirLoops(body, *dominance);
  const PlaceThresholds thresholds = recurrenceThresholds(
      body, aliases, *dominance, loops, initial, closedCalls, address);
  if (thresholds.empty()) {
    return initial;
  }
  return solveStatesOnce(body, aliases, &thresholds, entryState, closedCalls,
                         address);
}

[[nodiscard]] std::optional<std::size_t>
originFor(const MirInstruction &instruction, DefinedFailureOutcome outcome) {
  std::optional<std::size_t> result;
  for (std::size_t origin = 0;
       origin < instruction.definedFailure.localOrigins.size(); ++origin) {
    const auto &outcomes =
        instruction.definedFailure.localOrigins[origin].outcomes;
    if (std::find(outcomes.begin(), outcomes.end(), outcome) !=
        outcomes.end()) {
      if (result) {
        return std::nullopt;
      }
      result = origin;
    }
  }
  return result;
}

[[nodiscard]] std::optional<DefinedFailureOutcome>
uniqueOutcomeForCode(const MirInstruction &instruction,
                     DefinedFailureCode code) {
  std::optional<DefinedFailureOutcome> result;
  for (const DefinedFailureOrigin &origin :
       instruction.definedFailure.localOrigins) {
    for (const DefinedFailureOutcome outcome : origin.outcomes) {
      if (outcome.code != code) {
        continue;
      }
      if (result) {
        return std::nullopt;
      }
      result = outcome;
    }
  }
  return result;
}

void appendProof(MirSafetyAnalysis &analysis, MirBodyAddress address,
                 const MirBlock &block, const MirInstruction &instruction,
                 MirSafetyCheckFamily family, DefinedFailureOutcome outcome,
                 Evidence evidence) {
  const std::optional<std::size_t> origin = originFor(instruction, outcome);
  if (!origin) {
    return;
  }
  add(evidence, block.id);
  analysis.proofs.push_back(
      {.body = address,
       .block = block.id,
       .instruction = instruction.id,
       .discharge = {.family = family, .origin = *origin, .outcome = outcome},
       .relatedBlocks = std::move(evidence.blocks),
       .relatedInstructions = std::move(evidence.instructions),
       .relatedValues = std::move(evidence.values),
       .relatedPlaces = std::move(evidence.places)});
}

void collectProofs(MirBodyAddress address, const MirBody &body,
                   const MirPlaceAliasEscapeAnalysis &aliases,
                   const std::vector<AbstractState> &incoming,
                   MirSafetyAnalysis &analysis,
                   ClosedCallRound *closedCalls = nullptr) {
  const MirProgram *program =
      closedCalls == nullptr ? nullptr : &closedCalls->program;
  for (const MirBlock &block : body.blocks) {
    if (block.id == 0 || block.id > incoming.size() ||
        !incoming[block.id - 1].reachable) {
      continue;
    }
    AbstractState state = incoming[block.id - 1];
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone) {
        continue;
      }
      if (instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic == IntrinsicKind::StorageBoundsCheck &&
          instruction.operands.size() == 2) {
        const std::optional<IntegerFact> index =
            integerForOperand(body, state, instruction.operands[0]);
        const std::optional<IntegerFact> length =
            integerForOperand(body, state, instruction.operands[1]);
        const std::optional<DefinedFailureOutcome> outcome =
            uniqueOutcomeForCode(instruction,
                                 DefinedFailureCode::IndexOutOfBounds);
        if (index && length && outcome && within(*index, *length)) {
          appendProof(analysis, address, block, instruction,
                      MirSafetyCheckFamily::Bounds, *outcome,
                      merged(index->evidence, length->evidence));
        }
      }
      const FixedArrayIndex fixed = fixedArrayIndex(body, state, instruction);
      if (fixed.index && fixed.parentsProven &&
          fixedArrayDimensionWithin(*fixed.index, fixed.length)) {
        Evidence evidence = fixed.index->evidence;
        addPlace(evidence, fixed.arrayPlace);
        addPlace(evidence, fixed.elementPlace);
        appendProof(analysis, address, block, instruction,
                    MirSafetyCheckFamily::Bounds,
                    {.code = DefinedFailureCode::IndexOutOfBounds,
                     .detail = DefinedFailureDetail::FixedArray},
                    std::move(evidence));
      }

      if (instruction.kind == MirInstructionKind::Compute ||
          instruction.kind == MirInstructionKind::Assign ||
          instruction.kind == MirInstructionKind::Modify) {
        const std::optional<IntegerFact> result =
            instruction.kind == MirInstructionKind::Compute
                ? computeIntegerFact(body, state, instruction)
                : compoundIntegerFact(body, program, aliases, state,
                                      instruction);
        DefinedFailureDetail detail = DefinedFailureDetail::None;
        if (instruction.operation == MirOperation::Add ||
            instruction.operation == MirOperation::AddAssign) {
          detail = DefinedFailureDetail::Addition;
        } else if (instruction.operation == MirOperation::Subtract ||
                   instruction.operation == MirOperation::SubtractAssign) {
          detail = DefinedFailureDetail::Subtraction;
        } else if (instruction.operation == MirOperation::Multiply ||
                   instruction.operation == MirOperation::MultiplyAssign) {
          detail = DefinedFailureDetail::Multiplication;
        } else if (instruction.operation == MirOperation::Negate) {
          detail = DefinedFailureDetail::Negation;
        }
        if (result && detail != DefinedFailureDetail::None) {
          appendProof(
              analysis, address, block, instruction,
              MirSafetyCheckFamily::ArithmeticOverflow,
              {.code = DefinedFailureCode::IntegerOverflow, .detail = detail},
              result->evidence);
        }
        if (result && instruction.operation == MirOperation::Remainder) {
          appendProof(analysis, address, block, instruction,
                      MirSafetyCheckFamily::ArithmeticDomain,
                      {.code = DefinedFailureCode::ModuloByZero,
                       .detail = DefinedFailureDetail::IntegerModulo},
                      result->evidence);
        }
      }

      applyInstruction(body, aliases, block, instruction, state, nullptr,
                       closedCalls, address);
    }
  }
}

[[nodiscard]] bool proofLess(const MirSafetyProof &left,
                             const MirSafetyProof &right) {
  return std::tuple{left.block,
                    left.instruction,
                    static_cast<std::size_t>(left.discharge.family),
                    left.discharge.origin,
                    static_cast<std::size_t>(left.discharge.outcome.code),
                    static_cast<std::size_t>(left.discharge.outcome.detail),
                    left.relatedBlocks,
                    left.relatedInstructions,
                    left.relatedValues,
                    left.relatedPlaces} <
         std::tuple{right.block,
                    right.instruction,
                    static_cast<std::size_t>(right.discharge.family),
                    right.discharge.origin,
                    static_cast<std::size_t>(right.discharge.outcome.code),
                    static_cast<std::size_t>(right.discharge.outcome.detail),
                    right.relatedBlocks,
                    right.relatedInstructions,
                    right.relatedValues,
                    right.relatedPlaces};
}

[[nodiscard]] MirPlaceId rootForBinding(const MirBody &body,
                                        HirBindingId binding) {
  const MirPlace *best = nullptr;
  for (const MirPlace &place : body.places) {
    if (place.root != MirPlaceRootKind::Binding || place.binding != binding ||
        !std::all_of(place.projections.begin(), place.projections.end(),
                     [](const MirPlaceProjection &projection) {
                       return transparentRootProjection(projection.kind);
                     })) {
      continue;
    }
    if (best == nullptr ||
        place.projections.size() < best->projections.size()) {
      best = &place;
    }
  }
  return best == nullptr ? 0 : best->id;
}

[[nodiscard]] MirPlaceId thisRoot(const MirBody &body) {
  const auto found = std::find_if(
      body.places.begin(), body.places.end(), [](const MirPlace &place) {
        return place.root == MirPlaceRootKind::This &&
               std::all_of(place.projections.begin(), place.projections.end(),
                           [](const MirPlaceProjection &projection) {
                             return transparentRootProjection(projection.kind);
                           });
      });
  return found == body.places.end() ? 0 : found->id;
}

[[nodiscard]] std::optional<IndexedAggregateAccess>
indexedAggregateAccess(const MirProgram *program, const MirBody &body,
                       const MirPlaceAliasEscapeAnalysis &aliases,
                       MirPlaceId placeId) {
  const MirPlace *place = body.findPlace(placeId);
  if (program == nullptr || place == nullptr) {
    return std::nullopt;
  }

  const auto directIndex =
      std::find_if(place->projections.begin(), place->projections.end(),
                   [](const MirPlaceProjection &projection) {
                     return projection.kind == MirProjectionKind::Index ||
                            projection.kind == MirProjectionKind::RawIndex;
                   });
  if (directIndex != place->projections.end()) {
    if (!std::all_of(place->projections.begin(), directIndex,
                     [](const MirPlaceProjection &projection) {
                       return transparentRootProjection(projection.kind);
                     })) {
      return std::nullopt;
    }
    FieldPath field;
    for (auto projection = std::next(directIndex);
         projection != place->projections.end(); ++projection) {
      if (projection->kind != MirProjectionKind::Field ||
          projection->field == 0) {
        return std::nullopt;
      }
      field.push_back(projection->field);
    }
    const MirPlaceId root = aggregateRoot(body, aliases, placeId);
    return root == 0 ? std::nullopt
                     : std::optional<IndexedAggregateAccess>{
                           {.root = root, .field = std::move(field)}};
  }

  // Source-defined owners use general raw lifetime operations. No private
  // prefix-container accessor supplies an implicit element-value summary.
  return std::nullopt;
}

void sanitizeObject(const MirProgram &, const SemanticType &type,
                    ObjectFact &object) {
  const SemanticType &valueType =
      type.kind == SemanticType::Reference && type.arguments.size() == 1
          ? type.arguments.front()
          : type;
  if (valueType.kind != SemanticType::Array) {
    object.elements.reset();
    object.elementIntegers.clear();
  }
}
[[nodiscard]] bool mergeObject(ObjectFact &target, const ObjectFact &incoming,
                               bool widen) {
  const ObjectFact before = target;
  joinIntegers(target.integers, incoming.integers, widen);
  joinOptionalInteger(target.elements, incoming.elements, widen);
  joinIntegers(target.elementIntegers, incoming.elementIntegers, widen);
  return target != before;
}

template <typename T, typename Merge>
bool mergeObserved(ObservedFact<T> &target, const ObservedFact<T> &incoming,
                   Merge merge) {
  if (!incoming.observed) {
    return false;
  }
  if (!target.observed) {
    target = incoming;
    return true;
  }
  const ObservedFact<T> before = target;
  if (!target.fact || !incoming.fact) {
    target.fact.reset();
  } else {
    merge(*target.fact, *incoming.fact);
  }
  return target != before;
}

[[nodiscard]] bool mergeContext(ClosedCallContext &target,
                                const ClosedCallContext &incoming, bool widen) {
  if (!incoming.reached) {
    return false;
  }
  if (!target.reached) {
    target = incoming;
    return true;
  }
  const ClosedCallContext before = target;
  mergeObserved(target.receiver, incoming.receiver,
                [widen](ObjectFact &left, const ObjectFact &right) {
                  (void)mergeObject(left, right, widen);
                });
  const std::size_t argumentCount =
      std::max(target.arguments.size(), incoming.arguments.size());
  target.arguments.resize(argumentCount,
                          ObservedFact<IntegerFact>{.observed = true});
  target.argumentObjects.resize(argumentCount,
                                ObservedFact<ObjectFact>{.observed = true});
  for (std::size_t index = 0; index < argumentCount; ++index) {
    const ObservedFact<IntegerFact> incomingInteger =
        index < incoming.arguments.size()
            ? incoming.arguments[index]
            : ObservedFact<IntegerFact>{.observed = true};
    const ObservedFact<ObjectFact> incomingObject =
        index < incoming.argumentObjects.size()
            ? incoming.argumentObjects[index]
            : ObservedFact<ObjectFact>{.observed = true};
    mergeObserved(target.arguments[index], incomingInteger,
                  [widen](IntegerFact &left, const IntegerFact &right) {
                    left = joinedInteger(left, right, widen);
                  });
    mergeObserved(target.argumentObjects[index], incomingObject,
                  [widen](ObjectFact &left, const ObjectFact &right) {
                    (void)mergeObject(left, right, widen);
                  });
  }
  return target != before;
}

[[nodiscard]] std::vector<HirBindingId>
parameterBindings(const MirProgram &program, MirBodyAddress address) {
  if (address.kind == MirBodyKind::Function) {
    const MirFunctionInstance *function =
        program.findFunctionInstance(address.owner);
    return function == nullptr ? std::vector<HirBindingId>{}
                               : function->parameterBindings;
  }
  if (address.kind == MirBodyKind::Constructor) {
    const MirConstructorInstance *constructor =
        program.findConstructorInstance(address.owner);
    return constructor == nullptr ? std::vector<HirBindingId>{}
                                  : constructor->parameterBindings;
  }
  return {};
}

[[nodiscard]] std::vector<SemanticType>
parameterTypes(const MirProgram &program, MirBodyAddress address) {
  if (address.kind == MirBodyKind::Function) {
    const MirFunctionInstance *function =
        program.findFunctionInstance(address.owner);
    return function == nullptr ? std::vector<SemanticType>{}
                               : function->parameterTypes;
  }
  if (address.kind == MirBodyKind::Constructor) {
    const MirConstructorInstance *constructor =
        program.findConstructorInstance(address.owner);
    return constructor == nullptr ? std::vector<SemanticType>{}
                                  : constructor->parameterTypes;
  }
  return {};
}

[[nodiscard]] AbstractState
entryStateForContext(const MirProgram &program, MirBodyAddress address,
                     const MirBody &body,
                     const MirPlaceAliasEscapeAnalysis &aliases,
                     const ClosedCallContext &context) {
  AbstractState entry;
  entry.reachable = true;
  const std::vector<HirBindingId> bindings =
      parameterBindings(program, address);
  for (std::size_t index = 0;
       index < bindings.size() && index < context.arguments.size(); ++index) {
    const MirPlaceId root = rootForBinding(body, bindings[index]);
    if (root == 0) {
      continue;
    }
    if (context.arguments[index].observed && context.arguments[index].fact) {
      IntegerFact fact = *context.arguments[index].fact;
      stabilize(fact);
      entry.integerPlaces[canonicalPlace(aliases, root)] =
          rebased(std::move(fact), 0, 0, 0, root);
    }
    if (index < context.argumentObjects.size() &&
        context.argumentObjects[index].observed &&
        context.argumentObjects[index].fact) {
      ObjectFact object = *context.argumentObjects[index].fact;
      const std::vector<SemanticType> types = parameterTypes(program, address);
      if (index < types.size()) {
        sanitizeObject(program, types[index], object);
      }
      stabilize(object);
      seedObjectFact(body, aliases, root, object, entry);
    }
  }
  if (context.receiver.observed && context.receiver.fact) {
    const MirPlaceId root = thisRoot(body);
    ObjectFact object = *context.receiver.fact;
    if (address.kind == MirBodyKind::Function) {
      const MirFunctionInstance *function =
          program.findFunctionInstance(address.owner);
      const MirClassInstance *owner =
          function && function->owner
              ? program.findClassInstance(*function->owner)
              : nullptr;
      if (owner != nullptr) {
        sanitizeObject(program, owner->type, object);
      }
    }
    stabilize(object);
    seedObjectFact(body, aliases, root, object, entry);
  }
  return entry;
}

[[nodiscard]] ObservedFact<ObjectFact>
objectAtExits(const MirProgram &program, const MirBody &body,
              const MirPlaceAliasEscapeAnalysis &aliases,
              const std::vector<AbstractState> &exits, MirPlaceId root,
              const SemanticType &type) {
  ObservedFact<ObjectFact> result;
  result.observed = true;
  for (const AbstractState &state : exits) {
    std::optional<ObjectFact> object =
        extractObjectFact(body, aliases, root, state);
    if (object) {
      sanitizeObject(program, type, *object);
    }
    if (!result.fact) {
      if (!object) {
        result.fact.reset();
        return result;
      }
      result.fact = std::move(*object);
      continue;
    }
    if (!object) {
      result.fact.reset();
      return result;
    }
    (void)mergeObject(*result.fact, *object, false);
  }
  return result;
}

[[nodiscard]] FieldPath prefixedFieldPath(SymbolId field,
                                          const FieldPath &suffix = {}) {
  FieldPath result;
  result.reserve(suffix.size() + 1);
  result.push_back(field);
  result.insert(result.end(), suffix.begin(), suffix.end());
  return result;
}

[[nodiscard]] ObservedFact<ObjectFact> fieldInitializersAtExits(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const MirClassInstance &owner, const std::vector<AbstractState> &exits) {
  ObservedFact<ObjectFact> result{.observed = true};
  for (const AbstractState &state : exits) {
    ObjectFact object;
    for (const MirClassFieldInfo &field : owner.declaredFields) {
      const MirPlaceId fieldRoot = rootForBinding(body, field.field);
      if (fieldRoot == 0) {
        continue;
      }
      const MirPlaceId root = canonicalPlace(aliases, fieldRoot);
      if (const auto integer = state.integerPlaces.find(root);
          integer != state.integerPlaces.end()) {
        object.integers.insert_or_assign(FieldPath{field.symbol},
                                         integer->second);
      }
      if (const std::optional<ObjectFact> nested =
              extractObjectFact(body, aliases, root, state)) {
        for (const auto &[path, fact] : nested->integers) {
          object.integers.insert_or_assign(
              prefixedFieldPath(field.symbol, path), fact);
        }
      }
    }
    if (!result.fact) {
      result.fact = std::move(object);
    } else {
      (void)mergeObject(*result.fact, object, false);
    }
  }
  return result;
}

struct ContextualBodyResult {
  ClosedCallSummary summary;
  MirSafetyAnalysis safety;
};

[[nodiscard]] ContextualBodyResult
analyzeContextualBody(const MirProgram &program, MirBodyAddress address,
                      const ClosedCallContext &context,
                      ClosedCallRound &round) {
  ContextualBodyResult result;
  const MirBody *body = findMirBody(program, address);
  if (body == nullptr || !context.reached) {
    return result;
  }
  const MirPlaceAliasEscapeAnalysis aliases =
      analyzeMirPlaceAliasEscape(program, address);
  AbstractState entry =
      entryStateForContext(program, address, *body, aliases, context);
  std::optional<ObjectFact> initialFields;
  const MirClassInstance *constructorOwner = nullptr;
  if (address.kind == MirBodyKind::Constructor) {
    const MirConstructorInstance *constructor =
        program.findConstructorInstance(address.owner);
    constructorOwner = constructor == nullptr
                           ? nullptr
                           : program.findClassInstance(constructor->owner);
    if (constructorOwner != nullptr) {
      const MirBodyAddress initializerAddress{
          .kind = MirBodyKind::FieldInitializers,
          .owner = constructorOwner->id};
      const auto initializers = round.summaries.find(initializerAddress);
      if (initializers == round.summaries.end() ||
          !initializers->second.complete) {
        round.incompleteBodies.insert(address);
      } else {
        if (initializers->second.hasOpaqueEffects) {
          if (initializers->second.liveMemoryBarrier ==
              LiveMemoryBarrier::VacantScalarConstruction)
            preserveLiveScalarFactsForConstruction(&program, address, *body,
                                                   entry);
          else
            killUnknownMemory(*body, entry);
          entry.hasOpaqueEffects = true;
        }
        entry.liveMemoryBarrier = std::max(
            entry.liveMemoryBarrier, initializers->second.liveMemoryBarrier);
        if (initializers->second.constructed.observed) {
          initialFields = initializers->second.constructed.fact;
          const MirPlaceId root = thisRoot(*body);
          if (initialFields) {
            seedObjectFact(*body, aliases, root, *initialFields, entry);
          }
        }
      }
    }
  }
  const std::vector<AbstractState> incoming =
      solveStates(*body, aliases, &entry, &round, address);
  if (incoming.empty()) {
    return result;
  }
  collectProofs(address, *body, aliases, incoming, result.safety, &round);
  std::sort(result.safety.proofs.begin(), result.safety.proofs.end(),
            proofLess);
  result.safety.proofs.erase(
      std::unique(result.safety.proofs.begin(), result.safety.proofs.end()),
      result.safety.proofs.end());

  std::vector<AbstractState> exits;
  std::vector<std::optional<IntegerFact>> returnFacts;
  std::vector<std::optional<ObjectFact>> returnObjects;
  for (const MirBlock &block : body->blocks) {
    const bool successfulExit =
        block.terminator.kind == MirTerminatorKind::Return ||
        ((address.kind == MirBodyKind::FieldInitializers ||
          address.kind == MirBodyKind::Constructor) &&
         block.terminator.kind == MirTerminatorKind::Exit);
    if (block.id == 0 || block.id > incoming.size() ||
        !incoming[block.id - 1].reachable || !successfulExit) {
      continue;
    }
    BlockTransfer transferred =
        transferBlock(*body, aliases, block, incoming[block.id - 1], nullptr,
                      &round, address);
    if (!transferred.output.reachable) {
      continue;
    }
    if (block.terminator.kind == MirTerminatorKind::Return &&
        block.terminator.value) {
      std::optional<IntegerFact> fact =
          integerForOperand(*body, transferred.output, *block.terminator.value);
      returnFacts.push_back(std::move(fact));
      if (body->returnType.kind == SemanticType::Class) {
        auto object = objectForOperand(*body, aliases, transferred.output,
                                       *block.terminator.value);
        if (object)
          sanitizeObject(program, body->returnType, *object);
        returnObjects.push_back(std::move(object));
      }
    }
    exits.push_back(std::move(transferred.output));
  }
  if (exits.empty() || round.incompleteBodies.contains(address)) {
    return result;
  }
  result.summary.complete = true;
  result.summary.hasOpaqueEffects =
      std::any_of(exits.begin(), exits.end(), [](const AbstractState &state) {
        return state.hasOpaqueEffects;
      });
  for (const auto &state : exits)
    result.summary.liveMemoryBarrier =
        std::max(result.summary.liveMemoryBarrier, state.liveMemoryBarrier);

  if (!returnFacts.empty()) {
    result.summary.result.observed = true;
    for (const std::optional<IntegerFact> &fact : returnFacts) {
      if (!fact) {
        result.summary.result.fact.reset();
        break;
      }
      result.summary.result.fact =
          result.summary.result.fact
              ? std::optional<IntegerFact>{joinedInteger(
                    *result.summary.result.fact, *fact, false)}
              : fact;
    }
  }
  if (!returnObjects.empty()) {
    result.summary.constructed.observed = true;
    for (const auto &object : returnObjects) {
      if (!object) {
        result.summary.constructed.fact.reset();
        break;
      }
      if (!result.summary.constructed.fact)
        result.summary.constructed.fact = *object;
      else
        (void)mergeObject(*result.summary.constructed.fact, *object, false);
    }
  }

  const std::vector<HirBindingId> bindings =
      parameterBindings(program, address);
  const std::vector<SemanticType> types = parameterTypes(program, address);
  result.summary.argumentObjects.resize(bindings.size());
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (index >= context.argumentObjects.size() ||
        !context.argumentObjects[index].observed) {
      continue;
    }
    const MirPlaceId root = rootForBinding(*body, bindings[index]);
    if (root != 0 && index < types.size()) {
      result.summary.argumentObjects[index] =
          objectAtExits(program, *body, aliases, exits, root, types[index]);
    }
  }
  if (context.receiver.observed) {
    const MirFunctionInstance *function =
        address.kind == MirBodyKind::Function
            ? program.findFunctionInstance(address.owner)
            : nullptr;
    const MirClassInstance *owner =
        function && function->owner
            ? program.findClassInstance(*function->owner)
            : nullptr;
    const MirPlaceId root = thisRoot(*body);
    if (owner != nullptr) {
      result.summary.receiver =
          objectAtExits(program, *body, aliases, exits, root, owner->type);
    }
  }
  if (address.kind == MirBodyKind::FieldInitializers) {
    const MirClassInstance *owner = program.findClassInstance(address.owner);
    if (owner != nullptr) {
      result.summary.constructed =
          fieldInitializersAtExits(*body, aliases, *owner, exits);
    }
  } else if (address.kind == MirBodyKind::Constructor) {
    const MirPlaceId root = thisRoot(*body);
    if (constructorOwner != nullptr) {
      result.summary.constructed = objectAtExits(program, *body, aliases, exits,
                                                 root, constructorOwner->type);
    }
  }
  return result;
}

[[nodiscard]] std::set<MirBodyAddress, MirBodyAddressLess>
rejectedClosedCallBodies(const MirProgram &program) {
  std::set<MirBodyAddress, MirBodyAddressLess> rejected;
  std::map<MirBodyAddress, std::vector<MirBodyAddress>, MirBodyAddressLess>
      graph;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    if (address.kind != MirBodyKind::Function &&
        address.kind != MirBodyKind::Constructor) {
      continue;
    }
    if (address.kind == MirBodyKind::Function) {
      const MirFunctionInstance *function =
          program.findFunctionInstance(address.owner);
      if (function == nullptr ||
          function->definitionKind != MirDefinitionKind::Source ||
          function->linkage != LanguageLinkage::Gti ||
          function->virtualMethod || function->pureVirtual) {
        rejected.insert(address);
      }
    }
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      rejected.insert(address);
      continue;
    }
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.synchronization.kind !=
                SynchronizationOperationKind::None ||
            instruction.nativeCallbackAdapter ||
            instruction.threadTaskAdapter) {
          rejected.insert(address);
        }
        if (const std::optional<MirBodyAddress> target =
                closedCallTarget(program, instruction)) {
          graph[address].push_back(*target);
        } else if (instruction.constructorTarget) {
          const MirConstructorInstance *target =
              program.findConstructorInstance(*instruction.constructorTarget);
          if (instruction.dispatch == CallDispatch::Static &&
              !instruction.nativeCallbackAdapter &&
              !instruction.threadTaskAdapter && !instruction.lambdaTarget &&
              target != nullptr &&
              target->definitionKind == MirDefinitionKind::Source) {
            graph[address].push_back({.kind = MirBodyKind::Constructor,
                                      .owner = *instruction.constructorTarget});
          }
        }
      }
    }
  }
  for (const MirNativeCallbackAdapter &adapter :
       program.nativeCallbackAdapters()) {
    rejected.insert({.kind = MirBodyKind::Function, .owner = adapter.target});
  }
  for (const MirThreadTaskAdapter &adapter : program.threadTaskAdapters()) {
    if (adapter.functionTarget) {
      rejected.insert(
          {.kind = MirBodyKind::Function, .owner = *adapter.functionTarget});
    }
  }

  std::map<MirBodyAddress, int, MirBodyAddressLess> color;
  std::vector<MirBodyAddress> stack;
  const auto visit = [&](const auto &self, MirBodyAddress address) -> void {
    color[address] = 1;
    stack.push_back(address);
    for (const MirBodyAddress target : graph[address]) {
      if (color[target] == 0) {
        self(self, target);
      } else if (color[target] == 1) {
        const auto first = std::find(stack.begin(), stack.end(), target);
        rejected.insert(first, stack.end());
      }
    }
    stack.pop_back();
    color[address] = 2;
  };
  for (const auto &[address, edges] : graph) {
    (void)edges;
    if (color[address] == 0) {
      visit(visit, address);
    }
  }
  return rejected;
}

[[nodiscard]] std::map<MirBodyAddress, ClosedCallContext, MirBodyAddressLess>
rootClosedCallContexts(const MirProgram &program) {
  std::map<MirBodyAddress, ClosedCallContext, MirBodyAddressLess> result;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    bool root = address.kind == MirBodyKind::Module ||
                address.kind == MirBodyKind::FieldInitializers ||
                address.kind == MirBodyKind::StaticFieldInitializers ||
                address.kind == MirBodyKind::HostedStartup;
    if (address.kind == MirBodyKind::Function) {
      const MirFunctionInstance *function =
          program.findFunctionInstance(address.owner);
      root =
          function != nullptr && function->entryKind != ProgramEntryKind::None;
    }
    if (root) {
      result[address].reached = true;
    }
  }
  return result;
}

[[nodiscard]] MirSafetyAnalysis
analyzeBodyLocalSafety(const MirProgram &program, MirBodyAddress address,
                       const MirPlaceAliasEscapeAnalysis &aliases) {
  MirSafetyAnalysis result;
  const MirBody *body = findMirBody(program, address);
  if (body == nullptr || !verifyMirOptimizationVocabulary(*body).valid()) {
    return result;
  }
  const std::vector<AbstractState> incoming = solveStates(*body, aliases);
  if (incoming.empty()) {
    return result;
  }
  collectProofs(address, *body, aliases, incoming, result);
  std::sort(result.proofs.begin(), result.proofs.end(), proofLess);
  result.proofs.erase(std::unique(result.proofs.begin(), result.proofs.end()),
                      result.proofs.end());
  return result;
}

} // namespace

const MirSafetyProof *
MirSafetyAnalysis::find(MirInstructionId instruction,
                        MirSafetyDischarge discharge) const {
  const auto found = std::find_if(
      proofs.begin(), proofs.end(), [&](const MirSafetyProof &proof) {
        return proof.instruction == instruction && proof.discharge == discharge;
      });
  return found == proofs.end() ? nullptr : &*found;
}

const MirClosedCallBodyAnalysis *
MirClosedCallSafetyAnalysis::find(MirBodyAddress body) const {
  const auto found =
      std::find_if(bodies.begin(), bodies.end(),
                   [&](const auto &analysis) { return analysis.body == body; });
  return found == bodies.end() ? nullptr : &*found;
}

MirClosedCallSafetyAnalysis
analyzeMirClosedCallSafetyChecks(const MirProgram &program) {
  constexpr std::size_t kMaximumBodyAnalyses = 32;
  constexpr std::size_t kWidenAfterContextUpdates = 4;
  constexpr std::size_t kMaximumContextUpdates = 8;

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program);
  for (const MirBodyAddress address : addresses) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr || !verifyMirOptimizationVocabulary(*body).valid()) {
      return {};
    }
  }
  std::set<MirBodyAddress, MirBodyAddressLess> rejected =
      rejectedClosedCallBodies(program);
  std::map<MirBodyAddress, std::size_t, MirBodyAddressLess> contextUpdates;
  std::map<MirBodyAddress, ClosedCallContext, MirBodyAddressLess> contexts =
      rootClosedCallContexts(program);
  std::map<MirBodyAddress, ClosedCallSummary, MirBodyAddressLess> summaries;
  std::map<MirBodyAddress, MirSafetyAnalysis, MirBodyAddressLess>
      contextualSafety;
  std::map<MirBodyAddress, std::set<MirBodyAddress, MirBodyAddressLess>,
           MirBodyAddressLess>
      dependents;
  for (const auto address : addresses) {
    for (const auto &block : findMirBody(program, address)->blocks)
      for (const auto &instruction : block.instructions)
        if (const auto target = closedCallTarget(program, instruction))
          dependents[*target].insert(address);
    if (address.kind == MirBodyKind::Constructor) {
      const auto *constructor = program.findConstructorInstance(address.owner);
      if (constructor)
        dependents[{.kind = MirBodyKind::FieldInitializers,
                    .owner = constructor->owner}]
            .insert(address);
    }
  }

  std::deque<MirBodyAddress> work;
  std::set<MirBodyAddress, MirBodyAddressLess> pending, changedContexts;
  std::map<MirBodyAddress, std::size_t, MirBodyAddressLess> analyses;
  const auto enqueue = [&](MirBodyAddress address) {
    const auto context = contexts.find(address);
    if (!rejected.contains(address) && context != contexts.end() &&
        context->second.reached && pending.insert(address).second)
      work.push_back(address);
  };
  for (const auto &[address, context] : contexts) {
    (void)context;
    enqueue(address);
  }

  MirClosedCallSafetyAnalysis result;
  while (!work.empty()) {
    const auto address = work.front();
    work.pop_front();
    pending.erase(address);
    if (rejected.contains(address))
      continue;
    const bool updatedContext = changedContexts.erase(address) != 0;
    // Count coalesced context changes only when the callee consumes them.
    // A growing program must not exhaust a global round budget merely to
    // propagate fixed facts along a longer, otherwise ordinary source chain.
    if (analyses[address] == kMaximumBodyAnalyses ||
        (updatedContext &&
         ++contextUpdates[address] > kMaximumContextUpdates)) {
      rejected.insert(address);
      contexts.erase(address);
      summaries.erase(address);
      contextualSafety.erase(address);
      for (const auto caller : dependents[address])
        enqueue(caller);
      continue;
    }
    result.iterations = std::max(result.iterations, ++analyses[address]);
    ClosedCallRound round{
        .program = program, .summaries = summaries, .rejected = rejected};
    auto body =
        analyzeContextualBody(program, address, contexts.at(address), round);
    contextualSafety.insert_or_assign(address, std::move(body.safety));
    const auto previous = summaries.find(address);
    const bool changed =
        body.summary.complete
            ? previous == summaries.end() || previous->second != body.summary
            : previous != summaries.end();
    if (body.summary.complete)
      summaries.insert_or_assign(address, std::move(body.summary));
    else
      summaries.erase(address);
    if (changed)
      for (const auto caller : dependents[address])
        enqueue(caller);

    std::stable_sort(round.discovered.begin(), round.discovered.end(),
                     [](const auto &left, const auto &right) {
                       return MirBodyAddressLess{}(left.first, right.first);
                     });
    for (const auto &[target, context] : round.discovered)
      if (!rejected.contains(target) &&
          mergeContext(contexts[target], context,
                       contextUpdates[target] >= kWidenAfterContextUpdates)) {
        changedContexts.insert(target);
        enqueue(target);
      }
  }
  result.converged = true;

  result.bodies.reserve(addresses.size());
  for (const MirBodyAddress address : addresses) {
    MirClosedCallBodyAnalysis body;
    body.body = address;
    if (const auto context = contexts.find(address);
        context != contexts.end()) {
      body.reached = context->second.reached;
    }
    const auto summary = summaries.find(address);
    body.closed = result.converged && !rejected.contains(address) &&
                  summary != summaries.end() && summary->second.complete &&
                  !summary->second.hasOpaqueEffects;
    body.safety = analyzeBodyLocalSafety(
        program, address, analyzeMirPlaceAliasEscape(program, address));
    if (result.converged) {
      if (const auto contextual = contextualSafety.find(address);
          contextual != contextualSafety.end()) {
        body.safety.proofs.insert(body.safety.proofs.end(),
                                  contextual->second.proofs.begin(),
                                  contextual->second.proofs.end());
      }
    }
    if (body.closed) {
      if (summary->second.result.observed && summary->second.result.fact) {
        const IntegerFact &interval = *summary->second.result.fact;
        body.returnInterval =
            MirClosedCallIntegerInterval{.domain = interval.domain,
                                         .lower = interval.lower,
                                         .upper = interval.upper};
      }
    }
    std::sort(body.safety.proofs.begin(), body.safety.proofs.end(), proofLess);
    body.safety.proofs.erase(
        std::unique(body.safety.proofs.begin(), body.safety.proofs.end()),
        body.safety.proofs.end());
    result.bodies.push_back(std::move(body));
  }
  return result;
}

MirSafetyAnalysis analyzeMirSafetyChecks(const MirProgram &program,
                                         MirBodyAddress address) {
  const MirClosedCallSafetyAnalysis analysis =
      analyzeMirClosedCallSafetyChecks(program);
  const MirClosedCallBodyAnalysis *body = analysis.find(address);
  return body == nullptr ? MirSafetyAnalysis{} : body->safety;
}

MirSafetyAnalysis
analyzeMirSafetyChecks(const MirProgram &program, MirBodyAddress address,
                       const MirPlaceAliasEscapeAnalysis &aliases) {
  return analyzeBodyLocalSafety(program, address, aliases);
}

} // namespace lang
