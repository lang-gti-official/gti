#include "gti/optimization/safety.h"
#include "evidence_set.h"
#include "gti/optimization/coverage.h"

#include "../mir_verification_queries.h"
#include "gti/checked_integer.h"
#include "gti/mir_dominance.h"
#include "gti/mir_effects.h"
#include "gti/optimization/alias.h"
#include "gti/optimization/dataflow.h"
#include "gti/optimization/loops.h"

#include <algorithm>
#include <compare>
#include <deque>
#include <iterator>
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

struct FieldOrigin {
  MirPlaceId root = 0;
  FieldPath path;
  friend bool operator==(const FieldOrigin &, const FieldOrigin &) = default;
  friend auto operator<=>(const FieldOrigin &, const FieldOrigin &) = default;
};

enum class AddressOriginKind { ExistingPlace, EntryPointer, EntryPointerField };

// One symbolic element displacement, equal to an owned receiver field at
// entry or a readonly integer formal plus a bounded checked offset. This names
// one slot, not every element in the input's calling-context interval.
struct EntryIndex {
  FieldPath field;
  CheckedIntegerDomain domain;
  CheckedIntegerValue offset;
  std::optional<std::size_t> parameter;
  friend bool operator==(const EntryIndex &, const EntryIndex &) = default;
  friend auto operator<=>(const EntryIndex &left, const EntryIndex &right) {
    return std::tie(left.parameter, left.field, left.domain.width,
                    left.domain.signedValue, left.offset.negative,
                    left.offset.magnitude) <=>
           std::tie(right.parameter, right.field, right.domain.width,
                    right.domain.signedValue, right.offset.negative,
                    right.offset.magnitude);
  }
};

// Actual address identity, distinct from a loan's lifetime witness. An
// ExistingPlace denotes live scalar storage; EntryPointer denotes only the
// address passed into a raw pointer formal, not that formal's own storage.
// EntryPointerField similarly names the initial value of one ordinary owned
// pointer field, not the field's storage. Neither establishes a pointee,
// allocation, extent, disjointness or initialization fact.
struct ScalarAddress {
  AddressOriginKind kind = AddressOriginKind::ExistingPlace;
  MirPlaceId place = 0;
  // Field addresses are relative to an ordinary object root, which may be the
  // implicit This root zero. Repeated indirect place IDs for the same owned
  // field path do not become different pointer values.
  FieldPath field;
  SemanticType fieldPointee;
  std::uint64_t elementOffset = 0;
  std::optional<EntryIndex> index;
  // Zero denotes an entry identity. Nonzero names a body-local observation of
  // a current field value, never a fresh allocation or an exportable formal.
  MirInstructionId observation = 0;
  FieldPath subobject;
  SemanticType subobjectType;
  Evidence evidence;
  friend bool operator==(const ScalarAddress &,
                         const ScalarAddress &) = default;
};

// One exact typed address, never an allocation extent or a whole initialized
// region. Different bases are not a disjointness proof. Successful construction
// or an exact checked scalar store establishes a pointee fact; unsupported
// writes/destruction invalidate it.
struct ScalarAddressKey {
  AddressOriginKind kind;
  MirPlaceId place;
  FieldPath field;
  std::uint64_t elementOffset;
  std::optional<EntryIndex> index;
  MirInstructionId observation = 0;
  FieldPath subobject;
  friend auto operator<=>(const ScalarAddressKey &,
                          const ScalarAddressKey &) = default;
};

[[nodiscard]] ScalarAddressKey addressKey(const ScalarAddress &address) {
  return {address.kind,          address.place, address.field,
          address.elementOffset, address.index, address.observation,
          address.subobject};
}

// One element at an unknown displacement within `offsets` of an exact base,
// such as a caller-local loop index. It answers initialized-prefix queries
// over that whole interval and never names one exact cell: it is never a
// constructed-scalar, field-address or loan-retained address identity.
struct AddressRegion {
  ScalarAddress base;
  IntegerFact offsets;
  friend bool operator==(const AddressRegion &,
                         const AddressRegion &) = default;
};

// Regions keep a zero base displacement and unsigned 64-bit offsets, so one
// loan's regions from different paths compare by their exact base alone. An
// index-free exact address is the region of its single displacement.
[[nodiscard]] std::optional<AddressRegion>
normalizedRegion(ScalarAddress base, IntegerFact offsets) {
  const auto shift = base.elementOffset;
  if (base.index || offsets.lower.negative || offsets.upper.negative ||
      offsets.upper.magnitude >
          std::numeric_limits<std::uint64_t>::max() - shift)
    return std::nullopt;
  if (shift != 0)
    offsets.wideningEnvelope.reset();
  offsets.lower.magnitude += shift;
  offsets.upper.magnitude += shift;
  offsets.domain = {.width = 64, .signedValue = false};
  base.elementOffset = 0;
  return AddressRegion{std::move(base), std::move(offsets)};
}

[[nodiscard]] std::optional<AddressRegion>
exactRegion(const ScalarAddress &address) {
  return normalizedRegion(
      address, IntegerFact{.domain = {.width = 64, .signedValue = false},
                           .lower = {},
                           .upper = {},
                           .evidence = address.evidence});
}

struct ConstructedScalar {
  IntegerFact value;
  // Exact equality to a read-only by-value formal or its owned scalar field,
  // not matching interval endpoints. Summary substitution uses a formal
  // ordinal and optional field path, never another body's local identities.
  std::optional<std::size_t> valueParameter;
  // Exact non-byte owned fields already known live when this vacant object
  // was constructed. Their storage cannot overlap the new object. This is a
  // lifetime relation captured then, not inferred from later scalar facts.
  std::vector<FieldOrigin> disjointFields;
  FieldPath valueField;
  friend bool operator==(const ConstructedScalar &,
                         const ConstructedScalar &) = default;
};
using ConstructedScalars = std::map<ScalarAddressKey, ConstructedScalar>;

struct InputAddress {
  // A missing ordinal denotes the receiver. An empty path denotes a raw
  // pointer formal; otherwise the address is that input object's entry field
  // value. No callee-local place identity crosses the call boundary.
  std::optional<std::size_t> parameter;
  FieldPath field;
  std::uint64_t elementOffset;
  std::optional<EntryIndex> index;
  FieldPath subobject;
  friend auto operator<=>(const InputAddress &, const InputAddress &) = default;
};

// Equality with one owned receiver field at entry, not an interval inferred
// from matching endpoints. Offsets are bounded checked signed-64 values;
// arithmetic in the actual field still obeys its own checked integer domain.
struct EntryRelativeField {
  FieldPath field;
  CheckedIntegerDomain domain;
  CheckedIntegerValue offset;
  Evidence evidence;
  friend bool operator==(const EntryRelativeField &,
                         const EntryRelativeField &) = default;
};
using EntryRelativeFields = std::map<FieldPath, EntryRelativeField>;

[[nodiscard]] std::optional<FieldPath> fieldPath(const MirBody &body,
                                                 MirPlaceId rootId,
                                                 MirPlaceId placeId,
                                                 bool includeRoot);
[[nodiscard]] MirPlaceId thisRoot(const MirBody &body);
[[nodiscard]] MirPlaceId rootForBinding(const MirBody &body,
                                        HirBindingId binding);
[[nodiscard]] bool liveScalarPath(const MirProgram *program,
                                  MirBodyAddress address, const MirBody &body,
                                  MirPlaceId root, const FieldPath &suffix);
[[nodiscard]] const SemanticType *liveOwnedPathType(const MirProgram *program,
                                                    MirBodyAddress address,
                                                    const MirBody &body,
                                                    MirPlaceId root,
                                                    const FieldPath &suffix);
struct AbstractState;
void publishFieldFact(const MirBody &body, AbstractState &state,
                      const FieldOrigin &origin, const IntegerFact &fact);

struct PointeeField {
  FieldPath pointer;
  FieldPath subobject;
  friend auto operator<=>(const PointeeField &, const PointeeField &) = default;
};

// Every element in [0, the current end field) at the current pointer field
// has this scalar/subobject envelope. This is a coverage relation, not an
// allocation extent or a promise about uninitialized capacity.
struct PrefixField {
  FieldPath pointer;
  FieldPath end;
  FieldPath subobject;
  friend auto operator<=>(const PrefixField &, const PrefixField &) = default;
};
using PrefixFacts = std::map<PrefixField, IntegerFact>;

struct ObjectFact {
  std::map<FieldPath, IntegerFact> integers;
  // Mutable contents at exact ordinary owned pointer-field values. Only
  // successful scalar construction/checked stores supply these facts; they
  // imply neither allocation extent nor disjointness from other pointer fields.
  std::map<PointeeField, IntegerFact> scalarPointees;
  PrefixFacts prefixes;
  std::optional<IntegerFact> elements;
  IntegerFieldFacts elementIntegers;

  friend bool operator==(const ObjectFact &, const ObjectFact &) = default;
};

// One verified result alternative guards facts about still-current owned
// fields. This is not an allocation/region invariant. The first observer
// profile is MIR's built-in two-alternative result state (success=0,error=1).
using AlternativeFields =
    std::map<std::size_t, std::map<FieldOrigin, IntegerFact>>;

struct InputField {
  std::optional<std::size_t> parameter;
  FieldPath field;
  friend auto operator<=>(const InputField &, const InputField &) = default;
};

// These are transfer-rule barriers, not replacement MIR effects. A vacant
// structural construction remains opaque, but has a narrower supported transfer
// than an unhandled operation which might write an already live object.
enum class LiveMemoryBarrier { None, VacantStructuralConstruction, Unknown };

[[nodiscard]] bool mergeObject(ObjectFact &target, const ObjectFact &incoming,
                               bool widen);

struct AbstractState {
  bool reachable = false;
  // Unknown effects on a failure-only path must not poison a successful
  // result summary, but an opaque effect on any successful path must survive
  // joins even when all scalar result facts later become constant.
  bool hasOpaqueEffects = false;
  bool mayWriteGlobals = false;
  LiveMemoryBarrier liveMemoryBarrier = LiveMemoryBarrier::None;
  std::map<MirValueId, IntegerFact> integers;
  std::map<MirPlaceId, IntegerFact> integerPlaces;
  std::map<MirValueId, IntegerFact> arrayValues;
  std::map<MirPlaceId, IntegerFact> arrayElements;
  std::map<MirValueId, MirPlaceId> loadedOrigins;
  // Correlation with still-current owned storage, not an immutable value fact.
  // An implicit receiver root is analysis-only and never a MIR identity.
  std::map<MirValueId, FieldOrigin> fieldOrigins;
  std::map<MirValueId, AlternativeFields> resultAlternatives;
  std::map<MirPlaceId, AlternativeFields> placeAlternatives;
  std::map<MirValueId, ScalarAddress> scalarAddressValues;
  std::map<MirPlaceId, ScalarAddress> scalarAddressPlaces;
  // Current values of ordinary owned pointer fields, including fields not
  // materialized as a place in this body. Keys name storage; values name the
  // address stored there. Neither alone establishes a live pointee.
  std::map<FieldOrigin, ScalarAddress> fieldAddresses;
  // Exact addresses supplied to checked reference formation. The loan's
  // witness is lifetime authority, never an alternative address source.
  std::map<MirLoanId, ScalarAddress> borrowedAddresses;
  // Checked references to some element of an exact region. They share the
  // invalidation of borrowedAddresses but never substitute for one.
  std::map<MirLoanId, AddressRegion> borrowedRegions;
  ConstructedScalars constructedScalars;
  EntryRelativeFields entryRelativeFields;
  std::map<MirValueId, EntryRelativeField> entryRelativeValues;
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

void publishConstructedScalar(AbstractState &state, ScalarAddressKey address,
                              ConstructedScalar value) {
  // Source call trees can compose exponentially many constant offsets.
  // Bounded retention loses precision, never assumes unrecorded coverage.
  constexpr std::size_t maximumAddresses = 1024;
  if (state.constructedScalars.contains(address) ||
      state.constructedScalars.size() < maximumAddresses)
    state.constructedScalars.insert_or_assign(address, std::move(value));
}

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
  bool mayWriteGlobals = false;
  LiveMemoryBarrier liveMemoryBarrier = LiveMemoryBarrier::None;
  ObservedFact<IntegerFact> result;
  ObservedFact<FieldPath> receiverResultField;
  std::optional<InputAddress> returnedPointer;
  std::optional<InputAddress> returnedBorrow;
  std::map<std::size_t, std::map<InputField, IntegerFact>> resultAlternatives;
  bool localPointerWritesOnly = false;
  std::map<InputField, InputAddress> pointerFieldValues;
  // A bounded typed-MIR mutation profile, not a replacement effect summary.
  // All writes to existing nonlocal objects are checked non-byte integers of
  // one width or checked floating scalars. Vacant structural construction is
  // independent; pointer, aggregate, byte, raw/native and destructive
  // lifecycle writes reject it.
  bool scalarStoresOnly = false;
  std::optional<unsigned> scalarStoreWidth;
  bool floatingScalarStores = false;
  std::map<InputAddress, ConstructedScalar> constructedParameters;
  ObservedFact<ObjectFact> receiver;
  EntryRelativeFields receiverUpdates;
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
  const MirProgramCaptureSummaries *captures = nullptr;
  std::map<MirBodyAddress, MirLocalCaptureAnalysis, MirBodyAddressLess>
      *localCaptureCache = nullptr;
  // Capture facts of the body this round analyzes.
  const MirLocalCaptureAnalysis *localCaptures = nullptr;
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
  optimization_detail::insertEvidenceId(evidence.blocks, block);
}

void addInstruction(Evidence &evidence, MirInstructionId instruction) {
  optimization_detail::insertEvidenceId(evidence.instructions, instruction);
}

void addValue(Evidence &evidence, MirValueId value) {
  optimization_detail::insertEvidenceId(evidence.values, value);
}

void addPlace(Evidence &evidence, MirPlaceId place) {
  optimization_detail::insertEvidenceId(evidence.places, place);
}

[[nodiscard]] Evidence merged(Evidence left, const Evidence &right) {
  optimization_detail::mergeEvidenceIds(left.blocks, right.blocks);
  optimization_detail::mergeEvidenceIds(left.instructions, right.instructions);
  optimization_detail::mergeEvidenceIds(left.values, right.values);
  optimization_detail::mergeEvidenceIds(left.places, right.places);
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

[[nodiscard]] const SemanticType *
addressPointeeType(const MirBody &body, const ScalarAddress &address) {
  if (!address.subobject.empty())
    return &address.subobjectType;
  if (address.kind == AddressOriginKind::EntryPointerField)
    return &address.fieldPointee;
  const auto *place = body.findPlace(address.place);
  if (!place)
    return nullptr;
  if (address.kind == AddressOriginKind::ExistingPlace)
    return &place->type;
  return place->type.kind == SemanticType::RawPointer &&
                 place->type.arguments.size() == 1
             ? &place->type.arguments.front()
             : nullptr;
}

[[nodiscard]] const SemanticType *
ownedPointeeFieldType(const MirProgram *program, const SemanticType &root,
                      const FieldPath &path) {
  const SemanticType *type = &root;
  if (!program || path.size() > 32)
    return path.empty() ? type : nullptr;
  for (const auto field : path) {
    if (type->kind != SemanticType::Class)
      return nullptr;
    const auto owner = std::find_if(
        program->classInstances().begin(), program->classInstances().end(),
        [&](const auto &record) { return record.type == *type; });
    if (owner == program->classInstances().end() ||
        owner->kind == ClassKind::Union || owner->unionLayout ||
        owner->compilerCapability != CompilerCapabilityTypeKind::None ||
        !owner->bases.empty() || !owner->structuralBases.empty())
      return nullptr;
    const auto member = std::find_if(
        owner->declaredFields.begin(), owner->declaredFields.end(),
        [&](const auto &candidate) { return candidate.symbol == field; });
    if (member == owner->declaredFields.end())
      return nullptr;
    type = &member->type;
  }
  return type;
}

[[nodiscard]] std::optional<ScalarAddress>
projectPointee(const MirProgram *program, const MirBody &body,
               ScalarAddress pointer, const FieldPath &path) {
  if (path.empty())
    return pointer;
  const auto *root = addressPointeeType(body, pointer);
  const auto *type =
      root ? ownedPointeeFieldType(program, *root, path) : nullptr;
  if (!type || path.size() + pointer.subobject.size() > 32)
    return std::nullopt;
  pointer.subobject.insert(pointer.subobject.end(), path.begin(), path.end());
  pointer.subobjectType = *type;
  return pointer;
}

[[nodiscard]] std::optional<ScalarAddress>
scalarAddressForOperand(const MirBody &body, const AbstractState &state,
                        const MirOperand &operand) {
  if (operand.type.kind != SemanticType::RawPointer ||
      operand.type.arguments.size() != 1)
    return std::nullopt;
  const auto found = state.scalarAddressValues.find(operand.value);
  if (operand.kind != MirOperandKind::Value ||
      found == state.scalarAddressValues.end())
    return std::nullopt;
  const auto *pointee = addressPointeeType(body, found->second);
  return pointee && *pointee == operand.type.arguments.front()
             ? std::optional{found->second}
             : std::nullopt;
}

[[nodiscard]] std::optional<ScalarAddress>
borrowedAddressForPlace(const MirProgram *program, const MirBody &body,
                        const AbstractState &state, MirPlaceId id) {
  const auto *place = body.findPlace(id);
  if (!place)
    return std::nullopt;
  std::optional<ScalarAddress> result;
  std::size_t firstField = 0;
  if (place->root == MirPlaceRootKind::Loan) {
    const auto found = state.borrowedAddresses.find(place->loan);
    if (found != state.borrowedAddresses.end())
      result = found->second;
  } else {
    if (place->root != MirPlaceRootKind::Binding ||
        place->projections.empty() ||
        place->projections.front().kind != MirProjectionKind::Dereference)
      return std::nullopt;
    firstField = 1;
    for (const auto &[id, pointer] : state.borrowedAddresses) {
      const auto *loan = body.findLoan(id);
      if (!loan || std::find(loan->carriers.begin(), loan->carriers.end(),
                             place->binding) == loan->carriers.end())
        continue;
      if (result && addressKey(*result) != addressKey(pointer))
        return std::nullopt;
      result = pointer;
    }
  }
  FieldPath path;
  for (std::size_t index = firstField; index < place->projections.size();
       ++index) {
    const auto &projection = place->projections[index];
    if (projection.kind != MirProjectionKind::Field || !projection.field)
      return std::nullopt;
    path.push_back(projection.field);
  }
  if (result)
    result = projectPointee(program, body, *result, path);
  if (result && (!addressPointeeType(body, *result) ||
                 *addressPointeeType(body, *result) != place->type))
    return std::nullopt;
  return result;
}

[[nodiscard]] bool sameRegion(const AddressRegion &left,
                              const AddressRegion &right) {
  return addressKey(left.base) == addressKey(right.base) &&
         left.base.subobjectType == right.base.subobjectType &&
         left.offsets.domain == right.offsets.domain &&
         left.offsets.lower == right.offsets.lower &&
         left.offsets.upper == right.offsets.upper;
}

// The region counterpart of borrowedAddressForPlace. Callers consult exact
// addresses first; a region never stands in for one exact cell.
[[nodiscard]] std::optional<AddressRegion>
borrowedRegionForPlace(const MirProgram *program, const MirBody &body,
                       const AbstractState &state, MirPlaceId id) {
  const auto *place = body.findPlace(id);
  if (!place)
    return std::nullopt;
  std::optional<AddressRegion> result;
  std::size_t firstField = 0;
  if (place->root == MirPlaceRootKind::Loan) {
    const auto found = state.borrowedRegions.find(place->loan);
    if (found != state.borrowedRegions.end())
      result = found->second;
  } else {
    if (place->root != MirPlaceRootKind::Binding ||
        place->projections.empty() ||
        place->projections.front().kind != MirProjectionKind::Dereference)
      return std::nullopt;
    firstField = 1;
    for (const auto &[id, region] : state.borrowedRegions) {
      const auto *loan = body.findLoan(id);
      if (!loan || std::find(loan->carriers.begin(), loan->carriers.end(),
                             place->binding) == loan->carriers.end())
        continue;
      if (result && !sameRegion(*result, region))
        return std::nullopt;
      result = region;
    }
  }
  if (!result)
    return std::nullopt;
  FieldPath path;
  for (std::size_t index = firstField; index < place->projections.size();
       ++index) {
    const auto &projection = place->projections[index];
    if (projection.kind != MirProjectionKind::Field || !projection.field)
      return std::nullopt;
    path.push_back(projection.field);
  }
  auto base = projectPointee(program, body, result->base, path);
  if (!base || !addressPointeeType(body, *base) ||
      *addressPointeeType(body, *base) != place->type)
    return std::nullopt;
  result->base = std::move(*base);
  return result;
}

[[nodiscard]] std::optional<IntegerFact>
integerInRegion(const MirBody &body, const AbstractState &state,
                const AddressRegion &region, const SemanticType &type);

[[nodiscard]] std::optional<IntegerFact>
integerInPrefix(const MirProgram *program, MirBodyAddress address,
                const MirBody &body, const AbstractState &state,
                const ScalarAddress &pointer, const SemanticType &type);

[[nodiscard]] std::optional<IntegerFact>
integerAtAddress(const MirBody &body, const AbstractState &state,
                 const ScalarAddress &pointer, const SemanticType &type,
                 const MirProgram *program = nullptr,
                 MirBodyAddress address = {}) {
  const auto *pointee = addressPointeeType(body, pointer);
  if (!pointee || *pointee != type)
    return std::nullopt;
  const auto initialized = state.constructedScalars.find(addressKey(pointer));
  if (initialized != state.constructedScalars.end() &&
      constantIntegerDomain(type) == initialized->second.value.domain)
    return initialized->second.value;
  if (pointer.kind == AddressOriginKind::ExistingPlace &&
      pointer.elementOffset == 0 && !pointer.index)
    return integerForPlace(body, state, pointer.place);
  return integerInPrefix(program, address, body, state, pointer, type);
}

template <typename Key>
void joinScalarAddresses(std::map<Key, ScalarAddress> &target,
                         const std::map<Key, ScalarAddress> &incoming) {
  for (auto entry = target.begin(); entry != target.end();) {
    const auto found = incoming.find(entry->first);
    if (found == incoming.end() || entry->second.place != found->second.place ||
        entry->second.kind != found->second.kind ||
        entry->second.field != found->second.field ||
        entry->second.fieldPointee != found->second.fieldPointee ||
        entry->second.observation != found->second.observation ||
        entry->second.subobject != found->second.subobject ||
        entry->second.subobjectType != found->second.subobjectType ||
        entry->second.index != found->second.index ||
        entry->second.elementOffset != found->second.elementOffset)
      entry = target.erase(entry);
    else {
      entry->second.evidence =
          merged(entry->second.evidence, found->second.evidence);
      ++entry;
    }
  }
}

[[nodiscard]] std::optional<ScalarAddress>
offsetAddress(ScalarAddress address, std::uint64_t offset, bool subtract) {
  if (offset && !address.subobject.empty())
    return std::nullopt;
  if (subtract) {
    if (offset > address.elementOffset)
      return std::nullopt;
    address.elementOffset -= offset;
  } else {
    if (offset >
        std::numeric_limits<std::uint64_t>::max() - address.elementOffset)
      return std::nullopt;
    address.elementOffset += offset;
  }
  return address;
}

// A pointer's SSA address value is independent of the pointee's mutable
// memory. Read-only by-value bindings likewise retain that value. This does
// not preserve liveness, initialized contents or any dereference permission.
void preserveEntryPointerIdentities(const MirBody &body, AbstractState &state,
                                    bool vacantConstruction = false) {
  if (!vacantConstruction) {
    state.fieldAddresses.clear();
    state.borrowedAddresses.clear();
    state.borrowedRegions.clear();
  }
  std::erase_if(state.scalarAddressValues, [&](const auto &entry) {
    return entry.second.kind != AddressOriginKind::EntryPointer &&
           !(vacantConstruction &&
             entry.second.kind == AddressOriginKind::EntryPointerField);
  });
  std::erase_if(state.scalarAddressPlaces, [&](const auto &entry) {
    // Constructing an object in vacant storage cannot overwrite a live owned
    // pointer field. Unknown memory writes still retire this relation.
    if (vacantConstruction &&
        entry.second.kind == AddressOriginKind::EntryPointerField)
      return false;
    const auto *place = body.findPlace(entry.first);
    return entry.second.kind != AddressOriginKind::EntryPointer || !place ||
           place->root != MirPlaceRootKind::Binding || !place->binding ||
           !place->projections.empty() ||
           place->access != AccessMode::ReadOnly ||
           place->type.kind != SemanticType::RawPointer ||
           place->traits.containsBorrowedState;
  });
}

[[nodiscard]] std::optional<EntryRelativeField>
shiftEntryRelative(EntryRelativeField value, CheckedIntegerValue offset) {
  const auto result =
      evaluateCheckedIntegerBinary(CheckedIntegerOperation::Add, value.offset,
                                   offset, {.width = 64, .signedValue = true});
  if (!result || !std::holds_alternative<CheckedIntegerValue>(*result))
    return std::nullopt;
  value.offset = std::get<CheckedIntegerValue>(*result);
  return value;
}

[[nodiscard]] std::optional<IntegerFact>
applyEntryRelative(const IntegerFact &value, const EntryRelativeField &update) {
  if (value.domain != update.domain)
    return std::nullopt;
  auto offset = normalizeCheckedInteger(update.offset);
  auto operation = CheckedIntegerOperation::Add;
  if (!checkedIntegerFits(offset, value.domain)) {
    if (!offset.negative)
      return std::nullopt;
    offset.negative = false;
    if (!checkedIntegerFits(offset, value.domain))
      return std::nullopt;
    operation = CheckedIntegerOperation::Subtract;
  }
  return binaryFact(operation, value,
                    {.domain = value.domain, .lower = offset, .upper = offset});
}

[[nodiscard]] std::optional<EntryRelativeField>
entryRelativeValue(const AbstractState &state, const MirOperand &operand) {
  if (operand.kind != MirOperandKind::Value)
    return std::nullopt;
  const auto found = state.entryRelativeValues.find(operand.value);
  return found != state.entryRelativeValues.end() &&
                 constantIntegerDomain(operand.type) == found->second.domain
             ? std::optional{found->second}
             : std::nullopt;
}

template <typename Key>
void joinEntryRelative(std::map<Key, EntryRelativeField> &target,
                       const std::map<Key, EntryRelativeField> &incoming) {
  for (auto entry = target.begin(); entry != target.end();) {
    const auto found = incoming.find(entry->first);
    if (found == incoming.end() || entry->second.field != found->second.field ||
        entry->second.domain != found->second.domain ||
        entry->second.offset != found->second.offset) {
      entry = target.erase(entry);
    } else {
      entry->second.evidence =
          merged(entry->second.evidence, found->second.evidence);
      ++entry;
    }
  }
}

[[nodiscard]] std::optional<EntryRelativeField>
computeEntryRelative(const MirBody &body, const AbstractState &state,
                     const MirInstruction &instruction,
                     MirOperation operation) {
  if (!instruction.result)
    return std::nullopt;
  const auto domain = constantIntegerDomain(instruction.info.type);
  if (operation == MirOperation::Identity && instruction.operands.size() == 1) {
    auto value = entryRelativeValue(state, instruction.operands.front());
    return value && domain == value->domain ? value : std::nullopt;
  }
  if ((operation != MirOperation::Add && operation != MirOperation::Subtract) ||
      instruction.operands.size() != 2)
    return std::nullopt;
  auto value = entryRelativeValue(state, instruction.operands[0]);
  auto constant = integerForOperand(body, state, instruction.operands[1]);
  if (!value && operation == MirOperation::Add) {
    value = entryRelativeValue(state, instruction.operands[1]);
    constant = integerForOperand(body, state, instruction.operands[0]);
  }
  if (!value || !constant || !singleton(*constant) || domain != value->domain ||
      constant->domain != value->domain)
    return std::nullopt;
  auto offset = constant->lower;
  if (operation == MirOperation::Subtract)
    offset.negative = !offset.negative;
  if (!checkedIntegerFits(normalizeCheckedInteger(offset),
                          {.width = 64, .signedValue = true}))
    return std::nullopt;
  value->evidence = merged(value->evidence, constant->evidence);
  return shiftEntryRelative(*value, offset);
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
  for (auto &[path, fact] : object.scalarPointees) {
    (void)path;
    stabilize(fact);
  }
  for (auto &[path, fact] : object.prefixes) {
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

[[nodiscard]] std::vector<HirBindingId>
parameterBindings(const MirProgram &program, MirBodyAddress address);
[[nodiscard]] std::vector<SemanticType>
parameterTypes(const MirProgram &program, MirBodyAddress address);

[[nodiscard]] std::optional<std::size_t>
parameterOrdinal(const std::vector<HirBindingId> &bindings,
                 const std::vector<SemanticType> &types,
                 const MirPlace &place) {
  if (place.root != MirPlaceRootKind::Binding || !place.binding ||
      bindings.size() != types.size())
    return std::nullopt;
  std::size_t occurrence = 0;
  if (!place.projections.empty()) {
    if (place.projections.size() != 1 ||
        place.projections.front().kind != MirProjectionKind::PackElement ||
        !place.projections.front().constantIndex)
      return std::nullopt;
    occurrence = *place.projections.front().constantIndex;
  } else if (std::count(bindings.begin(), bindings.end(), place.binding) != 1) {
    return std::nullopt;
  }
  for (std::size_t index = 0; index < bindings.size(); ++index)
    if (bindings[index] == place.binding) {
      if (occurrence == 0)
        return types[index] == place.type ? std::optional{index} : std::nullopt;
      --occurrence;
    }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::size_t>
parameterOrdinal(const MirProgram &program, MirBodyAddress address,
                 const MirPlace &place) {
  return parameterOrdinal(parameterBindings(program, address),
                          parameterTypes(program, address), place);
}

[[nodiscard]] std::vector<MirPlaceId> parameterPlaces(const MirProgram &program,
                                                      MirBodyAddress address,
                                                      const MirBody &body) {
  const auto bindings = parameterBindings(program, address);
  const auto types = parameterTypes(program, address);
  std::vector<MirPlaceId> roots(types.size());
  if (roots.empty())
    return roots;
  for (const auto &place : body.places)
    if (const auto index = parameterOrdinal(bindings, types, place))
      if (!roots[*index])
        roots[*index] = place.id;
  return roots;
}

[[nodiscard]] std::optional<IntegerFact>
prefixEnvelopeAt(const AbstractState &state, const ScalarAddress &pointer,
                 const IntegerFact &offset, const SemanticType &type) {
  // Every displacement in `offset` must lie inside one current initialized
  // prefix at this exact base; the index of `pointer` itself is ignored.
  if (offset.lower.negative)
    return std::nullopt;
  for (const auto &[field, base] : state.fieldAddresses) {
    if (base.index || !base.subobject.empty() ||
        pointer.elementOffset < base.elementOffset)
      continue;
    auto identity = addressKey(pointer);
    identity.index.reset();
    identity.elementOffset = base.elementOffset;
    identity.subobject.clear();
    if (identity != addressKey(base))
      continue;
    const auto object = state.objects.find(field.root);
    if (object == state.objects.end())
      continue;
    const auto displacement = pointer.elementOffset - base.elementOffset;
    if (offset.upper.magnitude >
        std::numeric_limits<std::uint64_t>::max() - displacement)
      continue;
    const auto last = offset.upper.magnitude + displacement;
    for (const auto &[path, envelope] : object->second.prefixes) {
      if (path.pointer != field.path || path.subobject != pointer.subobject ||
          constantIntegerDomain(type) != envelope.domain)
        continue;
      const auto end = object->second.integers.find(path.end);
      if (end == object->second.integers.end() || end->second.lower.negative ||
          last >= end->second.lower.magnitude)
        continue;
      auto result = envelope;
      result.evidence = merged(result.evidence, offset.evidence);
      result.evidence = merged(result.evidence, end->second.evidence);
      result.evidence = merged(result.evidence, pointer.evidence);
      return result;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<IntegerFact>
integerInPrefix(const MirProgram *program, MirBodyAddress address,
                const MirBody &body, const AbstractState &state,
                const ScalarAddress &pointer, const SemanticType &type) {
  if (!program)
    return std::nullopt;
  std::optional<IntegerFact> offset;
  if (pointer.index) {
    const auto &index = *pointer.index;
    if (index.parameter) {
      const auto roots = parameterPlaces(*program, address, body);
      if (*index.parameter < roots.size())
        offset = integerForPlace(body, state, roots[*index.parameter]);
    } else if (const auto receiver = state.objects.find(thisRoot(body));
               receiver != state.objects.end()) {
      const auto current = receiver->second.integers.find(index.field);
      if (current != receiver->second.integers.end())
        offset = current->second;
    }
    if (offset && offset->domain == index.domain)
      offset = applyEntryRelative(
          *offset, EntryRelativeField{index.field, index.domain, index.offset});
    else
      offset.reset();
  } else {
    offset = IntegerFact{.domain = {.width = 64, .signedValue = false},
                         .lower = {},
                         .upper = {}};
  }
  return offset ? prefixEnvelopeAt(state, pointer, *offset, type)
                : std::nullopt;
}

[[nodiscard]] std::optional<IntegerFact>
integerInRegion(const MirBody &body, const AbstractState &state,
                const AddressRegion &region, const SemanticType &type) {
  const auto *pointee = addressPointeeType(body, region.base);
  return pointee && *pointee == type
             ? prefixEnvelopeAt(state, region.base, region.offsets, type)
             : std::nullopt;
}

[[nodiscard]] std::optional<std::size_t>
readonlyIntegerParameter(const MirProgram *program, MirBodyAddress address,
                         const MirBody &body, MirOperand operand) {
  if (!program || !constantIntegerDomain(operand.type))
    return std::nullopt;
  std::set<MirValueId> seen;
  while (operand.kind == MirOperandKind::Value && operand.value &&
         seen.insert(operand.value).second) {
    const auto *instruction = definingInstruction(body, operand.value);
    if (!instruction || instruction->operands.size() != 1 ||
        instruction->info.type != operand.type ||
        instruction->operands.front().type != operand.type ||
        instruction->rawMemoryAccess || !instruction->lifecycle.empty() ||
        instruction->unsafeOperation != UnsafeOperationKind::None ||
        instruction->intrinsic != IntrinsicKind::None ||
        instruction->receiver || instruction->destination)
      return std::nullopt;
    const auto effect = effects(*instruction);
    if (effect.writesUnknownMemory || effect.invokesUserCode ||
        effect.invokesRuntime || effect.maySynchronize)
      return std::nullopt;
    const bool transport =
        instruction->kind == MirInstructionKind::Load ||
        instruction->kind == MirInstructionKind::Move ||
        (instruction->kind == MirInstructionKind::CallInput &&
         instruction->operation == MirOperation::None) ||
        (instruction->kind == MirInstructionKind::Compute &&
         instruction->operation == MirOperation::Identity);
    if (!transport)
      return std::nullopt;
    operand = instruction->operands.front();
  }
  const auto *place = body.findPlace(operand.place);
  if (!place || place->root != MirPlaceRootKind::Binding || !place->binding ||
      place->access != AccessMode::ReadOnly ||
      place->traits.containsBorrowedState || place->type != operand.type)
    return std::nullopt;
  // Concrete pack formals repeat their one binding in call-argument order.
  // The verified PackElement identity selects an occurrence, not a scalar
  // field inferred from a library name or a coincidentally matching type.
  return parameterOrdinal(*program, address, *place);
}

[[nodiscard]] std::optional<std::size_t>
readonlyRecordParameter(const MirProgram *program, MirBodyAddress address,
                        const MirBody &body, MirOperand operand) {
  if (!program || operand.type.kind != SemanticType::Class)
    return std::nullopt;
  std::set<MirValueId> seen;
  while (operand.kind == MirOperandKind::Value && operand.value &&
         seen.insert(operand.value).second) {
    const auto *instruction = definingInstruction(body, operand.value);
    if (!instruction || instruction->operands.size() != 1 ||
        instruction->info.type != operand.type ||
        instruction->operands.front().type != operand.type)
      return std::nullopt;
    if (!isMirStructuralValueTransfer(*program, *instruction) &&
        !(instruction->kind == MirInstructionKind::Load &&
          instruction->lifecycle.empty() && !instruction->rawMemoryAccess &&
          instruction->unsafeOperation == UnsafeOperationKind::None))
      return std::nullopt;
    operand = instruction->operands.front();
  }
  const auto *place = body.findPlace(operand.place);
  if (!place || place->access != AccessMode::ReadOnly ||
      place->traits.containsBorrowedState || place->type != operand.type)
    return std::nullopt;
  return parameterOrdinal(*program, address, *place);
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
  if (comparison && comparison->kind == MirInstructionKind::Compute &&
      comparison->operation == MirOperation::ExpectedHasValue &&
      comparison->operands.size() == 1 &&
      comparison->operands.front().type.kind == SemanticType::Expected) {
    const auto source = comparison->operands.front();
    const auto known = state.resultAlternatives.find(source.value);
    if (source.kind != MirOperandKind::Value ||
        known == state.resultAlternatives.end())
      return;
    const std::size_t selected = branchTaken ? 0 : 1;
    const auto alternative = known->second.find(selected);
    if (alternative == known->second.end()) {
      state.reachable = false;
      return;
    }
    const AlternativeFields retained{{selected, alternative->second}};
    for (const auto &[origin, saved] : alternative->second) {
      auto fact = saved;
      fact.evidence = merged(fact.evidence, conditionEvidence);
      add(fact.evidence, block);
      addInstruction(fact.evidence, comparison->id);
      addValue(fact.evidence, source.value);
      publishFieldFact(body, state, origin, fact);
    }
    state.resultAlternatives[source.value] = retained;
    if (const auto loaded = state.loadedOrigins.find(source.value);
        loaded != state.loadedOrigins.end() &&
        state.placeAlternatives.contains(loaded->second))
      state.placeAlternatives[loaded->second] = retained;
    return;
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
  if (singleton(*variableFact)) {
    const int order = compare(variableFact->lower, constantFact->lower);
    const bool taken = operation == MirOperation::Less           ? order < 0
                       : operation == MirOperation::LessEqual    ? order <= 0
                       : operation == MirOperation::Greater      ? order > 0
                       : operation == MirOperation::GreaterEqual ? order >= 0
                       : operation == MirOperation::Equal        ? order == 0
                                                                 : order != 0;
    if (taken != branchTaken) {
      state.reachable = false;
      return;
    }
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
  // This edge's predicate is an independent threshold. An earlier contextual
  // singleton is not an invariant of repeated executions of the guard: keeping
  // it here would make an affine loop expand its threshold one step at a time.
  auto branchEnvelope = std::pair{minimumValue(variableFact->domain),
                                  maximumValue(variableFact->domain)};
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
  if (const auto origin = state.fieldOrigins.find(variable->value);
      origin != state.fieldOrigins.end()) {
    auto &fields = state.objects[origin->second.root].integers;
    auto field = fields.find(origin->second.path);
    IntegerFact current = field == fields.end() ? *variableFact : field->second;
    if (!intersect(current, lower, upper, evidence)) {
      state.reachable = false;
      return;
    }
    current.wideningEnvelope = variableFact->wideningEnvelope;
    publishFieldFact(body, state, origin->second, current);
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

void publishFieldFact(const MirBody &body, AbstractState &state,
                      const FieldOrigin &origin, const IntegerFact &fact) {
  // The same owned field may be represented relative to both an enclosing
  // object and a selected nested receiver. A branch refines its current value,
  // not just one of those representations. Keep existing subobject summaries
  // coherent so extracting the outer object cannot overwrite the refinement
  // with an older, wider inner summary. Only exact owned paths participate;
  // this is not alias propagation through stored references or raw pointers.
  for (auto &[root, object] : state.objects) {
    if (const auto prefix = fieldPath(body, origin.root, root, true);
        prefix && prefix->size() < origin.path.size() &&
        std::equal(prefix->begin(), prefix->end(), origin.path.begin())) {
      const FieldPath suffix(origin.path.begin() + prefix->size(),
                             origin.path.end());
      object.integers.insert_or_assign(suffix, fact);
    } else if (const auto prefix = fieldPath(body, root, origin.root, true)) {
      auto path = *prefix;
      path.insert(path.end(), origin.path.begin(), origin.path.end());
      object.integers.insert_or_assign(std::move(path), fact);
    }
  }
  state.objects[origin.root].integers.insert_or_assign(origin.path, fact);
  // Root zero itself is never added to proof evidence or the place table.
  for (const auto &place : body.places)
    if (const auto path = fieldPath(body, origin.root, place.id);
        path && *path == origin.path &&
        constantIntegerDomain(place.type) == fact.domain)
      state.integerPlaces[place.id] = fact;
}

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
    for (const auto &[path, fact] : object.scalarPointees) {
      if (path.pointer.size() <= prefix->size() ||
          !std::equal(prefix->begin(), prefix->end(), path.pointer.begin()))
        continue;
      FieldPath suffix(path.pointer.begin() + prefix->size(),
                       path.pointer.end());
      result.scalarPointees.insert_or_assign(
          PointeeField{std::move(suffix), path.subobject}, fact);
    }
    for (const auto &[path, fact] : object.prefixes) {
      if (path.pointer.size() <= prefix->size() ||
          path.end.size() <= prefix->size() ||
          !std::equal(prefix->begin(), prefix->end(), path.pointer.begin()) ||
          !std::equal(prefix->begin(), prefix->end(), path.end.begin()))
        continue;
      result.prefixes.insert_or_assign(
          PrefixField{
              FieldPath(path.pointer.begin() + prefix->size(),
                        path.pointer.end()),
              FieldPath(path.end.begin() + prefix->size(), path.end.end()),
              path.subobject},
          fact);
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
      for (const auto &[suffix, fact] : nested->second.scalarPointees) {
        FieldPath combined = *path;
        combined.insert(combined.end(), suffix.pointer.begin(),
                        suffix.pointer.end());
        result.scalarPointees.insert_or_assign(
            PointeeField{std::move(combined), suffix.subobject}, fact);
      }
      for (const auto &[suffix, fact] : nested->second.prefixes) {
        auto pointer = *path;
        pointer.insert(pointer.end(), suffix.pointer.begin(),
                       suffix.pointer.end());
        auto end = *path;
        end.insert(end.end(), suffix.end.begin(), suffix.end.end());
        result.prefixes.insert_or_assign(
            PrefixField{std::move(pointer), std::move(end), suffix.subobject},
            fact);
      }
    }
  }
  for (const auto &[field, pointer] : state.fieldAddresses) {
    const auto prefix = root == field.root
                            ? std::optional<FieldPath>{FieldPath{}}
                            : fieldPath(body, root, field.root, true);
    if (!prefix)
      continue;
    auto path = *prefix;
    path.insert(path.end(), field.path.begin(), field.path.end());
    std::erase_if(result.scalarPointees, [&](const auto &entry) {
      return entry.first.pointer == path;
    });
    const auto base = addressKey(pointer);
    for (auto current = state.constructedScalars.lower_bound(base);
         current != state.constructedScalars.end(); ++current) {
      const auto &[cell, fact] = *current;
      auto compared = cell;
      compared.subobject = base.subobject;
      if (compared != base || cell.subobject.size() < base.subobject.size() ||
          !std::equal(base.subobject.begin(), base.subobject.end(),
                      cell.subobject.begin()))
        break;
      FieldPath suffix(cell.subobject.begin() + base.subobject.size(),
                       cell.subobject.end());
      if (result.scalarPointees.size() < 64)
        result.scalarPointees.insert_or_assign(
            PointeeField{path, std::move(suffix)}, fact.value);
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
  while (result.scalarPointees.size() > 64)
    result.scalarPointees.erase(std::prev(result.scalarPointees.end()));
  while (result.prefixes.size() > 64)
    result.prefixes.erase(std::prev(result.prefixes.end()));
  if (result.integers.empty() && result.scalarPointees.empty() &&
      result.prefixes.empty() && !result.elements &&
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

[[nodiscard]] std::optional<IntegerFact> compoundIntegerFact(
    const MirBody &body, const MirProgram *program, MirBodyAddress address,
    const MirPlaceAliasEscapeAnalysis &aliases, const AbstractState &state,
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
  if (const auto pointer = borrowedAddressForPlace(program, body, state,
                                                   *instruction.destination)) {
    if (const auto *place = body.findPlace(*instruction.destination))
      left = integerAtAddress(body, state, *pointer, place->type, program,
                              address);
  } else if (const auto region = borrowedRegionForPlace(
                 program, body, state, *instruction.destination)) {
    if (const auto *place = body.findPlace(*instruction.destination))
      left = integerInRegion(body, state, *region, place->type);
  }
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

void retireScalarPointeeSnapshots(AbstractState &state) {
  for (auto &[root, object] : state.objects) {
    (void)root;
    object.scalarPointees.clear();
    object.prefixes.clear();
  }
  for (auto &[value, object] : state.objectValues) {
    (void)value;
    object.scalarPointees.clear();
    object.prefixes.clear();
  }
}

void invalidatePlaceRoot(const MirBody &body,
                         const MirPlaceAliasEscapeAnalysis &aliases,
                         AbstractState &state, MirPlaceId destination,
                         bool initializesFreshTemporary = false) {
  const MirPlace *written = body.findPlace(destination);
  const bool fresh = initializesFreshTemporary && written &&
                     written->root == MirPlaceRootKind::Temporary &&
                     written->projections.empty() &&
                     !written->initiallyAvailable &&
                     !written->traits.containsBorrowedState;
  // This bounded address relay does not infer reference/write disjointness.
  state.resultAlternatives.clear();
  state.placeAlternatives.clear();
  // A shell copy does not freeze memory reached through its pointers. Without
  // an exact pointee write/disjointness proof, any storage change retires all
  // transported snapshots; newly proved postconditions may be published later.
  if (!fresh) {
    retireScalarPointeeSnapshots(state);
    state.borrowedAddresses.clear();
    state.borrowedRegions.clear();
  }
  if (written == nullptr) {
    state.constructedScalars.clear();
    std::erase_if(state.integerPlaces, [&](const auto &fact) {
      return !immutableScalarBinding(body, fact.first);
    });
    state.arrayElements.clear();
    state.aggregateElements.clear();
    state.aggregateElementIntegers.clear();
    state.objects.clear();
    state.loadedOrigins.clear();
    state.fieldOrigins.clear();
    state.entryRelativeFields.clear();
    state.entryRelativeValues.clear();
    state.scalarAddressValues.clear();
    state.scalarAddressPlaces.clear();
    state.fieldAddresses.clear();
    state.aggregateOrigins.clear();
    state.objectValues.clear();
    return;
  }
  const auto overlaps = [&](MirPlaceId place) {
    // Verified argument/result publication initializes a new temporary. Its
    // local shell cannot overwrite another live object, even when the general
    // escape analysis conservatively reports MayAlias. This says nothing
    // about memory later accessed through pointers copied into that shell.
    if (fresh) {
      const auto *other = body.findPlace(place);
      return other && samePlaceRoot(*written, *other);
    }
    return aliases.relation(destination, place) != MirAliasRelation::NoAlias;
  };
  const auto pointerFieldOverlaps = [&](const ScalarAddress &pointer) {
    // Different owned fields are disjoint storage, but walking through a
    // stored reference or raw pointer loses that guarantee. A checked root
    // reference may have its one leading dereference before the owned path.
    bool fieldSeen = false;
    const bool owned = std::all_of(
        written->projections.begin(), written->projections.end(),
        [&](const auto &projection) {
          if (projection.kind == MirProjectionKind::Field) {
            fieldSeen = true;
            return true;
          }
          return !fieldSeen &&
                 projection.kind == MirProjectionKind::Dereference &&
                 written->root == MirPlaceRootKind::Binding &&
                 body.findPlace(pointer.place) &&
                 body.findPlace(pointer.place)->type.kind ==
                     SemanticType::Reference;
        });
    if (const auto path = fieldPath(body, pointer.place, destination, true);
        owned && path) {
      const auto prefix = std::min(path->size(), pointer.field.size());
      return std::equal(path->begin(), path->begin() + prefix,
                        pointer.field.begin());
    }
    return overlaps(pointer.place);
  };
  std::erase_if(state.fieldAddresses, [&](const auto &entry) {
    return pointerFieldOverlaps(
        ScalarAddress{.kind = AddressOriginKind::EntryPointerField,
                      .place = entry.first.root,
                      .field = entry.first.path});
  });
  std::erase_if(state.constructedScalars, [&](const auto &entry) {
    if (entry.first.kind == AddressOriginKind::ExistingPlace)
      return overlaps(entry.first.place);
    if (fresh)
      return false;
    for (const auto &field : entry.second.disjointFields) {
      if (const auto path = fieldPath(body, field.root, destination);
          path && *path == field.path &&
          std::none_of(written->projections.begin(), written->projections.end(),
                       [](const auto &projection) {
                         return projection.kind ==
                                MirProjectionKind::RawDereference;
                       })) {
        // A stored reference adds a dereference after an owned field. It is
        // not the exact storage named by this relation.
        bool fieldSeen = false;
        bool indirect = false;
        for (const auto &projection : written->projections) {
          indirect |= fieldSeen && projection.kind != MirProjectionKind::Field;
          fieldSeen |= projection.kind == MirProjectionKind::Field;
        }
        if (!indirect)
          return false;
      }
    }
    // An incoming address cannot denote this invocation's independent
    // scalar/pointer variable. Reference/loan/aggregate writes still may
    // touch that address, and never use this exception.
    return written->root != MirPlaceRootKind::Binding || !written->binding ||
           !written->projections.empty() ||
           written->traits.containsBorrowedState ||
           (!constantIntegerDomain(written->type) &&
            written->type.kind != SemanticType::RawPointer);
  });
  std::erase_if(state.scalarAddressValues, [&](const auto &entry) {
    if (entry.second.kind == AddressOriginKind::EntryPointerField)
      return pointerFieldOverlaps(entry.second);
    return entry.second.kind != AddressOriginKind::EntryPointer &&
           overlaps(entry.second.place);
  });
  std::erase_if(state.scalarAddressPlaces, [&](const auto &entry) {
    const auto *binding = body.findPlace(entry.first);
    const bool independentPointer =
        entry.second.kind == AddressOriginKind::EntryPointer && binding &&
        binding->root == MirPlaceRootKind::Binding && binding->binding &&
        binding->projections.empty() &&
        binding->access == AccessMode::ReadOnly &&
        binding->type.kind == SemanticType::RawPointer &&
        !binding->traits.containsBorrowedState;
    const auto storedField =
        entry.second.kind == AddressOriginKind::EntryPointerField
            ? fieldPath(body, entry.second.place, entry.first)
            : std::nullopt;
    const bool disjointOwnedField = storedField &&
                                    *storedField == entry.second.field &&
                                    !pointerFieldOverlaps(entry.second);
    // A checked referent write cannot change an independent readonly pointer
    // variable. Exact replacement of that variable still retires its value.
    return (overlaps(entry.first) && !disjointOwnedField &&
            (!independentPointer ||
             aliases.relation(destination, entry.first) ==
                 MirAliasRelation::MustAlias)) ||
           (entry.second.kind == AddressOriginKind::EntryPointerField
                ? pointerFieldOverlaps(entry.second)
                : (entry.second.kind != AddressOriginKind::EntryPointer &&
                   overlaps(entry.second.place)));
  });
  // The relation belongs to an owned This path. Without such an exact path,
  // an aliasing write may change any receiver field. Do not turn different
  // place IDs into a disjointness claim.
  const auto receiverPath = fieldPath(body, 0, destination, true);
  const bool knownOwnedPath =
      receiverPath &&
      std::all_of(written->projections.begin(), written->projections.end(),
                  [](const auto &projection) {
                    return projection.kind == MirProjectionKind::Field;
                  }) &&
      (receiverPath->empty() ||
       std::any_of(state.entryRelativeFields.begin(),
                   state.entryRelativeFields.end(), [&](const auto &entry) {
                     return receiverPath->size() <= entry.first.size() &&
                            std::equal(receiverPath->begin(),
                                       receiverPath->end(),
                                       entry.first.begin());
                   }));
  const auto overlapsEntry = [&](const auto &entry) {
    if (fresh)
      return false;
    if (!knownOwnedPath)
      return true;
    const auto prefix =
        std::min(receiverPath->size(), entry.second.field.size());
    return std::equal(receiverPath->begin(), receiverPath->begin() + prefix,
                      entry.second.field.begin());
  };
  std::erase_if(state.entryRelativeFields, overlapsEntry);
  std::erase_if(state.entryRelativeValues, overlapsEntry);
  std::erase_if(state.fieldOrigins, [&](const auto &entry) {
    const auto &origin = entry.second;
    if ((origin.root != 0 || fresh) && !overlaps(origin.root))
      return false;
    if (const auto path = fieldPath(body, origin.root, destination)) {
      const auto prefix = std::min(path->size(), origin.path.size());
      return std::equal(path->begin(), path->begin() + prefix,
                        origin.path.begin());
    }
    return true;
  });
  std::erase_if(state.integerPlaces, [&](const auto &entry) {
    if (fresh)
      return overlaps(entry.first);
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
  for (auto iterator = state.objects.begin();
       iterator != state.objects.end();) {
    if ((iterator->first != 0 || fresh) && !overlaps(iterator->first)) {
      ++iterator;
      continue;
    }
    // Materialized and implicit receiver roots describe the same owned
    // storage. A direct field store invalidates its subtree, not its siblings.
    // No stored/raw reference projection receives this disjointness rule.
    const auto path = fieldPath(body, iterator->first, destination);
    if (written->root != MirPlaceRootKind::This || !path ||
        !std::all_of(written->projections.begin(), written->projections.end(),
                     [](const auto &projection) {
                       return projection.kind == MirProjectionKind::Field;
                     })) {
      iterator = state.objects.erase(iterator);
      continue;
    }
    std::erase_if(iterator->second.integers, [&](const auto &field) {
      const auto prefix = std::min(path->size(), field.first.size());
      return std::equal(path->begin(), path->begin() + prefix,
                        field.first.begin());
    });
    iterator->second.elements.reset();
    iterator->second.elementIntegers.clear();
    ++iterator;
  }
  std::erase_if(state.aggregateOrigins,
                [&](const auto &entry) { return overlaps(entry.second); });
  std::erase_if(state.loadedOrigins,
                [&](const auto &entry) { return overlaps(entry.second); });
  // Whole-root object invalidation is conservative for indirect aliases.
  // An independently retained typed field identity still denotes its current
  // scalar value after a disjoint owned-field write. Recover only that exact
  // correlation; overlapping writes already retired it above.
  for (const auto &[value, origin] : state.fieldOrigins)
    if (const auto fact = state.integers.find(value);
        fact != state.integers.end()) {
      auto [field, inserted] = state.objects[origin.root].integers.try_emplace(
          origin.path, fact->second);
      if (!inserted && field->second.domain == fact->second.domain &&
          !intersect(field->second, fact->second.lower, fact->second.upper,
                     fact->second.evidence)) {
        state.reachable = false;
        return;
      }
      for (const auto &place : body.places)
        if (const auto path = fieldPath(body, origin.root, place.id);
            path && *path == origin.path &&
            constantIntegerDomain(place.type) == field->second.domain)
          state.integerPlaces[canonicalPlace(aliases, place.id)] =
              field->second;
    }
}

[[nodiscard]] bool initializesReferenceCarrier(
    const MirBody &body, const MirInstruction &instruction,
    const MirProgram *program = nullptr, const AbstractState *state = nullptr) {
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
  bool exactDerivedAddress = false;
  if (program && state && address && loan) {
    const auto actual =
        borrowedAddressForPlace(program, body, *state, address->id);
    const auto retained = state->borrowedAddresses.find(loan->id);
    exactDerivedAddress =
        actual && retained != state->borrowedAddresses.end() &&
        addressKey(*actual) == addressKey(retained->second) &&
        addressPointeeType(body, retained->second) &&
        *addressPointeeType(body, retained->second) == address->type;
  }
  return loan != nullptr && address != nullptr && !loan->entry &&
         !loan->escapes &&
         (loan->kind == MirLoanKind::Local ||
          loan->kind == MirLoanKind::CallResult) &&
         loan->carriers.size() == 1 &&
         loan->carriers.front() == destination->binding &&
         ((address->root == MirPlaceRootKind::Loan &&
           address->loan == loan->id) ||
          loan->source == address->id || exactDerivedAddress);
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

// Final-address construction that runs one exact source constructor of an
// ordinary record. Its operands after the address are the constructor's.
[[nodiscard]] std::optional<MirBodyAddress>
closedObjectConstructTarget(const MirProgram &program,
                            const MirInstruction &instruction) {
  if (instruction.tombstone || instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectConstruct ||
      !instruction.constructorTarget || instruction.elementCleanup ||
      instruction.receiver || instruction.result ||
      instruction.nativeCallbackAdapter || instruction.threadTaskAdapter ||
      instruction.rawMemoryAccess || !instruction.lifecycle.empty() ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.dispatch != CallDispatch::Static ||
      instruction.constructorKind != ConstructorKind::Ordinary ||
      instruction.operation != MirOperation::None ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      instruction.operands.empty())
    return std::nullopt;
  const auto &address = instruction.operands.front().type;
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(*instruction.constructorTarget);
  const MirClassInstance *owner =
      constructor ? program.findClassInstance(constructor->owner) : nullptr;
  if (address.kind != SemanticType::RawPointer ||
      address.arguments.size() != 1 ||
      address.pointerAccess != AccessMode::Mutable || !constructor ||
      constructor->definitionKind != MirDefinitionKind::Source ||
      constructor->parameterTypes.size() + 1 != instruction.operands.size() ||
      !owner || owner->type != address.arguments.front() ||
      owner->kind == ClassKind::Union || owner->unionLayout ||
      owner->traits.containsBorrowedState || owner->polymorphic ||
      owner->abstract || !owner->bases.empty() ||
      !owner->structuralBases.empty() ||
      owner->compilerCapability != CompilerCapabilityTypeKind::None)
    return std::nullopt;
  return MirBodyAddress{.kind = MirBodyKind::Constructor,
                        .owner = *instruction.constructorTarget};
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
// A checked floating write targets one live floating object. Live objects
// never partially overlap, so it changes no live integer object and no
// pointer/reference carrier. It publishes no integer value.
[[nodiscard]] bool floatingScalar(const SemanticType &type) {
  return type.kind == SemanticType::Float || type.kind == SemanticType::Double;
}
struct DisjointScalarFields {
  std::map<MirPlaceId, IntegerFieldFacts> objects;
  std::map<MirPlaceId, IntegerFact> places;
  ConstructedScalars pointees;
};

[[nodiscard]] DisjointScalarFields
preserveDifferentWidthFields(const MirProgram *program, MirBodyAddress address,
                             const MirBody &body, const AbstractState &state,
                             std::optional<unsigned> writtenWidth) {
  DisjointScalarFields saved;
  for (const auto &[root, object] : state.objects)
    for (const auto &[path, fact] : object.integers)
      if (fact.domain.width != writtenWidth && fact.domain.width > 8 &&
          liveScalarPath(program, address, body, root, path))
        saved.objects[root].emplace(path, fact);
  for (const auto &[place, fact] : state.integerPlaces)
    if (fact.domain.width != writtenWidth && fact.domain.width > 8 &&
        liveScalarPath(program, address, body, place, {}))
      saved.places.emplace(place, fact);
  for (const auto &[pointer, fact] : state.constructedScalars)
    if (fact.value.domain.width > 8 && fact.value.domain.width != writtenWidth)
      saved.pointees.emplace(pointer, fact);
  return saved;
}

void restoreDifferentWidthFields(AbstractState &state,
                                 const DisjointScalarFields &saved) {
  for (const auto &[root, fields] : saved.objects)
    for (const auto &[path, fact] : fields)
      state.objects[root].integers.insert_or_assign(path, fact);
  for (const auto &[place, fact] : saved.places)
    state.integerPlaces.insert_or_assign(place, fact);
  for (const auto &[pointer, fact] : saved.pointees)
    publishConstructedScalar(state, pointer, fact);
}

// An opaque effect reaches only storage whose identity reached it. A local
// binding that no earlier instruction may have captured, and that this
// instruction does not take as an input, keeps its own non-byte integer
// fields and scalars. Facts about storage it points to, its prefixes and its
// address identities are not retained.
[[nodiscard]] DisjointScalarFields preserveUncapturedLocals(
    const MirProgram *program, MirBodyAddress address, const MirBody &body,
    const MirLocalCaptureAnalysis *captures, const MirInstruction &instruction,
    const AbstractState &state) {
  DisjointScalarFields saved;
  if (program == nullptr || captures == nullptr || !captures->valid)
    return saved;
  const auto uncaptured = [&](const MirPlace &place) {
    if (place.root != MirPlaceRootKind::Binding || place.binding == 0)
      return false;
    const MirPlace *root = body.findPlace(rootForBinding(body, place.binding));
    return root != nullptr && root->type.kind != SemanticType::Reference &&
           !captures->mayBeCapturedBefore(place.binding, instruction.id) &&
           !captures->involves(place.binding, instruction.id);
  };
  for (const auto &[root, object] : state.objects) {
    const MirPlace *place = body.findPlace(root);
    if (place == nullptr || !place->projections.empty() || !uncaptured(*place))
      continue;
    for (const auto &[path, fact] : object.integers)
      if (fact.domain.width > 8 &&
          liveScalarPath(program, address, body, root, path))
        saved.objects[root].emplace(path, fact);
  }
  for (const auto &[id, fact] : state.integerPlaces)
    if (const MirPlace *place = body.findPlace(id);
        place != nullptr && uncaptured(*place) && fact.domain.width > 8 &&
        liveScalarPath(program, address, body, id, {}))
      saved.places.emplace(id, fact);
  return saved;
}
[[nodiscard]] std::optional<std::vector<MirPlaceId>>
operandProtectedPlaces(const MirBody &body, const MirOperand &operand);
[[nodiscard]] bool
operandMayAliasPlace(const MirBody &body,
                     const MirPlaceAliasEscapeAnalysis &aliases,
                     const MirOperand &operand, MirPlaceId tracked);
void killUnknownMemory(const MirBody &body, AbstractState &state);
void killGlobalAliases(const MirBody &body, AbstractState &state);
void preserveLiveScalarFactsForConstruction(const MirProgram *program,
                                            MirBodyAddress address,
                                            const MirBody &body,
                                            AbstractState &state);
[[nodiscard]] std::vector<FieldOrigin> liveScalarFields(
    const MirProgram *program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, const AbstractState &state);
[[nodiscard]] std::optional<FieldOrigin> receiverFieldOrigin(
    const MirProgram &program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, const AbstractState &state,
    const MirInstruction &instruction, const FieldPath &path);
[[nodiscard]] std::optional<FieldOrigin>
normalizedFieldOrigin(const MirProgram &program, MirBodyAddress address,
                      const MirBody &body,
                      const MirPlaceAliasEscapeAnalysis &aliases,
                      MirPlaceId receiver, const FieldPath &path);
[[nodiscard]] std::optional<FieldOrigin>
ownedPointerFieldOrigin(const MirProgram *program, MirBodyAddress address,
                        const MirBody &body,
                        const MirPlaceAliasEscapeAnalysis &aliases,
                        MirPlaceId place, const FieldPath &suffix = {});

[[nodiscard]] bool
mayAliasRepresentedGlobal(const MirBody &body,
                          const MirPlaceAliasEscapeAnalysis &aliases,
                          MirPlaceId id) {
  const auto *place = body.findPlace(id);
  if (!place)
    return false;
  if (place->root == MirPlaceRootKind::Symbol)
    return true;
  // A carrier or loan-rooted place may protect a global even though the
  // write itself has no Symbol root. Only represented possible aliases
  // participate; an independent ordinary local is not a global write.
  return std::any_of(
      body.places.begin(), body.places.end(), [&](const auto &global) {
        return global.root == MirPlaceRootKind::Symbol &&
               aliases.relation(id, global.id) != MirAliasRelation::NoAlias;
      });
}

void invalidateClosedCallInputs(const MirProgram &program, const MirBody &body,
                                const MirPlaceAliasEscapeAnalysis &aliases,
                                const MirInstruction &instruction,
                                MirBodyAddress address, MirBodyAddress target,
                                bool opaqueEffects,
                                LiveMemoryBarrier liveMemoryBarrier,
                                bool pointerRelayOnly,
                                const MirLocalCaptureAnalysis *captures,
                                AbstractState &state) {
  const auto killUnknownMemory = [&](const MirBody &body,
                                     AbstractState &state) {
    const auto uncaptured = preserveUncapturedLocals(
        &program, address, body, captures, instruction, state);
    lang::killUnknownMemory(body, state);
    restoreDifferentWidthFields(state, uncaptured);
  };
  const auto summary = exactMirInvocationSummary(program, body, instruction);
  const bool structuralConstructionOnly =
      opaqueEffects &&
      liveMemoryBarrier == LiveMemoryBarrier::VacantStructuralConstruction;
  if (!summary || (opaqueEffects && (summary->traits.maySynchronize ||
                                     (summary->traits.writesUnknownMemory &&
                                      !structuralConstructionOnly)))) {
    killUnknownMemory(body, state);
    return;
  }
  if (structuralConstructionOnly)
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
  // An exact source address relay with no writes may return a checked borrow.
  // Returning the address is an escape, not a mutation of its lifetime witness
  // or pointee. Unknown retention/effects never enter this profile.
  if (pointerRelayOnly && !summary->traits.writesPlace &&
      !summary->traits.writesUnknownMemory)
    return;

  // Scalar/object return summaries do not describe every post-call referent.
  // Invalidate the exact staged inputs first; independently proven object
  // summaries may subsequently republish their supported field facts. A shell
  // reference also protects the external owners in its stored dependency.
  const auto invalidate = [&](const MirOperand &operand) {
    const auto sources = operandProtectedPlaces(body, operand);
    if (!sources) {
      // An unknown staged identity may name any local; preserve nothing.
      lang::killUnknownMemory(body, state);
      return;
    }
    // MayAlias is not transitive: invalidate from actual sources, never from
    // all places which might alias a source and then expand aliasing again.
    for (const auto source : *sources) {
      if (mayAliasRepresentedGlobal(body, aliases, source)) {
        state.mayWriteGlobals = true;
        killGlobalAliases(body, state);
      }
      invalidatePlaceRoot(body, aliases, state, source);
    }
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
    // live object. Only the fully interpreted vacant structural profile may use
    // that contract; actual raw writes, destruction, or an unknown call mark
    // the successful path Unknown instead. Checked references and ordinary
    // receiver/global writes still invalidate their exact protected inputs.
    if (structuralConstructionOnly &&
        (*parameters)[index].kind == SemanticType::RawPointer)
      continue;
    // An exact no-write source relay may return one input address. That escape
    // is not a pointee write. Keep this separate from unknown retention/calls,
    // and never apply the exception to reference or aggregate inputs.
    if (pointerRelayOnly &&
        (*parameters)[index].kind == SemanticType::RawPointer)
      continue;
    const bool escapes = index >= summary->argumentsEscape.size() ||
                         summary->argumentsEscape[index];
    if (parameterAllowsMutation((*parameters)[index]) &&
        (summary->traits.writesPlace || escapes))
      invalidate(instruction.operands[index]);
  }
}

// The broad writesPlace effect also includes reassigning a by-value pointer
// formal. Such an assignment changes the callee's pointer variable, not caller
// memory. This exact transport profile permits that distinction without making
// a raw store, reference write, cleanup, or unhandled operation harmless.
[[nodiscard]] bool onlyLocalPointerAssignments(const MirProgram &program,
                                               MirBodyAddress address) {
  const auto *function = address.kind == MirBodyKind::Function
                             ? program.findFunctionInstance(address.owner)
                             : nullptr;
  if (!function ||
      function->parameterBindings.size() != function->parameterTypes.size())
    return false;
  for (const auto &block : function->body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto effect = effects(program, instruction);
      if (!effect.writesPlace)
        continue;
      if (instruction.kind != MirInstructionKind::Assign ||
          !instruction.destination || instruction.operands.size() != 1 ||
          instruction.operation != MirOperation::Assign ||
          instruction.rawMemoryAccess || !instruction.lifecycle.empty() ||
          effect.writesUnknownMemory || effect.readsUnknownMemory ||
          effect.invokesUserCode || effect.invokesRuntime ||
          effect.maySynchronize || effect.movesValue || effect.dropsValue)
        return false;
      const auto *place = function->body.findPlace(*instruction.destination);
      if (!place || place->root != MirPlaceRootKind::Binding ||
          !place->projections.empty() || place->traits.containsBorrowedState ||
          place->type.kind != SemanticType::RawPointer)
        return false;
      const auto parameter =
          std::find(function->parameterBindings.begin(),
                    function->parameterBindings.end(), place->binding);
      if (parameter == function->parameterBindings.end() ||
          function->parameterTypes[static_cast<std::size_t>(
              parameter - function->parameterBindings.begin())] != place->type)
        return false;
    }
  return true;
}

[[nodiscard]] std::optional<ScalarAddress> applyInputAddressOffset(
    const MirProgram &program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, const AbstractState &state,
    const MirInstruction &instruction, const InputAddress &input,
    ScalarAddress pointer) {
  if (input.index) {
    if (pointer.index)
      return std::nullopt;
    const auto &index = *input.index;
    const EntryRelativeField update{
        .field = index.field, .domain = index.domain, .offset = index.offset};
    std::optional<IntegerFact> currentValue;
    std::optional<EntryRelativeField> relative;
    std::optional<std::size_t> parameter;
    if (index.parameter) {
      if (*index.parameter >= instruction.operands.size())
        return std::nullopt;
      const auto &operand = instruction.operands[*index.parameter];
      if (constantIntegerDomain(operand.type) != index.domain)
        return std::nullopt;
      currentValue = integerForOperand(body, state, operand);
      relative = entryRelativeValue(state, operand);
      parameter = readonlyIntegerParameter(&program, address, body, operand);
    } else {
      const auto origin = receiverFieldOrigin(program, address, body, aliases,
                                              state, instruction, index.field);
      if (!origin)
        return std::nullopt;
      if (const auto object =
              extractObjectFact(body, aliases, origin->root, state))
        if (const auto field = object->integers.find(origin->path);
            field != object->integers.end())
          currentValue = field->second;
      const auto *root = body.findPlace(origin->root);
      if (!origin->root || (root && root->root == MirPlaceRootKind::This &&
                            root->projections.empty()))
        if (const auto current = state.entryRelativeFields.find(origin->path);
            current != state.entryRelativeFields.end())
          relative = current->second;
    }
    const auto value =
        currentValue ? applyEntryRelative(*currentValue, update) : std::nullopt;
    if (parameter) {
      pointer.index = EntryIndex{{}, index.domain, index.offset, parameter};
    } else if (value && singleton(*value) && !value->lower.negative) {
      auto displaced = offsetAddress(pointer, value->lower.magnitude, false);
      if (!displaced)
        return std::nullopt;
      pointer = std::move(*displaced);
      pointer.evidence = merged(pointer.evidence, value->evidence);
    } else if (relative && relative->domain == index.domain) {
      const auto composed = shiftEntryRelative(*relative, index.offset);
      if (!composed)
        return std::nullopt;
      pointer.index =
          EntryIndex{composed->field, composed->domain, composed->offset};
      pointer.evidence = merged(pointer.evidence, composed->evidence);
    } else {
      return std::nullopt;
    }
  }
  const auto displaced =
      offsetAddress(std::move(pointer), input.elementOffset, false);
  return displaced ? projectPointee(&program, body, *displaced, input.subobject)
                   : std::nullopt;
}

// Substitute the actual current input address before applying a call's effects.
// A field path is not a fresh allocation or a license to recreate an entry
// identity after mutation. A previously unobserved field can receive a new
// body-local current-value snapshot; old pointee facts are never resurrected.
[[nodiscard]] std::optional<ScalarAddress> inputAddressForCall(
    const MirProgram &program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, AbstractState &state,
    const MirInstruction &instruction, const InputAddress &input) {
  if (input.parameter && *input.parameter >= instruction.operands.size())
    return std::nullopt;
  const auto *operand =
      input.parameter
          ? &instruction.operands[*input.parameter]
          : (instruction.receiver ? &*instruction.receiver : nullptr);
  if (!operand)
    return std::nullopt;
  const auto displaced = [&](std::optional<ScalarAddress> pointer) {
    return pointer
               ? applyInputAddressOffset(program, address, body, aliases, state,
                                         instruction, input, *pointer)
               : std::nullopt;
  };
  if (input.field.empty())
    return displaced(scalarAddressForOperand(body, state, *operand));
  const auto root = objectOriginForOperand(body, aliases, state, *operand);
  if (!root)
    return std::nullopt;
  if (const auto field = ownedPointerFieldOrigin(&program, address, body,
                                                 aliases, root, input.field))
    if (const auto found = state.fieldAddresses.find(*field);
        found != state.fieldAddresses.end())
      return displaced(found->second);
  for (const auto &place : body.places) {
    const auto path = fieldPath(body, root, place.id);
    if (!path || *path != input.field)
      continue;
    if (const auto found =
            state.scalarAddressPlaces.find(canonicalPlace(aliases, place.id));
        found != state.scalarAddressPlaces.end())
      return displaced(found->second);
  }
  if (const auto field = ownedPointerFieldOrigin(&program, address, body,
                                                 aliases, root, input.field)) {
    const auto *type =
        liveOwnedPathType(&program, address, body, field->root, field->path);
    if (type && type->kind == SemanticType::RawPointer &&
        type->arguments.size() == 1 && state.fieldAddresses.size() < 1024) {
      const auto contents =
          extractObjectFact(body, aliases, field->root, state);
      ScalarAddress pointer{.kind = AddressOriginKind::EntryPointerField,
                            .place = field->root,
                            .field = field->path,
                            .fieldPointee = type->arguments.front(),
                            .observation = instruction.id};
      addInstruction(pointer.evidence, instruction.id);
      // The same MIR call may execute again after an intervening field write.
      // Reusing its syntactic observation ID must not revive older contents
      // or equate any surviving saved address with this new observation.
      const auto sameObservation = [&](const auto &old) {
        return old.kind == pointer.kind && old.place == pointer.place &&
               old.field == pointer.field &&
               old.observation == pointer.observation;
      };
      std::erase_if(state.constructedScalars, [&](const auto &entry) {
        return sameObservation(entry.first);
      });
      const auto oldAddress = [&](const auto &entry) {
        return sameObservation(entry.second);
      };
      std::erase_if(state.scalarAddressValues, oldAddress);
      std::erase_if(state.scalarAddressPlaces, oldAddress);
      std::erase_if(state.fieldAddresses, oldAddress);
      std::erase_if(state.borrowedAddresses, oldAddress);
      std::erase_if(state.borrowedRegions, [&](const auto &entry) {
        return sameObservation(entry.second.base);
      });
      state.fieldAddresses.emplace(*field, pointer);
      if (contents)
        for (const auto &[path, fact] : contents->scalarPointees) {
          if (path.pointer != field->path)
            continue;
          const auto cell =
              projectPointee(&program, body, pointer, path.subobject);
          const auto *type = cell ? addressPointeeType(body, *cell) : nullptr;
          if (!type || constantIntegerDomain(*type) != fact.domain)
            continue;
          auto value = rebased(fact, 0, instruction.id);
          value.evidence = merged(value.evidence, pointer.evidence);
          publishConstructedScalar(state, addressKey(*cell),
                                   ConstructedScalar{std::move(value)});
        }
      return displaced(std::move(pointer));
    }
  }
  return std::nullopt;
}

// A returned element address whose index has no exact caller representation,
// such as a caller-local loop counter, still denotes one element at the
// substituted base displaced by the index's current interval.
[[nodiscard]] std::optional<AddressRegion> inputRegionForCall(
    const MirProgram &program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, AbstractState &state,
    const MirInstruction &instruction, const InputAddress &input) {
  if (!input.index)
    return std::nullopt;
  const auto &index = *input.index;
  std::optional<IntegerFact> current;
  if (index.parameter) {
    if (*index.parameter >= instruction.operands.size())
      return std::nullopt;
    const auto &operand = instruction.operands[*index.parameter];
    if (constantIntegerDomain(operand.type) != index.domain)
      return std::nullopt;
    current = integerForOperand(body, state, operand);
  } else if (const auto origin =
                 receiverFieldOrigin(program, address, body, aliases, state,
                                     instruction, index.field)) {
    if (const auto object =
            extractObjectFact(body, aliases, origin->root, state))
      if (const auto field = object->integers.find(origin->path);
          field != object->integers.end())
        current = field->second;
  }
  if (!current || current->domain != index.domain)
    return std::nullopt;
  auto offsets = applyEntryRelative(
      *current, EntryRelativeField{index.field, index.domain, index.offset});
  if (!offsets || offsets->lower.negative)
    return std::nullopt;
  InputAddress base = input;
  base.index.reset();
  auto pointer = inputAddressForCall(program, address, body, aliases, state,
                                     instruction, base);
  return pointer ? normalizedRegion(std::move(*pointer), std::move(*offsets))
                 : std::nullopt;
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
  const bool owningArgument = std::any_of(
      instruction.operands.begin(), instruction.operands.end(),
      [&](const MirOperand &operand) {
        const auto *input = definingInstruction(body, operand.value);
        return owningCallOperand(body, operand) &&
               (!input ||
                !isMirStructuralValueTransfer(closedCalls->program, *input));
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
  // A verified terminal source call has no successful continuation to
  // summarize. Still discover its body for checks before termination, but do
  // not postpone the caller's other successful paths waiting for a return
  // postcondition that cannot exist. Invoke failure edges retain their own
  // conservative pre-instruction transfer.
  if (target->kind == MirBodyKind::Function)
    if (const auto *function =
            closedCalls->program.findFunctionInstance(target->owner);
        function && function->neverReturns && instruction.neverReturns) {
      state.reachable = false;
      return true;
    }
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
  const auto resultField =
      summary.receiverResultField.fact
          ? receiverFieldOrigin(closedCalls->program, address, body, aliases,
                                state, instruction,
                                *summary.receiverResultField.fact)
          : std::nullopt;
  std::optional<IntegerFact> currentResultField;
  const auto invocation =
      exactMirInvocationSummary(closedCalls->program, body, instruction);
  const auto untouchedFields =
      summary.scalarStoresOnly && invocation &&
              !invocation->traits.maySynchronize
          ? preserveDifferentWidthFields(&closedCalls->program, address, body,
                                         state, summary.scalarStoreWidth)
          : DisjointScalarFields{};
  // A store profile without a store width admits only callee-frame writes:
  // independent locals, local transfers/drops and equally closed callees.
  const bool frameLocalWrites = summary.scalarStoresOnly &&
                                !summary.scalarStoreWidth &&
                                !summary.floatingScalarStores;
  const bool pointerRelayOnly =
      (summary.returnedPointer || summary.returnedBorrow) &&
      !summary.hasOpaqueEffects && !summary.mayWriteGlobals && invocation &&
      (!invocation->traits.writesPlace || summary.localPointerWritesOnly ||
       frameLocalWrites) &&
      !invocation->traits.writesUnknownMemory &&
      !invocation->traits.maySynchronize;
  std::optional<ScalarAddress> returnedAddress;
  if (pointerRelayOnly && summary.returnedPointer)
    returnedAddress =
        inputAddressForCall(closedCalls->program, address, body, aliases, state,
                            instruction, *summary.returnedPointer);
  std::optional<ScalarAddress> borrowedAddress;
  if (pointerRelayOnly && summary.returnedBorrow)
    borrowedAddress =
        inputAddressForCall(closedCalls->program, address, body, aliases, state,
                            instruction, *summary.returnedBorrow);
  std::optional<AddressRegion> borrowedRegion;
  if (pointerRelayOnly && summary.returnedBorrow && !borrowedAddress)
    borrowedRegion =
        inputRegionForCall(closedCalls->program, address, body, aliases, state,
                           instruction, *summary.returnedBorrow);
  AlternativeFields alternatives;
  if (instruction.result &&
      instruction.info.type.kind == SemanticType::Expected)
    for (const auto &[alternative, fields] : summary.resultAlternatives) {
      auto &selected = alternatives[alternative];
      for (const auto &[input, saved] : fields) {
        std::optional<FieldOrigin> origin;
        if (!input.parameter) {
          origin =
              receiverFieldOrigin(closedCalls->program, address, body, aliases,
                                  state, instruction, input.field);
        } else if (*input.parameter < argumentOrigins.size()) {
          const auto root = argumentOrigins[*input.parameter];
          if (root)
            origin = normalizedFieldOrigin(closedCalls->program, address, body,
                                           aliases, root, input.field);
        }
        if (origin && selected.size() < 64)
          selected.emplace(*origin, rebased(saved, block.id, instruction.id));
      }
    }
  std::vector<std::pair<ScalarAddress, ConstructedScalar>> constructed;
  std::vector<std::pair<MirPlaceId, std::pair<PrefixField, IntegerFact>>>
      appendedPrefixes;
  std::vector<std::pair<FieldOrigin, ScalarAddress>> pointerFields;
  for (const auto &[output, input] : summary.pointerFieldValues) {
    const auto root = output.parameter
                          ? (*output.parameter < argumentOrigins.size()
                                 ? argumentOrigins[*output.parameter]
                                 : 0)
                          : receiverOrigin;
    if (!root)
      continue;
    const auto field = ownedPointerFieldOrigin(
        &closedCalls->program, address, body, aliases, root, output.field);
    auto pointer = inputAddressForCall(closedCalls->program, address, body,
                                       aliases, state, instruction, input);
    const auto *type = field ? liveOwnedPathType(&closedCalls->program, address,
                                                 body, field->root, field->path)
                             : nullptr;
    const auto *pointee =
        pointer ? addressPointeeType(body, *pointer) : nullptr;
    if (field && pointer && type && pointee &&
        type->kind == SemanticType::RawPointer && type->arguments.size() == 1 &&
        type->arguments.front() == *pointee)
      pointerFields.emplace_back(*field, std::move(*pointer));
  }
  for (const auto &[destination, postcondition] :
       summary.constructedParameters) {
    auto pointer =
        inputAddressForCall(closedCalls->program, address, body, aliases, state,
                            instruction, destination);
    auto value = rebased(postcondition.value, block.id, instruction.id);
    stabilize(value);
    std::optional<std::size_t> valueParameter;
    if (postcondition.valueParameter) {
      if (*postcondition.valueParameter >= instruction.operands.size())
        continue;
      const auto &operand = instruction.operands[*postcondition.valueParameter];
      auto actual = postcondition.valueField.empty()
                        ? integerForOperand(body, state, operand)
                        : std::optional<IntegerFact>{};
      if (!postcondition.valueField.empty())
        if (const auto object = objectForOperand(body, aliases, state, operand))
          if (const auto field =
                  object->integers.find(postcondition.valueField);
              field != object->integers.end())
            actual = field->second;
      if (!actual || actual->domain != value.domain)
        continue;
      value = postcondition.valueField.empty()
                  ? *actual
                  : rebased(*actual, block.id, instruction.id, operand.value,
                            operand.place);
      valueParameter = postcondition.valueField.empty()
                           ? readonlyIntegerParameter(&closedCalls->program,
                                                      address, body, operand)
                           : readonlyRecordParameter(&closedCalls->program,
                                                     address, body, operand);
    }
    // A closed append proves construction at the old endpoint, advances that
    // endpoint by exactly one, and leaves the pointer field unchanged. The
    // existing typed-store profile excludes overlapping payload-width writes;
    // raw/native/destructive effects never enter this transfer.
    if (!destination.parameter && !destination.field.empty() &&
        destination.elementOffset == 0 && destination.index &&
        !destination.index->parameter && !destination.index->offset.negative &&
        destination.index->offset.magnitude == 0 && summary.scalarStoresOnly &&
        !summary.mayWriteGlobals && invocation &&
        !invocation->traits.maySynchronize &&
        summary.liveMemoryBarrier != LiveMemoryBarrier::Unknown &&
        value.domain.width > 8 &&
        (!summary.scalarStoreWidth ||
         *summary.scalarStoreWidth != value.domain.width)) {
      const auto &endPath = destination.index->field;
      const auto update = summary.receiverUpdates.find(endPath);
      const auto unchanged = summary.pointerFieldValues.find(
          InputField{std::nullopt, destination.field});
      if (update != summary.receiverUpdates.end() &&
          update->second.field == endPath &&
          update->second.domain == destination.index->domain &&
          !update->second.offset.negative &&
          update->second.offset.magnitude == 1 &&
          unchanged != summary.pointerFieldValues.end() &&
          unchanged->second ==
              InputAddress{
                  std::nullopt, destination.field, 0, std::nullopt, {}}) {
        const auto end =
            receiverFieldOrigin(closedCalls->program, address, body, aliases,
                                state, instruction, endPath);
        const auto field =
            ownedPointerFieldOrigin(&closedCalls->program, address, body,
                                    aliases, receiverOrigin, destination.field);
        if (end && field && end->root == field->root)
          if (const auto object =
                  extractObjectFact(body, aliases, end->root, state)) {
            const auto length = object->integers.find(end->path);
            const PrefixField path{field->path, end->path,
                                   destination.subobject};
            const auto previous = object->prefixes.find(path);
            if (length != object->integers.end() &&
                !length->second.lower.negative &&
                applyEntryRelative(length->second, update->second) &&
                ((singleton(length->second) &&
                  length->second.lower.magnitude == 0) ||
                 (previous != object->prefixes.end() &&
                  previous->second.domain == value.domain))) {
              auto envelope = previous == object->prefixes.end() ||
                                      (singleton(length->second) &&
                                       length->second.lower.magnitude == 0)
                                  ? value
                                  : joinedElementFact(previous->second, value);
              envelope.evidence =
                  merged(envelope.evidence, length->second.evidence);
              addInstruction(envelope.evidence, instruction.id);
              appendedPrefixes.emplace_back(
                  end->root, std::pair{path, std::move(envelope)});
            }
          }
      }
    }
    if (!pointer)
      continue;
    const auto *pointee = addressPointeeType(body, *pointer);
    if (!pointee || constantIntegerDomain(*pointee) != value.domain)
      continue;
    value.evidence = merged(value.evidence, pointer->evidence);
    add(value.evidence, block.id);
    addInstruction(value.evidence, instruction.id);
    constructed.emplace_back(
        *pointer, ConstructedScalar{value,
                                    valueParameter,
                                    {},
                                    valueParameter ? postcondition.valueField
                                                   : FieldPath{}});
  }
  // A missing numerical postcondition is not a write. Closed, non-writing,
  // non-escaping source calls retain caller memory even if analysis of local
  // result preparation no longer contains the borrowed input's object fact.
  const bool preservesMemory =
      !summary.hasOpaqueEffects && !summary.mayWriteGlobals && invocation &&
      (!invocation->traits.writesPlace || frameLocalWrites) &&
      !invocation->traits.writesUnknownMemory &&
      !invocation->traits.maySynchronize &&
      (pointerRelayOnly ||
       (!invocation->receiverEscapes &&
        std::none_of(invocation->argumentsEscape.begin(),
                     invocation->argumentsEscape.end(),
                     [](bool escapes) { return escapes; })));
  std::map<MirPlaceId, PrefixFacts> unchangedPrefixes;
  if (preservesMemory)
    for (const auto &[root, object] : state.objects)
      if (!object.prefixes.empty())
        unchangedPrefixes.emplace(root, object.prefixes);
  std::vector<std::pair<FieldOrigin, IntegerFact>> updatedFields;
  EntryRelativeFields composedUpdates;
  if (!summary.mayWriteGlobals && invocation &&
      !invocation->traits.maySynchronize &&
      summary.liveMemoryBarrier != LiveMemoryBarrier::Unknown) {
    for (const auto &[path, update] : summary.receiverUpdates) {
      const auto origin =
          receiverFieldOrigin(closedCalls->program, address, body, aliases,
                              state, instruction, path);
      if (!origin)
        continue;
      if (const auto object =
              extractObjectFact(body, aliases, origin->root, state))
        if (const auto field = object->integers.find(origin->path);
            field != object->integers.end())
          if (auto fact = applyEntryRelative(field->second, update)) {
            // Callee instruction/place identities are body-local. The call is
            // the replay boundary; retain caller input evidence and this call.
            add(fact->evidence, block.id);
            addInstruction(fact->evidence, instruction.id);
            updatedFields.emplace_back(*origin, std::move(*fact));
          }
      const auto *root = body.findPlace(origin->root);
      if (origin->root && (!root || root->root != MirPlaceRootKind::This ||
                           !root->projections.empty()))
        continue;
      if (const auto old = state.entryRelativeFields.find(origin->path);
          old != state.entryRelativeFields.end() &&
          old->second.domain == update.domain)
        if (auto composed = shiftEntryRelative(old->second, update.offset)) {
          add(composed->evidence, block.id);
          addInstruction(composed->evidence, instruction.id);
          composedUpdates.emplace(origin->path, std::move(*composed));
        }
    }
  }
  if (resultField && !summary.mayWriteGlobals && invocation &&
      !invocation->traits.writesPlace &&
      !invocation->traits.writesUnknownMemory &&
      !invocation->traits.maySynchronize) {
    if (const auto object =
            extractObjectFact(body, aliases, resultField->root, state))
      if (const auto field = object->integers.find(resultField->path);
          field != object->integers.end() &&
          constantIntegerDomain(instruction.info.type) == field->second.domain)
        currentResultField = field->second;
  }
  invalidateClosedCallInputs(closedCalls->program, body, aliases, instruction,
                             address, *target, summary.hasOpaqueEffects,
                             summary.liveMemoryBarrier, pointerRelayOnly,
                             closedCalls->localCaptures, state);
  if (summary.mayWriteGlobals) {
    // A checked reference or receiver can denote a callee-only global whose
    // Symbol is absent from this body. Current effect summaries do not carry
    // a written-global set, so invalidate possible aliases conservatively.
    // Independent SSA values and actual readonly scalar copies remain valid.
    state.mayWriteGlobals = true;
    killGlobalAliases(body, state);
  }
  state.liveMemoryBarrier =
      std::max(state.liveMemoryBarrier, summary.liveMemoryBarrier);
  const auto disjointFields =
      constructed.empty() ? std::vector<FieldOrigin>{}
                          : liveScalarFields(&closedCalls->program, address,
                                             body, aliases, state);
  for (auto &[pointer, value] : constructed) {
    // Capture after the complete call's input invalidation and before its
    // postconditions. A field rebuilt by the callee is not an independently
    // live object merely because the callee also exports a scalar value.
    value.disjointFields = disjointFields;
    publishConstructedScalar(state, addressKey(pointer), std::move(value));
  }
  if (summary.hasOpaqueEffects) {
    // Opacity blocks recurrence invariance; it is not independent write
    // authority. The exact effect summary above governs caller invalidation.
    state.hasOpaqueEffects = true;
  }
  // A call-result loan designates the returned referent whatever value
  // category the call expression itself received; a compound-assignment
  // target, for example, produces that loan from a value-category call.
  const MirLoan *resultLoan =
      instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
  const bool referentResult =
      resultLoan && (instruction.info.category == ValueCategory::Place ||
                     resultLoan->kind == MirLoanKind::CallResult);
  if (instruction.result) {
    if (borrowedAddress && referentResult &&
        addressPointeeType(body, *borrowedAddress) &&
        *addressPointeeType(body, *borrowedAddress) == instruction.info.type) {
      add(borrowedAddress->evidence, block.id);
      addInstruction(borrowedAddress->evidence, instruction.id);
      addValue(borrowedAddress->evidence, *instruction.result);
      state.borrowedAddresses[*instruction.loan] = *borrowedAddress;
    } else if (borrowedRegion && referentResult &&
               addressPointeeType(body, borrowedRegion->base) &&
               *addressPointeeType(body, borrowedRegion->base) ==
                   instruction.info.type) {
      add(borrowedRegion->base.evidence, block.id);
      addInstruction(borrowedRegion->base.evidence, instruction.id);
      addValue(borrowedRegion->base.evidence, *instruction.result);
      state.borrowedRegions[*instruction.loan] = std::move(*borrowedRegion);
    }
    if (returnedAddress &&
        instruction.info.type.kind == SemanticType::RawPointer &&
        instruction.info.type.arguments.size() == 1 &&
        addressPointeeType(body, *returnedAddress) &&
        *addressPointeeType(body, *returnedAddress) ==
            instruction.info.type.arguments.front()) {
      add(returnedAddress->evidence, block.id);
      addInstruction(returnedAddress->evidence, instruction.id);
      addValue(returnedAddress->evidence, *instruction.result);
      state.scalarAddressValues[*instruction.result] =
          std::move(*returnedAddress);
    }
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
    // Source-constructor effects have already been applied above. Publishing
    // its successful result into a fresh temporary is only a shell write.
    invalidatePlaceRoot(body, aliases, state, *instruction.destination, true);
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
    } else if (!preservesMemory) {
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
    } else if (!preservesMemory) {
      invalidateObjectOperand(body, aliases, instruction.operands[index],
                              state);
    }
  }
  // Joined absolute postconditions describe every invocation. A proved
  // entry-relative update instead transforms this particular caller's state.
  // Publish it last so older nested summaries cannot widen it away.
  for (auto &[origin, fact] : updatedFields) {
    // A callee's absolute postcondition can impose an invariant envelope
    // (for example a guarded increment never exceeds four). Retain that
    // independently proved bound when it contains this caller's translated
    // interval; otherwise normal context discovery must widen the callee.
    if (const auto object =
            extractObjectFact(body, aliases, origin.root, state))
      if (const auto current = object->integers.find(origin.path);
          current != object->integers.end() &&
          current->second.domain == fact.domain &&
          current->second.wideningEnvelope) {
        const auto bounds = *current->second.wideningEnvelope;
        if (compare(bounds.first, fact.lower) <= 0 &&
            compare(bounds.second, fact.upper) >= 0) {
          auto envelope = fact.wideningEnvelope.value_or(bounds);
          if (compare(bounds.first, envelope.first) > 0)
            envelope.first = bounds.first;
          if (compare(bounds.second, envelope.second) < 0)
            envelope.second = bounds.second;
          fact.wideningEnvelope = envelope;
          fact.evidence = merged(fact.evidence, current->second.evidence);
        }
      }
    publishFieldFact(body, state, origin, fact);
  }
  for (auto &[path, update] : composedUpdates)
    state.entryRelativeFields.insert_or_assign(path, std::move(update));
  if (instruction.result && resultField) {
    state.fieldOrigins.insert_or_assign(*instruction.result, *resultField);
    if (currentResultField) {
      // A shared getter summary joins all calling contexts. It must not erase
      // this caller's more precise current field when the exact invocation
      // cannot write memory. The return identity is independently proved.
      state.integers[*instruction.result] = rebased(
          *currentResultField, block.id, instruction.id, *instruction.result);
      publishFieldFact(body, state, *resultField, *currentResultField);
    }
    const auto *root = body.findPlace(resultField->root);
    if (!resultField->root || (root && root->root == MirPlaceRootKind::This &&
                               root->projections.empty()))
      if (const auto relative =
              state.entryRelativeFields.find(resultField->path);
          relative != state.entryRelativeFields.end()) {
        auto value = relative->second;
        addInstruction(value.evidence, instruction.id);
        addValue(value.evidence, *instruction.result);
        state.entryRelativeValues[*instruction.result] = std::move(value);
      }
  } else if (instruction.result)
    state.fieldOrigins.erase(*instruction.result);
  if (instruction.result && !alternatives.empty() &&
      state.resultAlternatives.size() < 64)
    state.resultAlternatives[*instruction.result] = std::move(alternatives);
  restoreDifferentWidthFields(state, untouchedFields);
  for (auto &[field, pointer] : pointerFields) {
    if (state.fieldAddresses.size() >= 1024 &&
        !state.fieldAddresses.contains(field))
      continue;
    add(pointer.evidence, block.id);
    addInstruction(pointer.evidence, instruction.id);
    state.fieldAddresses.insert_or_assign(field, std::move(pointer));
  }
  for (auto &[root, prefixes] : unchangedPrefixes)
    if (const auto object = state.objects.find(root);
        object != state.objects.end())
      object->second.prefixes = std::move(prefixes);
  for (auto &[root, prefix] : appendedPrefixes) {
    auto &object = state.objects[root];
    if (object.prefixes.size() < 64 || object.prefixes.contains(prefix.first))
      object.prefixes.insert_or_assign(std::move(prefix.first),
                                       std::move(prefix.second));
  }
  return true;
}

void killUnknownMemory(const MirBody &body, AbstractState &state) {
  state.resultAlternatives.clear();
  state.placeAlternatives.clear();
  state.liveMemoryBarrier = LiveMemoryBarrier::Unknown;
  state.constructedScalars.clear();
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
  state.fieldOrigins.clear();
  state.entryRelativeFields.clear();
  state.entryRelativeValues.clear();
  preserveEntryPointerIdentities(body, state);
  state.aggregateOrigins.clear();
  state.objectValues.clear();
}

void killGlobalAliases(const MirBody &body, AbstractState &state) {
  std::map<MirPlaceId, IntegerFact> independent;
  for (const auto &[id, fact] : state.integerPlaces) {
    const auto *place = body.findPlace(id);
    if (place && place->root == MirPlaceRootKind::Binding && place->binding &&
        place->projections.empty() && !place->traits.containsBorrowedState &&
        constantIntegerDomain(place->type))
      independent.emplace(id, fact);
  }
  // A represented direct global write can alias checked referents, but does
  // not overwrite independent scalar local/parameter storage. Actual raw or
  // unknown writes still take killUnknownMemory instead.
  killUnknownMemory(body, state);
  state.integerPlaces.insert(independent.begin(), independent.end());
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

[[nodiscard]] bool ownerWitnessFormation(const MirBody &body,
                                         const MirInstruction &instruction) {
  // The verified loan asserts lifetime/access, not address identity or the
  // pointee's value. Forming it does not modify its witness or other memory.
  // Actual reads/writes through the loan still use normal alias invalidation.
  const auto effect = effects(instruction);
  return mir_detail::rawBorrowResultLoan(body, instruction) &&
         !instruction.rawMemoryAccess && !effect.writesPlace &&
         !effect.writesUnknownMemory && !effect.invokesUserCode &&
         !effect.invokesRuntime && !effect.maySynchronize;
}

[[nodiscard]] bool nonMutatingLoanBoundary(const MirBody &body,
                                           const MirInstruction &instruction) {
  if ((instruction.kind != MirInstructionKind::Borrow &&
       instruction.kind != MirInstructionKind::ActivateBorrow &&
       instruction.kind != MirInstructionKind::EndBorrow) ||
      !instruction.loan || !body.findLoan(*instruction.loan) ||
      instruction.result || instruction.destination || instruction.receiver ||
      instruction.rawMemoryAccess || !instruction.lifecycle.empty() ||
      instruction.operation != MirOperation::None ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.bodyTarget || instruction.nativeCallbackAdapter ||
      instruction.threadTaskAdapter || instruction.hostedStartupOperation ||
      instruction.synchronization.kind != SynchronizationOperationKind::None)
    return false;
  const auto effect = effects(instruction);
  return !effect.writesPlace && !effect.writesUnknownMemory &&
         !effect.invokesUserCode && !effect.invokesRuntime &&
         !effect.maySynchronize;
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

[[nodiscard]] const SemanticType *
liveOwnedPathType(const MirProgram *program, MirBodyAddress address,
                  const MirBody &body, MirPlaceId root,
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
        return nullptr;
      fieldSeen = true;
      type = ordinaryFieldType(program, type, projection.field);
      if (!type)
        return nullptr;
    }
  for (const auto field : suffix) {
    type = ordinaryFieldType(program, type, field);
    if (!type)
      return nullptr;
  }
  return type;
}

[[nodiscard]] bool liveScalarPath(const MirProgram *program,
                                  MirBodyAddress address, const MirBody &body,
                                  MirPlaceId root,
                                  const FieldPath &suffix = {}) {
  const auto *type = liveOwnedPathType(program, address, body, root, suffix);
  const auto domain = type ? constantIntegerDomain(*type) : std::nullopt;
  return domain && domain->width > 8;
}

[[nodiscard]] std::optional<FieldOrigin>
ownedPointerFieldOrigin(const MirProgram *program, MirBodyAddress address,
                        const MirBody &body,
                        const MirPlaceAliasEscapeAnalysis &aliases,
                        MirPlaceId id, const FieldPath &suffix) {
  const auto *place = body.findPlace(id);
  if (!place || (place->root != MirPlaceRootKind::This &&
                 place->root != MirPlaceRootKind::Binding))
    return std::nullopt;
  const auto root =
      canonicalPlace(aliases, place->root == MirPlaceRootKind::This
                                  ? thisRoot(body)
                                  : rootForBinding(body, place->binding));
  if (!root && place->root != MirPlaceRootKind::This)
    return std::nullopt;
  auto path = fieldPath(body, root, id, true);
  if (!path)
    return std::nullopt;
  path->insert(path->end(), suffix.begin(), suffix.end());
  const auto *type = liveOwnedPathType(program, address, body, root, *path);
  if (path->empty() || !type || type->kind != SemanticType::RawPointer ||
      type->arguments.size() != 1)
    return std::nullopt;
  return FieldOrigin{root, std::move(*path)};
}

[[nodiscard]] std::optional<FieldOrigin> fieldOriginForPlace(
    const MirProgram *program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, MirPlaceId source) {
  const auto *place = body.findPlace(source);
  if (!place)
    return std::nullopt;
  const bool receiver = place->root == MirPlaceRootKind::This;
  const MirPlaceId root = receiver ? canonicalPlace(aliases, thisRoot(body))
                                   : aggregateRoot(body, aliases, source);
  if (!receiver && !root)
    return std::nullopt;
  const auto path = fieldPath(body, root, source);
  if (!path || !liveScalarPath(program, address, body, root, *path))
    return std::nullopt;
  return FieldOrigin{.root = root, .path = *path};
}

[[nodiscard]] std::optional<FieldOrigin> receiverFieldOrigin(
    const MirProgram &program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, const AbstractState &state,
    const MirInstruction &instruction, const FieldPath &path) {
  if (!instruction.receiver)
    return std::nullopt;
  const MirPlaceId receiver =
      objectOriginForOperand(body, aliases, state, *instruction.receiver);
  return normalizedFieldOrigin(program, address, body, aliases, receiver, path);
}

[[nodiscard]] std::optional<FieldOrigin>
normalizedFieldOrigin(const MirProgram &program, MirBodyAddress address,
                      const MirBody &body,
                      const MirPlaceAliasEscapeAnalysis &aliases,
                      MirPlaceId receiver, const FieldPath &path) {
  const auto *place = body.findPlace(receiver);
  if (!place || !liveScalarPath(&program, address, body, receiver, path))
    return std::nullopt;
  FieldOrigin origin{.root = receiver, .path = path};
  if (place->root == MirPlaceRootKind::This ||
      place->root == MirPlaceRootKind::Binding) {
    const auto root =
        canonicalPlace(aliases, place->root == MirPlaceRootKind::This
                                    ? thisRoot(body)
                                    : rootForBinding(body, place->binding));
    const auto prefix = fieldPath(body, root, receiver, true);
    if (!prefix)
      return std::nullopt;
    origin.root = root;
    origin.path = *prefix;
    origin.path.insert(origin.path.end(), path.begin(), path.end());
  }
  if (!liveScalarPath(&program, address, body, origin.root, origin.path))
    return std::nullopt;
  return origin;
}

void preserveLiveScalarFactsForConstruction(const MirProgram *program,
                                            MirBodyAddress address,
                                            const MirBody &body,
                                            AbstractState &state) {
  state.resultAlternatives.clear();
  state.placeAlternatives.clear();
  // The sealed operation requires vacant, nonconflicting final storage. Its
  // structural form invokes no constructor/user code, so it cannot overwrite an
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
    object.scalarPointees.clear();
    object.prefixes.clear();
  }
  state.arrayElements.clear();
  state.aggregateElements.clear();
  state.aggregateElementIntegers.clear();
  state.loadedOrigins.clear();
  state.fieldOrigins.clear();
  std::erase_if(state.entryRelativeFields, [&](const auto &entry) {
    return !liveScalarPath(program, address, body, 0, entry.first);
  });
  state.entryRelativeValues.clear();
  preserveEntryPointerIdentities(body, state, true);
  state.aggregateOrigins.clear();
  state.objectValues.clear();
  state.hasOpaqueEffects = true;
  state.liveMemoryBarrier = std::max(
      state.liveMemoryBarrier, LiveMemoryBarrier::VacantStructuralConstruction);
}

[[nodiscard]] std::vector<FieldOrigin> liveScalarFields(
    const MirProgram *program, MirBodyAddress address, const MirBody &body,
    const MirPlaceAliasEscapeAnalysis &aliases, const AbstractState &state) {
  std::vector<FieldOrigin> result;
  const auto retain = [&](FieldOrigin field) {
    // Omitted relations lose precision. Never substitute a larger region or
    // use an interval of field identities when the budget is exhausted.
    if (result.size() < 64 &&
        std::find(result.begin(), result.end(), field) == result.end())
      result.push_back(std::move(field));
  };
  for (const auto &[root, object] : state.objects)
    for (const auto &[path, fact] : object.integers) {
      (void)fact;
      if (liveScalarPath(program, address, body, root, path))
        retain({root, path});
    }
  for (const auto &[place, fact] : state.integerPlaces) {
    (void)fact;
    if (const auto field =
            fieldOriginForPlace(program, address, body, aliases, place))
      retain(*field);
  }
  return result;
}

[[nodiscard]] bool independentValueParameter(const MirProgram *program,
                                             MirBodyAddress address,
                                             const MirPlace &place) {
  if (!program || place.root != MirPlaceRootKind::Binding || !place.binding ||
      place.traits.containsBorrowedState)
    return false;
  // The selected formal, not a reference's projected pointee type, proves
  // that this body owns independent parameter storage. Callers still prove
  // the particular scalar/generated operation touches only that storage.
  return parameterOrdinal(*program, address, place).has_value() &&
         (isMirScalarValueType(program, place.type) ||
          place.type.kind == SemanticType::Class);
}

void invalidateValueStorage(const MirProgram *program, MirBodyAddress address,
                            const MirBody &body,
                            const MirPlaceAliasEscapeAnalysis &aliases,
                            AbstractState &state, MirPlaceId source,
                            bool passiveLocalDrop = false) {
  const auto *place = body.findPlace(source);
  std::map<MirPlaceId, ObjectFact> receivers;
  std::map<MirPlaceId, IntegerFact> fields;
  EntryRelativeFields updates;
  ConstructedScalars pointees;
  std::map<FieldOrigin, ScalarAddress> receiverPointers;
  // The existing formal profile requires the exact by-value signature. A
  // passive drop may additionally retire independent local storage, witnessed
  // by its unprojected owning initialization. Reference/payload projections,
  // initially available inputs and synthesized field bindings do not qualify.
  const bool independentLocal =
      passiveLocalDrop && place &&
      (address.kind == MirBodyKind::Function ||
       address.kind == MirBodyKind::Constructor) &&
      place->root == MirPlaceRootKind::Binding && place->binding &&
      place->projections.empty() && !place->initiallyAvailable &&
      !place->traits.containsBorrowedState &&
      place->type.kind != SemanticType::Reference &&
      rootForBinding(body, place->binding) == source &&
      std::any_of(
          body.blocks.begin(), body.blocks.end(), [&](const auto &block) {
            return std::any_of(
                block.instructions.begin(), block.instructions.end(),
                [&](const auto &initialization) {
                  return !initialization.tombstone &&
                         initialization.kind ==
                             MirInstructionKind::Initialize &&
                         initialization.destination == source &&
                         initialization.operands.size() == 1 &&
                         initialization.operands.front().type == place->type;
                });
          });
  const bool independentTemporary =
      place && place->root == MirPlaceRootKind::Temporary &&
      place->projections.empty() && !place->initiallyAvailable &&
      !place->traits.containsBorrowedState;
  if (place && (independentLocal || independentTemporary ||
                independentValueParameter(program, address, *place))) {
    // Structural movement/cleanup of independent value storage cannot write
    // an owned receiver field. Retain entry pointer identities, not addresses
    // observed after retargeting to storage that might now be retired.
    for (const auto &[field, pointer] : state.fieldAddresses) {
      const auto *root = body.findPlace(field.root);
      if ((!field.root || (root && root->root == MirPlaceRootKind::This)) &&
          pointer.kind != AddressOriginKind::ExistingPlace &&
          !pointer.observation)
        receiverPointers.emplace(field, pointer);
    }
    for (const auto &[pointer, fact] : state.constructedScalars)
      if (pointer.kind != AddressOriginKind::ExistingPlace &&
          !pointer.observation)
        pointees.emplace(pointer, fact);
    updates = state.entryRelativeFields;
    std::erase_if(updates, [&](const auto &entry) {
      return !liveScalarPath(program, address, body, 0, entry.first);
    });
    for (const auto &[root, object] : state.objects) {
      const auto *storage = body.findPlace(root);
      if (root && (!storage || storage->root != MirPlaceRootKind::This))
        continue;
      ObjectFact receiver = object;
      std::erase_if(receiver.integers, [&](const auto &entry) {
        return !liveScalarPath(program, address, body, root, entry.first);
      });
      receiver.elements.reset();
      receiver.elementIntegers.clear();
      std::erase_if(receiver.scalarPointees, [&](const auto &entry) {
        return !receiverPointers.contains({root, entry.first.pointer});
      });
      std::erase_if(receiver.prefixes, [&](const auto &entry) {
        return !receiverPointers.contains({root, entry.first.pointer}) ||
               !receiver.integers.contains(entry.first.end);
      });
      receivers.emplace(root, std::move(receiver));
    }
    for (const auto &[root, fact] : state.integerPlaces)
      if (const auto *storage = body.findPlace(root);
          storage && storage->root == MirPlaceRootKind::This &&
          liveScalarPath(program, address, body, root))
        fields.emplace(root, fact);
  }
  invalidatePlaceRoot(body, aliases, state, source);
  for (auto &[root, object] : receivers)
    state.objects.insert_or_assign(root, std::move(object));
  for (auto &[root, fact] : fields)
    state.integerPlaces.insert_or_assign(root, std::move(fact));
  state.entryRelativeFields.insert(updates.begin(), updates.end());
  state.fieldAddresses.insert(receiverPointers.begin(), receiverPointers.end());
  for (auto &[pointer, fact] : pointees)
    publishConstructedScalar(state, pointer, std::move(fact));
}

[[nodiscard]] bool scalarResultMove(const MirProgram *program,
                                    const MirInstruction &instruction,
                                    const MirBody &body) {
  if (instruction.kind != MirInstructionKind::Move || !instruction.result ||
      instruction.destination || instruction.operands.size() != 1 ||
      instruction.operands.front().kind != MirOperandKind::Move ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      !instruction.lifecycle.empty() || instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      !isMirScalarValueType(program, instruction.info.type))
    return false;
  const auto *source = body.findPlace(instruction.operands.front().place);
  const auto effect = effects(instruction);
  return source && source->type == instruction.info.type &&
         instruction.operands.front().type == source->type &&
         !source->traits.containsBorrowedState && !effect.readsUnknownMemory &&
         !effect.writesUnknownMemory && !effect.invokesUserCode &&
         !effect.invokesRuntime && !effect.maySynchronize;
}

// Constructing an ordinary record in vacant final storage with an exact source
// constructor applies that constructor's closed summary. Its exit field facts
// become constructed cells at the exact pointee; vacant storage cannot
// overwrite a live object. A pending summary postpones the caller like any
// other closed call, and an unsupported one keeps the general barrier.
[[nodiscard]] bool applyClosedObjectConstruction(
    const MirBody &body, const MirPlaceAliasEscapeAnalysis &aliases,
    const MirBlock &block, const MirInstruction &instruction,
    AbstractState &state, ClosedCallRound *closedCalls,
    MirBodyAddress address) {
  if (closedCalls == nullptr)
    return false;
  const MirProgram *program = &closedCalls->program;
  const auto target = closedObjectConstructTarget(*program, instruction);
  if (!target)
    return false;
  const auto pointer =
      scalarAddressForOperand(body, state, instruction.operands.front());
  MirInstruction constructor = instruction;
  constructor.kind = MirInstructionKind::Construct;
  constructor.intrinsic = IntrinsicKind::None;
  constructor.unsafeOperation = UnsafeOperationKind::None;
  constructor.operands.erase(constructor.operands.begin());
  constructor.info.type = instruction.operands.front().type.arguments.front();
  if (!applyClosedCall(body, aliases, block, constructor, state, closedCalls,
                       address))
    return false;
  const auto summary = closedCalls->summaries.find(*target);
  if (!state.reachable || summary == closedCalls->summaries.end() ||
      !summary->second.complete)
    return true;
  preserveLiveScalarFactsForConstruction(program, address, body, state);
  const auto &constructed = summary->second.constructed;
  if (!pointer || !constructed.observed || !constructed.fact)
    return true;
  const auto disjoint =
      liveScalarFields(program, address, body, aliases, state);
  for (const auto &[path, field] : constructed.fact->integers) {
    auto cell = projectPointee(program, body, *pointer, path);
    const auto *type = cell ? addressPointeeType(body, *cell) : nullptr;
    if (path.empty() || !type || field.domain.width <= 8 ||
        constantIntegerDomain(*type) != field.domain)
      continue;
    auto fact = field;
    stabilize(fact);
    fact = rebased(std::move(fact), block.id, instruction.id);
    fact.evidence = merged(fact.evidence, pointer->evidence);
    add(fact.evidence, block.id);
    addInstruction(fact.evidence, instruction.id);
    publishConstructedScalar(
        state, addressKey(*cell),
        ConstructedScalar{std::move(fact), std::nullopt, disjoint});
  }
  return true;
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
  if (instruction.result) {
    state.resultAlternatives.erase(*instruction.result);
    state.entryRelativeValues.erase(*instruction.result);
    state.scalarAddressValues.erase(*instruction.result);
    if (instruction.loan) {
      state.borrowedAddresses.erase(*instruction.loan);
      state.borrowedRegions.erase(*instruction.loan);
    }
  }
  const auto unknownEffectBarrier = [&] {
    const auto uncaptured = preserveUncapturedLocals(
        program, address, body,
        closedCalls == nullptr ? nullptr : closedCalls->localCaptures,
        instruction, state);
    killUnknownMemory(body, state);
    restoreDifferentWidthFields(state, uncaptured);
    state.hasOpaqueEffects = true;
  };
  const auto effect = effects(instruction);
  if (((effect.writesPlace || effect.initializesValue || effect.dropsValue) &&
       instruction.destination &&
       mayAliasRepresentedGlobal(body, aliases, *instruction.destination)) ||
      std::any_of(instruction.operands.begin(), instruction.operands.end(),
                  [&](const auto &operand) {
                    return operand.kind == MirOperandKind::Move &&
                           mayAliasRepresentedGlobal(body, aliases,
                                                     operand.place);
                  })) {
    state.mayWriteGlobals = true;
    killGlobalAliases(body, state);
  }
  // The empty full-expression marker can inherit the source expression's
  // unsafe classification. It does not repeat that expression's operation;
  // its actual Load/Call/Compute instruction already carries the effects.
  // The same holds for checked loan bookkeeping without raw access: keep its
  // observable schedule, but do not repeat the producer's memory operation.
  if (nonMutatingLoanBoundary(body, instruction)) {
    if (instruction.kind == MirInstructionKind::EndBorrow) {
      state.borrowedAddresses.erase(*instruction.loan);
      state.borrowedRegions.erase(*instruction.loan);
    } else if (instruction.kind == MirInstructionKind::Borrow) {
      std::optional<ScalarAddress> pointer;
      // A field reborrow can carry its parent loan as the operand. The
      // child's typed source place owns the projection; the lifetime witness
      // alone never establishes a pointee address.
      if (const auto *loan = body.findLoan(*instruction.loan))
        pointer = borrowedAddressForPlace(program, body, state, loan->source);
      if (!pointer && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Loan)
        if (const auto found =
                state.borrowedAddresses.find(instruction.operands.front().loan);
            found != state.borrowedAddresses.end() &&
            addressPointeeType(body, found->second) &&
            *addressPointeeType(body, found->second) == instruction.info.type)
          pointer = found->second;
      if (!pointer && instruction.operands.size() == 1 &&
          instruction.operands.front().place)
        pointer = borrowedAddressForPlace(program, body, state,
                                          instruction.operands.front().place);
      state.borrowedAddresses.erase(*instruction.loan);
      state.borrowedRegions.erase(*instruction.loan);
      if (pointer) {
        state.borrowedAddresses[*instruction.loan] = *pointer;
      } else {
        // The same typed-source rule relays a region reborrow.
        std::optional<AddressRegion> region;
        if (const auto *loan = body.findLoan(*instruction.loan))
          region = borrowedRegionForPlace(program, body, state, loan->source);
        if (!region && instruction.operands.size() == 1 &&
            instruction.operands.front().kind == MirOperandKind::Loan)
          if (const auto found =
                  state.borrowedRegions.find(instruction.operands.front().loan);
              found != state.borrowedRegions.end() &&
              addressPointeeType(body, found->second.base) &&
              *addressPointeeType(body, found->second.base) ==
                  instruction.info.type)
            region = found->second;
        if (!region && instruction.operands.size() == 1 &&
            instruction.operands.front().place)
          region = borrowedRegionForPlace(program, body, state,
                                          instruction.operands.front().place);
        if (region)
          state.borrowedRegions[*instruction.loan] = std::move(*region);
      }
    }
    return;
  }
  if (inertLifecycleMarker(instruction))
    return;
  // Preparing an already evaluated pointer SSA value does not repeat the
  // source expression's unsafe operation. Keep this exact staging transfer
  // before the raw-operation barrier; the original operation is unchanged.
  if (instruction.kind == MirInstructionKind::CallInput && instruction.result &&
      instruction.operands.size() == 1 &&
      instruction.operands[0].kind == MirOperandKind::Value &&
      instruction.operands[0].type.kind == SemanticType::RawPointer &&
      instruction.info.type.kind == SemanticType::RawPointer &&
      instruction.operands[0].type.arguments ==
          instruction.info.type.arguments &&
      !instruction.rawMemoryAccess && instruction.lifecycle.empty() &&
      !instruction.destination && !instruction.receiver &&
      instruction.operation == MirOperation::None &&
      instruction.intrinsic == IntrinsicKind::None && !effect.writesPlace &&
      !effect.readsUnknownMemory && !effect.writesUnknownMemory &&
      !effect.invokesUserCode && !effect.invokesRuntime &&
      !effect.maySynchronize && !effect.movesValue && !effect.dropsValue) {
    if (auto pointer =
            scalarAddressForOperand(body, state, instruction.operands[0])) {
      add(pointer->evidence, block.id);
      addInstruction(pointer->evidence, instruction.id);
      addValue(pointer->evidence, *instruction.result);
      state.scalarAddressValues[*instruction.result] = std::move(*pointer);
    }
    return;
  }
  if (applyClosedObjectConstruction(body, aliases, block, instruction, state,
                                    closedCalls, address))
    return;
  if (program ? isMirStructuralFinalAddressConstruction(*program, instruction)
              : isMirScalarFinalAddressConstruction(instruction)) {
    std::optional<ScalarAddress> pointer;
    std::optional<IntegerFact> value;
    std::optional<ObjectFact> record;
    std::optional<std::size_t> parameter;
    pointer = scalarAddressForOperand(body, state, instruction.operands[0]);
    // Every scalar leaf shares the vacant-construction effect profile; only
    // an integer pointee also publishes the constructed value.
    const auto integerPointee =
        isMirScalarFinalAddressConstruction(instruction)
            ? constantIntegerDomain(
                  instruction.operands[0].type.arguments.front())
            : std::nullopt;
    if (integerPointee && instruction.operands.size() == 1) {
      // The sealed zero-argument scalar construction value-initializes its
      // exact pointee. This is a successful construction postcondition, not
      // a claim that vacant bytes or neighbouring elements contain zero.
      value = IntegerFact{.domain = *integerPointee, .lower = {}, .upper = {}};
    } else if (instruction.operands.size() == 2 &&
               (integerPointee ||
                !isMirScalarFinalAddressConstruction(instruction))) {
      if (integerPointee) {
        value = integerForOperand(body, state, instruction.operands[1]);
        parameter = readonlyIntegerParameter(program, address, body,
                                             instruction.operands[1]);
      } else {
        record =
            objectForOperand(body, aliases, state, instruction.operands[1]);
        parameter = readonlyRecordParameter(program, address, body,
                                            instruction.operands[1]);
      }
    }
    preserveLiveScalarFactsForConstruction(program, address, body, state);
    // Byte scalars can be backing representation for later construction, so
    // this live-object profile deliberately does not retain them.
    if (pointer && value && value->domain.width > 8) {
      value->evidence = merged(value->evidence, pointer->evidence);
      add(value->evidence, block.id);
      addInstruction(value->evidence, instruction.id);
      publishConstructedScalar(
          state, addressKey(*pointer),
          ConstructedScalar{
              *value, parameter,
              liveScalarFields(program, address, body, aliases, state)});
    }
    if (pointer && record) {
      const auto disjoint =
          liveScalarFields(program, address, body, aliases, state);
      for (const auto &[path, field] : record->integers) {
        auto cell = projectPointee(program, body, *pointer, path);
        const auto *type = cell ? addressPointeeType(body, *cell) : nullptr;
        if (path.empty() || !type || field.domain.width <= 8 ||
            constantIntegerDomain(*type) != field.domain)
          continue;
        auto fact = rebased(field, block.id, instruction.id,
                            instruction.operands[1].value,
                            instruction.operands[1].place);
        fact.evidence = merged(fact.evidence, pointer->evidence);
        add(fact.evidence, block.id);
        addInstruction(fact.evidence, instruction.id);
        publishConstructedScalar(
            state, addressKey(*cell),
            ConstructedScalar{std::move(fact), parameter, disjoint,
                              parameter ? path : FieldPath{}});
      }
    }
    return;
  }
  if (inertRawAddressOperation(instruction)) {
    // Address arithmetic/conversion neither observes nor modifies a pointee.
    // An exact nonnegative element displacement preserves typed identity, but
    // establishes no extent, lifetime or initialized-element coverage.
    if (instruction.operands.size() == 1 &&
        instruction.info.type.kind == SemanticType::RawPointer &&
        instruction.info.type.arguments.size() == 1 &&
        instruction.operands[0].type.kind == SemanticType::RawPointer &&
        instruction.operands[0].type.arguments ==
            instruction.info.type.arguments &&
        (instruction.intrinsic == IntrinsicKind::RawPointerConversion ||
         instruction.operation == MirOperation::Convert)) {
      if (auto pointer =
              scalarAddressForOperand(body, state, instruction.operands[0])) {
        add(pointer->evidence, block.id);
        addInstruction(pointer->evidence, instruction.id);
        addValue(pointer->evidence, *instruction.result);
        state.scalarAddressValues[*instruction.result] = std::move(*pointer);
      }
    } else if ((instruction.operation == MirOperation::PointerAdd ||
                instruction.operation == MirOperation::PointerSubtract) &&
               instruction.operands.size() == 2) {
      auto pointer =
          scalarAddressForOperand(body, state, instruction.operands[0]);
      const auto offset =
          integerForOperand(body, state, instruction.operands[1]);
      if (pointer && offset && !offset->lower.negative &&
          instruction.info.type.kind == SemanticType::RawPointer &&
          instruction.info.type.arguments ==
              instruction.operands[0].type.arguments) {
        const auto relative =
            entryRelativeValue(state, instruction.operands[1]);
        if (relative && !pointer->index &&
            instruction.operation == MirOperation::PointerAdd) {
          pointer->index =
              EntryIndex{relative->field, relative->domain, relative->offset};
          pointer->evidence = merged(pointer->evidence, relative->evidence);
        } else if (const auto parameter = readonlyIntegerParameter(
                       program, address, body, instruction.operands[1]);
                   parameter && !pointer->index &&
                   instruction.operation == MirOperation::PointerAdd) {
          pointer->index = EntryIndex{{}, offset->domain, {}, parameter};
        } else if (singleton(*offset)) {
          pointer = offsetAddress(*pointer, offset->lower.magnitude,
                                  instruction.operation ==
                                      MirOperation::PointerSubtract);
        } else {
          pointer.reset();
        }
        if (pointer) {
          pointer->evidence = merged(pointer->evidence, offset->evidence);
          add(pointer->evidence, block.id);
          addInstruction(pointer->evidence, instruction.id);
          addValue(pointer->evidence, *instruction.result);
          state.scalarAddressValues[*instruction.result] = std::move(*pointer);
        }
      }
    }
    state.integers.erase(*instruction.result);
    state.arrayValues.erase(*instruction.result);
    state.objectValues.erase(*instruction.result);
    state.fieldOrigins.erase(*instruction.result);
    return;
  }
  if (ownerWitnessFormation(body, instruction)) {
    if (auto pointer =
            scalarAddressForOperand(body, state, instruction.operands[0])) {
      add(pointer->evidence, block.id);
      addInstruction(pointer->evidence, instruction.id);
      addValue(pointer->evidence, *instruction.result);
      state.borrowedAddresses[*instruction.loan] = *pointer;
    }
    state.integers.erase(*instruction.result);
    state.arrayValues.erase(*instruction.result);
    state.objectValues.erase(*instruction.result);
    state.loadedOrigins.erase(*instruction.result);
    state.fieldOrigins.erase(*instruction.result);
    state.aggregateOrigins.erase(*instruction.result);
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
    // A typed read through an exact address-of can use that place's current
    // value. A loan witness is never an address identity. Keep the raw load
    // itself, its unsafe obligation and the opaque call boundary; this is
    // solely a local value fact, not permission to speculate or remove it.
    if (instruction.kind == MirInstructionKind::Load && instruction.result &&
        instruction.operands.size() == 1 && instruction.rawMemoryAccess &&
        instruction.unsafeOperation == UnsafeOperationKind::RawDereference &&
        instruction.lifecycle.empty() && !effect.writesPlace &&
        !effect.writesUnknownMemory && !effect.invokesUserCode &&
        !effect.maySynchronize &&
        constantIntegerDomain(instruction.info.type)) {
      const auto *place = body.findPlace(instruction.operands.front().place);
      if (place && place->root == MirPlaceRootKind::Value &&
          place->projections.size() == 1 &&
          place->projections.front().kind ==
              MirProjectionKind::RawDereference) {
        const auto found = state.scalarAddressValues.find(place->value);
        if (found != state.scalarAddressValues.end() &&
            place->type == instruction.info.type) {
          if (auto value =
                  integerAtAddress(body, state, found->second,
                                   instruction.info.type, program, address)) {
            value->evidence = merged(value->evidence, found->second.evidence);
            state.integers[*instruction.result] =
                rebased(std::move(*value), block.id, instruction.id,
                        *instruction.result, place->id);
            state.hasOpaqueEffects = true;
            return;
          }
        }
      }
    }
    // Other raw operations have no supported load/store transfer and cannot
    // establish an effect-closed invocation. Earlier contextual facts remain
    // useful, but later mutable memory is unknown.
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
  if (scalarResultMove(program, instruction, body)) {
    const auto &source = instruction.operands.front();
    const auto fact =
        integerForPlace(body, state, canonicalPlace(aliases, source.place));
    // Movement retires the source. Do not retain place-derived origins or
    // make a later source read usable; only the independent result SSA value
    // receives the old scalar envelope. Exact by-value scalar parameters
    // cannot overlap owned receiver fields, including an unmaterialized This.
    invalidateValueStorage(program, address, body, aliases, state,
                           source.place);
    state.loadedOrigins.erase(*instruction.result);
    state.fieldOrigins.erase(*instruction.result);
    state.aggregateOrigins.erase(*instruction.result);
    state.arrayValues.erase(*instruction.result);
    state.objectValues.erase(*instruction.result);
    state.integers.erase(*instruction.result);
    if (fact)
      state.integers[*instruction.result] = rebased(
          *fact, block.id, instruction.id, *instruction.result, source.place);
    return;
  }
  if (program && isMirStructuralValueTransfer(*program, instruction)) {
    const auto &source = instruction.operands.front();
    auto object = objectForOperand(body, aliases, state, source);
    if (object) {
      object->scalarPointees.clear();
      object->prefixes.clear();
    }
    // Prepared values own an independent snapshot. In particular a later
    // argument may modify the source without modifying this earlier copy.
    // Movement retires its old place; no source correlation crosses transport.
    if (source.kind == MirOperandKind::Move && source.place)
      invalidateValueStorage(program, address, body, aliases, state,
                             source.place);
    if (instruction.destination)
      invalidatePlaceRoot(body, aliases, state, *instruction.destination,
                          instruction.kind == MirInstructionKind::CallInput);
    state.loadedOrigins.erase(*instruction.result);
    state.fieldOrigins.erase(*instruction.result);
    state.aggregateOrigins.erase(*instruction.result);
    state.objectValues.erase(*instruction.result);
    if (object) {
      state.objectValues[*instruction.result] = *object;
      if (instruction.destination)
        seedObjectFact(body, aliases, *instruction.destination, *object, state);
    }
    return;
  }
  if (instruction.kind == MirInstructionKind::Compute) {
    if (instruction.result)
      state.fieldOrigins.erase(*instruction.result);
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
    if (instruction.result && instruction.operands.size() == 1 &&
        instruction.info.type.kind == SemanticType::RawPointer &&
        instruction.info.type.arguments.size() == 1) {
      std::optional<ScalarAddress> origin;
      if (instruction.operation == MirOperation::AddressOf) {
        const auto *place = body.findPlace(instruction.operands.front().place);
        if (place &&
            (place->root == MirPlaceRootKind::Binding ||
             place->root == MirPlaceRootKind::This) &&
            std::all_of(place->projections.begin(), place->projections.end(),
                        [](const auto &projection) {
                          return projection.kind == MirProjectionKind::Field;
                        }) &&
            constantIntegerDomain(place->type) &&
            place->type == instruction.info.type.arguments.front())
          origin = ScalarAddress{.place = place->id};
      } else if (instruction.operation == MirOperation::Identity ||
                 (instruction.operation == MirOperation::Convert &&
                  instruction.operands.front().type.kind ==
                      SemanticType::RawPointer &&
                  instruction.operands.front().type.arguments ==
                      instruction.info.type.arguments)) {
        origin =
            scalarAddressForOperand(body, state, instruction.operands.front());
      }
      if (origin) {
        add(origin->evidence, block.id);
        addInstruction(origin->evidence, instruction.id);
        addValue(origin->evidence, *instruction.result);
        addPlace(origin->evidence, origin->place);
        state.scalarAddressValues[*instruction.result] = std::move(*origin);
      }
    }
    if (auto relative = computeEntryRelative(body, state, instruction,
                                             instruction.operation)) {
      add(relative->evidence, block.id);
      addInstruction(relative->evidence, instruction.id);
      addValue(relative->evidence, *instruction.result);
      state.entryRelativeValues[*instruction.result] = std::move(*relative);
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
      if (const auto field =
              state.fieldOrigins.find(instruction.operands.front().value);
          field != state.fieldOrigins.end())
        state.fieldOrigins[*instruction.result] = field->second;
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
    state.fieldOrigins.erase(*instruction.result);
    const MirOperand &source = instruction.operands.front();
    if (source.type == instruction.info.type ||
        (source.type.kind == SemanticType::RawPointer &&
         instruction.info.type.kind == SemanticType::RawPointer &&
         source.type.arguments == instruction.info.type.arguments))
      if (auto origin = scalarAddressForOperand(body, state, source)) {
        addInstruction(origin->evidence, instruction.id);
        addValue(origin->evidence, *instruction.result);
        state.scalarAddressValues[*instruction.result] = std::move(*origin);
      }
    if (source.kind == MirOperandKind::Value &&
        source.type == instruction.info.type)
      if (auto relative = entryRelativeValue(state, source)) {
        addInstruction(relative->evidence, instruction.id);
        addValue(relative->evidence, *instruction.result);
        state.entryRelativeValues[*instruction.result] = std::move(*relative);
      }
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
      if (const auto field = state.fieldOrigins.find(source.value);
          field != state.fieldOrigins.end())
        state.fieldOrigins[*instruction.result] = field->second;
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
    state.fieldOrigins.erase(*instruction.result);
    const MirOperand &source = instruction.operands.front();
    if (source.place != 0) {
      const MirPlaceId sourcePlace = canonicalPlace(aliases, source.place);
      if (instruction.info.type.kind == SemanticType::Expected &&
          state.resultAlternatives.size() < 64)
        if (const auto found = state.placeAlternatives.find(sourcePlace);
            found != state.placeAlternatives.end()) {
          state.resultAlternatives[*instruction.result] = found->second;
          state.loadedOrigins[*instruction.result] = sourcePlace;
        }
      if (instruction.info.type.kind == SemanticType::RawPointer &&
          instruction.info.type.arguments.size() == 1) {
        const auto found = state.scalarAddressPlaces.find(sourcePlace);
        std::optional<ScalarAddress> pointer =
            found == state.scalarAddressPlaces.end()
                ? std::nullopt
                : std::optional{found->second};
        if (!pointer)
          if (const auto field = ownedPointerFieldOrigin(program, address, body,
                                                         aliases, sourcePlace))
            if (const auto stored = state.fieldAddresses.find(*field);
                stored != state.fieldAddresses.end())
              pointer = stored->second;
        const auto *pointee =
            pointer ? addressPointeeType(body, *pointer) : nullptr;
        if (pointee && *pointee == instruction.info.type.arguments.front()) {
          auto origin = *pointer;
          addInstruction(origin.evidence, instruction.id);
          addValue(origin.evidence, *instruction.result);
          addPlace(origin.evidence, sourcePlace);
          state.scalarAddressValues[*instruction.result] = std::move(origin);
        }
      }
      if (const auto path = fieldPath(body, 0, sourcePlace))
        if (const auto found = state.entryRelativeFields.find(*path);
            found != state.entryRelativeFields.end() &&
            constantIntegerDomain(instruction.info.type) ==
                found->second.domain) {
          auto relative = found->second;
          add(relative.evidence, block.id);
          addInstruction(relative.evidence, instruction.id);
          addValue(relative.evidence, *instruction.result);
          addPlace(relative.evidence, sourcePlace);
          state.entryRelativeValues[*instruction.result] = std::move(relative);
        }
      const FixedArrayProjection projected =
          fixedArrayProjection(body, source.place);
      const MirPlaceId arrayPlace =
          canonicalPlace(aliases, projected.arrayPlace);
      const std::optional<IndexedAggregateAccess> indexed =
          indexedAggregateAccess(program, body, aliases, source.place);
      const auto arrayElements = state.arrayElements.find(arrayPlace);
      std::optional<IntegerFact> fact;
      if (const auto pointer =
              borrowedAddressForPlace(program, body, state, source.place)) {
        if (auto value =
                integerAtAddress(body, state, *pointer, instruction.info.type,
                                 program, address)) {
          value->evidence = merged(value->evidence, pointer->evidence);
          fact = std::move(*value);
        }
      } else if (const auto region = borrowedRegionForPlace(
                     program, body, state, source.place)) {
        fact = integerInRegion(body, state, *region, instruction.info.type);
      }
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
        if (const auto field = fieldOriginForPlace(program, address, body,
                                                   aliases, source.place))
          state.fieldOrigins[*instruction.result] = *field;
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
        initializesReferenceCarrier(body, instruction, program, &state);
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
    std::optional<AlternativeFields> initializedAlternatives;
    const auto *destinationPlace = body.findPlace(destination);
    const auto *physicalDestination = body.findPlace(*instruction.destination);
    const bool independentScalarWrite =
        (instruction.kind == MirInstructionKind::Initialize ||
         instruction.kind == MirInstructionKind::Assign) &&
        physicalDestination &&
        physicalDestination->root == MirPlaceRootKind::Binding &&
        physicalDestination->projections.empty() &&
        (constantIntegerDomain(physicalDestination->type) ||
         floatingScalar(physicalDestination->type)) &&
        !physicalDestination->traits.containsBorrowedState &&
        instruction.lifecycle.empty() && !instruction.rawMemoryAccess &&
        !effect.invokesUserCode && !effect.invokesRuntime &&
        !effect.writesUnknownMemory && !effect.maySynchronize;
    std::map<MirPlaceId, PrefixFacts> retainedPrefixes;
    if (referenceCarrierInitialization || independentScalarWrite)
      for (const auto &[root, object] : state.objects)
        if (!object.prefixes.empty())
          retainedPrefixes.emplace(root, object.prefixes);
    // Writing an independent local scalar cannot retarget a reference;
    // checked references never reseat. Their exact addresses and regions stay.
    const auto retainedBorrows = independentScalarWrite
                                     ? state.borrowedAddresses
                                     : decltype(state.borrowedAddresses){};
    const auto retainedRegions = independentScalarWrite
                                     ? state.borrowedRegions
                                     : decltype(state.borrowedRegions){};
    const auto writtenDomain =
        physicalDestination ? constantIntegerDomain(physicalDestination->type)
                            : std::nullopt;
    const std::optional<unsigned> writtenWidth =
        writtenDomain ? std::optional<unsigned>{writtenDomain->width}
                      : std::nullopt;
    const bool typedScalarWrite =
        (writtenDomain && writtenDomain->width > 8) ||
        (physicalDestination && floatingScalar(physicalDestination->type));
    const auto untouchedFields =
        instruction.kind == MirInstructionKind::Assign && typedScalarWrite &&
                !instruction.rawMemoryAccess &&
                instruction.unsafeOperation == UnsafeOperationKind::None &&
                instruction.lifecycle.empty() && !effect.invokesUserCode &&
                !effect.invokesRuntime && !effect.writesUnknownMemory &&
                !effect.maySynchronize
            ? preserveDifferentWidthFields(program, address, body, state,
                                           writtenWidth)
            : DisjointScalarFields{};
    // The written scalar is the physical destination; its canonical alias
    // representative may be the borrowed owner rather than the referent.
    const bool typedCheckedWrite =
        instruction.kind == MirInstructionKind::Assign && physicalDestination &&
        !instruction.rawMemoryAccess &&
        instruction.unsafeOperation == UnsafeOperationKind::None &&
        instruction.lifecycle.empty() && !effect.invokesUserCode &&
        !effect.invokesRuntime && !effect.writesUnknownMemory &&
        !effect.maySynchronize && typedScalarWrite;
    const auto checkedAddress =
        typedCheckedWrite ? borrowedAddressForPlace(program, body, state,
                                                    *instruction.destination)
                          : std::nullopt;
    // A region reference writes one unknown element of an exact base. It can
    // still join an initialized-prefix envelope, never publish an exact cell.
    const auto checkedRegion =
        typedCheckedWrite && !checkedAddress
            ? borrowedRegionForPlace(program, body, state,
                                     *instruction.destination)
            : std::nullopt;
    const ScalarAddress *checkedBase =
        checkedAddress ? &*checkedAddress
                       : (checkedRegion ? &checkedRegion->base : nullptr);
    // A typed non-byte scalar write cannot replace a live pointer/reference
    // carrier. Retain address identities, not other mutable pointee values.
    // The exact selected cell is republished only on the successful edge.
    const auto savedFieldAddresses =
        checkedBase ? state.fieldAddresses : decltype(state.fieldAddresses){};
    const auto savedBorrowedAddresses =
        checkedBase ? state.borrowedAddresses
                    : decltype(state.borrowedAddresses){};
    const auto savedBorrowedRegions =
        checkedBase ? state.borrowedRegions : decltype(state.borrowedRegions){};
    ConstructedScalars siblingPointees;
    if (checkedAddress && !checkedAddress->subobject.empty()) {
      auto written = addressKey(*checkedAddress);
      for (const auto &[cell, fact] : state.constructedScalars) {
        auto compared = cell;
        compared.subobject = written.subobject;
        const auto prefix =
            std::min(cell.subobject.size(), written.subobject.size());
        if (compared == written && !cell.subobject.empty() &&
            !std::equal(cell.subobject.begin(), cell.subobject.begin() + prefix,
                        written.subobject.begin()))
          siblingPointees.emplace(cell, fact);
      }
    }
    if (instruction.kind == MirInstructionKind::Initialize &&
        destinationPlace &&
        destinationPlace->root == MirPlaceRootKind::Binding &&
        destinationPlace->projections.empty() &&
        destinationPlace->type.kind == SemanticType::Expected &&
        !destinationPlace->traits.containsBorrowedState &&
        (address.kind == MirBodyKind::Function ||
         address.kind == MirBodyKind::Constructor))
      if (const auto found = state.resultAlternatives.find(source.value);
          source.kind == MirOperandKind::Value &&
          found != state.resultAlternatives.end())
        initializedAlternatives = found->second;
    std::optional<IntegerFact> fact =
        source.place != 0
            ? integerForPlace(body, state,
                              canonicalPlace(aliases, source.place))
            : integerForOperand(body, state, source);
    const auto receiverPath = fieldPath(body, 0, destination);
    const auto scalarAddress =
        (instruction.operation == MirOperation::None ||
         instruction.operation == MirOperation::Assign)
            ? scalarAddressForOperand(body, state, source)
            : std::nullopt;
    auto relative = entryRelativeValue(state, source);
    if (compoundCheckedOperation(instruction.operation)) {
      relative.reset();
      if (receiverPath && fact && singleton(*fact) &&
          (instruction.operation == MirOperation::AddAssign ||
           instruction.operation == MirOperation::SubtractAssign)) {
        if (const auto found = state.entryRelativeFields.find(*receiverPath);
            found != state.entryRelativeFields.end() &&
            fact->domain == found->second.domain) {
          auto offset = fact->lower;
          if (instruction.operation == MirOperation::SubtractAssign)
            offset.negative = !offset.negative;
          if (checkedIntegerFits(normalizeCheckedInteger(offset),
                                 {.width = 64, .signedValue = true})) {
            relative = shiftEntryRelative(found->second, offset);
            if (relative)
              relative->evidence = merged(relative->evidence, fact->evidence);
          }
        }
      }
    } else if (instruction.operation != MirOperation::None &&
               instruction.operation != MirOperation::Assign) {
      relative.reset();
    }
    if (compoundCheckedOperation(instruction.operation)) {
      fact = compoundIntegerFact(body, program, address, aliases, state,
                                 instruction);
    }
    if (checkedBase && typedScalarWrite &&
        (instruction.operation == MirOperation::None ||
         instruction.operation == MirOperation::Assign ||
         compoundCheckedOperation(instruction.operation))) {
      // A checked scalar assignment changes an existing live element, not
      // the region's coverage. Prefixes with this exact current base and
      // subobject weakly join the stored value with all previous elements.
      // Owned record fields which neither contain nor lie within the written
      // subobject are other storage of every element and stay unchanged.
      // Different pointer fields are never assumed disjoint.
      const auto disjointSubobject = [&](const FieldPath &other) {
        const auto &written = checkedBase->subobject;
        const auto common = std::min(other.size(), written.size());
        return !written.empty() && !other.empty() &&
               !std::equal(other.begin(), other.begin() + common,
                           written.begin());
      };
      for (const auto &[root, object] : state.objects) {
        for (const auto &[path, envelope] : object.prefixes) {
          const bool joined = path.subobject == checkedBase->subobject &&
                              fact && writtenDomain &&
                              envelope.domain == fact->domain &&
                              fact->domain == *writtenDomain;
          if (!joined && !disjointSubobject(path.subobject))
            continue;
          const auto base = state.fieldAddresses.find({root, path.pointer});
          const auto end = object.integers.find(path.end);
          if (base == state.fieldAddresses.end() || base->second.index ||
              !base->second.subobject.empty() || end == object.integers.end() ||
              end->second.domain.width == writtenWidth)
            continue;
          auto compared = addressKey(*checkedBase);
          compared.index.reset();
          compared.elementOffset = base->second.elementOffset;
          compared.subobject.clear();
          if (compared != addressKey(base->second))
            continue;
          auto updated = joined ? joinedElementFact(envelope, *fact) : envelope;
          updated.evidence = merged(updated.evidence, checkedBase->evidence);
          addInstruction(updated.evidence, instruction.id);
          retainedPrefixes[root].insert_or_assign(path, std::move(updated));
        }
      }
    }
    const std::optional<IntegerFact> sourceArray =
        arrayForOperand(state, aliases, source);
    std::optional<ObjectFact> sourceObject =
        objectForOperand(body, aliases, state, source);
    const auto *producer = source.kind == MirOperandKind::Value
                               ? definingInstruction(body, source.value)
                               : nullptr;
    // A binding which reparents the constructor result's own drop obligation
    // adopts that SSA object; no copy or move constructor runs in between.
    const bool adoptsConstructedResult =
        producer && producer->successResultDrop && !producer->destination &&
        std::count_if(instruction.lifecycle.begin(),
                      instruction.lifecycle.end(),
                      [&](const MirLifecycleEvent &event) {
                        return event.kind == MirLifecycleEventKind::Reparent &&
                               event.source == *producer->successResultDrop;
                      }) == 1;
    const bool publishesConstructedResult =
        instruction.kind == MirInstructionKind::Initialize && producer &&
        producer->kind == MirInstructionKind::Construct &&
        (producer->successResultDestination == instruction.destination ||
         adoptsConstructedResult) &&
        producer->info.type == source.type;
    // Verified final-address publication and result adoption do not copy the
    // shell again. Ordinary copies, moves and replacement receive no pointee
    // snapshots.
    if (sourceObject && !publishesConstructedResult) {
      sourceObject->scalarPointees.clear();
      sourceObject->prefixes.clear();
    }
    // Even exact indexed writes which retain aggregate envelopes retire
    // conditional memory correlations. Independent local initialization may
    // restore only its producer's freshly captured alternatives below.
    state.resultAlternatives.clear();
    state.placeAlternatives.clear();
    retireScalarPointeeSnapshots(state);
    if (!referenceCarrierInitialization &&
        (!indexed || (!previousAggregateElements && !previousAggregateField))) {
      invalidatePlaceRoot(body, aliases, state, destination);
    }
    restoreDifferentWidthFields(state, untouchedFields);
    if (checkedBase) {
      // A raw-derived checked reference may write a caller-owned object. This
      // is not vacant construction: callers must invalidate possible aliases
      // before consuming the exact successful postcondition.
      state.hasOpaqueEffects = true;
      state.liveMemoryBarrier = LiveMemoryBarrier::Unknown;
      state.fieldAddresses = savedFieldAddresses;
      state.borrowedAddresses = savedBorrowedAddresses;
      state.borrowedRegions = savedBorrowedRegions;
    }
    if (checkedAddress) {
      for (auto &[pointer, fact] : siblingPointees)
        publishConstructedScalar(state, pointer, std::move(fact));
      const auto *pointee = addressPointeeType(body, *checkedAddress);
      if (fact && pointee && *pointee == physicalDestination->type &&
          constantIntegerDomain(*pointee) == fact->domain) {
        auto stored = *fact;
        stored.evidence = merged(stored.evidence, checkedAddress->evidence);
        add(stored.evidence, block.id);
        addInstruction(stored.evidence, instruction.id);
        addPlace(stored.evidence, destination);
        publishConstructedScalar(state, addressKey(*checkedAddress),
                                 ConstructedScalar{std::move(stored)});
      }
    }
    if (scalarAddress && body.findPlace(destination) &&
        body.findPlace(destination)->type == source.type) {
      auto origin = *scalarAddress;
      add(origin.evidence, block.id);
      addInstruction(origin.evidence, instruction.id);
      addPlace(origin.evidence, destination);
      state.scalarAddressPlaces[destination] = origin;
      if (const auto field = ownedPointerFieldOrigin(program, address, body,
                                                     aliases, destination);
          field && (state.fieldAddresses.contains(*field) ||
                    state.fieldAddresses.size() < 1024))
        state.fieldAddresses.insert_or_assign(*field, origin);
      if (instruction.result && instruction.info.type == source.type) {
        addValue(origin.evidence, *instruction.result);
        state.scalarAddressValues[*instruction.result] = std::move(origin);
      }
    }
    if (receiverPath && relative && *receiverPath == relative->field &&
        body.findPlace(destination) &&
        constantIntegerDomain(body.findPlace(destination)->type) ==
            relative->domain) {
      add(relative->evidence, block.id);
      addInstruction(relative->evidence, instruction.id);
      addPlace(relative->evidence, destination);
      state.entryRelativeFields[*receiverPath] = std::move(*relative);
    }
    if (fact && projected.arrayPlace == 0) {
      IntegerFact stored = *fact;
      // A constructor initializer executes once before the body. Its exact
      // entry value can join a later callee's invariant envelope without
      // treating arbitrary loop-produced singleton values as invariants.
      if (address.kind == MirBodyKind::Constructor &&
          instruction.constructorInitializer && receiverPath &&
          singleton(stored))
        stabilize(stored);
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
    for (auto &[root, prefixes] : retainedPrefixes)
      if (const auto object = state.objects.find(root);
          object != state.objects.end())
        object->second.prefixes = std::move(prefixes);
    if (independentScalarWrite) {
      state.borrowedAddresses = retainedBorrows;
      state.borrowedRegions = retainedRegions;
    }
    // Initializing a new independent local shell does not write the objects
    // described by the call that produced it. Reassignment/projection and
    // borrowed-state carriers deliberately receive no such restoration.
    if (initializedAlternatives && state.placeAlternatives.size() < 64)
      state.placeAlternatives[destination] =
          std::move(*initializedAlternatives);
    return;
  }
  if (instruction.kind == MirInstructionKind::Construct &&
      applyClosedCall(body, aliases, block, instruction, state, closedCalls,
                      address)) {
    return;
  }
  if (instruction.kind == MirInstructionKind::Drop && instruction.destination) {
    if (program && isMirStructuralValueDrop(*program, instruction)) {
      invalidateValueStorage(program, address, body, aliases, state,
                             *instruction.destination, true);
      return;
    }
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

  if ((instruction.intrinsic == IntrinsicKind::IntegerWrappingAdd ||
       instruction.intrinsic == IntrinsicKind::IntegerWrappingSubtract ||
       instruction.intrinsic == IntrinsicKind::IntegerWrappingMultiply) &&
      instruction.result && !instruction.receiver &&
      instruction.operands.size() == 2) {
    // Modular arithmetic agrees with checked arithmetic only in a proved
    // non-wrapping input envelope. Failure to prove that envelope loses the
    // fact, never adds a trap or assumes native signed-overflow semantics.
    const auto operation =
        instruction.intrinsic == IntrinsicKind::IntegerWrappingAdd
            ? CheckedIntegerOperation::Add
        : instruction.intrinsic == IntrinsicKind::IntegerWrappingSubtract
            ? CheckedIntegerOperation::Subtract
            : CheckedIntegerOperation::Multiply;
    const auto left = integerForOperand(body, state, instruction.operands[0]);
    const auto right = integerForOperand(body, state, instruction.operands[1]);
    state.integers.erase(*instruction.result);
    if (left && right &&
        constantIntegerDomain(instruction.info.type) == left->domain)
      if (const auto fact = binaryFact(operation, *left, *right)) {
        state.integers[*instruction.result] =
            rebased(*fact, block.id, instruction.id, *instruction.result);
        const auto relativeOperation =
            operation == CheckedIntegerOperation::Add ? MirOperation::Add
            : operation == CheckedIntegerOperation::Subtract
                ? MirOperation::Subtract
                : MirOperation::Multiply;
        if (auto relative = computeEntryRelative(body, state, instruction,
                                                 relativeOperation)) {
          relative->evidence = merged(relative->evidence, fact->evidence);
          add(relative->evidence, block.id);
          addInstruction(relative->evidence, instruction.id);
          addValue(relative->evidence, *instruction.result);
          state.entryRelativeValues[*instruction.result] = std::move(*relative);
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
  // A threshold which itself grows on a widening edge is not a stable
  // result-domain bound. Repeated affine updates must widen, not enumerate
  // the trip count by expanding their hint one element at a time.
  if (widen && envelope && left.wideningEnvelope &&
      (compare(envelope->first, left.wideningEnvelope->first) < 0 ||
       compare(envelope->second, left.wideningEnvelope->second) > 0))
    envelope.reset();
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

template <typename Key>
void joinAlternatives(std::map<Key, AlternativeFields> &target,
                      const std::map<Key, AlternativeFields> &incoming,
                      bool widen) {
  for (auto entry = target.begin(); entry != target.end();) {
    const auto found = incoming.find(entry->first);
    if (found == incoming.end()) {
      entry = target.erase(entry);
      continue;
    }
    // Missing alternative means impossible, not unknown. Both sides possess
    // the tagged result fact; a wholly unknown result would erase the key.
    for (const auto &[alternative, fields] : found->second) {
      auto [slot, inserted] = entry->second.try_emplace(alternative, fields);
      if (!inserted)
        joinIntegers(slot->second, fields, widen);
    }
    ++entry;
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

template <typename Key>
void joinConstructed(std::map<Key, ConstructedScalar> &target,
                     const std::map<Key, ConstructedScalar> &incoming,
                     bool widen) {
  for (auto entry = target.begin(); entry != target.end();) {
    const auto found = incoming.find(entry->first);
    if (found == incoming.end() ||
        entry->second.value.domain != found->second.value.domain) {
      entry = target.erase(entry);
      continue;
    }
    entry->second.value =
        joinedInteger(entry->second.value, found->second.value, widen);
    if (entry->second.valueParameter != found->second.valueParameter ||
        entry->second.valueField != found->second.valueField) {
      entry->second.valueParameter.reset();
      entry->second.valueField.clear();
    }
    std::erase_if(entry->second.disjointFields, [&](const auto &field) {
      return std::find(found->second.disjointFields.begin(),
                       found->second.disjointFields.end(),
                       field) == found->second.disjointFields.end();
    });
    ++entry;
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
  target.mayWriteGlobals |= incoming.mayWriteGlobals;
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
  joinAlternatives(target.resultAlternatives, incoming.resultAlternatives,
                   widen);
  joinAlternatives(target.placeAlternatives, incoming.placeAlternatives, widen);
  joinEntryRelative(target.entryRelativeFields, incoming.entryRelativeFields);
  joinEntryRelative(target.entryRelativeValues, incoming.entryRelativeValues);
  joinScalarAddresses(target.scalarAddressValues, incoming.scalarAddressValues);
  joinScalarAddresses(target.scalarAddressPlaces, incoming.scalarAddressPlaces);
  joinScalarAddresses(target.fieldAddresses, incoming.fieldAddresses);
  // One loan reaching this join with different exact element addresses, or
  // with an exact address on one path and a region on another, still denotes
  // one element of their common exact base.
  const auto sameAddress = [](const ScalarAddress &left,
                              const ScalarAddress &right) {
    return addressKey(left) == addressKey(right) &&
           left.fieldPointee == right.fieldPointee &&
           left.subobjectType == right.subobjectType;
  };
  auto incomingRegions = incoming.borrowedRegions;
  const auto convert = [&](const AbstractState &from, const AbstractState &to,
                           std::map<MirLoanId, AddressRegion> &regions) {
    for (const auto &[loan, pointer] : from.borrowedAddresses) {
      const auto other = to.borrowedAddresses.find(loan);
      if (other != to.borrowedAddresses.end()
              ? sameAddress(other->second, pointer)
              : !to.borrowedRegions.contains(loan))
        continue;
      if (auto region = exactRegion(pointer))
        regions.emplace(loan, std::move(*region));
    }
  };
  convert(before, incoming, target.borrowedRegions);
  convert(incoming, before, incomingRegions);
  joinScalarAddresses(target.borrowedAddresses, incoming.borrowedAddresses);
  // A region survives only with the same exact base. Its displacement
  // interval joins and widens like any other integer fact.
  for (auto entry = target.borrowedRegions.begin();
       entry != target.borrowedRegions.end();) {
    const auto found = incomingRegions.find(entry->first);
    if (found == incomingRegions.end() ||
        addressKey(entry->second.base) != addressKey(found->second.base) ||
        entry->second.base.subobjectType != found->second.base.subobjectType ||
        entry->second.offsets.domain != found->second.offsets.domain) {
      entry = target.borrowedRegions.erase(entry);
      continue;
    }
    entry->second.base.evidence =
        merged(entry->second.base.evidence, found->second.base.evidence);
    entry->second.offsets =
        joinedInteger(entry->second.offsets, found->second.offsets, widen);
    ++entry;
  }
  joinConstructed(target.constructedScalars, incoming.constructedScalars,
                  widen);
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
  std::erase_if(target.fieldOrigins, [&](const auto &entry) {
    const auto found = incoming.fieldOrigins.find(entry.first);
    return found == incoming.fieldOrigins.end() ||
           found->second != entry.second;
  });
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
  // Block arguments are simultaneous assignments. In particular, a backedge
  // may swap two parameters; publishing the first before reading the second
  // would substitute the new value for the old one. Snapshot just the argument
  // facts, not the entire (potentially large) abstract memory state.
  struct ArgumentFacts {
    std::optional<IntegerFact> integer;
    std::optional<MirPlaceId> loadedOrigin;
    std::optional<FieldOrigin> fieldOrigin;
    std::optional<EntryRelativeField> relative;
    std::optional<ScalarAddress> address;
  };
  std::vector<ArgumentFacts> inputs;
  inputs.reserve(arguments.size());
  for (const MirOperand &argument : arguments) {
    ArgumentFacts facts{.integer = integerForOperand(body, state, argument),
                        .relative = entryRelativeValue(state, argument),
                        .address =
                            scalarAddressForOperand(body, state, argument)};
    const auto loadedOrigin = argument.kind == MirOperandKind::Value
                                  ? state.loadedOrigins.find(argument.value)
                                  : state.loadedOrigins.end();
    if (loadedOrigin != state.loadedOrigins.end())
      facts.loadedOrigin = loadedOrigin->second;
    const auto fieldOrigin = argument.kind == MirOperandKind::Value
                                 ? state.fieldOrigins.find(argument.value)
                                 : state.fieldOrigins.end();
    if (fieldOrigin != state.fieldOrigins.end())
      facts.fieldOrigin = fieldOrigin->second;
    inputs.push_back(std::move(facts));
  }
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const MirValueId parameter = block->parameters[index].value;
    ArgumentFacts &facts = inputs[index];
    if (facts.integer) {
      addValue(facts.integer->evidence, parameter);
      state.integers[parameter] = std::move(*facts.integer);
    } else {
      state.integers.erase(parameter);
    }
    if (facts.loadedOrigin)
      state.loadedOrigins[parameter] = *facts.loadedOrigin;
    else
      state.loadedOrigins.erase(parameter);
    if (facts.fieldOrigin)
      state.fieldOrigins[parameter] = *facts.fieldOrigin;
    else
      state.fieldOrigins.erase(parameter);
    if (facts.relative) {
      addValue(facts.relative->evidence, parameter);
      state.entryRelativeValues[parameter] = std::move(*facts.relative);
    } else {
      state.entryRelativeValues.erase(parameter);
    }
    if (facts.address) {
      addValue(facts.address->evidence, parameter);
      state.scalarAddressValues[parameter] = std::move(*facts.address);
    } else {
      state.scalarAddressValues.erase(parameter);
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
  if (inertLifecycleMarker(instruction) ||
      nonMutatingLoanBoundary(body, instruction) ||
      ownerWitnessFormation(body, instruction))
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
      effects->traits.writesUnknownMemory || effects->traits.allocates) {
    return true;
  }
  // Invoking exact source code is not itself a write: the complete closed
  // summary bounds every nested exact callee, and target-dependent address
  // arithmetic writes nothing. A by-value input carrying borrowed state, such
  // as a closure capturing a reference, could still let those callees reach
  // caller storage without a mutable reference parameter.
  const auto hidesBorrow = [&](const MirOperand &operand) {
    if (operand.type.kind == SemanticType::Reference ||
        operand.type.kind == SemanticType::RawPointer || operand.literal)
      return false;
    if (const auto *place = body.findPlace(operand.place))
      return place->traits.containsBorrowedState;
    const auto *definition = operand.kind == MirOperandKind::Value
                                 ? definingInstruction(body, operand.value)
                                 : nullptr;
    return definition == nullptr ||
           definition->info.traits.containsBorrowedState;
  };
  if (effects->traits.invokesUserCode &&
      ((instruction.receiver && hidesBorrow(*instruction.receiver)) ||
       std::any_of(instruction.operands.begin(), instruction.operands.end(),
                   hidesBorrow)))
    return true;
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
                MirBodyAddress address = {},
                const std::vector<bool> *wideningBlocks = nullptr) {
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
      [&](AbstractState &target, const AbstractState &incoming,
          MirDataflowMergeContext context) {
        // Every cycle of a reducible body passes through its header, so
        // widening there bounds every ascending chain. Facts inside the loop
        // are recomputed from that header state rather than widened again.
        const bool widen =
            context.widen &&
            (wideningBlocks == nullptr || context.edge.target == 0 ||
             context.edge.target > wideningBlocks->size() ||
             (*wideningBlocks)[context.edge.target - 1]);
        return mergeInto(target, incoming, widen);
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
  const std::optional<MirDominanceInfo> dominance = computeMirDominance(body);
  const std::optional<MirLoopAnalysis> loops =
      dominance ? std::optional{analyzeMirLoops(body, *dominance)}
                : std::nullopt;
  std::vector<bool> headers;
  if (loops && !loops->hasIrreducibleControlFlow) {
    headers.assign(body.blocks.size(), false);
    for (const MirNaturalLoop &loop : loops->loops)
      if (loop.header != 0 && loop.header <= headers.size())
        headers[loop.header - 1] = true;
  }
  const std::vector<bool> *wideningBlocks =
      headers.empty() ? nullptr : &headers;
  const std::vector<AbstractState> initial = solveStatesOnce(
      body, aliases, nullptr, entryState, closedCalls, address, wideningBlocks);
  if (initial.empty()) {
    return {};
  }
  if (!dominance) {
    return initial;
  }
  const PlaceThresholds thresholds = recurrenceThresholds(
      body, aliases, *dominance, *loops, initial, closedCalls, address);
  if (thresholds.empty()) {
    return initial;
  }
  return solveStatesOnce(body, aliases, &thresholds, entryState, closedCalls,
                         address, wideningBlocks);
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
                : compoundIntegerFact(body, program, address, aliases, state,
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
  const auto empty = [](const ObjectFact &object, const PrefixField &path) {
    const auto end = object.integers.find(path.end);
    return end != object.integers.end() && singleton(end->second) &&
           !end->second.lower.negative && end->second.lower.magnitude == 0;
  };
  for (auto entry = target.prefixes.begin(); entry != target.prefixes.end();) {
    if (empty(incoming, entry->first)) {
      ++entry;
      continue;
    }
    const auto found = incoming.prefixes.find(entry->first);
    if (found == incoming.prefixes.end() ||
        found->second.domain != entry->second.domain) {
      entry = target.prefixes.erase(entry);
      continue;
    }
    entry->second = empty(before, entry->first)
                        ? found->second
                        : joinedInteger(entry->second, found->second, widen);
    ++entry;
  }
  for (const auto &[path, fact] : incoming.prefixes)
    if (!target.prefixes.contains(path) && empty(before, path) &&
        target.prefixes.size() < 64)
      target.prefixes.emplace(path, fact);
  joinIntegers(target.integers, incoming.integers, widen);
  joinIntegers(target.scalarPointees, incoming.scalarPointees, widen);
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
  const std::vector<HirBindingId> *declared = nullptr;
  if (address.kind == MirBodyKind::Function) {
    const MirFunctionInstance *function =
        program.findFunctionInstance(address.owner);
    if (function)
      declared = &function->parameterBindings;
  } else if (address.kind == MirBodyKind::Constructor) {
    const MirConstructorInstance *constructor =
        program.findConstructorInstance(address.owner);
    if (constructor)
      declared = &constructor->parameterBindings;
  }
  const auto *body = findMirBody(program, address);
  if (!declared || !body)
    return {};
  // MIR preserves one source binding for a concrete pack, while invocation
  // types and operands are flattened. Translate that binding through its
  // verified pack shape, including an empty pack, before matching ordinals.
  // parameterOrdinal still requires exact per-element projected types.
  std::vector<HirBindingId> result;
  for (const auto binding : *declared) {
    const auto *place = body->findPlace(rootForBinding(*body, binding));
    if (place && place->type.kind == SemanticType::TypePack) {
      if (!place->type.concretePack)
        return {};
      result.insert(result.end(), place->type.arguments.size(), binding);
    } else {
      result.push_back(binding);
    }
  }
  return result;
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
  const auto types = parameterTypes(program, address);
  const auto roots = parameterPlaces(program, address, body);
  // A function may name the initial pointer value in an ordinary owned field
  // of its receiver or checked-reference parameter. Do not seed constructor
  // receivers (not initialized yet), local objects, stored references, unions,
  // raw dereferences or capability backing paths. Exact writes retire the
  // field relation; it is never recreated from a later arbitrary load.
  if (address.kind == MirBodyKind::Function) {
    std::size_t remaining = 1024;
    const auto seed = [&](const auto &self, MirPlaceId root,
                          const SemanticType &type, FieldPath path) -> void {
      if (!remaining || path.size() > 32)
        return;
      --remaining;
      if (type.kind == SemanticType::RawPointer && type.arguments.size() == 1) {
        const auto *live =
            liveOwnedPathType(&program, address, body, root, path);
        if (!path.empty() && live && *live == type)
          entry.fieldAddresses.emplace(
              FieldOrigin{root, path},
              ScalarAddress{.kind = AddressOriginKind::EntryPointerField,
                            .place = root,
                            .field = path,
                            .fieldPointee = type.arguments.front()});
        return;
      }
      if (type.kind != SemanticType::Class)
        return;
      const auto record = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == type; });
      if (record == program.classInstances().end() ||
          record->kind == ClassKind::Union ||
          record->compilerCapability != CompilerCapabilityTypeKind::None)
        return;
      for (const auto &field : record->declaredFields) {
        auto nested = path;
        nested.push_back(field.symbol);
        self(self, root, field.type, std::move(nested));
      }
    };
    if (const auto *type = implicitReceiverType(&program, address))
      seed(seed, canonicalPlace(aliases, thisRoot(body)), *type, {});
    for (std::size_t index = 0; index < bindings.size() && index < types.size();
         ++index)
      if (types[index].kind == SemanticType::Reference &&
          types[index].arguments.size() == 1)
        if (const auto root = roots[index])
          seed(seed, canonicalPlace(aliases, root),
               types[index].arguments.front(), {});
  }
  for (std::size_t index = 0; index < bindings.size() && index < types.size();
       ++index) {
    if (types[index].kind != SemanticType::RawPointer ||
        types[index].arguments.size() != 1)
      continue;
    const auto root = roots[index];
    const auto *place = body.findPlace(root);
    if (place && place->type == types[index])
      entry.scalarAddressPlaces[canonicalPlace(aliases, root)] =
          ScalarAddress{.kind = AddressOriginKind::EntryPointer, .place = root};
  }
  for (std::size_t index = 0;
       index < bindings.size() && index < context.arguments.size(); ++index) {
    const MirPlaceId root = roots[index];
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
  // Contexts describe memory at the current input's pointer-field value, not
  // the field's own storage. Entry identities are generated above from typed
  // owned paths; no arbitrary load recreates an identity after a write.
  for (const auto &[field, pointer] : entry.fieldAddresses) {
    const auto object = entry.objects.find(field.root);
    if (object == entry.objects.end())
      continue;
    for (const auto &[path, fact] : object->second.scalarPointees) {
      if (path.pointer != field.path)
        continue;
      const auto cell = projectPointee(&program, body, pointer, path.subobject);
      const auto *type = cell ? addressPointeeType(body, *cell) : nullptr;
      if (type && constantIntegerDomain(*type) == fact.domain)
        publishConstructedScalar(entry, addressKey(*cell),
                                 ConstructedScalar{rebased(fact)});
    }
  }
  // Identity with an entry field does not require a known numerical envelope.
  // Seed from the same typed owned-path profile even when one calling context
  // has an unknown/overflowing value. No raw pointee or stored reference is
  // traversed, and large/deep records conservatively omit excess paths.
  if (address.kind == MirBodyKind::Function) {
    std::size_t remaining = 1024;
    const auto seed = [&](const auto &self, const SemanticType &type,
                          FieldPath path) -> void {
      if (!remaining || path.size() > 32)
        return;
      --remaining;
      if (const auto domain = constantIntegerDomain(type)) {
        if (!path.empty() && liveScalarPath(&program, address, body, 0, path))
          entry.entryRelativeFields.emplace(
              path, EntryRelativeField{
                        .field = path, .domain = *domain, .offset = {}});
        return;
      }
      if (type.kind != SemanticType::Class)
        return;
      const auto record = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == type; });
      if (record == program.classInstances().end() ||
          record->kind == ClassKind::Union ||
          record->compilerCapability != CompilerCapabilityTypeKind::None)
        return;
      for (const auto &field : record->declaredFields) {
        auto nested = path;
        nested.push_back(field.symbol);
        self(self, field.type, std::move(nested));
      }
    };
    if (const auto *type = implicitReceiverType(&program, address))
      seed(seed, *type, {});
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

struct ScalarStoreProfile {
  bool supported = false;
  std::optional<unsigned> width;
  bool floating = false;
};

[[nodiscard]] std::optional<InputAddress>
exportedInputAddress(const MirProgram &program, MirBodyAddress address,
                     const MirBody &body, const ScalarAddressKey &pointer) {
  if (pointer.observation ||
      (pointer.kind != AddressOriginKind::EntryPointer &&
       pointer.kind != AddressOriginKind::EntryPointerField))
    return std::nullopt;
  const auto *origin = body.findPlace(pointer.place);
  if (pointer.kind == AddressOriginKind::EntryPointerField &&
      implicitReceiverType(&program, address) &&
      (!pointer.place || (origin && origin->root == MirPlaceRootKind::This &&
                          origin->projections.empty())))
    return InputAddress{std::nullopt, pointer.field, pointer.elementOffset,
                        pointer.index, pointer.subobject};
  if (!origin || origin->root != MirPlaceRootKind::Binding ||
      !origin->projections.empty())
    return std::nullopt;
  const auto bindings = parameterBindings(program, address);
  const auto types = parameterTypes(program, address);
  for (std::size_t index = 0; index < bindings.size() && index < types.size();
       ++index)
    if (bindings[index] == origin->binding && types[index] == origin->type)
      return InputAddress{index, pointer.field, pointer.elementOffset,
                          pointer.index, pointer.subobject};
  return std::nullopt;
}

[[nodiscard]] ScalarStoreProfile
scalarStoreProfile(const MirProgram &program, MirBodyAddress address,
                   const MirBody &body, const ClosedCallRound &round) {
  // Construction bodies initialize their own vacant receiver; that is not a
  // store to live memory. Their other effects follow the ordinary profile.
  const bool construction = address.kind == MirBodyKind::Constructor ||
                            address.kind == MirBodyKind::FieldInitializers;
  if (address.kind != MirBodyKind::Function && !construction)
    return {};
  ScalarStoreProfile result{.supported = true};
  const auto addWidth = [&](unsigned width) {
    if (width <= 8 || (result.width && *result.width != width))
      return false;
    result.width = width;
    return true;
  };
  const auto closedProfile = [&](MirBodyAddress target) {
    const auto callee = round.summaries.find(target);
    if (round.rejected.contains(target) || callee == round.summaries.end() ||
        !callee->second.complete || !callee->second.scalarStoresOnly ||
        (callee->second.scalarStoreWidth &&
         !addWidth(*callee->second.scalarStoreWidth)))
      return false;
    result.floating = result.floating || callee->second.floatingScalarStores;
    return true;
  };
  if (address.kind == MirBodyKind::Constructor)
    if (const auto *constructor =
            program.findConstructorInstance(address.owner))
      if (const MirBodyAddress initializers{.kind =
                                                MirBodyKind::FieldInitializers,
                                            .owner = constructor->owner};
          findMirBody(program, initializers) && !closedProfile(initializers))
        return {};
  const auto vacantReceiverField = [&](const MirInstruction &instruction) {
    const auto *destination = instruction.destination
                                  ? body.findPlace(*instruction.destination)
                                  : nullptr;
    const auto effect = effects(program, instruction);
    return construction && instruction.kind == MirInstructionKind::Initialize &&
           destination && destination->root == MirPlaceRootKind::This &&
           !destination->projections.empty() &&
           std::all_of(destination->projections.begin(),
                       destination->projections.end(),
                       [](const MirPlaceProjection &projection) {
                         return projection.kind == MirProjectionKind::Field;
                       }) &&
           std::all_of(
               instruction.lifecycle.begin(), instruction.lifecycle.end(),
               [](const MirLifecycleEvent &event) {
                 return event.kind == MirLifecycleEventKind::Initialize ||
                        event.kind == MirLifecycleEventKind::Reparent;
               }) &&
           !instruction.rawMemoryAccess &&
           instruction.unsafeOperation == UnsafeOperationKind::None &&
           !effect.writesUnknownMemory && !effect.invokesUserCode &&
           !effect.invokesRuntime && !effect.maySynchronize;
  };
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.tombstone || inertLifecycleMarker(instruction) ||
          nonMutatingLoanBoundary(body, instruction) ||
          ownerWitnessFormation(body, instruction) ||
          inertRawAddressOperation(instruction) ||
          initializesReferenceCarrier(body, instruction) ||
          isMirStructuralFinalAddressConstruction(program, instruction) ||
          vacantReceiverField(instruction))
        continue;
      if (const auto constructor =
              closedObjectConstructTarget(program, instruction)) {
        if (!closedProfile(*constructor))
          return {};
        continue;
      }
      if (scalarResultMove(&program, instruction, body)) {
        const auto *source = body.findPlace(instruction.operands.front().place);
        if (source && source->root == MirPlaceRootKind::Binding &&
            source->projections.empty())
          continue;
      }
      if (isMirStructuralValueTransfer(program, instruction)) {
        const auto &operand = instruction.operands.front();
        const auto *source = body.findPlace(operand.place);
        const auto *destination = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
        const bool localDestination =
            !instruction.destination ||
            (destination &&
             (destination->root == MirPlaceRootKind::Binding ||
              destination->root == MirPlaceRootKind::Temporary) &&
             destination->projections.empty() &&
             !destination->traits.containsBorrowedState);
        if (localDestination &&
            (operand.kind != MirOperandKind::Move ||
             (source && !source->traits.containsBorrowedState &&
              ((source->root == MirPlaceRootKind::Binding &&
                source->projections.empty()) ||
               independentValueParameter(&program, address, *source)))))
          continue;
      }
      if (isMirStructuralValueDrop(program, instruction)) {
        const auto *destination = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
        if (destination && !destination->traits.containsBorrowedState &&
            (((destination->root == MirPlaceRootKind::Binding ||
               destination->root == MirPlaceRootKind::Temporary) &&
              destination->projections.empty()) ||
             independentValueParameter(&program, address, *destination)))
          continue;
      }
      const auto effect = effects(program, instruction);
      if (instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic == IntrinsicKind::None) {
        const auto target = closedCallTarget(program, instruction);
        const auto callee =
            target ? round.summaries.find(*target) : round.summaries.end();
        if (instruction.rawMemoryAccess || !target ||
            round.rejected.contains(*target) ||
            callee == round.summaries.end() || !callee->second.complete ||
            !callee->second.scalarStoresOnly || effect.maySynchronize ||
            (callee->second.scalarStoreWidth &&
             !addWidth(*callee->second.scalarStoreWidth)))
          return {};
        result.floating =
            result.floating || callee->second.floatingScalarStores;
        continue;
      }
      if (instruction.rawMemoryAccess ||
          instruction.unsafeOperation != UnsafeOperationKind::None ||
          effect.writesUnknownMemory || effect.maySynchronize ||
          effect.invokesUserCode || effect.allocates ||
          instruction.kind == MirInstructionKind::Construct)
        return {};
      const auto *destination = instruction.destination
                                    ? body.findPlace(*instruction.destination)
                                    : nullptr;
      const auto domain =
          destination ? constantIntegerDomain(destination->type) : std::nullopt;
      const bool floating = destination && floatingScalar(destination->type);
      const bool independentScalar =
          destination && (domain || floating) &&
          destination->root == MirPlaceRootKind::Binding &&
          destination->projections.empty() &&
          !destination->traits.containsBorrowedState;
      if (independentScalar &&
          (instruction.kind == MirInstructionKind::Initialize ||
           instruction.kind == MirInstructionKind::Assign))
        continue;
      if (instruction.kind == MirInstructionKind::Assign && domain &&
          instruction.lifecycle.empty() && !effect.invokesRuntime &&
          !effect.movesValue && !effect.dropsValue && addWidth(domain->width))
        continue;
      if (instruction.kind == MirInstructionKind::Assign && floating &&
          instruction.lifecycle.empty() && !effect.invokesRuntime &&
          !effect.movesValue && !effect.dropsValue) {
        result.floating = true;
        continue;
      }
      if (effect.writesPlace || effect.movesValue || effect.dropsValue)
        return {};
    }
  return result;
}

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
  // Capture facts depend only on the program, so one analysis per body
  // serves every context.
  if (round.captures != nullptr && round.localCaptureCache != nullptr) {
    auto found = round.localCaptureCache->find(address);
    if (found == round.localCaptureCache->end())
      found = round.localCaptureCache
                  ->emplace(address, analyzeMirLocalCaptures(program, address,
                                                             *round.captures))
                  .first;
    round.localCaptures = &found->second;
  }
  struct ResetCaptures {
    ClosedCallRound &round;
    ~ResetCaptures() { round.localCaptures = nullptr; }
  } resetCaptures{round};
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
        if (initializers->second.mayWriteGlobals) {
          entry.mayWriteGlobals = true;
          killGlobalAliases(*body, entry);
        }
        if (initializers->second.hasOpaqueEffects) {
          if (initializers->second.liveMemoryBarrier ==
              LiveMemoryBarrier::VacantStructuralConstruction)
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
  std::map<std::size_t, std::vector<AbstractState>> alternativeExits;
  std::vector<std::optional<IntegerFact>> returnFacts;
  std::vector<std::optional<FieldPath>> returnFields;
  std::vector<std::optional<InputAddress>> returnPointers;
  std::vector<std::optional<InputAddress>> returnBorrows;
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
      if (!fact && block.terminator.value->kind == MirOperandKind::Loan &&
          body->returnType.kind == SemanticType::Reference &&
          body->returnType.arguments.size() == 1)
        if (const auto pointer = transferred.output.borrowedAddresses.find(
                block.terminator.value->loan);
            pointer != transferred.output.borrowedAddresses.end())
          fact = integerAtAddress(*body, transferred.output, pointer->second,
                                  body->returnType.arguments.front(), &program,
                                  address);
      returnFacts.push_back(std::move(fact));
      std::optional<FieldPath> returnedField;
      const auto field =
          transferred.output.fieldOrigins.find(block.terminator.value->value);
      if (field != transferred.output.fieldOrigins.end()) {
        const auto *root = body->findPlace(field->second.root);
        if (field->second.root == 0 ||
            (root && root->root == MirPlaceRootKind::This &&
             root->projections.empty()))
          returnedField = field->second.path;
      }
      returnFields.push_back(std::move(returnedField));
      std::optional<InputAddress> returnedPointer;
      auto pointer = scalarAddressForOperand(*body, transferred.output,
                                             *block.terminator.value);
      const bool borrowed =
          block.terminator.value->kind == MirOperandKind::Loan;
      if (borrowed)
        if (const auto found = transferred.output.borrowedAddresses.find(
                block.terminator.value->loan);
            found != transferred.output.borrowedAddresses.end())
          pointer = found->second;
      if (pointer)
        returnedPointer =
            exportedInputAddress(program, address, *body, addressKey(*pointer));
      returnPointers.push_back(borrowed ? std::nullopt : returnedPointer);
      returnBorrows.push_back(borrowed ? returnedPointer : std::nullopt);
      if (body->returnType.kind == SemanticType::Class) {
        auto object = objectForOperand(*body, aliases, transferred.output,
                                       *block.terminator.value);
        if (object)
          sanitizeObject(program, body->returnType, *object);
        returnObjects.push_back(std::move(object));
      }
    }
    if (body->returnType.kind == SemanticType::Expected) {
      AlternativeFields alternatives;
      if (!block.terminator.value) {
        alternatives[0];
      } else if (block.terminator.value->type.kind ==
                 SemanticType::Unexpected) {
        alternatives[1];
      } else if (block.terminator.value->type.kind != SemanticType::Expected) {
        alternatives[0];
      } else if (const auto found = transferred.output.resultAlternatives.find(
                     block.terminator.value->value);
                 block.terminator.value->kind == MirOperandKind::Value &&
                 found != transferred.output.resultAlternatives.end()) {
        alternatives = found->second;
      } else {
        alternatives[0];
        alternatives[1];
      }
      for (const auto &[alternative, fields] : alternatives) {
        auto state = transferred.output;
        for (const auto &[origin, fact] : fields)
          publishFieldFact(*body, state, origin, fact);
        alternativeExits[alternative].push_back(std::move(state));
      }
    }
    exits.push_back(std::move(transferred.output));
  }
  if (exits.empty() || round.incompleteBodies.contains(address)) {
    return result;
  }
  result.summary.complete = true;
  const auto scalarStores = scalarStoreProfile(program, address, *body, round);
  result.summary.scalarStoresOnly = scalarStores.supported;
  result.summary.scalarStoreWidth = scalarStores.width;
  result.summary.floatingScalarStores = scalarStores.floating;
  result.summary.hasOpaqueEffects =
      std::any_of(exits.begin(), exits.end(), [](const AbstractState &state) {
        return state.hasOpaqueEffects;
      });
  result.summary.mayWriteGlobals =
      std::any_of(exits.begin(), exits.end(), [](const AbstractState &state) {
        return state.mayWriteGlobals;
      });
  for (const auto &state : exits)
    result.summary.liveMemoryBarrier =
        std::max(result.summary.liveMemoryBarrier, state.liveMemoryBarrier);

  const auto *function = address.kind == MirBodyKind::Function
                             ? program.findFunctionInstance(address.owner)
                             : nullptr;
  auto initialized = exits.front().constructedScalars;
  for (std::size_t index = 1; index < exits.size(); ++index)
    joinConstructed(initialized, exits[index].constructedScalars, false);
  for (auto &[pointer, value] : initialized) {
    // These local storage identities justify only transfers inside this body.
    // The caller independently captures surviving pre-call live fields.
    value.disjointFields.clear();
    if (const auto input =
            exportedInputAddress(program, address, *body, pointer))
      result.summary.constructedParameters.emplace(*input, value);
  }
  if (address.kind == MirBodyKind::Function) {
    auto pointers = exits.front().fieldAddresses;
    for (std::size_t index = 1; index < exits.size(); ++index)
      joinScalarAddresses(pointers, exits[index].fieldAddresses);
    for (const auto &[field, pointer] : pointers) {
      const auto output = exportedInputAddress(
          program, address, *body,
          ScalarAddressKey{AddressOriginKind::EntryPointerField, field.root,
                           field.path, 0, std::nullopt});
      const auto input =
          exportedInputAddress(program, address, *body, addressKey(pointer));
      if (output && input && result.summary.pointerFieldValues.size() < 64)
        result.summary.pointerFieldValues.emplace(
            InputField{output->parameter, output->field}, *input);
    }
  }
  if (!returnPointers.empty() && returnPointers.size() == exits.size() &&
      returnPointers.front() &&
      std::all_of(returnPointers.begin(), returnPointers.end(),
                  [&](const auto &candidate) {
                    return candidate == returnPointers.front();
                  })) {
    result.summary.returnedPointer = returnPointers.front();
    result.summary.localPointerWritesOnly =
        onlyLocalPointerAssignments(program, address);
  }
  if (!returnBorrows.empty() && returnBorrows.size() == exits.size() &&
      returnBorrows.front() &&
      std::all_of(returnBorrows.begin(), returnBorrows.end(),
                  [&](const auto &candidate) {
                    return candidate == returnBorrows.front();
                  }))
    result.summary.returnedBorrow = returnBorrows.front();
  if (function && function->owner) {
    result.summary.receiverUpdates = exits.front().entryRelativeFields;
    for (std::size_t index = 1; index < exits.size(); ++index)
      joinEntryRelative(result.summary.receiverUpdates,
                        exits[index].entryRelativeFields);
  }
  if (function && function->owner &&
      !receiverAllowsMutation(function->receiverMutability) &&
      !result.summary.hasOpaqueEffects && !returnFields.empty() &&
      returnFields.size() == exits.size()) {
    result.summary.receiverResultField.observed = true;
    const auto &origin = returnFields.front();
    if (origin &&
        std::all_of(returnFields.begin(), returnFields.end(),
                    [&](const auto &candidate) { return candidate == origin; }))
      result.summary.receiverResultField.fact = *origin;
  }

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
    if (index >= types.size() || types[index].kind != SemanticType::Reference ||
        index >= context.argumentObjects.size() ||
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
  for (const auto &[alternative, states] : alternativeExits) {
    auto &fields = result.summary.resultAlternatives[alternative];
    const auto collect = [&](std::optional<std::size_t> parameter,
                             const ObservedFact<ObjectFact> &object) {
      if (!object.fact)
        return;
      for (const auto &[path, fact] : object.fact->integers)
        if (fields.size() < 64)
          fields.emplace(InputField{parameter, path}, fact);
    };
    if (function && function->owner)
      if (const auto *owner = program.findClassInstance(*function->owner))
        collect(std::nullopt, objectAtExits(program, *body, aliases, states,
                                            thisRoot(*body), owner->type));
    for (std::size_t index = 0; index < bindings.size() && index < types.size();
         ++index)
      if (types[index].kind == SemanticType::Reference)
        if (const auto root = rootForBinding(*body, bindings[index]))
          collect(index, objectAtExits(program, *body, aliases, states, root,
                                       types[index]));
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
        else if (const auto constructor =
                     closedObjectConstructTarget(program, instruction))
          dependents[*constructor].insert(address);
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

  const MirProgramCaptureSummaries captureSummaries =
      deriveMirProgramCaptureSummaries(program);
  std::map<MirBodyAddress, MirLocalCaptureAnalysis, MirBodyAddressLess>
      localCaptureCache;
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
    ClosedCallRound round{.program = program,
                          .summaries = summaries,
                          .rejected = rejected,
                          .captures = &captureSummaries,
                          .localCaptureCache = &localCaptureCache};
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
