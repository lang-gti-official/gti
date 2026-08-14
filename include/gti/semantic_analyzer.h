#pragma once

#include "gti/ast.h"
#include "gti/checked_integer.h"
#include "gti/constant_evaluator.h"
#include "gti/diagnostic.h"
#include "gti/generic_constraint.h"
#include "gti/source_graph.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace lang {

using ClassId = std::size_t;
using ConceptId = std::size_t;
using ConstructorId = std::size_t;
using EnumId = std::size_t;
using GenericParameterId = std::size_t;
using FunctionId = std::size_t;
using LambdaId = std::size_t;
using SemanticLoanId = std::size_t;
using SymbolId = std::size_t;
using TypeAliasId = std::size_t;

struct PlaceDomain {
  std::size_t snapshot = 0;
  std::size_t body = 0;
  std::size_t revision = 0;

  friend bool operator==(const PlaceDomain &, const PlaceDomain &) = default;
};

[[nodiscard]] inline std::size_t acquirePlaceSnapshotIdentity() {
  static std::atomic_size_t next{1};
  std::size_t result = next.fetch_add(1, std::memory_order_relaxed);
  if (result == 0) {
    result = next.fetch_add(1, std::memory_order_relaxed);
  }
  return result;
}

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

[[nodiscard]] inline PlaceRelationResult placeRelation(const PlaceKey &left,
                                                       const PlaceKey &right) {
  if (left.domain != right.domain) {
    return {.compatibleDomain = false};
  }
  if (!left.valid() || !right.valid()) {
    return {.relation = PlaceRelation::MayAlias};
  }
  if (left.receiver != right.receiver || left.root != right.root) {
    return {.relation = PlaceRelation::Disjoint};
  }

  const std::size_t common =
      std::min(left.projections.size(), right.projections.size());
  for (std::size_t projectionIndex = 0; projectionIndex < common;
       ++projectionIndex) {
    const PlaceProjection &lhs = left.projections[projectionIndex];
    const PlaceProjection &rhs = right.projections[projectionIndex];
    if (lhs == rhs) {
      continue;
    }
    if (lhs.kind == PlaceProjectionKind::Field &&
        rhs.kind == PlaceProjectionKind::Field && lhs.field != 0 &&
        rhs.field != 0 && lhs.field != rhs.field) {
      return {.relation = PlaceRelation::Disjoint};
    }
    if (lhs.kind == PlaceProjectionKind::ConstantIndex &&
        rhs.kind == PlaceProjectionKind::ConstantIndex &&
        lhs.index != rhs.index) {
      return {.relation = PlaceRelation::Disjoint};
    }
    if (lhs.kind == PlaceProjectionKind::DynamicIndex &&
        rhs.kind == PlaceProjectionKind::DynamicIndex && lhs.selection != 0 &&
        lhs.selection == rhs.selection) {
      continue;
    }
    return {.relation = PlaceRelation::MayAlias};
  }
  if (left.projections.size() == right.projections.size()) {
    return {.relation = PlaceRelation::Equal};
  }
  return {.relation = left.projections.size() < right.projections.size()
                          ? PlaceRelation::LeftStrictPrefix
                          : PlaceRelation::RightStrictPrefix};
}

[[nodiscard]] inline bool placesMayOverlap(const PlaceKey &left,
                                           const PlaceKey &right) {
  const PlaceRelationResult relation = placeRelation(left, right);
  return !relation.compatibleDomain ||
         relation.relation != PlaceRelation::Disjoint;
}

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
};

struct OwnershipEvent {
  OwnershipEventKind kind = OwnershipEventKind::Read;
  PlaceKey place;
  OwnershipStateSet before = OwnershipStateSet::Available;
  OwnershipStateSet after = OwnershipStateSet::Available;
  bool reachable = true;
};

struct GenericParameterInfo {
  GenericParameterId id = 0;
  Token name;
  bool pack = false;
  bool value = false;
  GenericConstraintSet constraints = 0;
  std::optional<NamePath> constraintName;
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

[[nodiscard]] inline ArrayExtentEvaluation
evaluateArrayExtent(const ArrayExtentExpr &expression) {
  if (expression.isAtom()) {
    if (const auto *value =
            std::get_if<std::uint64_t>(&expression.token.literal)) {
      return {.value = *value};
    }
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }
  if (!expression.left || !expression.right) {
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }

  const ArrayExtentEvaluation left = evaluateArrayExtent(*expression.left);
  if (!left.value) {
    return left;
  }
  const ArrayExtentEvaluation right = evaluateArrayExtent(*expression.right);
  if (!right.value) {
    return right;
  }

  std::optional<CheckedIntegerOperation> operation;
  switch (expression.token.kind) {
  case TokenKind::PLUS:
    operation = CheckedIntegerOperation::Add;
    break;
  case TokenKind::MINUS:
    operation = CheckedIntegerOperation::Subtract;
    break;
  case TokenKind::STAR:
    operation = CheckedIntegerOperation::Multiply;
    break;
  case TokenKind::SLASH:
    operation = CheckedIntegerOperation::Divide;
    break;
  case TokenKind::PERCENT:
    operation = CheckedIntegerOperation::Remainder;
    break;
  default:
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }

  const std::optional<CheckedIntegerOutcome> evaluated =
      evaluateCheckedIntegerBinary(*operation, {.magnitude = *left.value},
                                   {.magnitude = *right.value},
                                   CheckedIntegerDomain{.width = 64});
  if (!evaluated) {
    return {.error = ArrayExtentEvaluationError::NonLiteral,
            .token = &expression.token};
  }
  if (const auto *value = std::get_if<CheckedIntegerValue>(&*evaluated)) {
    return {.value = value->magnitude};
  }

  const CheckedIntegerFailure failure =
      std::get<CheckedIntegerFailure>(*evaluated);
  if (failure == CheckedIntegerFailure::Overflow) {
    return {.error = expression.token.kind == TokenKind::MINUS
                         ? ArrayExtentEvaluationError::Underflow
                         : ArrayExtentEvaluationError::Overflow,
            .token = &expression.token};
  }
  if (failure == CheckedIntegerFailure::DivisionByZero ||
      failure == CheckedIntegerFailure::ModuloByZero) {
    return {.error = ArrayExtentEvaluationError::ZeroDivisor,
            .token = &expression.token};
  }
  return {.error = ArrayExtentEvaluationError::NonLiteral,
          .token = &expression.token};
}

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
    NullPtr,
    RawPointer,
    Array,
    Class,
    Enum,
    Reference,
    UniqueOwner,
    SharedPointer,
    Storage,
    TypeParameter,
    TypePack,
    TypeName,
    Function,
    Lambda,
    Expected,
    Unexpected,
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

  [[nodiscard]] static SemanticType typeName(ClassId id) {
    SemanticType type(TypeName);
    type.classId = id;
    return type;
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

  [[nodiscard]] static SemanticType storageOf(SemanticType element) {
    return SemanticType(Storage, {std::move(element)});
  }

  friend bool operator==(const SemanticType &, const SemanticType &) = default;

  Kind kind;
  std::vector<SemanticType> arguments;
  std::vector<CompileTimeValue> valueArguments;
  ClassId classId = 0;
  EnumId enumId = 0;
  GenericParameterId genericParameterId = 0;
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
  bool concretePack = false;
};

struct AppliedConceptRequirement {
  ConceptId conceptId = 0;
  const ConceptApplication *syntax = nullptr;
  std::vector<SemanticType> arguments;
};

[[nodiscard]] inline std::optional<CheckedIntegerDomain>
constantIntegerDomain(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
    return CheckedIntegerDomain{.width = 8, .signedValue = true};
  case SemanticType::Int16:
    return CheckedIntegerDomain{.width = 16, .signedValue = true};
  case SemanticType::Int32:
    return CheckedIntegerDomain{.width = 32, .signedValue = true};
  case SemanticType::Int64:
    return CheckedIntegerDomain{.width = 64, .signedValue = true};
  case SemanticType::UInt8:
    return CheckedIntegerDomain{.width = 8};
  case SemanticType::UInt16:
    return CheckedIntegerDomain{.width = 16};
  case SemanticType::UInt32:
    return CheckedIntegerDomain{.width = 32};
  case SemanticType::UInt64:
    return CheckedIntegerDomain{.width = 64};
  default:
    return std::nullopt;
  }
}

[[nodiscard]] inline std::optional<BinaryFloatFormat>
semanticFloatFormat(const SemanticType &type) {
  if (type == SemanticType::Float) {
    return BinaryFloatFormat::Binary32;
  }
  if (type == SemanticType::Double) {
    return BinaryFloatFormat::Binary64;
  }
  return std::nullopt;
}

[[nodiscard]] inline SemanticType
semanticIntegerType(CheckedIntegerDomain domain) {
  if (domain.signedValue) {
    switch (domain.width) {
    case 8:
      return SemanticType::Int8;
    case 16:
      return SemanticType::Int16;
    case 32:
      return SemanticType::Int32;
    case 64:
      return SemanticType::Int64;
    default:
      return SemanticType::Unknown;
    }
  }
  switch (domain.width) {
  case 8:
    return SemanticType::UInt8;
  case 16:
    return SemanticType::UInt16;
  case 32:
    return SemanticType::UInt32;
  case 64:
    return SemanticType::UInt64;
  default:
    return SemanticType::Unknown;
  }
}

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
};

// Nominal class traits require collected field metadata. Semantic analysis
// records those resolved traits in ExpressionInfo and BindingInfo.
[[nodiscard]] inline SemanticTypeTraits
semanticTraits(const SemanticType &type) {
  SemanticTypeTraits traits;
  switch (type.kind) {
  case SemanticType::Unknown:
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Void:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
  case SemanticType::Function:
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Lambda:
    traits.drop = DropKind::Lexical;
    return traits;
  case SemanticType::Reference:
    traits.ownership = OwnershipKind::Borrowed;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.containsBorrowedState = true;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::RawPointer:
  case SemanticType::StringView:
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Array:
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.drop = element.drop;
      traits.copyable = element.copyable;
      traits.movable = element.movable;
      traits.copyAssignable = element.copyAssignable;
      traits.moveAssignable = element.moveAssignable;
      traits.containsBorrowedState = element.containsBorrowedState;
      traits.transferCapable = element.transferCapable;
      traits.shareCapable = element.shareCapable;
      return traits;
    }
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.movable = false;
    traits.copyAssignable = false;
    traits.moveAssignable = false;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::UniqueOwner:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.copyAssignable = false;
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.transferCapable = element.transferCapable;
      traits.shareCapable = element.shareCapable;
    } else {
      traits.transferCapable = false;
      traits.shareCapable = false;
    }
    return traits;
  case SemanticType::Storage:
    traits.ownership = OwnershipKind::Unique;
    traits.drop = DropKind::Lexical;
    traits.copyable = false;
    traits.copyAssignable = false;
    if (type.arguments.size() == 1) {
      const SemanticTypeTraits element = semanticTraits(type.arguments[0]);
      traits.transferCapable = element.transferCapable;
      traits.shareCapable = element.shareCapable;
    } else {
      traits.transferCapable = false;
      traits.shareCapable = false;
    }
    return traits;
  case SemanticType::SharedPointer:
    traits.ownership = OwnershipKind::Shared;
    traits.drop = DropKind::Lexical;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Class:
  case SemanticType::TypeParameter:
    traits.drop = DropKind::Lexical;
    traits.transferCapable = false;
    traits.shareCapable = false;
    return traits;
  case SemanticType::Expected:
  case SemanticType::Unexpected:
    traits.drop = DropKind::Lexical;
    if (type.arguments.empty()) {
      traits.transferCapable = false;
      traits.shareCapable = false;
      return traits;
    }
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (type.kind == SemanticType::Expected && index == 0 &&
          type.arguments[index] == SemanticType::Void) {
        continue;
      }
      const SemanticTypeTraits component =
          semanticTraits(type.arguments[index]);
      traits.copyable = traits.copyable && component.copyable;
      traits.movable = traits.movable && component.movable;
      traits.copyAssignable = traits.copyAssignable && component.copyAssignable;
      traits.moveAssignable = traits.moveAssignable && component.moveAssignable;
      traits.containsBorrowedState =
          traits.containsBorrowedState || component.containsBorrowedState;
      traits.transferCapable =
          traits.transferCapable && component.transferCapable;
      traits.shareCapable = traits.shareCapable && component.shareCapable;
    }
    return traits;
  default:
    return traits;
  }
}

struct ExpressionInfo {
  SemanticType type = SemanticType::Unknown;
  ValueCategory category = ValueCategory::Value;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
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
};

enum class SemanticLoanKind {
  Reference,
  StoredValue,
};

enum class SemanticLoanEndKind {
  AfterStatement,
  ThenBranchEntry,
  ElseBranchEntry,
  SwitchArmEntry,
};

enum class UnsafeOperationKind {
  None,
  AddressOf,
  RawDereference,
  RawIndex,
  RawMember,
  UnionMember,
  PointerArithmetic,
  ForeignPointerCall,
};

struct SemanticLoanEndpoint {
  SemanticLoanEndKind kind = SemanticLoanEndKind::AfterStatement;
  const Stmt *statement = nullptr;
  std::size_t switchArm = 0;
};

using SemanticLoanPlaceProjectionKind = PlaceProjectionKind;
using SemanticLoanPlaceProjection = PlaceProjection;
using SemanticLoanPlace = PlaceKey;

struct SemanticLoanInfo {
  SemanticLoanId id = 0;
  SemanticLoanKind kind = SemanticLoanKind::Reference;
  const Expr *origin = nullptr;
  SymbolId owner = 0;
  bool receiverOrigin = false;
  SemanticLoanId parent = 0;
  bool entry = false;
  SemanticLoanPlace place;
  AccessMode access = AccessMode::ReadOnly;
  bool protectsStorage = false;
  std::vector<SymbolId> carriers;
  std::vector<SemanticLoanEndpoint> endpoints;
};

struct SemanticConditionalLoanEnds {
  std::vector<SemanticLoanId> thenEntry;
  std::vector<SemanticLoanId> elseEntry;
};

enum class StructuredBindingProjectionKind {
  Field,
  ArrayElement,
};

struct StructuredBindingElementInfo {
  const VariableDecl *declaration = nullptr;
  BindingInfo binding;
  StructuredBindingProjectionKind projection =
      StructuredBindingProjectionKind::Field;
  SymbolId field = 0;
  std::uint64_t index = 0;
};

struct StructuredBindingInfo {
  BindingInfo source;
  std::vector<StructuredBindingElementInfo> elements;
};

enum class CallableInvocationCapability {
  Read,
  Mutable,
  Once,
};

[[nodiscard]] constexpr CallableInvocationCapability
callableInvocationCapability(AccessMode access) {
  return access == AccessMode::Mutable ? CallableInvocationCapability::Mutable
                                       : CallableInvocationCapability::Read;
}

[[nodiscard]] constexpr CallableInvocationCapability
callableInvocationCapability(ReceiverMutability mutability) {
  switch (mutability) {
  case ReceiverMutability::ReadOnly:
    return CallableInvocationCapability::Read;
  case ReceiverMutability::Mutable:
    return CallableInvocationCapability::Mutable;
  case ReceiverMutability::Consuming:
    return CallableInvocationCapability::Once;
  }
  return CallableInvocationCapability::Read;
}

[[nodiscard]] constexpr bool
callableCapabilitySatisfies(CallableInvocationCapability provided,
                            CallableInvocationCapability required) {
  switch (required) {
  case CallableInvocationCapability::Read:
    return provided == CallableInvocationCapability::Read;
  case CallableInvocationCapability::Mutable:
    return provided == CallableInvocationCapability::Read ||
           provided == CallableInvocationCapability::Mutable;
  case CallableInvocationCapability::Once:
    return true;
  }
  return false;
}

struct CallableSignatureRequirement {
  const Call *source = nullptr;
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
};

struct CallableForwardingRequirement {
  const Call *source = nullptr;
  FunctionId function = 0;
  std::size_t parameterIndex = 0;
};

enum class CallableBoundary {
  Confined,
  Owned,
};

enum class CallableOwnedTransportKind {
  ExactReturn,
  ExactField,
};

struct CallableOwnedTransport {
  CallableOwnedTransportKind kind = CallableOwnedTransportKind::ExactReturn;
  SemanticType destinationType = SemanticType::Unknown;
  SymbolId field = 0;

  friend bool operator==(const CallableOwnedTransport &,
                         const CallableOwnedTransport &) = default;
};

struct CallableArgumentBoundary {
  std::size_t parameterIndex = 0;
  CallableBoundary boundary = CallableBoundary::Confined;

  friend bool operator==(const CallableArgumentBoundary &,
                         const CallableArgumentBoundary &) = default;
};

struct CallableParameterContract {
  std::size_t parameterIndex = 0;
  GenericParameterId genericParameter = 0;
  SemanticType callableType = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  CallableBoundary boundary = CallableBoundary::Confined;
  std::optional<CallableOwnedTransport> ownedTransport;
  std::vector<CallableSignatureRequirement> signatures;
  std::vector<CallableForwardingRequirement> forwardings;
};

enum class IntrinsicKind {
  None,
  NumericTypeParameterConversion,
  NumericAliasConversion,
  DefaultTypeParameterConstruction,
  Move,
  AllocateUniqueOwner,
  UniqueOwnerBorrow,
  UniqueOwnerBorrowMut,
  UniqueOwnerIsNull,
  AllocateStorage,
  StorageConstruct,
  StorageRead,
  StorageReadMut,
  StorageDestroy,
  StorageRelocate,
  IntegerWrappingAdd,
  IntegerWrappingSubtract,
  IntegerWrappingMultiply,
  IntegerSaturatingAdd,
  IntegerSaturatingSubtract,
  IntegerSaturatingMultiply,
  IntegerCheckedAdd,
  IntegerCheckedSubtract,
  IntegerCheckedMultiply,
  Count,
};

struct IntegerArithmeticIntrinsic {
  CheckedIntegerOperation operation = CheckedIntegerOperation::Add;
  IntegerArithmeticMode mode = IntegerArithmeticMode::Wrapping;
};

[[nodiscard]] inline std::optional<IntegerArithmeticIntrinsic>
integerArithmeticIntrinsic(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::IntegerWrappingAdd:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Add,
                                      IntegerArithmeticMode::Wrapping};
  case IntrinsicKind::IntegerWrappingSubtract:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Subtract,
                                      IntegerArithmeticMode::Wrapping};
  case IntrinsicKind::IntegerWrappingMultiply:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Multiply,
                                      IntegerArithmeticMode::Wrapping};
  case IntrinsicKind::IntegerSaturatingAdd:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Add,
                                      IntegerArithmeticMode::Saturating};
  case IntrinsicKind::IntegerSaturatingSubtract:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Subtract,
                                      IntegerArithmeticMode::Saturating};
  case IntrinsicKind::IntegerSaturatingMultiply:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Multiply,
                                      IntegerArithmeticMode::Saturating};
  case IntrinsicKind::IntegerCheckedAdd:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Add,
                                      IntegerArithmeticMode::CheckedResult};
  case IntrinsicKind::IntegerCheckedSubtract:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Subtract,
                                      IntegerArithmeticMode::CheckedResult};
  case IntrinsicKind::IntegerCheckedMultiply:
    return IntegerArithmeticIntrinsic{CheckedIntegerOperation::Multiply,
                                      IntegerArithmeticMode::CheckedResult};
  default:
    return std::nullopt;
  }
}

// These classify selected compiler-owned type declarations from the trusted
// prelude. Source spelling alone never selects one of these capabilities.
enum class CompilerCapabilityTypeKind {
  None,
  UniqueOwner,
  Storage,
  TextView,
};

enum class ProgramEntryKind {
  None,
  NoArguments,
  OwnedArguments,
};

enum class BorrowOriginKind {
  None,
  Receiver,
  Argument,
};

struct FunctionInfo {
  FunctionId id = 0;
  SourceUnitId sourceUnit = 0;
  const FunctionDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<GenericParameterInfo> genericParameters;
  std::vector<AppliedConceptRequirement> requirements;
  bool parameterPack = false;
  ClassId ownerClass = 0;
  bool entryPoint = false;
  ProgramEntryKind entryKind = ProgramEntryKind::None;
  FunctionId entryArgumentAppendFunction = 0;
  bool staticMember = false;
  bool internalLinkage = false;
  bool constexprFunction = false;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::vector<FunctionId> virtualRoots;
  std::vector<CallableParameterContract> callableParameters;
  bool compilerPrivate = false;
};

struct LambdaCaptureInfo {
  Token capture;
  Token declaration;
  const Expr *initializer = nullptr;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits{};
  LambdaCaptureMode mode = LambdaCaptureMode::Copy;
  SymbolId sourceSymbol = 0;
  SymbolId bindingSymbol = 0;
};

struct LambdaInfo {
  LambdaId id = 0;
  const Lambda *declaration = nullptr;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<LambdaCaptureInfo> captures;
  SemanticTypeTraits traits{};
};

struct ClassFieldTypeInfo {
  const VariableDecl *declaration = nullptr;
  SemanticType type = SemanticType::Unknown;
};

struct StoredReferenceInfo {
  const VariableDecl *field = nullptr;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
};

struct ClassBaseTypeInfo {
  const BaseSpecifier *syntax = nullptr;
  SemanticType type = SemanticType::Unknown;
  bool interface = false;
};

struct CAbiRecordFieldLayout {
  const VariableDecl *declaration = nullptr;
  SemanticType type = SemanticType::Unknown;
  std::uint64_t offsetBytes = 0;
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
};

struct CAbiRecordLayout {
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
  std::vector<CAbiRecordFieldLayout> fields;
};

struct UnionFieldLayout {
  const VariableDecl *declaration = nullptr;
  SemanticType type = SemanticType::Unknown;
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
};

struct UnionLayout {
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
  std::vector<UnionFieldLayout> fields;
};

struct ClassTypeInfo {
  ClassId id = 0;
  SourceUnitId sourceUnit = 0;
  const ClassDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  std::vector<GenericParameterInfo> genericParameters;
  std::vector<ClassFieldTypeInfo> fields;
  std::vector<ClassFieldTypeInfo> staticFields;
  std::optional<StoredReferenceInfo> storedReference;
  ClassKind kind = ClassKind::Class;
  std::vector<ClassBaseTypeInfo> bases;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  bool cOpaqueHandle = false;
  std::optional<CAbiRecordLayout> cAbiLayout;
  std::optional<UnionLayout> unionLayout;
  SemanticTypeTraits traits{};
  ConcurrencyCapabilityPolicy transferPolicy =
      ConcurrencyCapabilityPolicy::Structural;
  ConcurrencyCapabilityPolicy sharePolicy =
      ConcurrencyCapabilityPolicy::Structural;
  bool compilerPrivate = false;
  CompilerCapabilityTypeKind compilerCapability =
      CompilerCapabilityTypeKind::None;
};

struct EnumConstant {
  bool negative = false;
  std::uint64_t magnitude = 0;

  friend bool operator==(const EnumConstant &, const EnumConstant &) = default;
};

enum class SwitchCaseKind {
  Integer,
  Character,
  Enumerator,
};

struct SwitchCaseValue {
  SwitchCaseKind kind = SwitchCaseKind::Integer;
  SemanticType type = SemanticType::Unknown;
  EnumConstant value;
  EnumId enumOwner = 0;

  friend bool operator==(const SwitchCaseValue &,
                         const SwitchCaseValue &) = default;
};

struct EnumeratorInfo {
  const EnumeratorDecl *declaration = nullptr;
  EnumConstant value;
  bool explicitValue = false;
  std::size_t variantIndex = 0;
  std::vector<SemanticType> payloadTypes;
};

struct EnumTypeInfo {
  EnumId id = 0;
  SourceUnitId sourceUnit = 0;
  const EnumDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<EnumeratorInfo> enumerators;
  bool payload = false;
  bool compilerPrivate = false;
};

struct ResolvedEnumeratorInfo {
  EnumId owner = 0;
  const EnumeratorDecl *declaration = nullptr;
  EnumConstant value;
  std::size_t variantIndex = 0;
};

struct ResolvedPayloadConstructionInfo {
  EnumId owner = 0;
  const EnumeratorDecl *declaration = nullptr;
  std::size_t variantIndex = 0;
  std::vector<SemanticType> parameterTypes;
};

struct PayloadBindingInfo {
  const Token *name = nullptr;
  const Expr *source = nullptr;
  BindingInfo binding;
  std::size_t payloadIndex = 0;
};

struct ResolvedPayloadPatternInfo {
  EnumId owner = 0;
  const EnumeratorDecl *declaration = nullptr;
  std::size_t variantIndex = 0;
  std::vector<PayloadBindingInfo> bindings;
};

struct TypeAliasInfo {
  SourceUnitId sourceUnit = 0;
  const TypeAliasDecl *declaration = nullptr;
  std::string qualifiedName;
  SemanticType type = SemanticType::Unknown;
  bool compilerPrivate = false;
};

enum class SpecialMemberStatus {
  Declared,
  Generated,
  Deleted,
};

enum class ConstructorKind {
  Ordinary,
  Copy,
  Move,
};

struct ConstructorInfo {
  ConstructorId id = 0;
  ClassId owner = 0;
  const ConstructorDecl *declaration = nullptr;
  ConstructorKind kind = ConstructorKind::Ordinary;
  AccessModifier access = AccessModifier::Public;
  std::vector<GenericParameterInfo> genericParameters;
  std::vector<SemanticType> parameterTypes;
  std::optional<std::size_t> borrowParameter;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool compilerPrivate = false;
};

struct DestructorInfo {
  ClassId owner = 0;
  const DestructorDecl *declaration = nullptr;
  AccessModifier access = AccessModifier::Public;
};

struct ClassLifecycleInfo {
  ClassId id = 0;
  const ClassDecl *declaration = nullptr;
  std::vector<ConstructorInfo> constructors;
  std::optional<ConstructorInfo> declaredCopyConstructor;
  std::optional<ConstructorInfo> declaredMoveConstructor;
  std::optional<DestructorInfo> declaredDestructor;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus destructor = SpecialMemberStatus::Generated;
  bool requiresActiveDropState = false;
  SemanticTypeTraits traits{};
  bool polymorphic = false;
};

struct ResolvedConstructionInfo {
  ConstructorId constructor = 0;
  const ConstructorDecl *declaration = nullptr;
  SemanticType constructedType = SemanticType::Unknown;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<SemanticType> parameterTypes;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool generatedDefault = false;
  ConstructorKind kind = ConstructorKind::Ordinary;
};

enum class ConstructorInitializerTargetKind {
  Field,
  Base,
};

struct ResolvedConstructorInitializerInfo {
  ConstructorInitializerTargetKind kind =
      ConstructorInitializerTargetKind::Field;
  SemanticType targetType = SemanticType::Unknown;
  SymbolId field = 0;
  ConstructorId constructor = 0;
  const ConstructorDecl *declaration = nullptr;
  std::vector<SemanticType> parameterTypes;
  bool storesReference = false;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool generatedDefault = false;
  std::optional<std::size_t> ownedParameter;
};

struct ResolvedClassArguments {
  std::vector<SemanticType> types;
  std::vector<CompileTimeValue> values;
  bool valid = true;
};

enum class CallDispatch {
  Static,
  Virtual,
};

struct ResolvedCallInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<AppliedConceptRequirement> requirements;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::vector<CallableArgumentBoundary> callableArguments;
};

struct ResolvedLambdaCallInfo {
  LambdaId lambda = 0;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
  std::optional<CallableBoundary> boundary;
};

struct DeferredCallableCallInfo {
  GenericParameterId genericParameter = 0;
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
  AccessMode access = AccessMode::ReadOnly;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
  CallableBoundary boundary = CallableBoundary::Confined;
};

struct ResolvedOperatorInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  OverloadedOperator kind = OverloadedOperator::Dereference;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
  std::optional<CallableBoundary> boundary;
};

enum class SemanticBindingKind {
  None,
  GlobalVariable,
  LocalVariable,
  Parameter,
  Field,
  StaticField,
  LambdaCapture,
};

enum class SemanticCompletionKind {
  Unqualified,
  Namespace,
  Enum,
  Member,
};

enum class SemanticCompletionCandidateKind {
  Namespace,
  TypeAlias,
  Class,
  Struct,
  Enum,
  Enumerator,
  Function,
  Method,
  Field,
  GlobalVariable,
  LocalVariable,
  Parameter,
  TypeParameter,
  ValueParameter,
};

struct SemanticCompletionCandidateRecord {
  SemanticCompletionCandidateKind kind =
      SemanticCompletionCandidateKind::LocalVariable;
  std::string name;
  std::string qualifiedName;
  std::string detail;
  SemanticType type = SemanticType::Unknown;
  bool mutableBinding = false;
  std::size_t scopeDistance = 0;
  FunctionId function = 0;
  ClassId classType = 0;
  EnumId enumType = 0;
  const TypeAliasDecl *typeAlias = nullptr;
  std::vector<SemanticType> parameterTypes;
  std::vector<AppliedConceptRequirement> requirements;
  bool substitutedCallable = false;
  bool staticMember = false;
  SymbolId symbol = 0;
  bool compilerPrivate = false;
};

struct SemanticCompletionContext {
  SemanticCompletionKind kind = SemanticCompletionKind::Unqualified;
  SourceUnitId sourceUnit = 0;
  SourceSpan replacementRange;
  std::string prefix;
  std::vector<SemanticCompletionCandidateRecord> candidates;
};

enum class SemanticOccurrenceKind {
  Expression,
  Binding,
  Symbol,
  InferredType,
  Function,
  ClassType,
  EnumType,
  TypeAlias,
  Constructor,
  Destructor,
  SelectedCall,
  SelectedConstruction,
};

enum class SymbolKind {
  Namespace,
  NamespaceAlias,
  Concept,
  TypeAlias,
  Class,
  Struct,
  Enum,
  Enumerator,
  Constructor,
  Destructor,
  Function,
  Method,
  Operator,
  Field,
  GlobalVariable,
  LocalVariable,
  Parameter,
  TypeParameter,
  ValueParameter,
  LambdaCapture,
};

enum class OccurrenceRole : std::uint32_t {
  None = 0,
  Declaration = 1U << 0U,
  Definition = 1U << 1U,
  Reference = 1U << 2U,
  Read = 1U << 3U,
  Write = 1U << 4U,
  Call = 1U << 5U,
  TypeUse = 1U << 6U,
};

[[nodiscard]] constexpr OccurrenceRole operator|(OccurrenceRole left,
                                                 OccurrenceRole right) {
  return static_cast<OccurrenceRole>(static_cast<std::uint32_t>(left) |
                                     static_cast<std::uint32_t>(right));
}

constexpr OccurrenceRole &operator|=(OccurrenceRole &left,
                                     OccurrenceRole right) {
  left = left | right;
  return left;
}

[[nodiscard]] constexpr bool hasRole(OccurrenceRole roles,
                                     OccurrenceRole role) {
  return (static_cast<std::uint32_t>(roles) &
          static_cast<std::uint32_t>(role)) != 0;
}

struct SymbolRecord {
  SymbolId id = 0;
  SymbolKind kind = SymbolKind::LocalVariable;
  std::string name;
  std::string qualifiedName;
  SourceUnitId sourceUnit = 0;
  SourceSpan nameSpan;
  SourceSpan declarationSpan;
  std::optional<SourceSpan> definitionSpan;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits{};
  AccessModifier access = AccessModifier::Public;
  bool mutableBinding = false;
  bool defaultLibrary = false;
  bool staticMember = false;
  bool internalLinkage = false;
  bool generated = false;
  bool compilerPrivate = false;
};

struct SemanticOccurrence {
  SourceUnitId sourceUnit = 0;
  SourceSpan span;
  SemanticOccurrenceKind kind = SemanticOccurrenceKind::Expression;
  SymbolId symbol = 0;
  OccurrenceRole roles = OccurrenceRole::None;
  std::string name;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits{};
  AccessMode access = AccessMode::ReadOnly;
  bool mutableBinding = false;
  SemanticBindingKind bindingKind = SemanticBindingKind::None;
  const FunctionDecl *function = nullptr;
  const ClassDecl *classType = nullptr;
  const EnumDecl *enumType = nullptr;
  const TypeAliasDecl *typeAlias = nullptr;
  const ConstructorDecl *constructor = nullptr;
  const DestructorDecl *destructor = nullptr;
  std::optional<ResolvedCallInfo> selectedCall;
  std::optional<ResolvedConstructionInfo> selectedConstruction;
  bool staticMember = false;
};

class SemanticDatabase {
public:
  [[nodiscard]] const std::vector<SymbolRecord> &symbols() const {
    return symbolRecords;
  }

  [[nodiscard]] const SymbolRecord *findSymbol(SymbolId id) const {
    if (id == 0) {
      return nullptr;
    }
    if (base != nullptr && id <= baseSymbolCount) {
      return base->findSymbol(id);
    }
    const SymbolId local = id - baseSymbolCount;
    return local > symbolRecords.size() ? nullptr : &symbolRecords[local - 1];
  }

  [[nodiscard]] std::vector<const SemanticOccurrence *>
  occurrencesForSymbol(SymbolId id) const {
    std::vector<const SemanticOccurrence *> result;
    if (id == 0) {
      return result;
    }
    for (const auto &[_, unitOccurrences] : occurrencesByUnit) {
      for (const SemanticOccurrence &occurrence : unitOccurrences) {
        if (occurrence.symbol == id) {
          result.push_back(&occurrence);
        }
      }
    }
    return result;
  }

  [[nodiscard]] const std::vector<SemanticOccurrence> &
  occurrences(SourceUnitId sourceUnit) const {
    static const std::vector<SemanticOccurrence> empty;
    const auto found = occurrencesByUnit.find(sourceUnit);
    return found == occurrencesByUnit.end() ? empty : found->second;
  }

  [[nodiscard]] const SemanticOccurrence *
  findOccurrence(SourceUnitId sourceUnit, std::size_t byteOffset) const {
    const std::vector<SemanticOccurrence> &unitOccurrences =
        occurrences(sourceUnit);
    const auto after = std::upper_bound(
        unitOccurrences.begin(), unitOccurrences.end(), byteOffset,
        [](std::size_t offset, const SemanticOccurrence &occurrence) {
          return offset < occurrence.span.start;
        });
    if (after == unitOccurrences.begin()) {
      return nullptr;
    }

    auto current = after;
    --current;
    const std::size_t candidateStart = current->span.start;
    const SemanticOccurrence *best = nullptr;
    while (true) {
      if (current->span.start != candidateStart) {
        break;
      }
      if (current->span.start <= byteOffset && byteOffset < current->span.end &&
          (best == nullptr || priority(current->kind) > priority(best->kind))) {
        best = &*current;
      }
      if (current == unitOccurrences.begin()) {
        break;
      }
      --current;
    }
    return best;
  }

  [[nodiscard]] const SymbolRecord *findSymbolAt(SourceUnitId sourceUnit,
                                                 std::size_t byteOffset) const {
    const SemanticOccurrence *occurrence =
        findOccurrence(sourceUnit, byteOffset);
    return occurrence == nullptr ? nullptr : findSymbol(occurrence->symbol);
  }

private:
  friend class SemanticModel;

  struct DeclarationKey {
    SourceUnitId sourceUnit = 0;
    std::size_t start = 0;
    std::size_t end = 0;
    std::string generatedName;

    friend bool operator==(const DeclarationKey &,
                           const DeclarationKey &) = default;
  };

  struct DeclarationKeyHash {
    std::size_t operator()(const DeclarationKey &key) const {
      std::size_t result = std::hash<std::size_t>{}(key.sourceUnit);
      result ^= std::hash<std::size_t>{}(key.start) + 0x9e3779b9U +
                (result << 6U) + (result >> 2U);
      result ^= std::hash<std::size_t>{}(key.end) + 0x9e3779b9U +
                (result << 6U) + (result >> 2U);
      result ^= std::hash<std::string>{}(key.generatedName) + 0x9e3779b9U +
                (result << 6U) + (result >> 2U);
      return result;
    }
  };

  static int priority(SemanticOccurrenceKind kind) {
    switch (kind) {
    case SemanticOccurrenceKind::SelectedCall:
    case SemanticOccurrenceKind::SelectedConstruction:
      return 4;
    case SemanticOccurrenceKind::Function:
    case SemanticOccurrenceKind::Symbol:
    case SemanticOccurrenceKind::ClassType:
    case SemanticOccurrenceKind::EnumType:
    case SemanticOccurrenceKind::TypeAlias:
    case SemanticOccurrenceKind::Constructor:
    case SemanticOccurrenceKind::Destructor:
      return 3;
    case SemanticOccurrenceKind::Binding:
    case SemanticOccurrenceKind::InferredType:
      return 2;
    case SemanticOccurrenceKind::Expression:
      return 1;
    }
    return 0;
  }

  void clear() {
    symbolRecords.clear();
    symbolsByDeclaration.clear();
    occurrencesByUnit.clear();
    base = nullptr;
    baseSymbolCount = 0;
  }

  // Turns this database into an instance-analysis delta over baseDatabase:
  // lookups fall back to the base, and new symbol identities continue after
  // the base's so instance records never collide with base SymbolIds.
  void beginInstanceDelta(const SemanticDatabase &baseDatabase) {
    clear();
    base = &baseDatabase;
    baseSymbolCount = baseDatabase.symbolRecords.size();
  }

  void rebase(const SemanticDatabase *baseDatabase) { base = baseDatabase; }

  SymbolId recordSymbol(SymbolRecord symbol) {
    if (symbol.sourceUnit == 0 ||
        symbol.nameSpan.end <= symbol.nameSpan.start) {
      return 0;
    }
    const DeclarationKey key{symbol.sourceUnit, symbol.nameSpan.start,
                             symbol.nameSpan.end,
                             symbol.generated ? symbol.name : std::string{}};
    if (const auto found = symbolsByDeclaration.find(key);
        found != symbolsByDeclaration.end()) {
      SymbolRecord &existing = symbolRecords[found->second - 1];
      if (existing.qualifiedName.empty()) {
        existing.qualifiedName = std::move(symbol.qualifiedName);
      }
      if (existing.type == SemanticType::Unknown &&
          symbol.type != SemanticType::Unknown) {
        existing.type = std::move(symbol.type);
        existing.traits = symbol.traits;
      }
      if (symbol.definitionSpan) {
        existing.definitionSpan = std::move(symbol.definitionSpan);
      }
      existing.access = symbol.access;
      existing.mutableBinding = symbol.mutableBinding;
      existing.defaultLibrary =
          existing.defaultLibrary || symbol.defaultLibrary;
      existing.staticMember = existing.staticMember || symbol.staticMember;
      existing.internalLinkage =
          existing.internalLinkage || symbol.internalLinkage;
      existing.compilerPrivate =
          existing.compilerPrivate || symbol.compilerPrivate;
      return found->second;
    }

    if (base != nullptr) {
      if (const auto inherited = base->symbolsByDeclaration.find(key);
          inherited != base->symbolsByDeclaration.end()) {
        // The declaration already has a base identity; instance analysis
        // reuses it and skips base-record enrichment (the delta is
        // discarded after lowering, so enrichment would be invisible).
        return inherited->second;
      }
    }

    symbol.id = baseSymbolCount + symbolRecords.size() + 1;
    const SymbolId id = symbol.id;
    symbolRecords.emplace_back(std::move(symbol));
    symbolsByDeclaration.emplace(key, id);
    return id;
  }

  [[nodiscard]] SymbolId
  symbolForDeclaration(SourceUnitId sourceUnit, const SourceSpan &span,
                       std::string_view generatedName = {}) const {
    const auto found = symbolsByDeclaration.find(DeclarationKey{
        sourceUnit, span.start, span.end, std::string(generatedName)});
    if (found != symbolsByDeclaration.end()) {
      return found->second;
    }
    return base == nullptr
               ? 0
               : base->symbolForDeclaration(sourceUnit, span, generatedName);
  }

  void record(SemanticOccurrence occurrence) {
    if (!toolingOccurrences || occurrence.sourceUnit == 0 ||
        occurrence.span.end <= occurrence.span.start) {
      return;
    }
    occurrencesByUnit[occurrence.sourceUnit].push_back(std::move(occurrence));
  }

  void setToolingOccurrencesEnabled(bool enabled) {
    toolingOccurrences = enabled;
  }

  void finalize() {
    for (auto &[_, unitOccurrences] : occurrencesByUnit) {
      std::stable_sort(
          unitOccurrences.begin(), unitOccurrences.end(),
          [](const SemanticOccurrence &left, const SemanticOccurrence &right) {
            if (left.span.start != right.span.start) {
              return left.span.start < right.span.start;
            }
            if (left.span.end != right.span.end) {
              return left.span.end < right.span.end;
            }
            return priority(left.kind) < priority(right.kind);
          });
      std::vector<SemanticOccurrence> compacted;
      compacted.reserve(unitOccurrences.size());
      for (SemanticOccurrence &occurrence : unitOccurrences) {
        if (!compacted.empty() && occurrence.symbol != 0 &&
            compacted.back().span.start == occurrence.span.start &&
            compacted.back().span.end == occurrence.span.end &&
            compacted.back().kind == occurrence.kind &&
            compacted.back().symbol == occurrence.symbol) {
          compacted.back().roles |= occurrence.roles;
          continue;
        }
        compacted.emplace_back(std::move(occurrence));
      }
      unitOccurrences = std::move(compacted);
    }
  }

  std::vector<SymbolRecord> symbolRecords;
  std::unordered_map<DeclarationKey, SymbolId, DeclarationKeyHash>
      symbolsByDeclaration;
  std::unordered_map<SourceUnitId, std::vector<SemanticOccurrence>>
      occurrencesByUnit;
  // Instance-delta base; see beginInstanceDelta.
  const SemanticDatabase *base = nullptr;
  SymbolId baseSymbolCount = 0;
  // Occurrence recording is editor-tooling work; see
  // SemanticModel::setToolingOccurrencesEnabled.
  bool toolingOccurrences = true;
};

[[nodiscard]] inline ExpressionInfo
makeExpressionInfo(SemanticType type,
                   ValueCategory category = ValueCategory::Value,
                   AccessMode access = AccessMode::ReadOnly) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return ExpressionInfo{.type = std::move(type),
                        .category = category,
                        .access = access,
                        .traits = traits};
}

[[nodiscard]] inline BindingInfo
makeBindingInfo(SemanticType type, AccessMode access = AccessMode::ReadOnly) {
  const SemanticTypeTraits traits = semanticTraits(type);
  return BindingInfo{
      .type = std::move(type), .access = access, .traits = traits};
}

using TypeSubstitution = std::unordered_map<GenericParameterId, SemanticType>;
using ValueSubstitution =
    std::unordered_map<GenericParameterId, CompileTimeValue>;

struct GenericSubstitution {
  TypeSubstitution types;
  ValueSubstitution values;
};

using SemanticDiagnostic = Diagnostic;

struct SemanticFullExpression {
  std::size_t order = 0;
  const Stmt *statement = nullptr;
  const ConstructorInitializer *constructorInitializer = nullptr;
  std::vector<const Expr *> roots;
};

class SemanticModel {
public:
  [[nodiscard]] ExecutionProfile executionProfile() const {
    return executionProfile_;
  }

  [[nodiscard]] std::size_t placeSnapshot() const { return placeSnapshot_; }

  // AST identities remain valid while the analyzed Program is alive.
  [[nodiscard]] const ExpressionInfo *
  findExpression(const Expr &expression) const {
    const auto found = expressions.find(&expression);
    if (found != expressions.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findExpression(expression);
  }

  [[nodiscard]] UnsafeOperationKind
  unsafeOperation(const Expr &expression) const {
    const auto found = unsafeOperations.find(&expression);
    if (found != unsafeOperations.end()) {
      return found->second;
    }
    return base == nullptr ? UnsafeOperationKind::None
                           : base->unsafeOperation(expression);
  }

  [[nodiscard]] const PlaceKey *findPlace(const Expr &expression) const {
    const auto found = places.find(&expression);
    if (found != places.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findPlace(expression);
  }

  [[nodiscard]] const OwnershipEvent *
  findOwnershipEvent(const Expr &expression) const {
    const auto found = ownershipEvents.find(&expression);
    if (found != ownershipEvents.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findOwnershipEvent(expression);
  }

  [[nodiscard]] std::optional<LambdaCaptureMode>
  lambdaCaptureMode(SymbolId symbol) const {
    const auto found = lambdaCaptureModes.find(symbol);
    if (found != lambdaCaptureModes.end()) {
      return found->second;
    }
    return base == nullptr ? std::nullopt : base->lambdaCaptureMode(symbol);
  }

  [[nodiscard]] std::size_t placeSelection(const Expr &expression) const {
    const auto found = placeSelections.find(&expression);
    if (found != placeSelections.end()) {
      return found->second;
    }
    return base == nullptr ? 0 : base->placeSelection(expression);
  }

  [[nodiscard]] std::size_t ownershipEventCount() const {
    return ownershipEventOrder.size();
  }

  [[nodiscard]] CompilerCapabilityTypeKind
  compilerCapabilityType(const TypeRef &type) const {
    const auto found = compilerCapabilityTypes.find(&type);
    if (found != compilerCapabilityTypes.end()) {
      return found->second;
    }
    return base == nullptr ? CompilerCapabilityTypeKind::None
                           : base->compilerCapabilityType(type);
  }

  [[nodiscard]] bool isCompilerPrivateType(const SemanticType &type) const {
    switch (type.kind) {
    case SemanticType::UniqueOwner:
    case SemanticType::Storage:
      return true;
    case SemanticType::Class: {
      const ClassTypeInfo *info = findClassType(type.classId);
      if (info != nullptr && info->compilerPrivate) {
        return true;
      }
      break;
    }
    case SemanticType::Enum: {
      const EnumTypeInfo *info = findEnumType(type.enumId);
      if (info != nullptr && info->compilerPrivate) {
        return true;
      }
      break;
    }
    default:
      break;
    }
    return std::any_of(type.arguments.begin(), type.arguments.end(),
                       [this](const SemanticType &argument) {
                         return isCompilerPrivateType(argument);
                       });
  }

  [[nodiscard]] bool canPresent(SourceUnitId requester,
                                const SymbolRecord &symbol,
                                const SourceGraph &sourceGraph) const {
    return sourceGraph.isCompilerTrusted(requester) ||
           (!symbol.compilerPrivate && !isCompilerPrivateType(symbol.type));
  }

  [[nodiscard]] bool canPresent(SourceUnitId requester,
                                const SemanticOccurrence &occurrence,
                                const SourceGraph &sourceGraph) const {
    if (sourceGraph.isCompilerTrusted(requester)) {
      return true;
    }
    if (isCompilerPrivateType(occurrence.type)) {
      return false;
    }
    const SymbolRecord *symbol = semanticDatabase.findSymbol(occurrence.symbol);
    return symbol == nullptr || canPresent(requester, *symbol, sourceGraph);
  }

  [[nodiscard]] ExpressionInfo expressionInfo(const Expr &expression) const {
    const ExpressionInfo *info = findExpression(expression);
    return info == nullptr ? makeExpressionInfo(SemanticType::Unknown) : *info;
  }

  [[nodiscard]] const SemanticType *findType(const Expr &expression) const {
    const ExpressionInfo *info = findExpression(expression);
    return info == nullptr ? nullptr : &info->type;
  }

  [[nodiscard]] std::optional<ConstantValue>
  findConstant(const Expr &expression) const {
    const auto found = constants.find(&expression);
    if (found != constants.end()) {
      return found->second;
    }
    return base == nullptr ? std::nullopt : base->findConstant(expression);
  }

  [[nodiscard]] const CompileTimeValue *
  findArrayExtent(const ArrayExtentExpr &extent) const {
    const auto found = arrayExtents.find(&extent);
    if (found != arrayExtents.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findArrayExtent(extent);
  }

  [[nodiscard]] SemanticType typeOf(const Expr &expression) const {
    const SemanticType *type = findType(expression);
    return type == nullptr ? SemanticType::Unknown : *type;
  }

  [[nodiscard]] bool hasType(const Expr &expression) const {
    return expressions.contains(&expression) ||
           (base != nullptr && base->hasType(expression));
  }

  [[nodiscard]] std::size_t expressionCount() const {
    return expressions.size() + (base == nullptr ? 0 : base->expressionCount());
  }

  [[nodiscard]] const std::vector<SemanticFullExpression> &
  fullExpressionsFor(const Stmt &statement) const {
    const auto found = statementFullExpressions.find(&statement);
    if (found != statementFullExpressions.end()) {
      return found->second;
    }
    if (base != nullptr) {
      return base->fullExpressionsFor(statement);
    }
    static const std::vector<SemanticFullExpression> empty;
    return empty;
  }

  [[nodiscard]] const std::vector<SemanticFullExpression> &
  fullExpressionsFor(const ConstructorInitializer &initializer) const {
    const auto found = constructorFullExpressions.find(&initializer);
    if (found != constructorFullExpressions.end()) {
      return found->second;
    }
    if (base != nullptr) {
      return base->fullExpressionsFor(initializer);
    }
    static const std::vector<SemanticFullExpression> empty;
    return empty;
  }

  [[nodiscard]] const std::vector<SemanticFullExpression> &
  fullExpressions() const {
    return fullExpressionOrder;
  }

  [[nodiscard]] const BindingInfo *
  findBinding(const VariableDecl &declaration) const {
    const auto found = variableBindings.find(&declaration);
    if (found != variableBindings.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findBinding(declaration);
  }

  [[nodiscard]] const BindingInfo *
  findBinding(const Parameter &parameter) const {
    const auto found = parameterBindings.find(&parameter);
    if (found != parameterBindings.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findBinding(parameter);
  }

  [[nodiscard]] std::size_t bindingCount() const {
    return variableBindings.size() + parameterBindings.size() +
           payloadBindings.size() +
           (base == nullptr ? 0 : base->bindingCount());
  }

  [[nodiscard]] const BindingInfo *findPayloadBinding(const Token &name) const {
    const auto found = payloadBindings.find(&name);
    if (found != payloadBindings.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findPayloadBinding(name);
  }

  [[nodiscard]] const SemanticLoanInfo *findLoan(SemanticLoanId id) const {
    if (id == 0 || id > retainedLoans.size()) {
      return nullptr;
    }
    const SemanticLoanInfo &loan = retainedLoans[id - 1];
    return loan.id == id ? &loan : nullptr;
  }

  [[nodiscard]] const std::vector<SemanticLoanInfo> &loans() const {
    return retainedLoans;
  }

  [[nodiscard]] std::vector<SemanticLoanId>
  loansEndingAfter(const Stmt &statement) const {
    const auto found = loanEnds.find(&statement);
    return found == loanEnds.end() ? std::vector<SemanticLoanId>{}
                                   : found->second;
  }

  [[nodiscard]] std::vector<SemanticLoanId>
  loansEndingAtConditionalEntry(const IfStmt &statement,
                                bool thenBranch) const {
    const auto found = conditionalLoanEnds.find(&statement);
    if (found == conditionalLoanEnds.end()) {
      return {};
    }
    return thenBranch ? found->second.thenEntry : found->second.elseEntry;
  }

  [[nodiscard]] std::optional<bool>
  findConstexprBranch(const IfStmt &statement) const {
    const auto found = constexprBranches.find(&statement);
    if (found != constexprBranches.end()) {
      return found->second;
    }
    return base == nullptr ? std::nullopt
                           : base->findConstexprBranch(statement);
  }

  [[nodiscard]] std::vector<SemanticLoanId>
  loansEndingAtSwitchArmEntry(const SwitchStmt &statement,
                              std::size_t armIndex) const {
    const auto found = switchArmLoanEnds.find(&statement);
    if (found == switchArmLoanEnds.end() || armIndex >= found->second.size()) {
      return {};
    }
    return found->second[armIndex];
  }

  [[nodiscard]] const StructuredBindingInfo *
  findStructuredBinding(const StructuredBindingDecl &declaration) const {
    const auto found = structuredBindings.find(&declaration);
    if (found != structuredBindings.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findStructuredBinding(declaration);
  }

  [[nodiscard]] const FunctionInfo *
  findFunction(const FunctionDecl &declaration) const {
    const auto found = functions.find(&declaration);
    if (found != functions.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findFunction(declaration);
  }

  [[nodiscard]] const FunctionInfo *findFunction(FunctionId id) const {
    const auto found = functionsById.find(id);
    if (found != functionsById.end() && found->second != nullptr) {
      return findFunction(*found->second);
    }
    return base == nullptr ? nullptr : base->findFunction(id);
  }

  [[nodiscard]] const LambdaInfo *findLambda(const Lambda &declaration) const {
    const auto found = lambdas.find(&declaration);
    if (found != lambdas.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findLambda(declaration);
  }

  [[nodiscard]] const LambdaInfo *findLambda(LambdaId id) const {
    const auto found = lambdasById.find(id);
    if (found != lambdasById.end() && found->second != nullptr) {
      return findLambda(*found->second);
    }
    return base == nullptr ? nullptr : base->findLambda(id);
  }

  [[nodiscard]] const ClassTypeInfo *
  findClassType(const ClassDecl &declaration) const {
    const auto found = classTypes.find(&declaration);
    if (found != classTypes.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findClassType(declaration);
  }

  [[nodiscard]] const ClassTypeInfo *findClassType(ClassId id) const {
    const auto found = classTypesById.find(id);
    if (found != classTypesById.end() && found->second != nullptr) {
      return findClassType(*found->second);
    }
    return base == nullptr ? nullptr : base->findClassType(id);
  }

  [[nodiscard]] const TypeAliasInfo *
  findTypeAlias(const TypeAliasDecl &declaration) const {
    const auto found = typeAliases.find(&declaration);
    if (found != typeAliases.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findTypeAlias(declaration);
  }

  [[nodiscard]] const EnumTypeInfo *
  findEnumType(const EnumDecl &declaration) const {
    const auto found = enumTypes.find(&declaration);
    if (found != enumTypes.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findEnumType(declaration);
  }

  [[nodiscard]] const EnumTypeInfo *findEnumType(EnumId id) const {
    const auto found = enumTypesById.find(id);
    if (found != enumTypesById.end() && found->second != nullptr) {
      return findEnumType(*found->second);
    }
    return base == nullptr ? nullptr : base->findEnumType(id);
  }

  [[nodiscard]] const ResolvedEnumeratorInfo *
  findEnumerator(const QualifiedName &expression) const {
    const auto found = enumerators.find(&expression);
    if (found != enumerators.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findEnumerator(expression);
  }

  [[nodiscard]] const SwitchCaseValue *
  findSwitchCase(const Expr &expression) const {
    const auto found = switchCases.find(&expression);
    if (found != switchCases.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findSwitchCase(expression);
  }

  [[nodiscard]] bool isExhaustiveSwitch(const SwitchStmt &statement) const {
    return exhaustiveSwitches.contains(&statement) ||
           (base != nullptr && base->isExhaustiveSwitch(statement));
  }

  [[nodiscard]] const ResolvedPayloadConstructionInfo *
  findPayloadConstruction(const Call &call) const {
    const auto found = payloadConstructions.find(&call);
    if (found != payloadConstructions.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findPayloadConstruction(call);
  }

  [[nodiscard]] const ResolvedPayloadPatternInfo *
  findPayloadPattern(const Expr &expression) const {
    const auto found = payloadPatterns.find(&expression);
    if (found != payloadPatterns.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findPayloadPattern(expression);
  }

  [[nodiscard]] const ResolvedCallInfo *findCall(const Call &call) const {
    const auto found = calls.find(&call);
    if (found != calls.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findCall(call);
  }

  [[nodiscard]] const ResolvedLambdaCallInfo *
  findLambdaCall(const Call &call) const {
    const auto found = lambdaCalls.find(&call);
    if (found != lambdaCalls.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findLambdaCall(call);
  }

  [[nodiscard]] const DeferredCallableCallInfo *
  findDeferredCallableCall(const Call &call) const {
    const auto found = deferredCallableCalls.find(&call);
    if (found != deferredCallableCalls.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findDeferredCallableCall(call);
  }

  [[nodiscard]] const ResolvedOperatorInfo *
  findOperator(const Expr &expression) const {
    const auto found = operators.find(&expression);
    if (found != operators.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findOperator(expression);
  }

  [[nodiscard]] const ResolvedOperatorInfo *
  findContextualConversion(const Expr &expression) const {
    const auto found = contextualConversions.find(&expression);
    if (found != contextualConversions.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr
                           : base->findContextualConversion(expression);
  }

  [[nodiscard]] bool isContextualIntegerOperand(const Expr &expression) const {
    if (contextualIntegerOperands.contains(&expression)) {
      return true;
    }
    return base != nullptr && base->isContextualIntegerOperand(expression);
  }

  [[nodiscard]] const ClassLifecycleInfo *
  findClassLifecycle(const ClassDecl &declaration) const {
    const auto found = classLifecycles.find(&declaration);
    if (found != classLifecycles.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findClassLifecycle(declaration);
  }

  [[nodiscard]] const ResolvedConstructionInfo *
  findConstruction(const Expr &expression) const {
    const auto found = constructions.find(&expression);
    if (found != constructions.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr : base->findConstruction(expression);
  }

  [[nodiscard]] const ResolvedConstructorInitializerInfo *
  findConstructorInitializer(const ConstructorInitializer &initializer) const {
    const auto found = constructorInitializers.find(&initializer);
    if (found != constructorInitializers.end()) {
      return &found->second;
    }
    return base == nullptr ? nullptr
                           : base->findConstructorInitializer(initializer);
  }

  [[nodiscard]] SymbolId findResolvedSymbol(const Expr &expression) const {
    const auto found = resolvedSymbols.find(&expression);
    if (found != resolvedSymbols.end()) {
      return found->second;
    }
    return base == nullptr ? 0 : base->findResolvedSymbol(expression);
  }

  [[nodiscard]] std::size_t functionCount() const {
    return functions.size() + (base == nullptr ? 0 : base->functionCount());
  }

  [[nodiscard]] std::size_t lambdaCount() const {
    return lambdas.size() + (base == nullptr ? 0 : base->lambdaCount());
  }

  [[nodiscard]] std::size_t resolvedCallCount() const {
    return calls.size() + (base == nullptr ? 0 : base->resolvedCallCount());
  }

  [[nodiscard]] std::size_t classLifecycleCount() const {
    return classLifecycles.size() +
           (base == nullptr ? 0 : base->classLifecycleCount());
  }

  [[nodiscard]] std::size_t resolvedConstructionCount() const {
    return constructions.size() +
           (base == nullptr ? 0 : base->resolvedConstructionCount());
  }

  [[nodiscard]] const SemanticDatabase &database() const {
    return semanticDatabase;
  }

  [[nodiscard]] const std::optional<SemanticCompletionContext> &
  completionContext() const {
    return completion;
  }

  [[nodiscard]] const GenericParameterInfo *
  findGenericParameter(GenericParameterId id) const {
    const auto find =
        [id](const auto &records) -> const GenericParameterInfo * {
      for (const auto &[_, record] : records) {
        const auto parameter = std::find_if(
            record.genericParameters.begin(), record.genericParameters.end(),
            [id](const GenericParameterInfo &candidate) {
              return candidate.id == id;
            });
        if (parameter != record.genericParameters.end()) {
          return &*parameter;
        }
      }
      return nullptr;
    };
    if (const GenericParameterInfo *parameter = find(functions)) {
      return parameter;
    }
    if (const GenericParameterInfo *parameter = find(classTypes)) {
      return parameter;
    }
    for (const auto &[_, lifecycle] : classLifecycles) {
      for (const ConstructorInfo &constructor : lifecycle.constructors) {
        const auto parameter =
            std::find_if(constructor.genericParameters.begin(),
                         constructor.genericParameters.end(),
                         [id](const GenericParameterInfo &candidate) {
                           return candidate.id == id;
                         });
        if (parameter != constructor.genericParameters.end()) {
          return &*parameter;
        }
      }
    }
    return base == nullptr ? nullptr : base->findGenericParameter(id);
  }

  [[nodiscard]] const ConstructorInfo *
  findConstructor(const ConstructorDecl &declaration) const {
    for (const auto &[_, lifecycle] : classLifecycles) {
      const auto found = std::find_if(
          lifecycle.constructors.begin(), lifecycle.constructors.end(),
          [&declaration](const ConstructorInfo &candidate) {
            return candidate.declaration == &declaration;
          });
      if (found != lifecycle.constructors.end()) {
        return &*found;
      }
      if (lifecycle.declaredCopyConstructor &&
          lifecycle.declaredCopyConstructor->declaration == &declaration) {
        return &*lifecycle.declaredCopyConstructor;
      }
      if (lifecycle.declaredMoveConstructor &&
          lifecycle.declaredMoveConstructor->declaration == &declaration) {
        return &*lifecycle.declaredMoveConstructor;
      }
    }
    return base == nullptr ? nullptr : base->findConstructor(declaration);
  }

  [[nodiscard]] const DestructorInfo *
  findDestructor(const DestructorDecl &declaration) const {
    for (const auto &[_, lifecycle] : classLifecycles) {
      if (lifecycle.declaredDestructor &&
          lifecycle.declaredDestructor->declaration == &declaration) {
        return &*lifecycle.declaredDestructor;
      }
    }
    return base == nullptr ? nullptr : base->findDestructor(declaration);
  }

private:
  friend class SemanticVisitor;

  struct PendingCallableForwarding {
    const FunctionDecl *source = nullptr;
    std::size_t sourceParameterIndex = 0;
    GenericParameterId sourceGenericParameter = 0;
    SemanticType sourceType = SemanticType::Unknown;
    AccessMode sourceAccess = AccessMode::ReadOnly;
    const Call *call = nullptr;
    FunctionId target = 0;
    std::size_t targetParameterIndex = 0;
  };

  void clear() {
    expressions.clear();
    statementFullExpressions.clear();
    constructorFullExpressions.clear();
    fullExpressionOrder.clear();
    constants.clear();
    unsafeOperations.clear();
    places.clear();
    ownershipEvents.clear();
    ownershipEventOrder.clear();
    placeSelections.clear();
    compilerCapabilityTypes.clear();
    arrayExtents.clear();
    variableBindings.clear();
    parameterBindings.clear();
    payloadBindings.clear();
    retainedLoans.clear();
    loanEnds.clear();
    conditionalLoanEnds.clear();
    constexprBranches.clear();
    switchArmLoanEnds.clear();
    structuredBindings.clear();
    functions.clear();
    functionsById.clear();
    lambdas.clear();
    lambdasById.clear();
    classTypes.clear();
    classTypesById.clear();
    typeAliases.clear();
    enumTypes.clear();
    enumTypesById.clear();
    enumerators.clear();
    switchCases.clear();
    exhaustiveSwitches.clear();
    payloadConstructions.clear();
    payloadPatterns.clear();
    calls.clear();
    lambdaCalls.clear();
    deferredCallableCalls.clear();
    pendingCallableForwardings.clear();
    operators.clear();
    contextualConversions.clear();
    contextualIntegerOperands.clear();
    classLifecycles.clear();
    constructions.clear();
    constructorInitializers.clear();
    resolvedSymbols.clear();
    semanticDatabase.clear();
    completion.reset();
    executionProfile_ = ExecutionProfile::SingleThreaded;
    placeSnapshot_ = 0;
    base = nullptr;
  }

  void setExecutionProfile(ExecutionProfile profile) {
    executionProfile_ = profile;
  }

  void setPlaceSnapshot(std::size_t snapshot) { placeSnapshot_ = snapshot; }

  // Turns this model into an instance-analysis delta over baseModel: reads
  // fall back to the base while writes stay local, so concrete instance
  // reanalysis records only what it produces instead of copying the whole
  // program's model. Loan tables deliberately do not fall back - instance
  // analysis restarts loan identities, mirroring the clearLoans() semantics
  // the previous whole-model copy relied on.
  void beginInstanceDelta(const SemanticModel &baseModel) {
    clear();
    executionProfile_ = baseModel.executionProfile();
    placeSnapshot_ = baseModel.placeSnapshot();
    base = &baseModel;
    semanticDatabase.beginInstanceDelta(baseModel.semanticDatabase);
  }

  // Re-points an instance delta at a relocated base (the analyzer restores
  // its model after each instance analysis; the delta must follow it).
  void rebase(const SemanticModel *baseModel) {
    base = baseModel;
    semanticDatabase.rebase(
        baseModel == nullptr ? nullptr : &baseModel->semanticDatabase);
  }

  // Copies a base function record into the delta so record mutators can
  // update it locally. Returns the local entry, or functions.end() when the
  // declaration is unknown to both the delta and the base.
  [[nodiscard]] std::unordered_map<const FunctionDecl *, FunctionInfo>::iterator
  materializeFunction(const FunctionDecl &declaration) {
    auto local = functions.find(&declaration);
    if (local != functions.end() || base == nullptr) {
      return local;
    }
    const FunctionInfo *inherited = base->findFunction(declaration);
    if (inherited == nullptr) {
      return functions.end();
    }
    local = functions.insert_or_assign(&declaration, *inherited).first;
    functionsById.insert_or_assign(local->second.id, &declaration);
    return local;
  }

  [[nodiscard]] bool validLoan(SemanticLoanId id) const {
    return id != 0 && id <= retainedLoans.size() &&
           retainedLoans[id - 1].id == id;
  }

  static void appendUniqueLoan(std::vector<SemanticLoanId> &loans,
                               SemanticLoanId loan) {
    if (std::find(loans.begin(), loans.end(), loan) == loans.end()) {
      loans.push_back(loan);
    }
  }

  void recordLoanEndpoint(SemanticLoanId id, SemanticLoanEndKind kind,
                          const Stmt &statement, std::size_t switchArm = 0) {
    std::vector<SemanticLoanEndpoint> &endpoints =
        retainedLoans[id - 1].endpoints;
    const auto duplicate =
        std::find_if(endpoints.begin(), endpoints.end(),
                     [&](const SemanticLoanEndpoint &endpoint) {
                       return endpoint.kind == kind &&
                              endpoint.statement == &statement &&
                              (kind != SemanticLoanEndKind::SwitchArmEntry ||
                               endpoint.switchArm == switchArm);
                     });
    if (duplicate == endpoints.end()) {
      endpoints.push_back(
          {.kind = kind, .statement = &statement, .switchArm = switchArm});
    }
  }

  void record(const Expr &expression, ExpressionInfo info) {
    expressions.insert_or_assign(&expression, std::move(info));
  }

  static bool
  appendFullExpression(std::vector<SemanticFullExpression> &expressions,
                       const SemanticFullExpression &expression) {
    if (expression.roots.empty()) {
      return false;
    }
    const auto duplicate =
        std::find_if(expressions.begin(), expressions.end(),
                     [&](const SemanticFullExpression &candidate) {
                       return candidate.roots == expression.roots;
                     });
    if (duplicate == expressions.end()) {
      expressions.push_back(expression);
      return true;
    }
    return false;
  }

  void recordFullExpression(const Stmt &statement, const ExprPtr &root) {
    if (root == nullptr) {
      return;
    }
    SemanticFullExpression expression{.order = fullExpressionOrder.size() + 1,
                                      .statement = &statement,
                                      .roots = {root.get()}};
    if (appendFullExpression(statementFullExpressions[&statement],
                             expression)) {
      fullExpressionOrder.push_back(std::move(expression));
    }
  }

  void recordFullExpression(const ConstructorInitializer &initializer) {
    SemanticFullExpression expression{.order = fullExpressionOrder.size() + 1,
                                      .constructorInitializer = &initializer};
    expression.roots.reserve(initializer.arguments.size());
    for (const ExprPtr &argument : initializer.arguments) {
      if (argument != nullptr) {
        expression.roots.push_back(argument.get());
      }
    }
    if (appendFullExpression(constructorFullExpressions[&initializer],
                             expression)) {
      fullExpressionOrder.push_back(std::move(expression));
    }
  }

  void recordConstant(const Expr &expression, ConstantValue value) {
    constants.insert_or_assign(&expression, std::move(value));
  }

  void recordUnsafeOperation(const Expr &expression,
                             UnsafeOperationKind operation) {
    unsafeOperations.insert_or_assign(&expression, operation);
  }

  void recordPlace(const Expr &expression, PlaceKey place) {
    places.insert_or_assign(&expression, std::move(place));
  }

  void recordOwnershipEvent(const Expr &expression, OwnershipEvent event) {
    if (!ownershipEvents.contains(&expression)) {
      ownershipEventOrder.push_back(&expression);
    }
    ownershipEvents.insert_or_assign(&expression, std::move(event));
  }

  void recordLambdaCaptureMode(SymbolId symbol, LambdaCaptureMode mode) {
    if (symbol != 0) {
      lambdaCaptureModes.insert_or_assign(symbol, mode);
    }
  }

  void markOwnershipEventsUnreachableFrom(std::size_t first) {
    for (std::size_t index = first; index < ownershipEventOrder.size();
         ++index) {
      ownershipEvents[ownershipEventOrder[index]].reachable = false;
    }
  }

  void recordPlaceSelection(const Expr &expression, std::size_t selection) {
    placeSelections.insert_or_assign(&expression, selection);
  }

  void record(const ArrayExtentExpr &extent, CompileTimeValue value) {
    arrayExtents.insert_or_assign(&extent, value);
  }

  void record(const VariableDecl &declaration, BindingInfo info) {
    variableBindings.insert_or_assign(&declaration, std::move(info));
  }

  void record(const Parameter &parameter, BindingInfo info) {
    parameterBindings.insert_or_assign(&parameter, std::move(info));
  }

  void recordPayloadBinding(const Token &name, BindingInfo info) {
    payloadBindings.insert_or_assign(&name, std::move(info));
  }

  void recordLoan(SemanticLoanInfo info) {
    if (info.id == 0) {
      return;
    }
    if (retainedLoans.size() < info.id) {
      retainedLoans.resize(info.id);
    }
    retainedLoans[info.id - 1] = std::move(info);
  }

  void recordBindingLoan(const VariableDecl &declaration, SemanticLoanId loan) {
    auto binding = variableBindings.find(&declaration);
    if (binding == variableBindings.end() && base != nullptr) {
      if (const BindingInfo *inherited = base->findBinding(declaration)) {
        binding =
            variableBindings.insert_or_assign(&declaration, *inherited).first;
      }
    }
    if (binding != variableBindings.end()) {
      binding->second.retainedLoan = loan;
    }
  }

  void recordBindingLoan(const Parameter &parameter, SemanticLoanId loan) {
    if (auto binding = parameterBindings.find(&parameter);
        binding != parameterBindings.end()) {
      binding->second.retainedLoan = loan;
    }
  }

  void recordLoanEndAfter(SemanticLoanId id, const Stmt &statement) {
    if (!validLoan(id)) {
      return;
    }
    recordLoanEndpoint(id, SemanticLoanEndKind::AfterStatement, statement);
    appendUniqueLoan(loanEnds[&statement], id);
  }

  void recordLoanEndAtConditionalEntry(SemanticLoanId id,
                                       const IfStmt &statement,
                                       bool thenBranch) {
    if (!validLoan(id)) {
      return;
    }
    const SemanticLoanEndKind kind = thenBranch
                                         ? SemanticLoanEndKind::ThenBranchEntry
                                         : SemanticLoanEndKind::ElseBranchEntry;
    recordLoanEndpoint(id, kind, statement);
    SemanticConditionalLoanEnds &ends = conditionalLoanEnds[&statement];
    appendUniqueLoan(thenBranch ? ends.thenEntry : ends.elseEntry, id);
  }

  void recordConstexprBranch(const IfStmt &statement, bool thenBranch) {
    constexprBranches.insert_or_assign(&statement, thenBranch);
  }

  void recordLoanEndAtSwitchArmEntry(SemanticLoanId id,
                                     const SwitchStmt &statement,
                                     std::size_t armIndex) {
    if (!validLoan(id)) {
      return;
    }
    recordLoanEndpoint(id, SemanticLoanEndKind::SwitchArmEntry, statement,
                       armIndex);
    std::vector<std::vector<SemanticLoanId>> &ends =
        switchArmLoanEnds[&statement];
    if (ends.size() <= armIndex) {
      ends.resize(armIndex + 1);
    }
    appendUniqueLoan(ends[armIndex], id);
  }

  void recordLoanCarrier(SemanticLoanId id, SymbolId carrier) {
    if (id == 0 || carrier == 0 || id > retainedLoans.size() ||
        retainedLoans[id - 1].id != id) {
      return;
    }
    std::vector<SymbolId> &carriers = retainedLoans[id - 1].carriers;
    if (std::find(carriers.begin(), carriers.end(), carrier) ==
        carriers.end()) {
      carriers.push_back(carrier);
    }
  }

  void clearLoans() {
    retainedLoans.clear();
    loanEnds.clear();
    conditionalLoanEnds.clear();
    switchArmLoanEnds.clear();
    for (auto &[_, binding] : variableBindings) {
      binding.retainedLoan = 0;
    }
    for (auto &[_, binding] : parameterBindings) {
      binding.retainedLoan = 0;
    }
  }

  void record(const StructuredBindingDecl &declaration,
              StructuredBindingInfo info) {
    structuredBindings.insert_or_assign(&declaration, std::move(info));
  }

  void recordExplicitMove(const VariableDecl &declaration) {
    auto binding = variableBindings.find(&declaration);
    if (binding == variableBindings.end() && base != nullptr) {
      if (const BindingInfo *inherited = base->findBinding(declaration)) {
        binding =
            variableBindings.insert_or_assign(&declaration, *inherited).first;
      }
    }
    if (binding != variableBindings.end()) {
      binding->second.explicitlyMoved = true;
    }
  }

  void recordExplicitMove(const Parameter &parameter) {
    auto binding = parameterBindings.find(&parameter);
    if (binding == parameterBindings.end() && base != nullptr) {
      if (const BindingInfo *inherited = base->findBinding(parameter)) {
        binding =
            parameterBindings.insert_or_assign(&parameter, *inherited).first;
      }
    }
    if (binding != parameterBindings.end()) {
      binding->second.explicitlyMoved = true;
    }
  }

  void record(const FunctionDecl &declaration, FunctionInfo info) {
    const auto [found, _] =
        functions.insert_or_assign(&declaration, std::move(info));
    functionsById.insert_or_assign(found->second.id, &declaration);
  }

  void record(const Lambda &declaration, LambdaInfo info) {
    const auto [found, _] =
        lambdas.insert_or_assign(&declaration, std::move(info));
    lambdasById.insert_or_assign(found->second.id, &declaration);
  }

  void recordClassType(const ClassDecl &declaration, ClassTypeInfo info) {
    const auto [found, _] =
        classTypes.insert_or_assign(&declaration, std::move(info));
    classTypesById.insert_or_assign(found->second.id, &declaration);
  }

  void record(const TypeAliasDecl &declaration, TypeAliasInfo info) {
    typeAliases.insert_or_assign(&declaration, std::move(info));
  }

  void recordEnumType(const EnumDecl &declaration, EnumTypeInfo info) {
    const auto [found, _] =
        enumTypes.insert_or_assign(&declaration, std::move(info));
    enumTypesById.insert_or_assign(found->second.id, &declaration);
  }

  void record(const QualifiedName &expression, ResolvedEnumeratorInfo info) {
    enumerators.insert_or_assign(&expression, std::move(info));
  }

  void recordSwitchCase(const Expr &expression, SwitchCaseValue value) {
    switchCases.insert_or_assign(&expression, std::move(value));
  }

  void recordExhaustiveSwitch(const SwitchStmt &statement) {
    exhaustiveSwitches.insert(&statement);
  }

  void recordPayloadConstruction(const Call &call,
                                 ResolvedPayloadConstructionInfo info) {
    payloadConstructions.insert_or_assign(&call, std::move(info));
  }

  void recordPayloadPattern(const Expr &expression,
                            ResolvedPayloadPatternInfo info) {
    payloadPatterns.insert_or_assign(&expression, std::move(info));
  }

  void record(const Call &call, ResolvedCallInfo info) {
    calls.insert_or_assign(&call, std::move(info));
  }

  void recordLambdaCall(const Call &call, ResolvedLambdaCallInfo info) {
    lambdaCalls.insert_or_assign(&call, std::move(info));
  }

  void recordDeferredCallableCall(const Call &call,
                                  DeferredCallableCallInfo info) {
    deferredCallableCalls.insert_or_assign(&call, std::move(info));
  }

  void recordCallableRequirement(const FunctionDecl &declaration,
                                 CallableParameterContract requirement) {
    const auto function = materializeFunction(declaration);
    if (function == functions.end()) {
      return;
    }
    auto existing = std::find_if(
        function->second.callableParameters.begin(),
        function->second.callableParameters.end(),
        [&](const CallableParameterContract &candidate) {
          return candidate.parameterIndex == requirement.parameterIndex;
        });
    if (existing == function->second.callableParameters.end()) {
      function->second.callableParameters.emplace_back(std::move(requirement));
      return;
    }
    if (requirement.boundary == CallableBoundary::Owned &&
        existing->signatures.empty() && existing->forwardings.empty()) {
      existing->boundary = CallableBoundary::Owned;
      existing->ownedTransport = std::move(requirement.ownedTransport);
    }
    for (CallableSignatureRequirement &signature : requirement.signatures) {
      const auto concrete =
          std::find_if(existing->signatures.begin(), existing->signatures.end(),
                       [&](const CallableSignatureRequirement &candidate) {
                         return candidate.source == signature.source;
                       });
      if (concrete == existing->signatures.end()) {
        existing->signatures.emplace_back(std::move(signature));
      } else {
        *concrete = std::move(signature);
      }
    }
    for (CallableForwardingRequirement &forwarding : requirement.forwardings) {
      if (std::none_of(
              existing->forwardings.begin(), existing->forwardings.end(),
              [&](const CallableForwardingRequirement &candidate) {
                return candidate.source == forwarding.source &&
                       candidate.parameterIndex == forwarding.parameterIndex;
              })) {
        existing->forwardings.emplace_back(std::move(forwarding));
      }
    }
  }

  void recordCallableForwarding(const FunctionDecl &source,
                                std::size_t sourceParameterIndex,
                                GenericParameterId sourceGenericParameter,
                                SemanticType sourceType,
                                AccessMode sourceAccess, const Call &call,
                                FunctionId target,
                                std::size_t targetParameterIndex) {
    const auto duplicate = std::find_if(
        pendingCallableForwardings.begin(), pendingCallableForwardings.end(),
        [&](const PendingCallableForwarding &candidate) {
          return candidate.source == &source && candidate.call == &call &&
                 candidate.sourceParameterIndex == sourceParameterIndex &&
                 candidate.targetParameterIndex == targetParameterIndex;
        });
    if (duplicate != pendingCallableForwardings.end()) {
      return;
    }
    pendingCallableForwardings.push_back(
        {.source = &source,
         .sourceParameterIndex = sourceParameterIndex,
         .sourceGenericParameter = sourceGenericParameter,
         .sourceType = std::move(sourceType),
         .sourceAccess = sourceAccess,
         .call = &call,
         .target = target,
         .targetParameterIndex = targetParameterIndex});
  }

  void recordOperator(const Expr &expression, ResolvedOperatorInfo info) {
    operators.insert_or_assign(&expression, std::move(info));
  }

  void recordContextualConversion(const Expr &expression,
                                  ResolvedOperatorInfo info) {
    contextualConversions.insert_or_assign(&expression, std::move(info));
  }

  void recordContextualIntegerOperand(const Expr &expression) {
    contextualIntegerOperands.insert(&expression);
  }

  void record(const ClassDecl &declaration, ClassLifecycleInfo info) {
    classLifecycles.insert_or_assign(&declaration, std::move(info));
  }

  void record(const Expr &expression, ResolvedConstructionInfo info) {
    constructions.insert_or_assign(&expression, std::move(info));
  }

  void record(const ConstructorInitializer &initializer,
              ResolvedConstructorInitializerInfo info) {
    constructorInitializers.insert_or_assign(&initializer, std::move(info));
  }

  void recordResolvedSymbol(const Expr &expression, SymbolId symbol) {
    if (symbol != 0) {
      resolvedSymbols.insert_or_assign(&expression, symbol);
    }
  }

  void recordCompilerCapabilityType(const TypeRef &type,
                                    CompilerCapabilityTypeKind kind) {
    if (kind != CompilerCapabilityTypeKind::None) {
      compilerCapabilityTypes.insert_or_assign(&type, kind);
    }
  }

  SymbolId recordSymbol(SymbolRecord symbol) {
    return semanticDatabase.recordSymbol(std::move(symbol));
  }

  [[nodiscard]] SymbolId
  symbolForDeclaration(SourceUnitId sourceUnit, const SourceSpan &span,
                       std::string_view generatedName = {}) const {
    return semanticDatabase.symbolForDeclaration(sourceUnit, span,
                                                 generatedName);
  }

  void recordOccurrence(SemanticOccurrence occurrence) {
    semanticDatabase.record(std::move(occurrence));
  }

  // Occurrences answer editor position queries (hover, definition, semantic
  // tokens) and are consumed only by language queries and the LSP. Symbols
  // stay recorded either way because HIR and the emitter resolve member
  // identity through them. A compile-only consumer disables occurrences so
  // analysis does not build, sort, and retain a table nothing will read.
  void setToolingOccurrencesEnabled(bool enabled) {
    semanticDatabase.setToolingOccurrencesEnabled(enabled);
  }

  void recordCompletion(SemanticCompletionContext context) {
    completion = std::move(context);
  }

  void finalizeOccurrences() { semanticDatabase.finalize(); }

  void finalizeCallableArguments(ResolvedCallInfo &call) const {
    const FunctionInfo *function = findFunction(call.function);
    if (function == nullptr) {
      return;
    }
    for (const CallableParameterContract &parameter :
         function->callableParameters) {
      if (parameter.parameterIndex < call.parameterTypes.size()) {
        const auto containsLambda = [&](const auto &self,
                                        const SemanticType &type) -> bool {
          return type.kind == SemanticType::Lambda ||
                 std::any_of(type.arguments.begin(), type.arguments.end(),
                             [&](const SemanticType &argument) {
                               return self(self, argument);
                             });
        };
        if (parameter.boundary == CallableBoundary::Owned &&
            !containsLambda(containsLambda,
                            call.parameterTypes[parameter.parameterIndex])) {
          continue;
        }
        const auto existing = std::find_if(
            call.callableArguments.begin(), call.callableArguments.end(),
            [&](const CallableArgumentBoundary &candidate) {
              return candidate.parameterIndex == parameter.parameterIndex;
            });
        if (existing == call.callableArguments.end()) {
          call.callableArguments.push_back(
              {.parameterIndex = parameter.parameterIndex,
               .boundary = parameter.boundary});
        } else {
          // The function contract is authoritative. The provisional lambda
          // marker recorded at a call site must not mask a later, more
          // precise boundary when owned callable transport is implemented.
          existing->boundary = parameter.boundary;
        }
      }
    }
    std::sort(call.callableArguments.begin(), call.callableArguments.end(),
              [](const CallableArgumentBoundary &left,
                 const CallableArgumentBoundary &right) {
                return left.parameterIndex < right.parameterIndex;
              });
    call.callableArguments.erase(
        std::unique(call.callableArguments.begin(),
                    call.callableArguments.end(),
                    [](const CallableArgumentBoundary &left,
                       const CallableArgumentBoundary &right) {
                      return left.parameterIndex == right.parameterIndex;
                    }),
        call.callableArguments.end());
  }

  void finalizeCallableArguments() {
    for (auto &[_, call] : calls) {
      finalizeCallableArguments(call);
    }
    for (auto &[_, occurrences] : semanticDatabase.occurrencesByUnit) {
      for (SemanticOccurrence &occurrence : occurrences) {
        if (occurrence.selectedCall) {
          finalizeCallableArguments(*occurrence.selectedCall);
        }
      }
    }
  }

  void finalizeCallableForwardings() {
    bool changed = true;
    while (changed) {
      changed = false;
      for (const PendingCallableForwarding &forwarding :
           pendingCallableForwardings) {
        const FunctionInfo *target = findFunction(forwarding.target);
        if (target == nullptr) {
          continue;
        }
        const auto targetContract = std::find_if(
            target->callableParameters.begin(),
            target->callableParameters.end(),
            [&](const CallableParameterContract &candidate) {
              return candidate.parameterIndex ==
                         forwarding.targetParameterIndex &&
                     candidate.boundary == CallableBoundary::Confined;
            });
        if (targetContract == target->callableParameters.end()) {
          continue;
        }

        const auto source = forwarding.source == nullptr
                                ? functions.end()
                                : materializeFunction(*forwarding.source);
        if (source == functions.end()) {
          continue;
        }
        auto contract =
            std::find_if(source->second.callableParameters.begin(),
                         source->second.callableParameters.end(),
                         [&](const CallableParameterContract &candidate) {
                           return candidate.parameterIndex ==
                                  forwarding.sourceParameterIndex;
                         });
        if (contract == source->second.callableParameters.end()) {
          source->second.callableParameters.push_back(
              {.parameterIndex = forwarding.sourceParameterIndex,
               .genericParameter = forwarding.sourceGenericParameter,
               .callableType = forwarding.sourceType,
               .access = forwarding.sourceAccess,
               .forwardings = {
                   {.source = forwarding.call,
                    .function = forwarding.target,
                    .parameterIndex = forwarding.targetParameterIndex}}});
          changed = true;
          continue;
        }

        const bool exists = std::any_of(
            contract->forwardings.begin(), contract->forwardings.end(),
            [&](const CallableForwardingRequirement &candidate) {
              return candidate.source == forwarding.call &&
                     candidate.parameterIndex ==
                         forwarding.targetParameterIndex;
            });
        if (!exists) {
          contract->forwardings.push_back(
              {.source = forwarding.call,
               .function = forwarding.target,
               .parameterIndex = forwarding.targetParameterIndex});
          changed = true;
        }
      }
    }
  }

  std::unordered_map<const Expr *, ExpressionInfo> expressions;
  std::unordered_map<const Stmt *, std::vector<SemanticFullExpression>>
      statementFullExpressions;
  std::unordered_map<const ConstructorInitializer *,
                     std::vector<SemanticFullExpression>>
      constructorFullExpressions;
  std::vector<SemanticFullExpression> fullExpressionOrder;
  std::unordered_map<const Expr *, ConstantValue> constants;
  std::unordered_map<const Expr *, UnsafeOperationKind> unsafeOperations;
  std::unordered_map<const Expr *, PlaceKey> places;
  std::unordered_map<const Expr *, OwnershipEvent> ownershipEvents;
  std::unordered_map<SymbolId, LambdaCaptureMode> lambdaCaptureModes;
  std::vector<const Expr *> ownershipEventOrder;
  std::unordered_map<const Expr *, std::size_t> placeSelections;
  std::unordered_map<const TypeRef *, CompilerCapabilityTypeKind>
      compilerCapabilityTypes;
  std::unordered_map<const ArrayExtentExpr *, CompileTimeValue> arrayExtents;
  std::unordered_map<const VariableDecl *, BindingInfo> variableBindings;
  std::unordered_map<const Parameter *, BindingInfo> parameterBindings;
  std::unordered_map<const Token *, BindingInfo> payloadBindings;
  std::vector<SemanticLoanInfo> retainedLoans;
  std::unordered_map<const Stmt *, std::vector<SemanticLoanId>> loanEnds;
  std::unordered_map<const IfStmt *, SemanticConditionalLoanEnds>
      conditionalLoanEnds;
  std::unordered_map<const IfStmt *, bool> constexprBranches;
  std::unordered_map<const SwitchStmt *,
                     std::vector<std::vector<SemanticLoanId>>>
      switchArmLoanEnds;
  std::unordered_map<const StructuredBindingDecl *, StructuredBindingInfo>
      structuredBindings;
  std::unordered_map<const FunctionDecl *, FunctionInfo> functions;
  std::unordered_map<FunctionId, const FunctionDecl *> functionsById;
  std::unordered_map<const Lambda *, LambdaInfo> lambdas;
  std::unordered_map<LambdaId, const Lambda *> lambdasById;
  std::unordered_map<const ClassDecl *, ClassTypeInfo> classTypes;
  std::unordered_map<ClassId, const ClassDecl *> classTypesById;
  std::unordered_map<const TypeAliasDecl *, TypeAliasInfo> typeAliases;
  std::unordered_map<const EnumDecl *, EnumTypeInfo> enumTypes;
  std::unordered_map<EnumId, const EnumDecl *> enumTypesById;
  std::unordered_map<const QualifiedName *, ResolvedEnumeratorInfo> enumerators;
  std::unordered_map<const Expr *, SwitchCaseValue> switchCases;
  std::unordered_set<const SwitchStmt *> exhaustiveSwitches;
  std::unordered_map<const Call *, ResolvedPayloadConstructionInfo>
      payloadConstructions;
  std::unordered_map<const Expr *, ResolvedPayloadPatternInfo> payloadPatterns;
  std::unordered_map<const Call *, ResolvedCallInfo> calls;
  std::unordered_map<const Call *, ResolvedLambdaCallInfo> lambdaCalls;
  std::unordered_map<const Call *, DeferredCallableCallInfo>
      deferredCallableCalls;
  std::vector<PendingCallableForwarding> pendingCallableForwardings;
  std::unordered_map<const Expr *, ResolvedOperatorInfo> operators;
  std::unordered_map<const Expr *, ResolvedOperatorInfo> contextualConversions;
  std::unordered_set<const Expr *> contextualIntegerOperands;
  std::unordered_map<const ClassDecl *, ClassLifecycleInfo> classLifecycles;
  std::unordered_map<const Expr *, ResolvedConstructionInfo> constructions;
  std::unordered_map<const ConstructorInitializer *,
                     ResolvedConstructorInitializerInfo>
      constructorInitializers;
  std::unordered_map<const Expr *, SymbolId> resolvedSymbols;
  SemanticDatabase semanticDatabase;
  std::optional<SemanticCompletionContext> completion;
  ExecutionProfile executionProfile_ = ExecutionProfile::SingleThreaded;
  std::size_t placeSnapshot_ = 0;
  // Instance-delta base; see beginInstanceDelta.
  const SemanticModel *base = nullptr;
};

class SemanticTypePrinter {
public:
  explicit SemanticTypePrinter(const SemanticModel &semantics)
      : semantics(semantics) {}

  [[nodiscard]] std::string print(const SemanticType &type) const {
    switch (type.kind) {
    case SemanticType::Unknown:
      return "unknown";
    case SemanticType::Void:
      return "void";
    case SemanticType::Int8:
      return "int8_t";
    case SemanticType::Int16:
      return "int16_t";
    case SemanticType::Int32:
      return "int32_t";
    case SemanticType::Int64:
      return "int64_t";
    case SemanticType::UInt8:
      return "uint8_t";
    case SemanticType::UInt16:
      return "uint16_t";
    case SemanticType::UInt32:
      return "uint32_t";
    case SemanticType::UInt64:
      return "uint64_t";
    case SemanticType::Float:
      return "float";
    case SemanticType::Double:
      return "double";
    case SemanticType::Bool:
      return "bool";
    case SemanticType::Char:
      return "char";
    case SemanticType::StringView:
      return "std::string_view";
    case SemanticType::NullPtr:
      return "nullptr_t";
    case SemanticType::RawPointer:
      if (type.arguments.size() == 1) {
        return (type.pointerAccess == AccessMode::ReadOnly ? "const " : "") +
               print(type.arguments.front()) + "*";
      }
      return "raw pointer";
    case SemanticType::Array:
      if (type.arguments.size() == 1) {
        return print(type.arguments.front()) + "[" + arrayExtent(type) + "]";
      }
      return "array";
    case SemanticType::Class:
      return classType(type);
    case SemanticType::Enum:
      if (const EnumTypeInfo *info = semantics.findEnumType(type.enumId)) {
        return info->qualifiedName;
      }
      return "unknown enum";
    case SemanticType::Reference:
      if (type.arguments.size() == 1) {
        return (type.referenceAccess == AccessMode::Mutable ? "mut " : "") +
               print(type.arguments.front()) + "&";
      }
      return "reference";
    case SemanticType::UniqueOwner:
      return unaryType("gti_internal::unique_owner", type);
    case SemanticType::SharedPointer:
      return unaryType("std::shared_ptr", type);
    case SemanticType::Storage:
      return unaryType("gti_internal::storage", type);
    case SemanticType::TypeParameter:
      return genericParameter(type.genericParameterId, false);
    case SemanticType::TypePack:
      return genericParameter(type.genericParameterId, true);
    case SemanticType::TypeName:
      if (const ClassTypeInfo *info = semantics.findClassType(type.classId)) {
        return info->qualifiedName;
      }
      return "type";
    case SemanticType::Function:
      return "function";
    case SemanticType::Lambda:
      return "lambda";
    case SemanticType::Expected:
      if (type.arguments.size() == 2) {
        return "expected<" + print(type.arguments[0]) + ", " +
               print(type.arguments[1]) + ">";
      }
      return "expected";
    case SemanticType::Unexpected:
      return unaryType("unexpected", type);
    }
    return "unknown";
  }

private:
  [[nodiscard]] std::string classType(const SemanticType &type) const {
    const ClassTypeInfo *info = semantics.findClassType(type.classId);
    if (info == nullptr) {
      return "unknown class";
    }
    std::string result = info->qualifiedName;
    if (type.arguments.empty() && type.valueArguments.empty()) {
      return result;
    }
    result += '<';
    bool first = true;
    for (const SemanticType &argument : type.arguments) {
      if (!first) {
        result += ", ";
      }
      first = false;
      result += print(argument);
    }
    for (const CompileTimeValue &argument : type.valueArguments) {
      if (!first) {
        result += ", ";
      }
      first = false;
      result += value(argument);
    }
    result += '>';
    return result;
  }

  [[nodiscard]] std::string unaryType(std::string_view name,
                                      const SemanticType &type) const {
    return type.arguments.size() == 1
               ? std::string(name) + '<' + print(type.arguments.front()) + '>'
               : std::string(name);
  }

  [[nodiscard]] std::string arrayExtent(const SemanticType &type) const {
    return type.arrayLengthParameterId == 0
               ? std::to_string(type.arrayLength)
               : genericParameter(type.arrayLengthParameterId, false);
  }

  [[nodiscard]] std::string value(const CompileTimeValue &value) const {
    if (value.kind == CompileTimeValue::UInt64) {
      return std::to_string(value.value);
    }
    if (value.kind == CompileTimeValue::Parameter) {
      return genericParameter(value.parameterId, false);
    }
    return "unknown value";
  }

  [[nodiscard]] std::string genericParameter(GenericParameterId id,
                                             bool pack) const {
    const GenericParameterInfo *parameter = semantics.findGenericParameter(id);
    if (parameter == nullptr) {
      return pack ? "type pack" : "type parameter";
    }
    return parameter->name.lexeme + (pack ? "..." : "");
  }

  const SemanticModel &semantics;
};

struct SemanticInstanceAnalysis {
  SemanticModel model;
  std::vector<SemanticDiagnostic> diagnostics;

  [[nodiscard]] bool valid() const { return diagnostics.empty(); }
};

class SemanticVisitor {
public:
  explicit SemanticVisitor(TargetInfo target = TargetInfo::host(),
                           const SourceGraph *sourceGraph = nullptr,
                           bool toolingOccurrences = true);
  ~SemanticVisitor();

  SemanticVisitor(const SemanticVisitor &) = delete;
  SemanticVisitor &operator=(const SemanticVisitor &) = delete;
  SemanticVisitor(SemanticVisitor &&) noexcept;
  SemanticVisitor &operator=(SemanticVisitor &&) noexcept;

  bool check(const Program &program);
  bool check(const Expr &expr);

  [[nodiscard]] bool hadError() const;
  [[nodiscard]] const std::vector<SemanticDiagnostic> &errors() const;
  [[nodiscard]] SemanticType expressionType() const;
  [[nodiscard]] const SemanticModel &model() const;
  [[nodiscard]] SemanticModel takeModel();
  [[nodiscard]] SemanticTypeTraits traitsFor(const SemanticType &type) const;
  [[nodiscard]] bool requiresActiveCleanupFor(const SemanticType &type) const;

  [[nodiscard]] SemanticInstanceAnalysis analyzeFunctionInstance(
      FunctionId functionId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &functionTypeArguments,
      const std::vector<CompileTimeValue> &functionValueArguments);
  [[nodiscard]] SemanticInstanceAnalysis analyzeConstructorInstance(
      ConstructorId constructorId,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments,
      const std::vector<SemanticType> &constructorTypeArguments,
      const std::vector<CompileTimeValue> &constructorValueArguments);
  [[nodiscard]] SemanticInstanceAnalysis analyzeDestructorInstance(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments);
  [[nodiscard]] SemanticInstanceAnalysis analyzeClassFieldInitializers(
      ClassId classId, const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments);

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
