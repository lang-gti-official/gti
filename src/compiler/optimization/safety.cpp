#include "gti/optimization/safety.h"

#include "gti/checked_integer.h"
#include "gti/mir_dominance.h"
#include "gti/optimization/alias.h"
#include "gti/optimization/dataflow.h"
#include "gti/optimization/effects.h"
#include "gti/optimization/loops.h"

#include <algorithm>
#include <compare>
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

struct StorageFact {
  IntegerFact capacity;
  std::optional<IntegerFact> length;
  std::optional<IntegerFact> elements;
  bool elementsUnknown = false;
  std::set<std::uint64_t> initialized;
  Evidence evidence;

  friend bool operator==(const StorageFact &, const StorageFact &) = default;
};

struct PrefixLengthFact {
  MirPlaceId storage = 0;
  Evidence evidence;

  friend bool operator==(const PrefixLengthFact &,
                         const PrefixLengthFact &) = default;
};

struct ScalarOrigin {
  MirPlaceId place = 0;
  MirValueId value = 0;

  friend auto operator<=>(const ScalarOrigin &, const ScalarOrigin &) = default;
};

using FieldPath = std::vector<SymbolId>;

struct ObjectFact {
  std::map<FieldPath, IntegerFact> integers;
  std::map<FieldPath, StorageFact> storages;
  std::optional<IntegerFact> elements;

  friend bool operator==(const ObjectFact &, const ObjectFact &) = default;
};

[[nodiscard]] bool mergeObject(ObjectFact &target, const ObjectFact &incoming,
                               bool widen);

struct AbstractState {
  bool reachable = false;
  std::map<MirValueId, IntegerFact> integers;
  std::map<MirPlaceId, IntegerFact> integerPlaces;
  std::map<MirValueId, IntegerFact> arrayValues;
  std::map<MirPlaceId, IntegerFact> arrayElements;
  std::map<MirValueId, MirPlaceId> loadedOrigins;
  std::map<MirValueId, StorageFact> storageValues;
  std::map<MirValueId, MirPlaceId> storageOrigins;
  std::map<MirPlaceId, StorageFact> storages;
  std::map<MirValueId, PrefixLengthFact> prefixLengthValues;
  std::map<MirPlaceId, PrefixLengthFact> prefixLengthPlaces;
  std::map<std::pair<MirPlaceId, ScalarOrigin>, Evidence> checkedPrefixBounds;
  std::map<MirPlaceId, IntegerFact> aggregateElements;
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

void stabilize(StorageFact &storage) {
  stabilize(storage.capacity);
  if (storage.length) {
    stabilize(*storage.length);
  }
  if (storage.elements) {
    stabilize(*storage.elements);
  }
}

void stabilize(ObjectFact &object) {
  for (auto &[path, fact] : object.integers) {
    (void)path;
    stabilize(fact);
  }
  for (auto &[path, storage] : object.storages) {
    (void)path;
    stabilize(storage);
  }
  if (object.elements) {
    stabilize(*object.elements);
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

[[nodiscard]] std::optional<std::uint64_t>
nonnegativeSingleton(const IntegerFact &fact) {
  return singleton(fact) && !normalizeCheckedInteger(fact.lower).negative
             ? std::optional<std::uint64_t>{fact.lower.magnitude}
             : std::nullopt;
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

void refineBranch(const MirBody &body, const MirBlock &block,
                  AbstractState &state, bool branchTaken) {
  if (!state.reachable || block.terminator.kind != MirTerminatorKind::Branch ||
      !block.terminator.value ||
      block.terminator.value->kind != MirOperandKind::Value) {
    return;
  }
  const MirInstruction *comparison =
      definingInstruction(body, block.terminator.value->value);
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
  add(evidence, block.id);
  addInstruction(evidence, comparison->id);
  addValue(evidence, block.terminator.value->value);
  if (!intersect(*variableFact, lower, upper, evidence)) {
    state.reachable = false;
    return;
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
    state.integerPlaces[place] = std::move(placeFact);
  }
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
        !std::all_of(candidate.projections.begin(), candidate.projections.end(),
                     [](const auto &projection) {
                       return transparentRootProjection(projection.kind);
                     })) {
      continue;
    }
    const SemanticType *type = &candidate.type;
    if (type->kind == SemanticType::Reference && type->arguments.size() == 1) {
      type = &type->arguments.front();
    }
    if (type->kind != SemanticType::Class &&
        place->type.kind != SemanticType::Class) {
      continue;
    }
    if (best == nullptr ||
        candidate.projections.size() < best->projections.size()) {
      best = &candidate;
    }
  }
  return best == nullptr ? 0 : canonicalPlace(aliases, best->id);
}

[[nodiscard]] std::optional<FieldPath>
fieldPath(const MirBody &body, MirPlaceId rootId, MirPlaceId placeId) {
  const MirPlace *root = body.findPlace(rootId);
  const MirPlace *place = body.findPlace(placeId);
  if (root == nullptr || place == nullptr || !samePlaceRoot(*root, *place)) {
    return std::nullopt;
  }
  FieldPath result;
  for (const MirPlaceProjection &projection : place->projections) {
    if (transparentRootProjection(projection.kind)) {
      continue;
    }
    if (projection.kind != MirProjectionKind::Field || projection.field == 0) {
      return std::nullopt;
    }
    result.push_back(projection.field);
  }
  return result.empty() ? std::nullopt
                        : std::optional<FieldPath>{std::move(result)};
}

[[nodiscard]] bool
indexedAggregatePlace(const MirProgram *program, const MirBody &body,
                      const MirPlaceAliasEscapeAnalysis &aliases,
                      MirPlaceId placeId, MirPlaceId aggregateRootId);

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

[[nodiscard]] StorageFact rebased(StorageFact fact, MirBlockId block = 0,
                                  MirInstructionId instruction = 0,
                                  MirPlaceId place = 0) {
  fact.capacity =
      rebased(std::move(fact.capacity), block, instruction, 0, place);
  if (fact.length) {
    fact.length =
        rebased(std::move(*fact.length), block, instruction, 0, place);
  }
  if (fact.elements) {
    fact.elements =
        rebased(std::move(*fact.elements), block, instruction, 0, place);
  }
  fact.evidence = {};
  add(fact.evidence, block);
  addInstruction(fact.evidence, instruction);
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
    if (const auto storage = object.storages.find(*path);
        storage != object.storages.end()) {
      state.storages[destination] = rebased(storage->second, 0, 0, destination);
    }
  }
  if (object.elements) {
    state.aggregateElements[root] = rebased(*object.elements, 0, 0, 0, root);
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
  bool observedElements = false;
  bool unknownElements = false;
  std::optional<IntegerFact> derivedElements;
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
    if (const auto storage = state.storages.find(source);
        storage != state.storages.end()) {
      result.storages.insert_or_assign(*path, storage->second);
      observedElements = true;
      if (storage->second.elementsUnknown) {
        unknownElements = true;
      }
      if (storage->second.elements) {
        derivedElements =
            derivedElements ? std::optional<IntegerFact>{joinedElementFact(
                                  *derivedElements, *storage->second.elements)}
                            : storage->second.elements;
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
  if (observedElements) {
    result.elements =
        unknownElements ? std::nullopt : std::move(derivedElements);
  }
  if (result.integers.empty() && result.storages.empty() && !result.elements) {
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
  const MirPlaceId root =
      aggregateRoot(body, aliases, *instruction.destination);
  if (root != 0 && indexedAggregatePlace(program, body, aliases,
                                         *instruction.destination, root)) {
    if (const auto elements = state.aggregateElements.find(root);
        elements != state.aggregateElements.end()) {
      left = elements->second;
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
    state.integerPlaces.clear();
    state.arrayElements.clear();
    state.aggregateElements.clear();
    state.objects.clear();
    state.loadedOrigins.clear();
    state.storageValues.clear();
    state.storageOrigins.clear();
    state.storages.clear();
    state.prefixLengthValues.clear();
    state.prefixLengthPlaces.clear();
    state.checkedPrefixBounds.clear();
    state.aggregateOrigins.clear();
    state.objectValues.clear();
    return;
  }
  const auto overlaps = [&](MirPlaceId place) {
    return aliases.relation(destination, place) != MirAliasRelation::NoAlias;
  };
  std::erase_if(state.integerPlaces,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.arrayElements,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.aggregateElements,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.objects,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.aggregateOrigins,
                [&](const auto &entry) { return overlaps(entry.second); });
  std::erase_if(state.loadedOrigins,
                [&](const auto &entry) { return overlaps(entry.second); });
  std::erase_if(state.storages,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.prefixLengthValues, [&](const auto &entry) {
    return overlaps(entry.second.storage);
  });
  std::erase_if(state.prefixLengthPlaces, [&](const auto &entry) {
    return overlaps(entry.first) || overlaps(entry.second.storage);
  });
  std::erase_if(state.checkedPrefixBounds, [&](const auto &entry) {
    return overlaps(entry.first.first) || (entry.first.second.place != 0 &&
                                           overlaps(entry.first.second.place));
  });
  for (auto iterator = state.storageOrigins.begin();
       iterator != state.storageOrigins.end();) {
    if (!overlaps(iterator->second)) {
      ++iterator;
      continue;
    }
    state.storageValues.erase(iterator->first);
    iterator = state.storageOrigins.erase(iterator);
  }
}

struct FixedArrayIndex {
  MirPlaceId arrayPlace = 0;
  MirPlaceId elementPlace = 0;
  std::uint64_t length = 0;
  std::optional<IntegerFact> index;
};

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
  std::optional<IntegerFact> index;
  if (projected.projection.constantIndex) {
    const std::optional<CheckedIntegerDomain> domain =
        constantIntegerDomain(SemanticType::UInt64);
    if (domain) {
      Evidence evidence;
      addPlace(evidence, projectedId);
      index = IntegerFact{
          .domain = *domain,
          .lower = {.magnitude = *projected.projection.constantIndex},
          .upper = {.magnitude = *projected.projection.constantIndex},
          .evidence = std::move(evidence)};
    }
  } else if (projected.projection.index != 0) {
    index = integerForValue(body, state, projected.projection.index);
  }
  return {.arrayPlace = projected.arrayPlace,
          .elementPlace = projectedId,
          .length = projected.length,
          .index = std::move(index)};
}

[[nodiscard]] bool within(const IntegerFact &index,
                          const IntegerFact &capacity) {
  return !normalizeCheckedInteger(index.lower).negative &&
         !normalizeCheckedInteger(capacity.lower).negative &&
         compare(index.lower, CheckedIntegerValue{}) >= 0 &&
         compare(index.upper, capacity.lower) < 0;
}

[[nodiscard]] std::optional<StorageFact>
storageForOperand(const AbstractState &state,
                  const MirPlaceAliasEscapeAnalysis &aliases,
                  const MirOperand &operand) {
  if (operand.value != 0) {
    const auto origin = state.storageOrigins.find(operand.value);
    if (origin != state.storageOrigins.end()) {
      const auto storage = state.storages.find(origin->second);
      if (storage != state.storages.end()) {
        return storage->second;
      }
      return std::nullopt;
    }
    const auto value = state.storageValues.find(operand.value);
    if (value != state.storageValues.end()) {
      return value->second;
    }
  }
  if (operand.place != 0) {
    const auto storage =
        state.storages.find(canonicalPlace(aliases, operand.place));
    if (storage != state.storages.end()) {
      return storage->second;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<MirPlaceId>
storageOriginForOperand(const AbstractState &state,
                        const MirPlaceAliasEscapeAnalysis &aliases,
                        const MirOperand &operand) {
  const MirPlaceId place = canonicalPlace(aliases, operand.place);
  if (place != 0 && state.storages.contains(place)) {
    return place;
  }
  const auto origin = state.storageOrigins.find(operand.value);
  return origin == state.storageOrigins.end()
             ? std::nullopt
             : std::optional<MirPlaceId>{origin->second};
}

[[nodiscard]] std::optional<MirPlaceId>
stableStorageOriginForOperand(const AbstractState &state,
                              const MirPlaceAliasEscapeAnalysis &aliases,
                              const MirOperand &operand) {
  const SemanticType *type = &operand.type;
  if (type->kind == SemanticType::Reference && type->arguments.size() == 1) {
    type = &type->arguments.front();
  }
  if (type->kind != SemanticType::Storage &&
      type->kind != SemanticType::PrefixStorage) {
    return std::nullopt;
  }
  if (const std::optional<MirPlaceId> tracked =
          storageOriginForOperand(state, aliases, operand)) {
    return tracked;
  }
  const MirPlaceId place = canonicalPlace(aliases, operand.place);
  return place == 0 ? std::nullopt : std::optional<MirPlaceId>{place};
}

[[nodiscard]] std::optional<ScalarOrigin>
scalarOriginForOperand(const AbstractState &state,
                       const MirPlaceAliasEscapeAnalysis &aliases,
                       const MirOperand &operand) {
  if (operand.literal) {
    return std::nullopt;
  }
  if (operand.value != 0) {
    if (const auto loaded = state.loadedOrigins.find(operand.value);
        loaded != state.loadedOrigins.end()) {
      return ScalarOrigin{.place = canonicalPlace(aliases, loaded->second)};
    }
    return ScalarOrigin{.value = operand.value};
  }
  const MirPlaceId place = canonicalPlace(aliases, operand.place);
  return place == 0 ? std::nullopt
                    : std::optional<ScalarOrigin>{ScalarOrigin{.place = place}};
}

[[nodiscard]] std::optional<PrefixLengthFact>
prefixLengthForOperand(const AbstractState &state,
                       const MirPlaceAliasEscapeAnalysis &aliases,
                       const MirOperand &operand) {
  if (operand.value != 0) {
    if (const auto value = state.prefixLengthValues.find(operand.value);
        value != state.prefixLengthValues.end()) {
      return value->second;
    }
  }
  const MirPlaceId place = canonicalPlace(aliases, operand.place);
  if (place != 0) {
    if (const auto stored = state.prefixLengthPlaces.find(place);
        stored != state.prefixLengthPlaces.end()) {
      return stored->second;
    }
  }
  return std::nullopt;
}

void invalidatePrefixStorageRelations(
    AbstractState &state, const MirPlaceAliasEscapeAnalysis &aliases,
    MirPlaceId storage) {
  storage = canonicalPlace(aliases, storage);
  if (storage == 0) {
    state.prefixLengthValues.clear();
    state.prefixLengthPlaces.clear();
    state.checkedPrefixBounds.clear();
    return;
  }
  const auto overlaps = [&](MirPlaceId candidate) {
    return candidate != 0 &&
           aliases.relation(storage, candidate) != MirAliasRelation::NoAlias;
  };
  std::erase_if(state.prefixLengthValues, [&](const auto &entry) {
    return overlaps(entry.second.storage);
  });
  std::erase_if(state.prefixLengthPlaces, [&](const auto &entry) {
    return overlaps(entry.second.storage);
  });
  std::erase_if(state.checkedPrefixBounds,
                [&](const auto &entry) { return overlaps(entry.first.first); });
}

void updateStorageOperand(AbstractState &state,
                          const MirPlaceAliasEscapeAnalysis &aliases,
                          const MirOperand &operand, StorageFact storage) {
  if (const std::optional<MirPlaceId> origin =
          storageOriginForOperand(state, aliases, operand)) {
    state.storages.insert_or_assign(*origin, std::move(storage));
    return;
  }
  if (operand.value != 0) {
    state.storageValues.insert_or_assign(operand.value, std::move(storage));
  }
}

[[nodiscard]] std::optional<SemanticType>
prefixElementType(const MirOperand &storage) {
  const SemanticType *type = &storage.type;
  if (type->kind == SemanticType::Reference && type->arguments.size() == 1) {
    type = &type->arguments.front();
  }
  if (type->kind != SemanticType::PrefixStorage ||
      type->arguments.size() != 1) {
    return std::nullopt;
  }
  return type->arguments.front();
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
                          operand.type.kind == SemanticType::Expected ||
                          operand.type.kind == SemanticType::PrefixStorage ||
                          operand.type.kind == SemanticType::Storage;
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
    closedCalls->incompleteBodies.insert(address);
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
  // Closed calls transport scalar/object summaries, but not relational
  // identity between a prior logical-size check and private prefix storage.
  // Drop those relations at the call boundary until exact effect summaries
  // can prove that every referenced storage and index origin is unchanged.
  state.prefixLengthValues.clear();
  state.prefixLengthPlaces.clear();
  state.checkedPrefixBounds.clear();
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
  if (instruction.receiver && summary.receiver.observed) {
    if (summary.receiver.fact) {
      ObjectFact object = *summary.receiver.fact;
      stabilize(object);
      applyObjectToOperand(body, aliases, *instruction.receiver, object, state);
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
      applyObjectToOperand(body, aliases, instruction.operands[index], object,
                           state);
    } else {
      invalidateObjectOperand(body, aliases, instruction.operands[index],
                              state);
    }
  }
  return true;
}

void killUnknownMemory(AbstractState &state) {
  state.integerPlaces.clear();
  state.arrayElements.clear();
  state.aggregateElements.clear();
  state.objects.clear();
  state.loadedOrigins.clear();
  state.storageValues.clear();
  state.storages.clear();
  state.storageOrigins.clear();
  state.prefixLengthValues.clear();
  state.prefixLengthPlaces.clear();
  state.checkedPrefixBounds.clear();
  state.aggregateOrigins.clear();
  state.objectValues.clear();
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
  if (instruction.kind == MirInstructionKind::Compute) {
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
      if (const auto length =
              state.prefixLengthValues.find(instruction.operands.front().value);
          length != state.prefixLengthValues.end()) {
        state.prefixLengthValues[*instruction.result] = length->second;
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
    if (const std::optional<StorageFact> storage =
            storageForOperand(state, aliases, source)) {
      state.storageValues[*instruction.result] = *storage;
    }
    if (const std::optional<MirPlaceId> origin =
            stableStorageOriginForOperand(state, aliases, source)) {
      state.storageOrigins[*instruction.result] = *origin;
    }
    if (const std::optional<PrefixLengthFact> length =
            prefixLengthForOperand(state, aliases, source)) {
      PrefixLengthFact copied = *length;
      add(copied.evidence, block.id);
      addInstruction(copied.evidence, instruction.id);
      addValue(copied.evidence, *instruction.result);
      state.prefixLengthValues[*instruction.result] = std::move(copied);
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
      const MirPlaceId objectRoot = aggregateRoot(body, aliases, source.place);
      const auto arrayElements = state.arrayElements.find(arrayPlace);
      const auto aggregateElements = state.aggregateElements.find(objectRoot);
      const std::optional<IntegerFact> fact =
          objectRoot != 0 &&
                  indexedAggregatePlace(program, body, aliases, source.place,
                                        objectRoot) &&
                  aggregateElements != state.aggregateElements.end()
              ? std::optional<IntegerFact>{aggregateElements->second}
          : projected.arrayPlace != 0 &&
                  arrayElements != state.arrayElements.end()
              ? std::optional<IntegerFact>{arrayElements->second}
              : integerForPlace(body, state, sourcePlace);
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
      const auto storage = state.storages.find(sourcePlace);
      if (storage != state.storages.end()) {
        state.storageValues[*instruction.result] = storage->second;
      }
      if (source.type.kind == SemanticType::Storage ||
          source.type.kind == SemanticType::PrefixStorage) {
        state.storageOrigins[*instruction.result] = sourcePlace;
      }
      if (objectRoot != 0) {
        state.aggregateOrigins[*instruction.result] = objectRoot;
        if (const std::optional<ObjectFact> object =
                extractObjectFact(body, aliases, objectRoot, state)) {
          state.objectValues[*instruction.result] = *object;
        }
      }
      if (const auto length = state.prefixLengthPlaces.find(sourcePlace);
          length != state.prefixLengthPlaces.end()) {
        PrefixLengthFact loaded = length->second;
        add(loaded.evidence, block.id);
        addInstruction(loaded.evidence, instruction.id);
        addValue(loaded.evidence, *instruction.result);
        addPlace(loaded.evidence, sourcePlace);
        state.prefixLengthValues[*instruction.result] = std::move(loaded);
      }
    }
    return;
  }
  if ((instruction.kind == MirInstructionKind::Initialize ||
       instruction.kind == MirInstructionKind::Assign ||
       instruction.kind == MirInstructionKind::Move) &&
      instruction.destination && !instruction.operands.empty()) {
    const MirPlaceId destination =
        canonicalPlace(aliases, *instruction.destination);
    const FixedArrayProjection projected =
        fixedArrayProjection(body, *instruction.destination);
    const MirPlaceId arrayPlace = canonicalPlace(aliases, projected.arrayPlace);
    const MirPlaceId objectRoot =
        aggregateRoot(body, aliases, *instruction.destination);
    const bool aggregateIndex =
        objectRoot != 0 &&
        indexedAggregatePlace(program, body, aliases, *instruction.destination,
                              objectRoot);
    const auto previousArray = state.arrayElements.find(arrayPlace);
    const std::optional<IntegerFact> previousElements =
        projected.arrayPlace != 0 && previousArray != state.arrayElements.end()
            ? std::optional<IntegerFact>{previousArray->second}
            : std::nullopt;
    const auto previousAggregate = state.aggregateElements.find(objectRoot);
    const std::optional<IntegerFact> previousAggregateElements =
        aggregateIndex && previousAggregate != state.aggregateElements.end()
            ? std::optional<IntegerFact>{previousAggregate->second}
            : std::nullopt;
    const MirOperand &source = instruction.operands.front();
    std::optional<IntegerFact> fact =
        source.place != 0
            ? integerForPlace(body, state,
                              canonicalPlace(aliases, source.place))
            : integerForOperand(body, state, source);
    if (compoundCheckedOperation(instruction.operation)) {
      fact = compoundIntegerFact(body, program, aliases, state, instruction);
    }
    const std::optional<StorageFact> storage =
        storageForOperand(state, aliases, source);
    const std::optional<IntegerFact> sourceArray =
        arrayForOperand(state, aliases, source);
    const std::optional<ObjectFact> sourceObject =
        objectForOperand(body, aliases, state, source);
    const std::optional<PrefixLengthFact> sourcePrefixLength =
        prefixLengthForOperand(state, aliases, source);
    if (!aggregateIndex || !previousAggregateElements) {
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
    if (aggregateIndex && fact) {
      IntegerFact elements =
          previousAggregateElements
              ? joinedElementFact(*previousAggregateElements, *fact)
              : *fact;
      add(elements.evidence, block.id);
      addInstruction(elements.evidence, instruction.id);
      addPlace(elements.evidence, objectRoot);
      addPlace(elements.evidence, *instruction.destination);
      state.aggregateElements[objectRoot] = std::move(elements);
      ObjectFact &object = state.objects[objectRoot];
      object.elements = state.aggregateElements[objectRoot];
    }
    if (storage) {
      StorageFact stored = *storage;
      add(stored.evidence, block.id);
      addInstruction(stored.evidence, instruction.id);
      addPlace(stored.evidence, destination);
      state.storages[destination] = std::move(stored);
    } else if (const MirPlace *place = body.findPlace(destination);
               place != nullptr &&
               (place->type.kind == SemanticType::Storage ||
                place->type.kind == SemanticType::PrefixStorage)) {
      state.storages.erase(destination);
    }
    if (sourceObject) {
      seedObjectFact(body, aliases, destination, *sourceObject, state);
    }
    if (sourcePrefixLength) {
      PrefixLengthFact stored = *sourcePrefixLength;
      add(stored.evidence, block.id);
      addInstruction(stored.evidence, instruction.id);
      addPlace(stored.evidence, destination);
      state.prefixLengthPlaces[destination] = std::move(stored);
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
      killUnknownMemory(state);
    } else {
      invalidatePlaceRoot(body, aliases, state, *instruction.destination);
    }
    return;
  }
  if (instruction.kind != MirInstructionKind::Call) {
    const MirEffectTraits effect = effects(instruction);
    if (instruction.rawMemoryAccess ||
        instruction.unsafeOperation != UnsafeOperationKind::None ||
        effect.writesPlace || effect.writesUnknownMemory ||
        effect.invokesUserCode || effect.maySynchronize) {
      killUnknownMemory(state);
    }
    return;
  }

  if (instruction.intrinsic == IntrinsicKind::AllocateStorage &&
      instruction.result && !instruction.operands.empty()) {
    const std::optional<IntegerFact> capacity =
        integerForOperand(body, state, instruction.operands.front());
    if (capacity) {
      Evidence evidence = capacity->evidence;
      add(evidence, block.id);
      addInstruction(evidence, instruction.id);
      addValue(evidence, *instruction.result);
      state.storageValues[*instruction.result] = {
          .capacity = *capacity, .evidence = std::move(evidence)};
    }
    return;
  }

  if (instruction.intrinsic == IntrinsicKind::AllocatePrefixStorage &&
      instruction.result && !instruction.operands.empty()) {
    const std::optional<IntegerFact> capacity =
        integerForOperand(body, state, instruction.operands.front());
    if (capacity) {
      Evidence evidence = capacity->evidence;
      add(evidence, block.id);
      addInstruction(evidence, instruction.id);
      addValue(evidence, *instruction.result);
      const std::optional<CheckedIntegerDomain> lengthDomain =
          constantIntegerDomain(SemanticType::UInt64);
      state.storageValues[*instruction.result] = {
          .capacity = *capacity,
          .length = lengthDomain ? std::optional<IntegerFact>{IntegerFact{
                                       .domain = *lengthDomain,
                                       .lower = {},
                                       .upper = {},
                                       .evidence = evidence}}
                                 : std::nullopt,
          .evidence = std::move(evidence)};
    }
    return;
  }

  if (instruction.intrinsic == IntrinsicKind::StorageBoundsCheck) {
    if (instruction.operands.size() == 2) {
      const std::optional<ScalarOrigin> index =
          scalarOriginForOperand(state, aliases, instruction.operands[0]);
      const std::optional<PrefixLengthFact> length =
          prefixLengthForOperand(state, aliases, instruction.operands[1]);
      if (index && length && length->storage != 0) {
        Evidence evidence = length->evidence;
        if (const std::optional<IntegerFact> indexFact =
                integerForOperand(body, state, instruction.operands[0])) {
          evidence = merged(std::move(evidence), indexFact->evidence);
        }
        add(evidence, block.id);
        addInstruction(evidence, instruction.id);
        addPlace(evidence, length->storage);
        if (index->place != 0) {
          addPlace(evidence, index->place);
        } else {
          addValue(evidence, index->value);
        }
        state.checkedPrefixBounds.insert_or_assign(
            std::pair{length->storage, *index}, std::move(evidence));
      }
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

  const bool prefixOperation =
      instruction.intrinsic == IntrinsicKind::PrefixStorageAppend ||
      instruction.intrinsic == IntrinsicKind::PrefixStoragePop ||
      instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
      instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut ||
      instruction.intrinsic == IntrinsicKind::PrefixStorageLength ||
      instruction.intrinsic == IntrinsicKind::PrefixStorageRelocate ||
      instruction.intrinsic == IntrinsicKind::PrefixStorageInsert ||
      instruction.intrinsic == IntrinsicKind::PrefixStorageErase;
  if (prefixOperation && !instruction.operands.empty()) {
    const std::optional<SemanticType> elementType =
        prefixElementType(instruction.operands.front());
    const std::optional<MirPlaceId> origin = stableStorageOriginForOperand(
        state, aliases, instruction.operands.front());
    std::optional<StorageFact> storage =
        storageForOperand(state, aliases, instruction.operands.front());
    if (!elementType) {
      killUnknownMemory(state);
      return;
    }
    if (instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
        instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut) {
      if (instruction.result && storage && storage->elements) {
        state.integers[*instruction.result] = rebased(
            *storage->elements, block.id, instruction.id, *instruction.result);
      }
      return;
    }
    if (instruction.intrinsic == IntrinsicKind::PrefixStorageLength) {
      if (instruction.result && storage && storage->length) {
        state.integers[*instruction.result] = rebased(
            *storage->length, block.id, instruction.id, *instruction.result);
      }
      if (instruction.result && origin) {
        Evidence evidence = storage ? storage->evidence : Evidence{};
        add(evidence, block.id);
        addInstruction(evidence, instruction.id);
        addValue(evidence, *instruction.result);
        addPlace(evidence, *origin);
        state.prefixLengthValues[*instruction.result] = {
            .storage = *origin, .evidence = std::move(evidence)};
      }
      return;
    }
    if (!storage) {
      killUnknownMemory(state);
      return;
    }
    add(storage->evidence, block.id);
    addInstruction(storage->evidence, instruction.id);
    if (instruction.intrinsic == IntrinsicKind::PrefixStorageRelocate) {
      if (instruction.operands.size() != 2) {
        killUnknownMemory(state);
        return;
      }
      const std::optional<MirPlaceId> destinationOrigin =
          stableStorageOriginForOperand(state, aliases,
                                        instruction.operands[1]);
      invalidatePrefixStorageRelations(state, aliases, origin.value_or(0));
      invalidatePrefixStorageRelations(state, aliases,
                                       destinationOrigin.value_or(0));
      updateStorageOperand(state, aliases, instruction.operands[1], *storage);
      const std::optional<CheckedIntegerDomain> lengthDomain =
          constantIntegerDomain(SemanticType::UInt64);
      if (lengthDomain) {
        storage->length =
            IntegerFact{.domain = *lengthDomain, .lower = {}, .upper = {}};
      } else {
        storage->length.reset();
      }
      storage->elements.reset();
      storage->elementsUnknown = false;
      updateStorageOperand(state, aliases, instruction.operands.front(),
                           std::move(*storage));
      return;
    }
    if (instruction.intrinsic == IntrinsicKind::PrefixStorageAppend ||
        instruction.intrinsic == IntrinsicKind::PrefixStorageInsert) {
      invalidatePrefixStorageRelations(state, aliases, origin.value_or(0));
      const std::size_t valueIndex =
          instruction.intrinsic == IntrinsicKind::PrefixStorageInsert ? 2 : 1;
      std::optional<IntegerFact> value;
      if (constantIntegerDomain(*elementType) &&
          instruction.operands.size() > valueIndex) {
        value =
            integerForOperand(body, state, instruction.operands[valueIndex]);
      } else if (const std::optional<CheckedIntegerDomain> domain =
                     constantIntegerDomain(*elementType)) {
        value = IntegerFact{.domain = *domain, .lower = {}, .upper = {}};
      }
      if (!value) {
        storage->elements.reset();
        storage->elementsUnknown = true;
      } else if (!storage->elementsUnknown) {
        storage->elements = storage->elements
                                ? std::optional<IntegerFact>{joinedElementFact(
                                      *storage->elements, *value)}
                                : value;
      }
      if (storage->length) {
        const IntegerFact one{.domain = storage->length->domain,
                              .lower = {.magnitude = 1},
                              .upper = {.magnitude = 1}};
        storage->length =
            binaryFact(CheckedIntegerOperation::Add, *storage->length, one);
      }
      updateStorageOperand(state, aliases, instruction.operands.front(),
                           std::move(*storage));
      return;
    }
    if (instruction.intrinsic == IntrinsicKind::PrefixStoragePop ||
        instruction.intrinsic == IntrinsicKind::PrefixStorageErase) {
      invalidatePrefixStorageRelations(state, aliases, origin.value_or(0));
      if (storage->length) {
        const IntegerFact one{.domain = storage->length->domain,
                              .lower = {.magnitude = 1},
                              .upper = {.magnitude = 1}};
        storage->length = binaryFact(CheckedIntegerOperation::Subtract,
                                     *storage->length, one);
      }
      updateStorageOperand(state, aliases, instruction.operands.front(),
                           std::move(*storage));
      return;
    }
  }

  const bool storageOperation =
      instruction.intrinsic == IntrinsicKind::StorageConstruct ||
      instruction.intrinsic == IntrinsicKind::StorageRead ||
      instruction.intrinsic == IntrinsicKind::StorageReadMut ||
      instruction.intrinsic == IntrinsicKind::StorageDestroy;
  if (storageOperation && instruction.operands.size() >= 2) {
    const std::optional<MirPlaceId> origin =
        storageOriginForOperand(state, aliases, instruction.operands[0]);
    const std::optional<IntegerFact> index =
        integerForOperand(body, state, instruction.operands[1]);
    std::optional<StorageFact> updated;
    std::vector<MirValueId> originValues;
    if (origin) {
      for (const auto &[value, place] : state.storageOrigins) {
        if (place == *origin) {
          originValues.push_back(value);
        }
      }
    }
    if (origin && index) {
      auto storage = state.storages.find(*origin);
      const std::optional<std::uint64_t> exact = nonnegativeSingleton(*index);
      if (storage != state.storages.end() && exact &&
          within(*index, storage->second.capacity)) {
        if (instruction.intrinsic == IntrinsicKind::StorageConstruct) {
          storage->second.initialized.insert(*exact);
          add(storage->second.evidence, block.id);
          addInstruction(storage->second.evidence, instruction.id);
          addValue(storage->second.evidence, instruction.operands[1].value);
        } else if (instruction.intrinsic == IntrinsicKind::StorageDestroy) {
          storage->second.initialized.erase(*exact);
          add(storage->second.evidence, block.id);
          addInstruction(storage->second.evidence, instruction.id);
        }
        updated = storage->second;
      } else if (storage != state.storages.end() &&
                 (instruction.intrinsic == IntrinsicKind::StorageConstruct ||
                  instruction.intrinsic == IntrinsicKind::StorageDestroy)) {
        updated = storage->second;
        updated->initialized.clear();
      }
    }
    if (instruction.intrinsic == IntrinsicKind::StorageConstruct ||
        instruction.intrinsic == IntrinsicKind::StorageDestroy) {
      // User lifecycle code may mutate any other reachable place. The first
      // storage operand remains uniquely borrowed for the intrinsic, so only
      // its post-success capacity/state fact survives that barrier.
      killUnknownMemory(state);
      if (origin && updated) {
        state.storages[*origin] = std::move(*updated);
        for (const MirValueId value : originValues) {
          state.storageOrigins[value] = *origin;
        }
      }
    }
    return;
  }

  if (applyClosedCall(body, aliases, block, instruction, state, closedCalls,
                      address)) {
    return;
  }

  // Calls outside the admitted private-storage vocabulary may mutate any
  // reachable place through a reference or opaque host boundary. Value facts
  // remain valid because MIR values are immutable.
  killUnknownMemory(state);
}

[[nodiscard]] BlockTransfer
transferBlock(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
              const MirBlock &block, AbstractState state,
              const PlaceThresholds *thresholds = nullptr,
              ClosedCallRound *closedCalls = nullptr,
              MirBodyAddress address = {}) {
  BlockTransfer result;
  for (const MirInstruction &instruction : block.instructions) {
    result.before.insert_or_assign(instruction.id, state);
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

void joinStorageFact(StorageFact &target, const StorageFact &incoming,
                     bool widen) {
  target.capacity = joinedInteger(target.capacity, incoming.capacity, widen);
  joinOptionalInteger(target.length, incoming.length, widen);
  if (target.elementsUnknown || incoming.elementsUnknown) {
    target.elements.reset();
    target.elementsUnknown = true;
  } else if (target.elements && incoming.elements) {
    target.elements =
        joinedInteger(*target.elements, *incoming.elements, widen);
  } else if (!target.elements && incoming.elements) {
    target.elements = incoming.elements;
  }
  std::set<std::uint64_t> intersection;
  std::set_intersection(target.initialized.begin(), target.initialized.end(),
                        incoming.initialized.begin(),
                        incoming.initialized.end(),
                        std::inserter(intersection, intersection.end()));
  target.initialized = std::move(intersection);
  target.evidence = merged(target.evidence, incoming.evidence);
}

void joinStorageFacts(std::map<MirPlaceId, StorageFact> &target,
                      const std::map<MirPlaceId, StorageFact> &incoming,
                      bool widen) {
  for (auto iterator = target.begin(); iterator != target.end();) {
    const auto found = incoming.find(iterator->first);
    if (found == incoming.end()) {
      iterator = target.erase(iterator);
      continue;
    }
    joinStorageFact(iterator->second, found->second, widen);
    ++iterator;
  }
}

void joinStorageValues(std::map<MirValueId, StorageFact> &target,
                       const std::map<MirValueId, StorageFact> &incoming,
                       bool widen) {
  for (auto iterator = target.begin(); iterator != target.end();) {
    const auto found = incoming.find(iterator->first);
    if (found == incoming.end()) {
      iterator = target.erase(iterator);
      continue;
    }
    joinStorageFact(iterator->second, found->second, widen);
    ++iterator;
  }
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
  joinIntegers(target.integers, incoming.integers, widen);
  joinIntegers(target.integerPlaces, incoming.integerPlaces, widen);
  joinIntegers(target.arrayValues, incoming.arrayValues, widen);
  joinIntegers(target.arrayElements, incoming.arrayElements, widen);
  joinIntegers(target.aggregateElements, incoming.aggregateElements, widen);
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
  joinStorageValues(target.storageValues, incoming.storageValues, widen);
  joinStorageFacts(target.storages, incoming.storages, widen);
  const auto joinPrefixLengths = [](auto &left, const auto &right) {
    for (auto iterator = left.begin(); iterator != left.end();) {
      const auto found = right.find(iterator->first);
      if (found == right.end() ||
          found->second.storage != iterator->second.storage) {
        iterator = left.erase(iterator);
        continue;
      }
      iterator->second.evidence =
          merged(std::move(iterator->second.evidence), found->second.evidence);
      ++iterator;
    }
  };
  joinPrefixLengths(target.prefixLengthValues, incoming.prefixLengthValues);
  joinPrefixLengths(target.prefixLengthPlaces, incoming.prefixLengthPlaces);
  for (auto iterator = target.checkedPrefixBounds.begin();
       iterator != target.checkedPrefixBounds.end();) {
    const auto found = incoming.checkedPrefixBounds.find(iterator->first);
    if (found == incoming.checkedPrefixBounds.end()) {
      iterator = target.checkedPrefixBounds.erase(iterator);
      continue;
    }
    iterator->second = merged(std::move(iterator->second), found->second);
    ++iterator;
  }
  for (auto iterator = target.storageOrigins.begin();
       iterator != target.storageOrigins.end();) {
    const auto found = incoming.storageOrigins.find(iterator->first);
    if (found == incoming.storageOrigins.end() ||
        found->second != iterator->second) {
      iterator = target.storageOrigins.erase(iterator);
    } else {
      ++iterator;
    }
  }
  return target != before;
}

void bindEdgeArguments(const MirBody &body,
                       const MirPlaceAliasEscapeAnalysis &aliases,
                       MirBlockId target,
                       const std::vector<MirOperand> &arguments,
                       AbstractState &state) {
  const MirBlock *block = body.findBlock(target);
  if (block == nullptr || block->parameters.size() != arguments.size()) {
    return;
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const MirValueId parameter = block->parameters[index].value;
    const MirOperand &argument = arguments[index];
    const std::optional<PrefixLengthFact> prefixLength =
        prefixLengthForOperand(state, aliases, argument);
    const std::optional<ScalarOrigin> scalar =
        scalarOriginForOperand(state, aliases, argument);
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
    if (prefixLength) {
      PrefixLengthFact rebound = *prefixLength;
      addValue(rebound.evidence, parameter);
      state.prefixLengthValues[parameter] = std::move(rebound);
    } else {
      state.prefixLengthValues.erase(parameter);
    }
    if (scalar) {
      std::vector<std::pair<std::pair<MirPlaceId, ScalarOrigin>, Evidence>>
          reboundBounds;
      for (const auto &[key, evidence] : state.checkedPrefixBounds) {
        if (key.second != *scalar) {
          continue;
        }
        Evidence rebound = evidence;
        addValue(rebound, parameter);
        reboundBounds.emplace_back(
            std::pair{key.first, ScalarOrigin{.value = parameter}},
            std::move(rebound));
      }
      for (auto &[key, evidence] : reboundBounds) {
        state.checkedPrefixBounds.insert_or_assign(key, std::move(evidence));
      }
    }
  }
}

struct Edge {
  MirBlockId target = 0;
  std::size_t ordinal = 0;
  AbstractState state;
};

[[nodiscard]] std::vector<Edge>
outgoingEdges(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
              const MirBlock &block, const BlockTransfer &transfer) {
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
    bindEdgeArguments(body, aliases, target, arguments, state);
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
    append(terminator.target, terminator.targetArguments, 0, transfer.output,
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

[[nodiscard]] bool opaqueLoopMutation(const MirInstruction &instruction) {
  const MirEffectTraits effect = effects(instruction);
  return instruction.rawMemoryAccess ||
         instruction.unsafeOperation != UnsafeOperationKind::None ||
         instruction.kind == MirInstructionKind::Call ||
         instruction.kind == MirInstructionKind::Construct ||
         effect.writesUnknownMemory || effect.invokesUserCode ||
         effect.maySynchronize;
}

struct ExactLoopCount {
  MirPlaceId counter = 0;
  std::uint64_t iterations = 0;
  Evidence evidence;
};

[[nodiscard]] std::vector<std::optional<ExactLoopCount>>
exactLoopCounts(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
                const MirDominanceInfo &dominance,
                const MirLoopAnalysis &loops) {
  std::vector<std::optional<ExactLoopCount>> result(loops.loops.size());
  if (loops.hasIrreducibleControlFlow) {
    return result;
  }
  for (std::size_t loopIndex = 0; loopIndex < loops.loops.size(); ++loopIndex) {
    const MirNaturalLoop &loop = loops.loops[loopIndex];
    const MirBlock *header = body.findBlock(loop.header);
    if (header == nullptr || loop.latches.size() != 1 ||
        header->terminator.kind != MirTerminatorKind::Branch ||
        !header->terminator.value ||
        header->terminator.value->kind != MirOperandKind::Value ||
        !loop.contains(header->terminator.target) ||
        loop.contains(header->terminator.elseTarget)) {
      continue;
    }
    const MirInstruction *comparison =
        definingInstruction(body, header->terminator.value->value);
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
    const std::optional<IntegerFact> boundFact =
        literalOperandFact(body, *bound);
    if (!counter || relation != MirOperation::Less || !boundFact ||
        !singleton(*boundFact) ||
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
        if (loop.contains(block.id) && opaqueLoopMutation(instruction)) {
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
    const std::uint64_t boundValue = boundFact->lower.magnitude;
    const std::uint64_t stepValue = step->lower.magnitude;
    const std::uint64_t distance =
        initialValue >= boundValue ? 0 : boundValue - initialValue;
    const std::uint64_t iterations =
        distance == 0 ? 0 : 1 + (distance - 1) / stepValue;
    Evidence evidence = merged(initial->evidence, boundFact->evidence);
    evidence = merged(std::move(evidence), step->evidence);
    add(evidence, loop.header);
    add(evidence, loop.latches.front());
    addInstruction(evidence, comparison->id);
    addInstruction(evidence, initialization.instruction->id);
    addInstruction(evidence, addition->id);
    addInstruction(evidence, update.instruction->id);
    addPlace(evidence, *counter);
    result[loopIndex] = ExactLoopCount{.counter = *counter,
                                       .iterations = iterations,
                                       .evidence = std::move(evidence)};
  }
  return result;
}

[[nodiscard]] std::optional<AbstractState> stateBeforeInstruction(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const std::vector<AbstractState> &incoming, InstructionSite site) {
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
    applyInstruction(body, aliases, *site.block, instruction, state);
  }
  return std::nullopt;
}

[[nodiscard]] PlaceThresholds recurrenceThresholds(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const MirDominanceInfo &dominance, const MirLoopAnalysis &loops,
    const std::vector<AbstractState> &incoming) {
  PlaceThresholds result;
  if (loops.hasIrreducibleControlFlow) {
    return result;
  }
  const std::vector<std::optional<ExactLoopCount>> counts =
      exactLoopCounts(body, aliases, dominance, loops);
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
        if (loops.loops[index].contains(block.id) && counts[index] &&
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
        if (!counts[*index] || loops.loops[*index].latches.size() != 1 ||
            (counts[*index]->iterations != 0 &&
             iterations > std::numeric_limits<std::uint64_t>::max() /
                              counts[*index]->iterations)) {
          exact = false;
          break;
        }
        iterations *= counts[*index]->iterations;
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
              opaqueLoopMutation(candidate)) {
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
          stateBeforeInstruction(body, aliases, incoming, addition);
      const std::optional<IntegerFact> delta =
          beforeAddition
              ? integerForOperand(body, *beforeAddition, *deltaOperand)
              : std::nullopt;
      if (!initial || !singleton(*initial) || initial->domain != *domain ||
          !delta || delta->domain != *domain || singleton(*delta) ||
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
        evidence = merged(std::move(evidence), counts[index]->evidence);
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
        for (Edge &edge : outgoingEdges(body, aliases, block, transfer)) {
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
  const PlaceThresholds thresholds =
      recurrenceThresholds(body, aliases, *dominance, loops, initial);
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
      const FixedArrayIndex fixed = fixedArrayIndex(body, state, instruction);
      if (fixed.index &&
          !normalizeCheckedInteger(fixed.index->lower).negative &&
          compare(fixed.index->upper,
                  CheckedIntegerValue{.magnitude = fixed.length}) < 0) {
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

      const bool sparseStorageRead =
          instruction.kind == MirInstructionKind::Call &&
          (instruction.intrinsic == IntrinsicKind::StorageRead ||
           instruction.intrinsic == IntrinsicKind::StorageReadMut) &&
          instruction.operands.size() >= 2;
      const bool prefixStorageRead =
          instruction.kind == MirInstructionKind::Call &&
          (instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
           instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut) &&
          instruction.operands.size() >= 2;
      if (sparseStorageRead || prefixStorageRead) {
        const std::optional<StorageFact> storage =
            storageForOperand(state, aliases, instruction.operands[0]);
        const std::optional<MirPlaceId> origin =
            prefixStorageRead
                ? stableStorageOriginForOperand(state, aliases,
                                                instruction.operands[0])
                : storageOriginForOperand(state, aliases,
                                          instruction.operands[0]);
        const std::optional<IntegerFact> index =
            integerForOperand(body, state, instruction.operands[1]);
        std::optional<Evidence> boundsEvidence;
        if (prefixStorageRead && storage && index && storage->length &&
            within(*index, *storage->length)) {
          boundsEvidence = merged(storage->evidence, index->evidence);
        } else if (prefixStorageRead && origin) {
          const std::optional<ScalarOrigin> scalar =
              scalarOriginForOperand(state, aliases, instruction.operands[1]);
          if (scalar) {
            const auto checked =
                state.checkedPrefixBounds.find(std::pair{*origin, *scalar});
            if (checked != state.checkedPrefixBounds.end()) {
              boundsEvidence = checked->second;
            }
          }
        } else if (sparseStorageRead && storage && index &&
                   within(*index, storage->capacity)) {
          boundsEvidence = merged(storage->evidence, index->evidence);
        }
        if (boundsEvidence) {
          Evidence evidence = std::move(*boundsEvidence);
          if (origin) {
            addPlace(evidence, *origin);
          }
          appendProof(analysis, address, block, instruction,
                      MirSafetyCheckFamily::Bounds,
                      {.code = DefinedFailureCode::IndexOutOfBounds,
                       .detail = DefinedFailureDetail::PrivateStorage},
                      evidence);
          const std::optional<std::uint64_t> exact =
              index ? nonnegativeSingleton(*index) : std::nullopt;
          if (sparseStorageRead && storage && exact &&
              storage->initialized.contains(*exact)) {
            appendProof(analysis, address, block, instruction,
                        MirSafetyCheckFamily::StorageState,
                        {.code = DefinedFailureCode::InvalidStorageState,
                         .detail = DefinedFailureDetail::UninitializedAccess},
                        std::move(evidence));
          }
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

[[nodiscard]] const MirClassInstance *classForType(const MirProgram &program,
                                                   const SemanticType &type) {
  const SemanticType *referent = &type;
  if (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
    referent = &type.arguments.front();
  }
  const auto found = std::find_if(program.classInstances().begin(),
                                  program.classInstances().end(),
                                  [&](const MirClassInstance &instance) {
                                    return instance.type == *referent;
                                  });
  return found == program.classInstances().end() ? nullptr : &*found;
}

[[nodiscard]] bool indexedElementBorrowBody(const MirBody &body,
                                            AccessMode access) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone) {
        continue;
      }
      if (instruction.rawMemoryAccess ||
          instruction.unsafeOperation != UnsafeOperationKind::None ||
          instruction.synchronization.kind !=
              SynchronizationOperationKind::None ||
          instruction.kind == MirInstructionKind::Construct ||
          instruction.kind == MirInstructionKind::Drop ||
          instruction.kind == MirInstructionKind::Modify ||
          instruction.kind == MirInstructionKind::Assign) {
        return false;
      }
      if (instruction.kind != MirInstructionKind::Call) {
        continue;
      }
      if (instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
          instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut ||
          instruction.intrinsic == IntrinsicKind::PrefixStorageLength) {
        continue;
      }
      if (instruction.intrinsic != IntrinsicKind::StorageBoundsCheck) {
        return false;
      }
    }
  }

  bool foundReturn = false;
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind != MirTerminatorKind::Return) {
      continue;
    }
    if (!block.terminator.returnLoan) {
      return false;
    }
    const MirLoan *returned = body.findLoan(*block.terminator.returnLoan);
    const MirPlace *source =
        returned == nullptr ? nullptr : body.findPlace(returned->source);
    if (returned == nullptr || returned->kind != MirLoanKind::Return ||
        returned->parent != 0 || returned->source == 0 ||
        returned->access != access || returned->producedBy == 0 ||
        !returned->carriers.empty() || returned->entry || !returned->escapes ||
        source == nullptr || source->root != MirPlaceRootKind::This ||
        source->type.kind != SemanticType::PrefixStorage) {
      return false;
    }

    const MirInstruction *producer = nullptr;
    for (const MirBlock &candidateBlock : body.blocks) {
      for (const MirInstruction &instruction : candidateBlock.instructions) {
        if (instruction.tombstone ||
            instruction.hirValue != returned->producedBy ||
            instruction.kind != MirInstructionKind::Call ||
            instruction.intrinsic != (access == AccessMode::Mutable
                                          ? IntrinsicKind::PrefixStorageReadMut
                                          : IntrinsicKind::PrefixStorageRead) ||
            !instruction.loan || instruction.operands.size() < 2) {
          continue;
        }
        if (producer != nullptr) {
          return false;
        }
        producer = &instruction;
      }
    }
    const MirLoan *produced =
        producer == nullptr ? nullptr : body.findLoan(*producer->loan);
    const MirInstruction *storageLoad =
        producer != nullptr && producer->operands.front().value != 0
            ? definingInstruction(body, producer->operands.front().value)
            : nullptr;
    if (producer == nullptr || producer->borrowAccess != access ||
        produced == nullptr || produced->kind != MirLoanKind::CallResult ||
        produced->source != returned->source || produced->access != access ||
        produced->producedBy != returned->producedBy || produced->escapes ||
        storageLoad == nullptr ||
        storageLoad->kind != MirInstructionKind::Load ||
        storageLoad->operands.size() != 1 ||
        storageLoad->operands.front().place != returned->source) {
      return false;
    }
    foundReturn = true;
  }
  return foundReturn;
}

[[nodiscard]] bool admitsIndexedElements(const MirProgram &program,
                                         const SemanticType &type) {
  const MirClassInstance *owner = classForType(program, type);
  if (owner == nullptr) {
    return false;
  }
  bool found = false;
  for (const MirFunctionInstance &function : program.functionInstances()) {
    if (function.owner != owner->id ||
        function.overloadedOperator != OverloadedOperator::Subscript) {
      continue;
    }
    found = true;
    if (function.definitionKind != MirDefinitionKind::Source ||
        function.virtualMethod || function.pureVirtual ||
        function.returnBorrowOrigin != BorrowOriginKind::Receiver ||
        function.returnType.kind != SemanticType::Reference ||
        function.returnType.arguments.size() != 1 ||
        !indexedElementBorrowBody(function.body, function.returnBorrowAccess)) {
      return false;
    }
  }
  return found;
}

[[nodiscard]] bool
indexedAggregatePlace(const MirProgram *program, const MirBody &body,
                      const MirPlaceAliasEscapeAnalysis &aliases,
                      MirPlaceId placeId, MirPlaceId aggregateRootId) {
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return false;
  }
  if (std::any_of(place->projections.begin(), place->projections.end(),
                  [](const MirPlaceProjection &projection) {
                    return projection.kind == MirProjectionKind::Index ||
                           projection.kind == MirProjectionKind::RawIndex;
                  })) {
    return true;
  }
  if (program == nullptr || aggregateRootId == 0 ||
      place->root != MirPlaceRootKind::Loan || place->loan == 0 ||
      !place->projections.empty()) {
    return false;
  }

  const MirLoan *loan = body.findLoan(place->loan);
  if (loan == nullptr || loan->kind != MirLoanKind::CallResult ||
      loan->parent != 0 || loan->source == 0 || loan->producedBy == 0 ||
      !loan->carriers.empty() || loan->entry || loan->escapes ||
      canonicalPlace(aliases, loan->source) != aggregateRootId) {
    return false;
  }

  const MirInstruction *producer = nullptr;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone ||
          instruction.kind != MirInstructionKind::Call ||
          instruction.hirValue != loan->producedBy || !instruction.loan ||
          *instruction.loan != loan->id) {
        continue;
      }
      if (producer != nullptr) {
        return false;
      }
      producer = &instruction;
    }
  }
  if (producer == nullptr || !producer->functionTarget || !producer->receiver ||
      producer->receiver->place == 0 ||
      canonicalPlace(aliases, producer->receiver->place) != aggregateRootId ||
      producer->operands.size() != 1 ||
      producer->borrowOrigin != BorrowOriginKind::Receiver ||
      producer->borrowAccess != loan->access ||
      producer->info.type != place->type) {
    return false;
  }

  const MirFunctionInstance *target =
      program->findFunctionInstance(*producer->functionTarget);
  const MirPlace *root = body.findPlace(aggregateRootId);
  const MirClassInstance *owner =
      root == nullptr ? nullptr : classForType(*program, root->type);
  if (target == nullptr || owner == nullptr || !target->owner ||
      *target->owner != owner->id ||
      target->definitionKind != MirDefinitionKind::Source ||
      target->linkage != LanguageLinkage::Gti || target->virtualMethod ||
      target->pureVirtual ||
      target->overloadedOperator != OverloadedOperator::Subscript ||
      target->parameterTypes.size() != 1 ||
      target->returnBorrowOrigin != BorrowOriginKind::Receiver ||
      target->returnBorrowAccess != loan->access ||
      target->returnType.kind != SemanticType::Reference ||
      target->returnType.arguments.size() != 1 ||
      target->returnType.arguments.front() != place->type ||
      (loan->access == AccessMode::Mutable
           ? target->receiverMutability != ReceiverMutability::Mutable
           : target->receiverMutability != ReceiverMutability::ReadOnly) ||
      !indexedElementBorrowBody(target->body, loan->access)) {
    return false;
  }
  return true;
}

void sanitizeObject(const MirProgram &program, const SemanticType &type,
                    ObjectFact &object) {
  const bool admitted = admitsIndexedElements(program, type);
  if (!admitted) {
    object.elements.reset();
  }
}

[[nodiscard]] bool mergeObject(ObjectFact &target, const ObjectFact &incoming,
                               bool widen) {
  const ObjectFact before = target;
  joinIntegers(target.integers, incoming.integers, widen);
  for (auto iterator = target.storages.begin();
       iterator != target.storages.end();) {
    const auto found = incoming.storages.find(iterator->first);
    if (found == incoming.storages.end()) {
      iterator = target.storages.erase(iterator);
      continue;
    }
    joinStorageFact(iterator->second, found->second, widen);
    ++iterator;
  }
  joinOptionalInteger(target.elements, incoming.elements, widen);
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
                                const ClosedCallContext &incoming) {
  if (!incoming.reached) {
    return false;
  }
  if (!target.reached) {
    target = incoming;
    return true;
  }
  const ClosedCallContext before = target;
  mergeObserved(target.receiver, incoming.receiver,
                [](ObjectFact &left, const ObjectFact &right) {
                  (void)mergeObject(left, right, false);
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
                  [](IntegerFact &left, const IntegerFact &right) {
                    left = joinedInteger(left, right, false);
                  });
    mergeObserved(target.argumentObjects[index], incomingObject,
                  [](ObjectFact &left, const ObjectFact &right) {
                    (void)mergeObject(left, right, false);
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
    if (root != 0) {
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
  const AbstractState entry =
      entryStateForContext(program, address, *body, aliases, context);
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
  for (const MirBlock &block : body->blocks) {
    if (block.id == 0 || block.id > incoming.size() ||
        !incoming[block.id - 1].reachable ||
        block.terminator.kind != MirTerminatorKind::Return) {
      continue;
    }
    BlockTransfer transferred =
        transferBlock(*body, aliases, block, incoming[block.id - 1], nullptr,
                      &round, address);
    if (!transferred.output.reachable) {
      continue;
    }
    if (block.terminator.value) {
      returnFacts.push_back(integerForOperand(*body, transferred.output,
                                              *block.terminator.value));
    }
    exits.push_back(std::move(transferred.output));
  }
  if (exits.empty() || round.incompleteBodies.contains(address)) {
    return result;
  }
  result.summary.complete = true;

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
    if (owner != nullptr && root != 0) {
      result.summary.receiver =
          objectAtExits(program, *body, aliases, exits, root, owner->type);
    }
  }
  if (address.kind == MirBodyKind::Constructor) {
    const MirConstructorInstance *constructor =
        program.findConstructorInstance(address.owner);
    const MirClassInstance *owner =
        constructor == nullptr ? nullptr
                               : program.findClassInstance(constructor->owner);
    const MirPlaceId root = thisRoot(*body);
    if (owner != nullptr && root != 0) {
      result.summary.constructed =
          objectAtExits(program, *body, aliases, exits, root, owner->type);
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
        if (instruction.rawMemoryAccess ||
            instruction.unsafeOperation != UnsafeOperationKind::None ||
            instruction.synchronization.kind !=
                SynchronizationOperationKind::None ||
            instruction.nativeCallbackAdapter ||
            instruction.threadTaskAdapter) {
          rejected.insert(address);
        }
        if (const std::optional<MirBodyAddress> target =
                closedCallTarget(program, instruction)) {
          graph[address].push_back(*target);
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
  if (body == nullptr) {
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
  constexpr std::size_t kMaximumIterations = 32;
  constexpr std::size_t kMaximumContextUpdates = 8;

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program);
  std::set<MirBodyAddress, MirBodyAddressLess> rejected =
      rejectedClosedCallBodies(program);
  std::map<MirBodyAddress, std::size_t, MirBodyAddressLess> contextUpdates;
  std::map<MirBodyAddress, ClosedCallContext, MirBodyAddressLess> contexts =
      rootClosedCallContexts(program);
  std::map<MirBodyAddress, ClosedCallSummary, MirBodyAddressLess> summaries;
  std::map<MirBodyAddress, MirSafetyAnalysis, MirBodyAddressLess>
      contextualSafety;

  MirClosedCallSafetyAnalysis result;
  for (std::size_t iteration = 0; iteration < kMaximumIterations; ++iteration) {
    ClosedCallRound round{
        .program = program, .summaries = summaries, .rejected = rejected};
    std::map<MirBodyAddress, ClosedCallSummary, MirBodyAddressLess>
        nextSummaries;
    std::map<MirBodyAddress, MirSafetyAnalysis, MirBodyAddressLess> nextSafety;
    for (const auto &[address, context] : contexts) {
      if (!context.reached || rejected.contains(address)) {
        continue;
      }
      ContextualBodyResult body =
          analyzeContextualBody(program, address, context, round);
      if (!body.summary.complete) {
        continue;
      }
      nextSummaries.emplace(address, std::move(body.summary));
      nextSafety.emplace(address, std::move(body.safety));
    }

    std::stable_sort(round.discovered.begin(), round.discovered.end(),
                     [](const auto &left, const auto &right) {
                       return MirBodyAddressLess{}(left.first, right.first);
                     });
    auto nextContexts = contexts;
    std::set<MirBodyAddress, MirBodyAddressLess> changedContexts;
    for (const auto &[address, context] : round.discovered) {
      if (!rejected.contains(address) &&
          mergeContext(nextContexts[address], context)) {
        changedContexts.insert(address);
      }
    }
    for (const MirBodyAddress address : changedContexts) {
      if (++contextUpdates[address] <= kMaximumContextUpdates) {
        continue;
      }
      // A mutable summary can feed a wider post-call fact back into a later
      // call to the same body even when the static call graph is acyclic.
      // Bound that contextual chain per callee so one unstable family cannot
      // consume the whole-program budget or suppress independent proofs.
      rejected.insert(address);
      nextContexts.erase(address);
      nextSummaries.erase(address);
      nextSafety.erase(address);
    }

    const bool stable = nextContexts == contexts && nextSummaries == summaries;
    contexts = std::move(nextContexts);
    summaries = std::move(nextSummaries);
    contextualSafety = std::move(nextSafety);
    result.iterations = iteration + 1;
    if (stable) {
      result.converged = true;
      break;
    }
  }

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
                  summary != summaries.end() && summary->second.complete;
    body.safety = analyzeBodyLocalSafety(
        program, address, analyzeMirPlaceAliasEscape(program, address));
    if (body.closed) {
      if (const auto contextual = contextualSafety.find(address);
          contextual != contextualSafety.end()) {
        body.safety.proofs.insert(body.safety.proofs.end(),
                                  contextual->second.proofs.begin(),
                                  contextual->second.proofs.end());
      }
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
