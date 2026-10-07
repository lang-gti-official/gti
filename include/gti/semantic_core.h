#pragma once

#include "gti/ast.h"
#include "gti/checked_integer.h"
#include "gti/concurrency.h"
#include "gti/constant_evaluator.h"
#include "gti/diagnostic.h"
#include "gti/failure.h"
#include "gti/generic_constraint.h"
#include "gti/source_graph.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

struct FrontendWorkMetrics;

// Optional diagnostics about programmer-owned unsafe obligations. This policy
// never changes typing, checked loans, MIR verification, or runtime semantics.
enum class RawPointerAuditMode { Off, Warn, Strict };

[[nodiscard]] constexpr std::optional<RawPointerAuditMode>
parseRawPointerAuditMode(std::string_view name) {
  if (name == "off")
    return RawPointerAuditMode::Off;
  if (name == "warn")
    return RawPointerAuditMode::Warn;
  if (name == "strict")
    return RawPointerAuditMode::Strict;
  return std::nullopt;
}

[[nodiscard]] constexpr std::string_view
rawPointerAuditModeName(RawPointerAuditMode mode) {
  switch (mode) {
  case RawPointerAuditMode::Off:
    return "off";
  case RawPointerAuditMode::Warn:
    return "warn";
  case RawPointerAuditMode::Strict:
    return "strict";
  }
  return "off";
}

using ClassId = std::size_t;
using ConceptId = std::size_t;
using ConstructorId = std::size_t;
using EnumId = std::size_t;
using GenericParameterId = std::size_t;
using FunctionId = std::size_t;
using LambdaId = std::size_t;
using ProgramInitializationStepId = std::size_t;
using SemanticLoanId = std::size_t;
using SymbolId = std::size_t;
using TypeAliasId = std::size_t;

struct PlaceDomain {
  std::size_t snapshot = 0;
  std::size_t body = 0;
  std::size_t revision = 0;

  friend bool operator==(const PlaceDomain &, const PlaceDomain &) = default;
};

[[nodiscard]] std::size_t acquirePlaceSnapshotIdentity();

enum class PlaceProjectionKind {
  Field,
  ConstantIndex,
  DynamicIndex,
  Dereference,
};

struct PlaceProjection {
  PlaceProjectionKind kind = PlaceProjectionKind::Field;
  SymbolId field = 0;
  std::uint64_t index = 0;
  std::size_t selection = 0;

  friend bool operator==(const PlaceProjection &,
                         const PlaceProjection &) = default;
};

struct PlaceKey {
  PlaceDomain domain;
  SymbolId root = 0;
  bool receiver = false;
  std::vector<PlaceProjection> projections;

  [[nodiscard]] bool valid() const { return root != 0 || receiver; }

  friend bool operator==(const PlaceKey &, const PlaceKey &) = default;
};

enum class PlaceRelation {
  Equal,
  LeftStrictPrefix,
  RightStrictPrefix,
  Disjoint,
  MayAlias,
};

struct PlaceRelationResult {
  PlaceRelation relation = PlaceRelation::MayAlias;
  bool compatibleDomain = true;
};

[[nodiscard]] PlaceRelationResult placeRelation(const PlaceKey &left,
                                                const PlaceKey &right);

[[nodiscard]] bool placesMayOverlap(const PlaceKey &left,
                                    const PlaceKey &right);

enum class OwnershipState : std::uint8_t {
  Uninitialized = 1U << 0U,
  Available = 1U << 1U,
  Moved = 1U << 2U,
};

struct OwnershipStateSet {
  std::uint8_t bits = static_cast<std::uint8_t>(OwnershipState::Available);

  static const OwnershipStateSet Uninitialized;
  static const OwnershipStateSet Available;
  static const OwnershipStateSet Moved;
  static const OwnershipStateSet MaybeMoved;

  [[nodiscard]] constexpr bool definitely(OwnershipState state) const {
    return bits == static_cast<std::uint8_t>(state);
  }

  [[nodiscard]] constexpr bool contains(OwnershipState state) const {
    return (bits & static_cast<std::uint8_t>(state)) != 0;
  }

  friend constexpr OwnershipStateSet operator|(OwnershipStateSet left,
                                               OwnershipStateSet right) {
    return {.bits = static_cast<std::uint8_t>(left.bits | right.bits)};
  }

  friend bool operator==(OwnershipStateSet, OwnershipStateSet) = default;
};

inline constexpr OwnershipStateSet OwnershipStateSet::Uninitialized{
    .bits = static_cast<std::uint8_t>(OwnershipState::Uninitialized)};
inline constexpr OwnershipStateSet OwnershipStateSet::Available{
    .bits = static_cast<std::uint8_t>(OwnershipState::Available)};
inline constexpr OwnershipStateSet OwnershipStateSet::Moved{
    .bits = static_cast<std::uint8_t>(OwnershipState::Moved)};
inline constexpr OwnershipStateSet OwnershipStateSet::MaybeMoved{
    .bits = static_cast<std::uint8_t>(OwnershipState::Available) |
            static_cast<std::uint8_t>(OwnershipState::Moved)};

enum class OwnershipEventKind {
  Read,
  Move,
  Reinitialize,
  DropBoundary,
  ConsumeReceiver,
};

struct OwnershipEvent {
  OwnershipEventKind kind = OwnershipEventKind::Read;
  PlaceKey place;
  OwnershipStateSet before = OwnershipStateSet::Available;
  OwnershipStateSet after = OwnershipStateSet::Available;
  bool reachable = true;

  friend bool operator==(const OwnershipEvent &,
                         const OwnershipEvent &) = default;
};

struct SemanticType;

struct GenericValueExpression {
  enum Kind {
    UInt64,
    Parameter,
    Binary,
  };

  Kind kind = UInt64;
  Token token;
  std::uint64_t value = 0;
  GenericParameterId parameterId = 0;
  std::shared_ptr<const GenericValueExpression> left;
  std::shared_ptr<const GenericValueExpression> right;
};

struct GenericParameterInfo {
  GenericParameterId id = 0;
  Token name;
  bool pack = false;
  bool value = false;
  GenericConstraintSet constraints = 0;
  std::optional<NamePath> constraintName;
  ConceptId conceptId = 0;
  std::shared_ptr<const SemanticType> defaultType;
  std::shared_ptr<const GenericValueExpression> defaultValue;
  // Semantics-resolved projection from the public class argument list.
  // Empty denotes an ordinary positional parameter. For a pack, the final
  // component selects the start of a tail, including an empty tail.
  std::vector<std::size_t> typeArgumentPath;
};

struct CompileTimeValue {
  enum Kind {
    Unknown,
    UInt64,
    Parameter,
  };

  [[nodiscard]] static CompileTimeValue uint64(std::uint64_t value) {
    return CompileTimeValue{.kind = UInt64, .value = value};
  }

  [[nodiscard]] static CompileTimeValue
  parameter(GenericParameterId parameterId) {
    return CompileTimeValue{.kind = Parameter, .parameterId = parameterId};
  }

  friend bool operator==(const CompileTimeValue &,
                         const CompileTimeValue &) = default;

  Kind kind = Unknown;
  std::uint64_t value = 0;
  GenericParameterId parameterId = 0;
};

enum class ArrayExtentEvaluationError {
  None,
  NonLiteral,
  Overflow,
  Underflow,
  ZeroDivisor,
};

struct ArrayExtentEvaluation {
  std::optional<std::uint64_t> value;
  ArrayExtentEvaluationError error = ArrayExtentEvaluationError::None;
  const Token *token = nullptr;
};

[[nodiscard]] ArrayExtentEvaluation
evaluateArrayExtent(const ArrayExtentExpr &expression);

enum class ValueCategory {
  Value,
  Place,
};

enum class AccessMode {
  ReadOnly,
  Mutable,
};

enum class OwnershipKind {
  Value,
  Borrowed,
  Unique,
  Shared,
};

enum class DropKind {
  Trivial,
  Lexical,
};

enum class ConcurrencyCapabilityPolicy : std::uint8_t {
  Structural,
  Denied,
  Required,
  UnsafeAsserted,
  UnsafeConditional,
};

struct ConcurrencyCapabilities {
  bool transferCapable = false;
  bool shareCapable = false;
};

struct SemanticType {
  enum Kind {
    Unknown,
    Void,
    Int8,
    Int16,
    Int32,
    Int64,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    Float,
    Double,
    Bool,
    Char,
    StringView,
    CString,
    NullPtr,
    RawPointer,
    Array,
    Class,
    Enum,
    Reference,
    UniqueOwner,
    SharedPointer,
    TypeParameter,
    TypePack,
    TypeName,
    Function,
    FunctionItem,
    NativeFunction,
    Lambda,
    Expected,
    Unexpected,
    PrefixStorage,
    FunctionSignature,
  };

  SemanticType(Kind kind = Unknown) : kind(kind) {}
  SemanticType(Kind kind, std::vector<SemanticType> arguments)
      : kind(kind), arguments(std::move(arguments)) {}

  [[nodiscard]] static SemanticType
  classType(ClassId id, std::vector<SemanticType> arguments = {},
            std::vector<CompileTimeValue> valueArguments = {}) {
    SemanticType type(Class, std::move(arguments));
    type.classId = id;
    type.valueArguments = std::move(valueArguments);
    return type;
  }

  [[nodiscard]] static SemanticType typeParameter(GenericParameterId id) {
    SemanticType type(TypeParameter);
    type.genericParameterId = id;
    return type;
  }

  [[nodiscard]] static SemanticType functionSignature(
      SemanticType result, std::vector<SemanticType> parameters,
      ReceiverMutability receiver = ReceiverMutability::ReadOnly) {
    parameters.insert(parameters.begin(), std::move(result));
    SemanticType type(FunctionSignature, std::move(parameters));
    type.signatureReceiver = receiver;
    return type;
  }

  [[nodiscard]] static SemanticType enumType(EnumId id) {
    SemanticType type(Enum);
    type.enumId = id;
    return type;
  }

  [[nodiscard]] static SemanticType typePack(GenericParameterId id) {
    SemanticType type(TypePack);
    type.genericParameterId = id;
    return type;
  }

  [[nodiscard]] static SemanticType
  concreteTypePack(GenericParameterId id, std::vector<SemanticType> elements) {
    SemanticType type(TypePack, std::move(elements));
    type.genericParameterId = id;
    type.concretePack = true;
    return type;
  }

  // Canonical argument lists contain expanded concrete elements, retaining
  // only symbolic packs until their declaration parameters are substituted.
  static void appendExpandedArgument(std::vector<SemanticType> &arguments,
                                     SemanticType argument) {
    if (argument.kind == TypePack && argument.concretePack) {
      for (SemanticType &element : argument.arguments) {
        appendExpandedArgument(arguments, std::move(element));
      }
    } else {
      arguments.emplace_back(std::move(argument));
    }
  }

  [[nodiscard]] static SemanticType typeName(ClassId id) {
    SemanticType type(TypeName);
    type.classId = id;
    return type;
  }

  [[nodiscard]] static SemanticType
  functionItemType(FunctionId id, SemanticType returnType,
                   std::span<const SemanticType> parameterTypes) {
    SemanticType type(FunctionItem);
    type.functionId = id;
    type.arguments.reserve(parameterTypes.size() + 1);
    type.arguments.emplace_back(std::move(returnType));
    type.arguments.insert(type.arguments.end(), parameterTypes.begin(),
                          parameterTypes.end());
    return type;
  }

  [[nodiscard]] bool hasFunctionItemShape() const {
    return kind == FunctionItem && functionId != 0 && !arguments.empty();
  }

  [[nodiscard]] const SemanticType *functionItemReturnType() const {
    return hasFunctionItemShape() ? &arguments.front() : nullptr;
  }

  [[nodiscard]] std::span<const SemanticType>
  functionItemParameterTypes() const {
    return hasFunctionItemShape()
               ? std::span<const SemanticType>(arguments).subspan(1)
               : std::span<const SemanticType>{};
  }

  [[nodiscard]] static SemanticType
  nativeFunctionType(SemanticType returnType,
                     std::span<const SemanticType> parameterTypes) {
    SemanticType type(NativeFunction);
    type.arguments.reserve(parameterTypes.size() + 1);
    type.arguments.emplace_back(std::move(returnType));
    type.arguments.insert(type.arguments.end(), parameterTypes.begin(),
                          parameterTypes.end());
    return type;
  }

  [[nodiscard]] bool hasNativeFunctionShape() const {
    return kind == NativeFunction && !arguments.empty();
  }

  [[nodiscard]] bool hasCThreadEntryShape() const {
    return kind == NativeFunction && arguments.size() == 2 &&
           arguments[0] == SemanticType::Int32 &&
           arguments[1] == rawPointerTo(SemanticType::Void);
  }

  [[nodiscard]] CThreadCallbackKind cThreadCallbackKind() const {
    if (hasCThreadEntryShape()) {
      return CThreadCallbackKind::ThreadEntry;
    }
    if (kind == NativeFunction && arguments.size() == 1 &&
        arguments[0] == SemanticType::Void) {
      return CThreadCallbackKind::OnceInitializer;
    }
    return CThreadCallbackKind::None;
  }

  [[nodiscard]] const SemanticType *nativeFunctionReturnType() const {
    return hasNativeFunctionShape() ? &arguments.front() : nullptr;
  }

  [[nodiscard]] std::span<const SemanticType>
  nativeFunctionParameterTypes() const {
    return hasNativeFunctionShape()
               ? std::span<const SemanticType>(arguments).subspan(1)
               : std::span<const SemanticType>{};
  }

  [[nodiscard]] static SemanticType
  lambdaType(LambdaId id, SemanticType returnType,
             std::span<const SemanticType> parameterTypes,
             std::span<const SemanticType> captureTypes,
             std::span<const SemanticType> classTypeArguments = {},
             std::span<const SemanticType> functionTypeArguments = {},
             std::span<const CompileTimeValue> classValueArguments = {},
             std::span<const CompileTimeValue> functionValueArguments = {}) {
    SemanticType type(Lambda);
    type.lambdaId = id;
    type.lambdaParameterCount = parameterTypes.size();
    type.lambdaCaptureCount = captureTypes.size();
    type.arguments.reserve(1 + parameterTypes.size() + captureTypes.size());
    type.arguments.emplace_back(std::move(returnType));
    type.arguments.insert(type.arguments.end(), parameterTypes.begin(),
                          parameterTypes.end());
    type.arguments.insert(type.arguments.end(), captureTypes.begin(),
                          captureTypes.end());
    type.lambdaEnclosingClassTypes.assign(classTypeArguments.begin(),
                                          classTypeArguments.end());
    type.lambdaEnclosingFunctionTypes.assign(functionTypeArguments.begin(),
                                             functionTypeArguments.end());
    type.lambdaEnclosingClassValues.assign(classValueArguments.begin(),
                                           classValueArguments.end());
    type.lambdaEnclosingFunctionValues.assign(functionValueArguments.begin(),
                                              functionValueArguments.end());
    return type;
  }

  [[nodiscard]] bool hasLambdaShape() const {
    return kind == Lambda && !arguments.empty() &&
           lambdaParameterCount + lambdaCaptureCount == arguments.size() - 1;
  }

  [[nodiscard]] const SemanticType *lambdaReturnType() const {
    return hasLambdaShape() ? &arguments.front() : nullptr;
  }

  [[nodiscard]] std::span<const SemanticType> lambdaParameterTypes() const {
    return hasLambdaShape() ? std::span<const SemanticType>(arguments).subspan(
                                  1, lambdaParameterCount)
                            : std::span<const SemanticType>{};
  }

  [[nodiscard]] std::span<const SemanticType> lambdaCaptureTypes() const {
    return hasLambdaShape() ? std::span<const SemanticType>(arguments).subspan(
                                  1 + lambdaParameterCount, lambdaCaptureCount)
                            : std::span<const SemanticType>{};
  }

  [[nodiscard]] std::span<const SemanticType> lambdaClassTypeArguments() const {
    return lambdaEnclosingClassTypes;
  }

  [[nodiscard]] std::span<const SemanticType>
  lambdaFunctionTypeArguments() const {
    return lambdaEnclosingFunctionTypes;
  }

  [[nodiscard]] std::span<const CompileTimeValue>
  lambdaClassValueArguments() const {
    return lambdaEnclosingClassValues;
  }

  [[nodiscard]] std::span<const CompileTimeValue>
  lambdaFunctionValueArguments() const {
    return lambdaEnclosingFunctionValues;
  }

  [[nodiscard]] static SemanticType arrayOf(SemanticType element,
                                            std::uint64_t length) {
    SemanticType type(Array, {std::move(element)});
    type.arrayLength = length;
    return type;
  }

  [[nodiscard]] static SemanticType arrayOf(SemanticType element,
                                            CompileTimeValue length) {
    SemanticType type(Array, {std::move(element)});
    if (length.kind == CompileTimeValue::UInt64) {
      type.arrayLength = length.value;
    } else if (length.kind == CompileTimeValue::Parameter) {
      type.arrayLengthParameterId = length.parameterId;
    }
    return type;
  }

  [[nodiscard]] static SemanticType
  referenceTo(SemanticType referent, AccessMode access = AccessMode::ReadOnly) {
    SemanticType type(Reference, {std::move(referent)});
    type.referenceAccess = access;
    return type;
  }

  [[nodiscard]] static SemanticType
  rawPointerTo(SemanticType pointee, AccessMode access = AccessMode::Mutable) {
    SemanticType type(RawPointer, {std::move(pointee)});
    type.pointerAccess = access;
    return type;
  }

  [[nodiscard]] static SemanticType uniqueOwnerOf(SemanticType pointee) {
    return SemanticType(UniqueOwner, {std::move(pointee)});
  }

  [[nodiscard]] static SemanticType sharedPointerTo(SemanticType pointee) {
    return SemanticType(SharedPointer, {std::move(pointee)});
  }

  [[nodiscard]] static SemanticType prefixStorageOf(SemanticType element) {
    return SemanticType(PrefixStorage, {std::move(element)});
  }

  friend bool operator==(const SemanticType &, const SemanticType &) = default;

  Kind kind;
  std::vector<SemanticType> arguments;
  std::vector<CompileTimeValue> valueArguments;
  ClassId classId = 0;
  EnumId enumId = 0;
  GenericParameterId genericParameterId = 0;
  FunctionId functionId = 0;
  LambdaId lambdaId = 0;
  std::uint64_t lambdaParameterCount = 0;
  std::uint64_t lambdaCaptureCount = 0;
  std::vector<SemanticType> lambdaEnclosingClassTypes;
  std::vector<SemanticType> lambdaEnclosingFunctionTypes;
  std::vector<CompileTimeValue> lambdaEnclosingClassValues;
  std::vector<CompileTimeValue> lambdaEnclosingFunctionValues;
  std::uint64_t arrayLength = 0;
  GenericParameterId arrayLengthParameterId = 0;
  AccessMode referenceAccess = AccessMode::ReadOnly;
  AccessMode pointerAccess = AccessMode::Mutable;
  ReceiverMutability signatureReceiver = ReceiverMutability::ReadOnly;
  bool concretePack = false;
};

struct AppliedConceptRequirement {
  ConceptId conceptId = 0;
  const ConceptApplication *syntax = nullptr;
  std::vector<SemanticType> arguments;
  const ConstraintExpression *expression = nullptr;
  std::size_t expressionApplication = ConstraintExpressionNode::invalidIndex;
};

[[nodiscard]] std::optional<CheckedIntegerDomain>
constantIntegerDomain(const SemanticType &type);

[[nodiscard]] std::optional<BinaryFloatFormat>
semanticFloatFormat(const SemanticType &type);

// Primitive by-value inputs have no loan or cleanup to transfer across an
// in-place construction preflight. Classes, enums, text and pointers are not
// admitted by representation similarity to a primitive.
[[nodiscard]] inline bool isPassiveScalarValueType(const SemanticType &type) {
  return constantIntegerDomain(type).has_value() ||
         semanticFloatFormat(type).has_value() || type == SemanticType::Bool ||
         type == SemanticType::Char;
}

[[nodiscard]] SemanticType semanticIntegerType(CheckedIntegerDomain domain);

// Apply a resolved binder projection; this does not perform name lookup,
// pattern matching, or overload selection in downstream consumers.
[[nodiscard]] inline std::optional<SemanticType>
projectGenericTypeArgument(std::span<const SemanticType> arguments,
                           std::span<const std::size_t> path,
                           GenericParameterId id, bool pack) {
  if (path.empty() || id == 0) {
    return std::nullopt;
  }
  for (std::size_t depth = 0; depth + 1 < path.size(); ++depth) {
    if (path[depth] >= arguments.size()) {
      return std::nullopt;
    }
    arguments = arguments[path[depth]].arguments;
  }
  const std::size_t index = path.back();
  if (!pack) {
    return index < arguments.size() ? std::optional{arguments[index]}
                                    : std::nullopt;
  }
  if (index > arguments.size()) {
    return std::nullopt;
  }
  const auto tail = arguments.subspan(index);
  if (tail.size() == 1 && tail.front().kind == SemanticType::TypePack &&
      !tail.front().concretePack) {
    return tail.front();
  }
  return SemanticType::concreteTypePack(
      id, std::vector<SemanticType>(tail.begin(), tail.end()));
}

// Relocation consumes one complete value into fresh, non-overlapping storage.
// It is not copying, assignment, or permission to invalidate outstanding loans.
// Bitwise is a language eligibility fact; a backend must also prove that its
// concrete representation permits bytewise lifetime creation.
enum class RelocationKind : std::uint8_t { Unavailable, Structural, Bitwise };

struct SemanticTypeTraits {
  OwnershipKind ownership = OwnershipKind::Value;
  DropKind drop = DropKind::Trivial;
  bool copyable = true;
  bool movable = true;
  bool copyAssignable = true;
  bool moveAssignable = true;
  bool containsBorrowedState = false;
  bool transferCapable = true;
  bool shareCapable = true;
  RelocationKind relocation = RelocationKind::Unavailable;

  friend bool operator==(const SemanticTypeTraits &,
                         const SemanticTypeTraits &) = default;
};

// Nominal class traits require collected field metadata. Semantic analysis
// records those resolved traits in ExpressionInfo and BindingInfo.
[[nodiscard]] SemanticTypeTraits semanticTraits(const SemanticType &type);

struct ExpressionInfo {
  SemanticType type = SemanticType::Unknown;
  ValueCategory category = ValueCategory::Value;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
  // The frontend-selected arithmetic domain of a built-in place update.
  // Unknown for other expressions; distinct from the destination/result type.
  SemanticType updateArithmeticType = SemanticType::Unknown;

  friend bool operator==(const ExpressionInfo &,
                         const ExpressionInfo &) = default;
};

struct BindingInfo {
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
  std::optional<ConstantValue> constant;
  bool explicitlyMoved = false;
  SymbolId symbol = 0;
  bool staticStorage = false;
  bool internalLinkage = false;
  SemanticLoanId retainedLoan = 0;

  friend bool operator==(const BindingInfo &, const BindingInfo &) = default;
};

} // namespace lang
