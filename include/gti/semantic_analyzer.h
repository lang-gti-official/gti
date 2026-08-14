#pragma once

#include "gti/ast.h"
#include "gti/checked_integer.h"
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

[[nodiscard]] std::optional<CheckedIntegerDomain>
constantIntegerDomain(const SemanticType &type);

[[nodiscard]] std::optional<BinaryFloatFormat>
semanticFloatFormat(const SemanticType &type);

[[nodiscard]] SemanticType semanticIntegerType(CheckedIntegerDomain domain);

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
[[nodiscard]] SemanticTypeTraits semanticTraits(const SemanticType &type);

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
  UniqueOwnerUpcast,
  AllocateStorage,
  StorageConstruct,
  StorageRead,
  StorageReadMut,
  StorageDestroy,
  StorageRelocate,
  StorageShiftRight,
  StorageShiftLeft,
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

[[nodiscard]] std::optional<IntegerArithmeticIntrinsic>
integerArithmeticIntrinsic(IntrinsicKind intrinsic);

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
  Global,
};

// An exact, domain-independent global/static storage origin. Callers qualify
// this place into their own semantic/HIR/MIR body domain.
struct BorrowOriginPlace {
  SymbolId root = 0;
  std::vector<PlaceProjection> projections;

  [[nodiscard]] bool valid() const { return root != 0; }

  friend bool operator==(const BorrowOriginPlace &,
                         const BorrowOriginPlace &) = default;
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
  std::optional<BorrowOriginPlace> returnBorrowPlace;
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
  std::optional<BorrowOriginPlace> borrowPlace;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::vector<CallableArgumentBoundary> callableArguments;
};

// A deliberately bounded comma-pack fold. The frontend retains the one
// declaration selected by the symbolic pattern and, for concrete generic
// instances, every exact element specialization in source-pack order.
// Backends must not reopen overload resolution for these calls.
struct ResolvedPackFoldElement {
  SemanticType elementType = SemanticType::Unknown;
  ResolvedCallInfo call;
};

struct ResolvedPackFoldInfo {
  const Call *pattern = nullptr;
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  SymbolId packSymbol = 0;
  GenericParameterId packParameter = 0;
  std::size_t packArgument = 0;
  std::vector<SemanticType> fixedArgumentTypes;
  std::vector<ResolvedPackFoldElement> elements;
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
  [[nodiscard]] const std::vector<SymbolRecord> &symbols() const;

  [[nodiscard]] const SymbolRecord *findSymbol(SymbolId id) const;

  [[nodiscard]] std::vector<const SemanticOccurrence *>
  occurrencesForSymbol(SymbolId id) const;

  [[nodiscard]] const std::vector<SemanticOccurrence> &
  occurrences(SourceUnitId sourceUnit) const;

  [[nodiscard]] const SemanticOccurrence *
  findOccurrence(SourceUnitId sourceUnit, std::size_t byteOffset) const;

  [[nodiscard]] const SymbolRecord *findSymbolAt(SourceUnitId sourceUnit,
                                                 std::size_t byteOffset) const;

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

  static int priority(SemanticOccurrenceKind kind);

  void clear();

  // Turns this database into an instance-analysis delta over baseDatabase:
  // lookups fall back to the base, and new symbol identities continue after
  // the base's so instance records never collide with base SymbolIds.
  void beginInstanceDelta(const SemanticDatabase &baseDatabase);

  void rebase(const SemanticDatabase *baseDatabase);

  SymbolId recordSymbol(SymbolRecord symbol);

  [[nodiscard]] SymbolId
  symbolForDeclaration(SourceUnitId sourceUnit, const SourceSpan &span,
                       std::string_view generatedName = {}) const;

  void record(SemanticOccurrence occurrence);

  void setToolingOccurrencesEnabled(bool enabled);

  void finalize();

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

[[nodiscard]] ExpressionInfo
makeExpressionInfo(SemanticType type,
                   ValueCategory category = ValueCategory::Value,
                   AccessMode access = AccessMode::ReadOnly);

[[nodiscard]] BindingInfo
makeBindingInfo(SemanticType type, AccessMode access = AccessMode::ReadOnly);

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
  [[nodiscard]] ExecutionProfile executionProfile() const;

  [[nodiscard]] std::size_t placeSnapshot() const;

  // AST identities remain valid while the analyzed Program is alive.
  [[nodiscard]] const ExpressionInfo *
  findExpression(const Expr &expression) const;

  [[nodiscard]] UnsafeOperationKind
  unsafeOperation(const Expr &expression) const;

  [[nodiscard]] const DefinedFailureOperation *
  findDefinedFailure(const Expr &expression) const;

  [[nodiscard]] const PlaceKey *findPlace(const Expr &expression) const;

  [[nodiscard]] const OwnershipEvent *
  findOwnershipEvent(const Expr &expression) const;

  [[nodiscard]] std::optional<LambdaCaptureMode>
  lambdaCaptureMode(SymbolId symbol) const;

  [[nodiscard]] std::size_t placeSelection(const Expr &expression) const;

  [[nodiscard]] std::size_t ownershipEventCount() const;

  [[nodiscard]] CompilerCapabilityTypeKind
  compilerCapabilityType(const TypeRef &type) const;

  [[nodiscard]] bool isCompilerPrivateType(const SemanticType &type) const;

  [[nodiscard]] bool canPresent(SourceUnitId requester,
                                const SymbolRecord &symbol,
                                const SourceGraph &sourceGraph) const;

  [[nodiscard]] bool canPresent(SourceUnitId requester,
                                const SemanticOccurrence &occurrence,
                                const SourceGraph &sourceGraph) const;

  [[nodiscard]] ExpressionInfo expressionInfo(const Expr &expression) const;

  [[nodiscard]] const SemanticType *findType(const Expr &expression) const;

  [[nodiscard]] std::optional<ConstantValue>
  findConstant(const Expr &expression) const;

  [[nodiscard]] const CompileTimeValue *
  findArrayExtent(const ArrayExtentExpr &extent) const;

  [[nodiscard]] SemanticType typeOf(const Expr &expression) const;

  [[nodiscard]] bool hasType(const Expr &expression) const;

  [[nodiscard]] std::size_t expressionCount() const;

  [[nodiscard]] const std::vector<SemanticFullExpression> &
  fullExpressionsFor(const Stmt &statement) const;

  [[nodiscard]] const std::vector<SemanticFullExpression> &
  fullExpressionsFor(const ConstructorInitializer &initializer) const;

  [[nodiscard]] const std::vector<SemanticFullExpression> &
  fullExpressions() const;

  [[nodiscard]] const BindingInfo *
  findBinding(const VariableDecl &declaration) const;

  [[nodiscard]] const BindingInfo *
  findBinding(const Parameter &parameter) const;

  [[nodiscard]] std::size_t bindingCount() const;

  [[nodiscard]] const BindingInfo *findPayloadBinding(const Token &name) const;

  [[nodiscard]] const SemanticLoanInfo *findLoan(SemanticLoanId id) const;

  [[nodiscard]] const std::vector<SemanticLoanInfo> &loans() const;

  [[nodiscard]] std::vector<SemanticLoanId>
  loansEndingAfter(const Stmt &statement) const;

  [[nodiscard]] std::vector<SemanticLoanId>
  loansEndingAtConditionalEntry(const IfStmt &statement, bool thenBranch) const;

  [[nodiscard]] std::optional<bool>
  findConstexprBranch(const IfStmt &statement) const;

  [[nodiscard]] std::vector<SemanticLoanId>
  loansEndingAtSwitchArmEntry(const SwitchStmt &statement,
                              std::size_t armIndex) const;

  [[nodiscard]] const StructuredBindingInfo *
  findStructuredBinding(const StructuredBindingDecl &declaration) const;

  [[nodiscard]] const FunctionInfo *
  findFunction(const FunctionDecl &declaration) const;

  [[nodiscard]] const FunctionInfo *findFunction(FunctionId id) const;

  [[nodiscard]] const LambdaInfo *findLambda(const Lambda &declaration) const;

  [[nodiscard]] const LambdaInfo *findLambda(LambdaId id) const;

  [[nodiscard]] const ClassTypeInfo *
  findClassType(const ClassDecl &declaration) const;

  [[nodiscard]] const ClassTypeInfo *findClassType(ClassId id) const;

  [[nodiscard]] const TypeAliasInfo *
  findTypeAlias(const TypeAliasDecl &declaration) const;

  [[nodiscard]] const EnumTypeInfo *
  findEnumType(const EnumDecl &declaration) const;

  [[nodiscard]] const EnumTypeInfo *findEnumType(EnumId id) const;

  [[nodiscard]] const ResolvedEnumeratorInfo *
  findEnumerator(const QualifiedName &expression) const;

  [[nodiscard]] const SwitchCaseValue *
  findSwitchCase(const Expr &expression) const;

  [[nodiscard]] bool isExhaustiveSwitch(const SwitchStmt &statement) const;

  [[nodiscard]] const ResolvedPayloadConstructionInfo *
  findPayloadConstruction(const Call &call) const;

  [[nodiscard]] const ResolvedPayloadPatternInfo *
  findPayloadPattern(const Expr &expression) const;

  [[nodiscard]] const ResolvedCallInfo *findCall(const Call &call) const;

  [[nodiscard]] const ResolvedPackFoldInfo *
  findPackFold(const PackFold &fold) const;

  [[nodiscard]] const ResolvedLambdaCallInfo *
  findLambdaCall(const Call &call) const;

  [[nodiscard]] const DeferredCallableCallInfo *
  findDeferredCallableCall(const Call &call) const;

  [[nodiscard]] const ResolvedOperatorInfo *
  findOperator(const Expr &expression) const;

  [[nodiscard]] const ResolvedOperatorInfo *
  findContextualConversion(const Expr &expression) const;

  [[nodiscard]] bool isContextualIntegerOperand(const Expr &expression) const;

  [[nodiscard]] const ClassLifecycleInfo *
  findClassLifecycle(const ClassDecl &declaration) const;

  [[nodiscard]] const ResolvedConstructionInfo *
  findConstruction(const Expr &expression) const;

  [[nodiscard]] const ResolvedConstructorInitializerInfo *
  findConstructorInitializer(const ConstructorInitializer &initializer) const;

  [[nodiscard]] SymbolId findResolvedSymbol(const Expr &expression) const;

  [[nodiscard]] std::size_t functionCount() const;

  [[nodiscard]] std::size_t lambdaCount() const;

  [[nodiscard]] std::size_t resolvedCallCount() const;

  [[nodiscard]] std::size_t classLifecycleCount() const;

  [[nodiscard]] std::size_t resolvedConstructionCount() const;

  [[nodiscard]] const SemanticDatabase &database() const;

  [[nodiscard]] const std::optional<SemanticCompletionContext> &
  completionContext() const;

  [[nodiscard]] const GenericParameterInfo *
  findGenericParameter(GenericParameterId id) const;

  [[nodiscard]] const ConstructorInfo *
  findConstructor(const ConstructorDecl &declaration) const;

  [[nodiscard]] const DestructorInfo *
  findDestructor(const DestructorDecl &declaration) const;

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

  void clear();

  void setExecutionProfile(ExecutionProfile profile);

  void setPlaceSnapshot(std::size_t snapshot);

  // Turns this model into an instance-analysis delta over baseModel: reads
  // fall back to the base while writes stay local, so concrete instance
  // reanalysis records only what it produces instead of copying the whole
  // program's model. Loan tables deliberately do not fall back - instance
  // analysis restarts loan identities, mirroring the clearLoans() semantics
  // the previous whole-model copy relied on.
  void beginInstanceDelta(const SemanticModel &baseModel);

  // Re-points an instance delta at a relocated base (the analyzer restores
  // its model after each instance analysis; the delta must follow it).
  void rebase(const SemanticModel *baseModel);

  // Copies a base function record into the delta so record mutators can
  // update it locally. Returns the local entry, or functions.end() when the
  // declaration is unknown to both the delta and the base.
  [[nodiscard]] std::unordered_map<const FunctionDecl *, FunctionInfo>::iterator
  materializeFunction(const FunctionDecl &declaration);

  [[nodiscard]] bool validLoan(SemanticLoanId id) const;

  static void appendUniqueLoan(std::vector<SemanticLoanId> &loans,
                               SemanticLoanId loan);

  void recordLoanEndpoint(SemanticLoanId id, SemanticLoanEndKind kind,
                          const Stmt &statement, std::size_t switchArm = 0);

  void record(const Expr &expression, ExpressionInfo info);

  static bool
  appendFullExpression(std::vector<SemanticFullExpression> &expressions,
                       const SemanticFullExpression &expression);

  void recordFullExpression(const Stmt &statement, const ExprPtr &root);

  void recordFullExpression(const ConstructorInitializer &initializer);

  void recordConstant(const Expr &expression, ConstantValue value);

  void recordUnsafeOperation(const Expr &expression,
                             UnsafeOperationKind operation);

  void recordDefinedFailure(const Expr &expression,
                            DefinedFailureOperation operation);

  void recordPlace(const Expr &expression, PlaceKey place);

  void recordOwnershipEvent(const Expr &expression, OwnershipEvent event);

  void recordLambdaCaptureMode(SymbolId symbol, LambdaCaptureMode mode);

  void markOwnershipEventsUnreachableFrom(std::size_t first);

  void recordPlaceSelection(const Expr &expression, std::size_t selection);

  void record(const ArrayExtentExpr &extent, CompileTimeValue value);

  void record(const VariableDecl &declaration, BindingInfo info);

  void record(const Parameter &parameter, BindingInfo info);

  void recordPayloadBinding(const Token &name, BindingInfo info);

  void recordLoan(SemanticLoanInfo info);

  void recordBindingLoan(const VariableDecl &declaration, SemanticLoanId loan);

  void recordBindingLoan(const Parameter &parameter, SemanticLoanId loan);

  void recordLoanEndAfter(SemanticLoanId id, const Stmt &statement);

  void recordLoanEndAtConditionalEntry(SemanticLoanId id,
                                       const IfStmt &statement,
                                       bool thenBranch);

  void recordConstexprBranch(const IfStmt &statement, bool thenBranch);

  void recordLoanEndAtSwitchArmEntry(SemanticLoanId id,
                                     const SwitchStmt &statement,
                                     std::size_t armIndex);

  void recordLoanCarrier(SemanticLoanId id, SymbolId carrier);

  void clearLoans();

  void record(const StructuredBindingDecl &declaration,
              StructuredBindingInfo info);

  void recordExplicitMove(const VariableDecl &declaration);

  void recordExplicitMove(const Parameter &parameter);

  void record(const FunctionDecl &declaration, FunctionInfo info);

  void record(const Lambda &declaration, LambdaInfo info);

  void recordClassType(const ClassDecl &declaration, ClassTypeInfo info);

  void record(const TypeAliasDecl &declaration, TypeAliasInfo info);

  void recordEnumType(const EnumDecl &declaration, EnumTypeInfo info);

  void record(const QualifiedName &expression, ResolvedEnumeratorInfo info);

  void recordSwitchCase(const Expr &expression, SwitchCaseValue value);

  void recordExhaustiveSwitch(const SwitchStmt &statement);

  void recordPayloadConstruction(const Call &call,
                                 ResolvedPayloadConstructionInfo info);

  void recordPayloadPattern(const Expr &expression,
                            ResolvedPayloadPatternInfo info);

  void record(const Call &call, ResolvedCallInfo info);

  void record(const PackFold &fold, ResolvedPackFoldInfo info);

  void recordLambdaCall(const Call &call, ResolvedLambdaCallInfo info);

  void recordDeferredCallableCall(const Call &call,
                                  DeferredCallableCallInfo info);

  void recordCallableRequirement(const FunctionDecl &declaration,
                                 CallableParameterContract requirement);

  void recordCallableForwarding(const FunctionDecl &source,
                                std::size_t sourceParameterIndex,
                                GenericParameterId sourceGenericParameter,
                                SemanticType sourceType,
                                AccessMode sourceAccess, const Call &call,
                                FunctionId target,
                                std::size_t targetParameterIndex);

  void recordOperator(const Expr &expression, ResolvedOperatorInfo info);

  void recordContextualConversion(const Expr &expression,
                                  ResolvedOperatorInfo info);

  void recordContextualIntegerOperand(const Expr &expression);

  void record(const ClassDecl &declaration, ClassLifecycleInfo info);

  void record(const Expr &expression, ResolvedConstructionInfo info);

  void record(const ConstructorInitializer &initializer,
              ResolvedConstructorInitializerInfo info);

  void recordResolvedSymbol(const Expr &expression, SymbolId symbol);

  void recordCompilerCapabilityType(const TypeRef &type,
                                    CompilerCapabilityTypeKind kind);

  SymbolId recordSymbol(SymbolRecord symbol);

  [[nodiscard]] SymbolId
  symbolForDeclaration(SourceUnitId sourceUnit, const SourceSpan &span,
                       std::string_view generatedName = {}) const;

  void recordOccurrence(SemanticOccurrence occurrence);

  // Occurrences answer editor position queries (hover, definition, semantic
  // tokens) and are consumed only by language queries and the LSP. Symbols
  // stay recorded either way because HIR and the emitter resolve member
  // identity through them. A compile-only consumer disables occurrences so
  // analysis does not build, sort, and retain a table nothing will read.
  void setToolingOccurrencesEnabled(bool enabled);

  void recordCompletion(SemanticCompletionContext context);

  void finalizeOccurrences();

  void finalizeCallableArguments(ResolvedCallInfo &call) const;

  void finalizeCallableArguments();

  void finalizeCallableForwardings();

  std::unordered_map<const Expr *, ExpressionInfo> expressions;
  std::unordered_map<const Stmt *, std::vector<SemanticFullExpression>>
      statementFullExpressions;
  std::unordered_map<const ConstructorInitializer *,
                     std::vector<SemanticFullExpression>>
      constructorFullExpressions;
  std::vector<SemanticFullExpression> fullExpressionOrder;
  std::unordered_map<const Expr *, ConstantValue> constants;
  std::unordered_map<const Expr *, UnsafeOperationKind> unsafeOperations;
  std::unordered_map<const Expr *, DefinedFailureOperation> definedFailures;
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
  std::unordered_map<const PackFold *, ResolvedPackFoldInfo> packFolds;
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
  explicit SemanticTypePrinter(const SemanticModel &semantics);

  [[nodiscard]] std::string print(const SemanticType &type) const;

private:
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
