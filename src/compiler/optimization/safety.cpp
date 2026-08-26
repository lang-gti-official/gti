#include "gti/optimization/safety.h"

#include "gti/checked_integer.h"
#include "gti/mir_dominance.h"
#include "gti/optimization/alias.h"
#include "gti/optimization/dataflow.h"
#include "gti/optimization/effects.h"
#include "gti/optimization/loops.h"

#include <algorithm>
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
  std::set<std::uint64_t> initialized;
  Evidence evidence;

  friend bool operator==(const StorageFact &, const StorageFact &) = default;
};

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

  friend bool operator==(const AbstractState &,
                         const AbstractState &) = default;
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
  return IntegerFact{
      .domain = left.domain,
      .lower = *std::min_element(results.begin(), results.end(), less),
      .upper = *std::max_element(results.begin(), results.end(), less),
      .evidence = merged(left.evidence, right.evidence)};
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
    if (checkedIntegerFits(left->lower, target) &&
        checkedIntegerFits(left->upper, target)) {
      result = IntegerFact{.domain = target,
                           .lower = left->lower,
                           .upper = left->upper,
                           .evidence = left->evidence};
    }
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
    state.loadedOrigins.clear();
    state.storageValues.clear();
    state.storageOrigins.clear();
    state.storages.clear();
    return;
  }
  const auto overlaps = [&](MirPlaceId place) {
    return aliases.relation(destination, place) != MirAliasRelation::NoAlias;
  };
  std::erase_if(state.integerPlaces,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.arrayElements,
                [&](const auto &entry) { return overlaps(entry.first); });
  std::erase_if(state.loadedOrigins,
                [&](const auto &entry) { return overlaps(entry.second); });
  std::erase_if(state.storages,
                [&](const auto &entry) { return overlaps(entry.first); });
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

void killUnknownMemory(AbstractState &state) {
  state.integerPlaces.clear();
  state.arrayElements.clear();
  state.loadedOrigins.clear();
  state.storageValues.clear();
  state.storages.clear();
  state.storageOrigins.clear();
}

void applyInstruction(const MirBody &body,
                      const MirPlaceAliasEscapeAnalysis &aliases,
                      const MirBlock &block, const MirInstruction &instruction,
                      AbstractState &state,
                      const PlaceThresholds *thresholds = nullptr) {
  if (!state.reachable || instruction.tombstone) {
    return;
  }
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
      const auto arrayElements = state.arrayElements.find(arrayPlace);
      const std::optional<IntegerFact> fact =
          projected.arrayPlace != 0 &&
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
        state.storageOrigins[*instruction.result] = sourcePlace;
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
    const auto previousArray = state.arrayElements.find(arrayPlace);
    const std::optional<IntegerFact> previousElements =
        projected.arrayPlace != 0 && previousArray != state.arrayElements.end()
            ? std::optional<IntegerFact>{previousArray->second}
            : std::nullopt;
    const MirOperand &source = instruction.operands.front();
    const std::optional<IntegerFact> fact =
        source.place != 0
            ? integerForPlace(body, state,
                              canonicalPlace(aliases, source.place))
            : integerForOperand(body, state, source);
    const std::optional<StorageFact> storage =
        storageForOperand(state, aliases, source);
    const std::optional<IntegerFact> sourceArray =
        arrayForOperand(state, aliases, source);
    invalidatePlaceRoot(body, aliases, state, destination);
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
      IntegerFact elements =
          previousElements ? joinedInteger(*previousElements, *fact, false)
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
    if (storage) {
      StorageFact stored = *storage;
      add(stored.evidence, block.id);
      addInstruction(stored.evidence, instruction.id);
      addPlace(stored.evidence, destination);
      state.storages[destination] = std::move(stored);
    } else if (const MirPlace *place = body.findPlace(destination);
               place != nullptr && place->type.kind == SemanticType::Storage) {
      state.storages.erase(destination);
    }
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

  // Calls outside the admitted private-storage vocabulary may mutate any
  // reachable place through a reference or opaque host boundary. Value facts
  // remain valid because MIR values are immutable.
  killUnknownMemory(state);
}

[[nodiscard]] BlockTransfer
transferBlock(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
              const MirBlock &block, AbstractState state,
              const PlaceThresholds *thresholds = nullptr) {
  BlockTransfer result;
  for (const MirInstruction &instruction : block.instructions) {
    result.before.insert_or_assign(instruction.id, state);
    applyInstruction(body, aliases, block, instruction, state, thresholds);
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

void joinStorageFacts(std::map<MirPlaceId, StorageFact> &target,
                      const std::map<MirPlaceId, StorageFact> &incoming,
                      bool widen) {
  for (auto iterator = target.begin(); iterator != target.end();) {
    const auto found = incoming.find(iterator->first);
    if (found == incoming.end()) {
      iterator = target.erase(iterator);
      continue;
    }
    iterator->second.capacity =
        joinedInteger(iterator->second.capacity, found->second.capacity, widen);
    std::set<std::uint64_t> intersection;
    std::set_intersection(iterator->second.initialized.begin(),
                          iterator->second.initialized.end(),
                          found->second.initialized.begin(),
                          found->second.initialized.end(),
                          std::inserter(intersection, intersection.end()));
    iterator->second.initialized = std::move(intersection);
    iterator->second.evidence =
        merged(iterator->second.evidence, found->second.evidence);
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
    iterator->second.capacity =
        joinedInteger(iterator->second.capacity, found->second.capacity, widen);
    std::set<std::uint64_t> intersection;
    std::set_intersection(iterator->second.initialized.begin(),
                          iterator->second.initialized.end(),
                          found->second.initialized.begin(),
                          found->second.initialized.end(),
                          std::inserter(intersection, intersection.end()));
    iterator->second.initialized = std::move(intersection);
    iterator->second.evidence =
        merged(iterator->second.evidence, found->second.evidence);
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
  joinStorageValues(target.storageValues, incoming.storageValues, widen);
  joinStorageFacts(target.storages, incoming.storages, widen);
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

void bindEdgeArguments(const MirBody &body, MirBlockId target,
                       const std::vector<MirOperand> &arguments,
                       AbstractState &state) {
  const MirBlock *block = body.findBlock(target);
  if (block == nullptr || block->parameters.size() != arguments.size()) {
    return;
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const MirValueId parameter = block->parameters[index].value;
    if (const std::optional<IntegerFact> fact =
            integerForOperand(body, state, arguments[index])) {
      IntegerFact bound = *fact;
      addValue(bound.evidence, parameter);
      state.integers[parameter] = std::move(bound);
    } else {
      state.integers.erase(parameter);
    }
    const MirOperand &argument = arguments[index];
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
                const PlaceThresholds *thresholds = nullptr) {
  AbstractState entry;
  entry.reachable = true;
  const MirForwardDataflowResult<AbstractState> solved = runMirForwardDataflow(
      body, std::move(entry),
      [&](const MirBlock &block, const AbstractState &incoming) {
        const BlockTransfer transfer =
            transferBlock(body, aliases, block, incoming, thresholds);
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
solveStates(const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases) {
  const std::vector<AbstractState> initial = solveStatesOnce(body, aliases);
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
  return solveStatesOnce(body, aliases, &thresholds);
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
                   MirSafetyAnalysis &analysis) {
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

      if (instruction.kind == MirInstructionKind::Compute) {
        const std::optional<IntegerFact> result =
            computeIntegerFact(body, state, instruction);
        DefinedFailureDetail detail = DefinedFailureDetail::None;
        if (instruction.operation == MirOperation::Add) {
          detail = DefinedFailureDetail::Addition;
        } else if (instruction.operation == MirOperation::Subtract) {
          detail = DefinedFailureDetail::Subtraction;
        } else if (instruction.operation == MirOperation::Multiply) {
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

      const bool storageRead =
          instruction.kind == MirInstructionKind::Call &&
          (instruction.intrinsic == IntrinsicKind::StorageRead ||
           instruction.intrinsic == IntrinsicKind::StorageReadMut) &&
          instruction.operands.size() >= 2;
      if (storageRead) {
        const std::optional<StorageFact> storage =
            storageForOperand(state, aliases, instruction.operands[0]);
        const std::optional<MirPlaceId> origin =
            storageOriginForOperand(state, aliases, instruction.operands[0]);
        const std::optional<IntegerFact> index =
            integerForOperand(body, state, instruction.operands[1]);
        if (storage && index && within(*index, storage->capacity)) {
          Evidence evidence = merged(storage->evidence, index->evidence);
          if (origin) {
            addPlace(evidence, *origin);
          }
          appendProof(analysis, address, block, instruction,
                      MirSafetyCheckFamily::Bounds,
                      {.code = DefinedFailureCode::IndexOutOfBounds,
                       .detail = DefinedFailureDetail::PrivateStorage},
                      evidence);
          const std::optional<std::uint64_t> exact =
              nonnegativeSingleton(*index);
          if (exact && storage->initialized.contains(*exact)) {
            appendProof(analysis, address, block, instruction,
                        MirSafetyCheckFamily::StorageState,
                        {.code = DefinedFailureCode::InvalidStorageState,
                         .detail = DefinedFailureDetail::UninitializedAccess},
                        std::move(evidence));
          }
        }
      }
      applyInstruction(body, aliases, block, instruction, state);
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

MirSafetyAnalysis analyzeMirSafetyChecks(const MirProgram &program,
                                         MirBodyAddress address) {
  return analyzeMirSafetyChecks(program, address,
                                analyzeMirPlaceAliasEscape(program, address));
}

MirSafetyAnalysis
analyzeMirSafetyChecks(const MirProgram &program, MirBodyAddress address,
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

} // namespace lang
