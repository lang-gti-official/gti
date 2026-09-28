#pragma once

#include "gti/concurrency.h"
#include "gti/semantic_core.h"

namespace lang {

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
  DeclaredUnsafeCall,
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
  // A lifecycle-bound owner dependency remains active until its one carrier
  // is destroyed. It cannot use ordinary last-use loan shortening because
  // the carrier's destructor performs the operation that releases the
  // protected resource.
  bool lifecycleBound = false;
  // For a StoredValue loan carried by a direct stored-reference parameter,
  // this is the exact semantic field that designates the external referent.
  // Zero for ordinary reference loans and non-entry retained loans.
  SymbolId designatedField = 0;
  std::vector<SymbolId> carriers;
  std::vector<SemanticLoanEndpoint> endpoints;
};

enum class RangeIterationStrategy {
  Structural,
  FixedArray,
};

// The semantic authority for one range statement. Generated syntax remains a
// recovery/implementation detail; later phases consume these exact owner,
// access, protocol, and loan identities instead of rediscovering a range from
// member spellings.
struct ResolvedRangeIterationInfo {
  RangeIterationStrategy strategy = RangeIterationStrategy::Structural;
  SemanticType rangeType = SemanticType::Unknown;
  SemanticType iteratorType = SemanticType::Unknown;
  SemanticType sentinelType = SemanticType::Unknown;
  SemanticType elementType = SemanticType::Unknown;
  AccessMode elementAccess = AccessMode::ReadOnly;
  SymbolId owner = 0;
  SymbolId rangeCarrier = 0;
  SymbolId iterator = 0;
  SymbolId sentinel = 0;
  SymbolId element = 0;
  SemanticLoanId rangeLoan = 0;
  SemanticLoanId elementLoan = 0;
  FunctionId beginFunction = 0;
  FunctionId endFunction = 0;
  FunctionId compareFunction = 0;
  FunctionId dereferenceFunction = 0;
  FunctionId incrementFunction = 0;
  std::uint64_t arrayLength = 0;
  bool ownsRangeTemporary = false;
  // The range source is the checked active-value projection of one named
  // local or by-value expected<T, E>. The range loan still protects the
  // complete expected root; this flag preserves the bounded projection
  // identity without admitting a general retained nested reference.
  bool expectedPayloadProjection = false;
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
  ThreadTask,
  // An ordinary owned value at parameter entry. Subsequent transfers and
  // cleanup are governed by value/loan/lifecycle analysis, not return syntax.
  Value,
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

// The exact source callable selected for a managed-thread task. A concrete
// task names exactly one function declaration or one lambda declaration; the
// selected invocation capability is retained so later phases never reopen
// operator() selection from the host-language type.
struct ResolvedThreadTaskEntry {
  FunctionId function = 0;
  LambdaId lambda = 0;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;

  friend bool operator==(const ResolvedThreadTaskEntry &,
                         const ResolvedThreadTaskEntry &) = default;
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
  StorageTryAllocate,
  StorageCapacity,
  StorageInitialized,
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
  StorageBoundsCheck,
  AllocatePrefixStorage,
  PrefixStorageAppend,
  PrefixStoragePop,
  PrefixStorageRead,
  PrefixStorageReadMut,
  PrefixStorageLength,
  PrefixStorageRelocate,
  PrefixStorageInsert,
  PrefixStorageErase,
  PrefixStorageTextView,
  PrefixStorageCString,
  PrefixStorageTryCString,
  OptionalStorageHasValue,
  OptionalStorageConstruct,
  OptionalStorageRead,
  OptionalStorageReadMut,
  OptionalStorageReset,
  StringViewSize,
  StringViewEmpty,
  ExpectedValue,
  ExpectedError,
  ExpectedValueOr,
  ArrayRangeBegin,
  ArraySize,
  ExpectedHasValue,
  ThreadHandleEmpty,
  ThreadSpawn,
  ThreadJoinable,
  ThreadJoin,
  ThreadAutoJoin,
  AtomicLoad,
  AtomicStore,
  AtomicExchange,
  AtomicCompareExchange,
  MutexLock,
  MutexUnlock,
  MutexGuardActive,
  MutexGuardRead,
  MutexGuardReadMut,
  MutexLockFailure,
  ExactTypeParameterConstruction,
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
  CString,
  PrefixStorage,
  OptionalStorage,
  ThreadHandle,
  AtomicStorage,
  MutexStorage,
  MutexLockToken,
};

enum class ProgramEntryKind {
  None,
  NoArguments,
  OwnedArguments,
};

enum class ProgramStorageKind {
  NamespaceGlobal,
  StaticField,
};

enum class ProgramInitializationStepRole {
  DataOnly,
  Initializer,
};

struct ProgramInitializationStep {
  ProgramInitializationStepId id = 0;
  SourceUnitId sourceUnit = 0;
  ProgramStorageKind kind = ProgramStorageKind::NamespaceGlobal;
  ProgramInitializationStepRole role =
      ProgramInitializationStepRole::Initializer;
  const VariableDecl *declaration = nullptr;
  SymbolId symbol = 0;
  ClassId ownerClass = 0;
  bool requiresActiveCleanup = false;
  SourceSpan declarationSpan;

  friend bool operator==(const ProgramInitializationStep &left,
                         const ProgramInitializationStep &right) {
    return left.id == right.id && left.sourceUnit == right.sourceUnit &&
           left.kind == right.kind && left.role == right.role &&
           left.declaration == right.declaration &&
           left.symbol == right.symbol && left.ownerClass == right.ownerClass &&
           left.requiresActiveCleanup == right.requiresActiveCleanup &&
           left.declarationSpan.source == right.declarationSpan.source &&
           left.declarationSpan.start == right.declarationSpan.start &&
           left.declarationSpan.end == right.declarationSpan.end &&
           left.declarationSpan.line == right.declarationSpan.line;
  }
};

struct ProgramInitializationPlan {
  std::vector<SourceUnitId> unitOrder;
  std::vector<ProgramInitializationStep> steps;

  [[nodiscard]] const ProgramInitializationStep *
  findStepForSymbol(SymbolId symbol) const;

  friend bool operator==(const ProgramInitializationPlan &,
                         const ProgramInitializationPlan &) = default;
};

struct HostedProgramEntryPlan {
  FunctionId entry = 0;
  ProgramEntryKind kind = ProgramEntryKind::None;
  FunctionId appendFunction = 0;
  ConstructorId vectorConstructor = 0;
  ConstructorId stringConstructor = 0;
  SourceUnitId sourceUnit = 0;
  SourceSpan mainAnchor;
  DefinedFailureOperation validateCount;
  DefinedFailureOperation convertCount;

  friend bool operator==(const HostedProgramEntryPlan &left,
                         const HostedProgramEntryPlan &right) {
    return left.entry == right.entry && left.kind == right.kind &&
           left.appendFunction == right.appendFunction &&
           left.vectorConstructor == right.vectorConstructor &&
           left.stringConstructor == right.stringConstructor &&
           left.sourceUnit == right.sourceUnit &&
           left.mainAnchor.source == right.mainAnchor.source &&
           left.mainAnchor.start == right.mainAnchor.start &&
           left.mainAnchor.end == right.mainAnchor.end &&
           left.mainAnchor.line == right.mainAnchor.line &&
           left.validateCount == right.validateCount &&
           left.convertCount == right.convertCount;
  }
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

// A receiver-relative path to private unique-owner storage returned through
// its checked borrow operation. This describes an accessor, not permission to
// consume an arbitrary reference. MIR independently verifies the source body.
struct OwnedPointeeProjection {
  std::vector<SymbolId> fields;
  IntrinsicKind borrowIntrinsic = IntrinsicKind::None;

  friend bool operator==(const OwnedPointeeProjection &,
                         const OwnedPointeeProjection &) = default;
};

struct FunctionInfo {
  FunctionId id = 0;
  SourceUnitId sourceUnit = 0;
  const FunctionDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::size_t requiredParameterCount = 0;
  std::vector<std::size_t> defaultArgumentEffectOwners;
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
  bool requiresUnsafe = false;
  CThreadRuntimeOperation cThreadOperation = CThreadRuntimeOperation::None;
  // Derived from resolved calls, after all source bodies have been analyzed.
  bool requiresCThreadRuntime = false;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  std::optional<std::size_t> cArrayCountParameter;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::optional<BorrowOriginPlace> returnBorrowPlace;
  std::optional<OwnedPointeeProjection> ownedPointeeProjection;
  std::vector<FunctionId> virtualRoots;
  std::vector<CallableParameterContract> callableParameters;
  bool compilerPrivate = false;

  // Source contracts use formal declaration indices. Concrete contracts use
  // flattened call indices, including each element of a final value pack.
  [[nodiscard]] std::vector<CallableParameterContract>
  instantiatedCallableParameters(
      const std::vector<SemanticType> &concreteParameters) const;
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

struct LambdaCaptureLink {
  SymbolId source = 0;
  SymbolId binding = 0;
  LambdaCaptureMode mode = LambdaCaptureMode::Copy;
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

  friend bool operator==(const CAbiRecordFieldLayout &,
                         const CAbiRecordFieldLayout &) = default;
};

struct CAbiRecordLayout {
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
  std::vector<CAbiRecordFieldLayout> fields;

  friend bool operator==(const CAbiRecordLayout &,
                         const CAbiRecordLayout &) = default;
};

struct UnionFieldLayout {
  const VariableDecl *declaration = nullptr;
  SemanticType type = SemanticType::Unknown;
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;

  friend bool operator==(const UnionFieldLayout &,
                         const UnionFieldLayout &) = default;
};

struct UnionLayout {
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
  std::vector<UnionFieldLayout> fields;

  friend bool operator==(const UnionLayout &, const UnionLayout &) = default;
};

struct ClassTypeInfo {
  ClassId id = 0;
  SourceUnitId sourceUnit = 0;
  const ClassDecl *declaration = nullptr;
  std::string qualifiedName;
  std::vector<std::string> namespaceScope;
  // An exact specialization is a distinct semantic class identity whose
  // qualified source name and canonical argument key still name this primary.
  // Zero denotes an ordinary (possibly generic) class declaration.
  ClassId exactSpecializationPrimary = 0;
  std::vector<SemanticType> exactTypeArguments;
  std::vector<CompileTimeValue> exactValueArguments;
  std::vector<GenericParameterInfo> genericParameters;
  std::vector<SemanticType> genericArgumentPatterns;
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
  std::vector<AppliedConceptRequirement> requirements;
  std::vector<SemanticType> parameterTypes;
  std::size_t requiredParameterCount = 0;
  std::vector<std::size_t> defaultArgumentEffectOwners;
  std::optional<std::size_t> borrowParameter;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool constexprConstructor = false;
  bool compilerPrivate = false;
  bool parameterPack = false;
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
  std::vector<AppliedConceptRequirement> requirements;
  std::vector<SemanticType> parameterTypes;
  std::vector<const Expr *> defaultArguments;
  // Designated struct construction supplies every field in declaration order,
  // including declaration defaults. These are caller-side MIR inputs.
  std::vector<const Expr *> designatedArguments;
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
  std::vector<const Expr *> defaultArguments;
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

enum class CallReceiverMode {
  None,
  Value,
  ReadOnlyPlace,
  MutablePlace,
  FreshTemporaryRead,
  FreshTemporaryMutable,
  Consuming,
};

struct ResolvedCallInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<const Expr *> defaultArguments;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<AppliedConceptRequirement> requirements;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  SynchronizationOperation synchronization;
  std::optional<ResolvedThreadTaskEntry> threadTaskEntry;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::optional<BorrowOriginPlace> borrowPlace;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  CallReceiverMode receiverMode = CallReceiverMode::None;
  // An immediate invocation keeps the owned field in consuming this.
  bool retainedReceiverProjection = false;
  // Exact owning accessor receiver whose availability is consumed, while its
  // returned pointee is invoked in place. Null for ordinary field forwarding.
  const Expr *retainedReceiverOwner = nullptr;
  // Exact read-only reference parameters that borrow one fresh value only for
  // this invocation. The full expression retains ownership of each value.
  std::vector<std::size_t> ephemeralBorrowArguments;
  std::vector<CallableArgumentBoundary> callableArguments;
};

// Contextual collection braces select ordinary static GTI functions. The
// compiler owns sequencing/transfer; the selected library owns storage policy.
struct ResolvedCollectionInfo {
  SemanticType type = SemanticType::Unknown;
  SemanticType elementType = SemanticType::Unknown;
  FunctionId initialize = 0;
  FunctionId insert = 0;
};

struct NativeFunctionConversionInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  SemanticType type = SemanticType::Unknown;
  CThreadCallbackKind cThreadCallback = CThreadCallbackKind::None;
};

// One context-selected, namespace-scope GTI function item. The semantic type
// carries the same declaration identity and exact signature; this record keeps
// the source expression tied to the selected declaration for HIR and tooling.
struct FunctionItemSelectionInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  SemanticType type = SemanticType::Unknown;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
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
  std::optional<CallableBoundary> boundary = CallableBoundary::Confined;
  // A stored receiver is checked again against its concrete field type.
  // It is not a confined parameter or an ownership-transfer boundary.
  SymbolId receiverField = 0;
};

struct ResolvedOperatorInfo {
  FunctionId function = 0;
  const FunctionDecl *declaration = nullptr;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  OverloadedOperator kind = OverloadedOperator::Dereference;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  CallReceiverMode receiverMode = CallReceiverMode::None;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
  std::optional<CallableBoundary> boundary;
  // Exact semantic receiver facts used by overload resolution. Synthetic
  // assignment receivers have no standalone AST occurrence, so downstream
  // lowering must not reconstruct these facts from a declaration symbol.
  ExpressionInfo receiverInfo;
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
  // The generic declaration intentionally has no single concrete type. Each
  // analyzed HIR instance owns the exact inferred type for this local.
  bool instanceDependentType = false;
  std::optional<std::string> documentationMarkdown;
};

// Byte-exact source geometry of one call's argument list, recorded by the
// semantic analyzer from parser tokens so editor signature help never has to
// re-parse punctuation: the delimiters are the '(' and ')' offsets and each
// separator is one argument-separating ',' offset.
struct SemanticCallGeometry {
  std::size_t leftDelimiter = 0;
  std::size_t rightDelimiter = 0;
  std::vector<std::size_t> argumentSeparators;
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
  const ConceptDecl *conceptDeclaration = nullptr;
  const ConstructorDecl *constructor = nullptr;
  const DestructorDecl *destructor = nullptr;
  std::optional<ResolvedCallInfo> selectedCall;
  std::optional<ResolvedConstructionInfo> selectedConstruction;
  std::optional<SemanticCallGeometry> callGeometry;
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

  void setSymbolDocumentation(SymbolId symbol,
                              std::string documentationMarkdown);

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

struct SemanticSourceUnitSeal {
  SourceUnitId id = 0;
  std::string path;
  std::size_t declarationStart = 0;
  std::size_t declarationCount = 0;
  std::optional<std::string> standardLibraryName;
  std::optional<std::string> packageIdentity;
  std::optional<std::string> packageRelativePath;
  SourceUnitRole role = SourceUnitRole::Application;
  bool entry = false;
  bool prelude = false;

  friend bool operator==(const SemanticSourceUnitSeal &,
                         const SemanticSourceUnitSeal &) = default;
};

struct SemanticSourceDependencySeal {
  SourceUnitId source = 0;
  SourceUnitId target = 0;
  SourceDependencyKind kind = SourceDependencyKind::Include;
  std::string directiveSource;
  std::size_t directiveStart = 0;
  std::size_t directiveEnd = 0;
  int directiveLine = 0;
  std::string includeSpelling;
  std::size_t includeOccurrence = 0;

  friend bool operator==(const SemanticSourceDependencySeal &,
                         const SemanticSourceDependencySeal &) = default;
};

struct SemanticSourceGraphSeal {
  SourceUnitId entry = 0;
  std::vector<SourceUnitId> preludeRoots;
  std::vector<SemanticSourceUnitSeal> units;
  std::vector<SemanticSourceDependencySeal> dependencies;

  friend bool operator==(const SemanticSourceGraphSeal &,
                         const SemanticSourceGraphSeal &) = default;
};

struct SemanticAnalysisSeal {
  std::uint64_t programSnapshot = 0;
  TargetInfo target;
  std::vector<const Stmt *> activeStatements;
  SemanticSourceGraphSeal sourceGraph;

  [[nodiscard]] bool matchesTarget(const TargetInfo &candidate) const {
    return target.os == candidate.os && target.vendor == candidate.vendor &&
           target.arch == candidate.arch &&
           target.dataLayout == candidate.dataLayout &&
           target.executionProfile == candidate.executionProfile &&
           target.runtimeCapabilities == candidate.runtimeCapabilities;
  }

  [[nodiscard]] bool matchesProgram(const Program &program,
                                    const TargetInfo &candidate) const;

  friend bool operator==(const SemanticAnalysisSeal &left,
                         const SemanticAnalysisSeal &right) {
    return left.matchesTarget(right.target) &&
           left.programSnapshot == right.programSnapshot &&
           left.activeStatements == right.activeStatements &&
           left.sourceGraph == right.sourceGraph;
  }
};

[[nodiscard]] SemanticAnalysisSeal
makeSemanticAnalysisSeal(const Program &program, const TargetInfo &target,
                         const SourceGraph *sourceGraph = nullptr);

struct SemanticFullExpression {
  std::size_t order = 0;
  const Stmt *statement = nullptr;
  const ConstructorInitializer *constructorInitializer = nullptr;
  std::vector<const Expr *> roots;
};

enum class StaticAssertResult {
  Passed,
  Failed,
  Deferred,
};

class SemanticModel {
public:
  // Own semantic-generated constructor syntax for the lifetime of every
  // downstream reference; cached parsed source trees remain immutable.
  std::vector<std::shared_ptr<const ConstructorDecl>> designatedConstructors;
  [[nodiscard]] const SemanticAnalysisSeal &analysisSeal() const;

  [[nodiscard]] ExecutionProfile executionProfile() const;

  [[nodiscard]] std::size_t placeSnapshot() const;

  // AST identities remain valid while the analyzed Program is alive.
  [[nodiscard]] const ExpressionInfo *
  findExpression(const Expr &expression) const;

  [[nodiscard]] UnsafeOperationKind
  unsafeOperation(const Expr &expression) const;

  [[nodiscard]] const DefinedFailureOperation *
  findDefinedFailure(const Expr &expression) const;

  [[nodiscard]] const ProgramInitializationPlan &
  programInitializationPlan() const;

  [[nodiscard]] const std::optional<HostedProgramEntryPlan> &
  hostedProgramEntryPlan() const;

  [[nodiscard]] bool
  isProgramConstantSubstitution(const Expr &expression) const;

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

  [[nodiscard]] std::optional<StaticAssertResult>
  findStaticAssertResult(const StaticAssertDecl &declaration) const;

  [[nodiscard]] std::vector<SemanticLoanId>
  loansEndingAtSwitchArmEntry(const SwitchStmt &statement,
                              std::size_t armIndex) const;

  [[nodiscard]] const StructuredBindingInfo *
  findStructuredBinding(const StructuredBindingDecl &declaration) const;

  [[nodiscard]] const ResolvedRangeIterationInfo *
  findRangeIteration(const RangeForStmt &statement) const;

  [[nodiscard]] const FunctionInfo *
  findFunction(const FunctionDecl &declaration) const;

  [[nodiscard]] const FunctionInfo *findFunction(FunctionId id) const;

  [[nodiscard]] const LambdaInfo *findLambda(const Lambda &declaration) const;

  [[nodiscard]] const LambdaInfo *findLambda(LambdaId id) const;

  // Every recorded lambda-capture pairing of enclosing source binding and
  // capture target, including base-model records. Copy-snapshot captures
  // couple the two names syntactically, so tooling that rewrites one symbol
  // must consult these links instead of matching spellings.
  [[nodiscard]] std::vector<LambdaCaptureLink> lambdaCaptureLinks() const;

  [[nodiscard]] const ClassTypeInfo *
  findClassType(const ClassDecl &declaration) const;

  [[nodiscard]] const ClassTypeInfo *findClassType(ClassId id) const;

  // Selects a registered full class specialization after recursively
  // resolving concrete nested class arguments. Symbolic generic types retain
  // their primary identity until instance substitution makes them exact.
  [[nodiscard]] SemanticType
  resolveExactClassSpecialization(SemanticType type) const;

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
  [[nodiscard]] const ResolvedCollectionInfo *
  findCollection(const ArrayInitializer &initializer) const;

  [[nodiscard]] const NativeFunctionConversionInfo *
  findNativeFunctionConversion(const Expr &expression) const;

  [[nodiscard]] const FunctionItemSelectionInfo *
  findFunctionItemSelection(const Expr &expression) const;

  [[nodiscard]] const ResolvedPackFoldInfo *
  findPackFold(const PackFold &fold) const;

  [[nodiscard]] const ResolvedLambdaCallInfo *
  findLambdaCall(const Call &call) const;

  [[nodiscard]] const DeferredCallableCallInfo *
  findDeferredCallableCall(const Call &call) const;

  [[nodiscard]] const ResolvedOperatorInfo *
  findOperator(const Expr &expression) const;

  [[nodiscard]] const ResolvedOperatorInfo *
  findProjectionOperator(const Expr &expression) const;

  [[nodiscard]] const ResolvedOperatorInfo *
  findContextualConversion(const Expr &expression) const;

  [[nodiscard]] bool isContextualIntegerOperand(const Expr &expression) const;

  [[nodiscard]] const ClassLifecycleInfo *
  findClassLifecycle(const ClassDecl &declaration) const;

  [[nodiscard]] const ResolvedConstructionInfo *
  findConstruction(const Expr &expression) const;

  [[nodiscard]] const ResolvedConstructionInfo *
  findArrayElementConstruction(const ArrayInitializer &initializer) const;

  [[nodiscard]] const ResolvedConstructorInitializerInfo *
  findConstructorInitializer(const ConstructorInitializer &initializer) const;

  [[nodiscard]] SymbolId findResolvedSymbol(const Expr &expression) const;

  [[nodiscard]] std::size_t functionCount() const;

  // Query after successful semantic analysis. Entry selection remains the
  // compiler's resolved function fact, not a driver/editor spelling heuristic.
  [[nodiscard]] std::optional<Diagnostic>
  executableEntryDiagnostic(SourceUnitId entryUnit,
                            const std::string &entryPath) const;

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

  void setAnalysisSeal(SemanticAnalysisSeal seal);

  void setProgramInitializationPlan(ProgramInitializationPlan plan);

  void setHostedProgramEntryPlan(std::optional<HostedProgramEntryPlan> plan);

  void recordProgramConstantSubstitution(const Expr &expression);
  void clearProgramConstantSubstitution(const Expr &expression);

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

  void recordStaticAssertResult(const StaticAssertDecl &declaration,
                                StaticAssertResult result);

  void recordLoanEndAtSwitchArmEntry(SemanticLoanId id,
                                     const SwitchStmt &statement,
                                     std::size_t armIndex);

  void recordLoanCarrier(SemanticLoanId id, SymbolId carrier);

  void removeLoanCarrier(SemanticLoanId id, SymbolId carrier);

  void clearLoans();

  void record(const StructuredBindingDecl &declaration,
              StructuredBindingInfo info);

  void record(const RangeForStmt &statement, ResolvedRangeIterationInfo info);

  void recordExplicitMove(const VariableDecl &declaration);

  void recordExplicitMove(const Parameter &parameter);

  void record(const FunctionDecl &declaration, FunctionInfo info);

  void record(const Lambda &declaration, LambdaInfo info);

  void recordClassType(const ClassDecl &declaration, ClassTypeInfo info);

  void
  recordExactClassSpecialization(ClassId primary, ClassId specialization,
                                 std::vector<SemanticType> typeArguments,
                                 std::vector<CompileTimeValue> valueArguments);

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
  void recordCollection(const ArrayInitializer &initializer,
                        ResolvedCollectionInfo info);

  void recordNativeFunctionConversion(const Expr &expression,
                                      NativeFunctionConversionInfo info);

  void recordFunctionItemSelection(const Expr &expression,
                                   FunctionItemSelectionInfo info);

  // Concrete instance analysis may replace a declaration-time call with a
  // different expression kind. Keep that replacement from falling through
  // to the superseded call record in the base model.
  void suppressInheritedCall(const Call &call);

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

  // Concrete instance analysis may replace a declaration-time symbolic
  // operator with a built-in operation. Prevent that expression from falling
  // through to the superseded operator row in the base semantic model.
  void suppressInheritedOperator(const Expr &expression);

  void recordProjectionOperator(const Expr &expression,
                                ResolvedOperatorInfo info);

  void recordContextualConversion(const Expr &expression,
                                  ResolvedOperatorInfo info);

  void recordContextualIntegerOperand(const Expr &expression);

  void record(const ClassDecl &declaration, ClassLifecycleInfo info);

  void record(const Expr &expression, ResolvedConstructionInfo info);

  void recordArrayElementConstruction(const ArrayInitializer &initializer,
                                      ResolvedConstructionInfo info);

  void record(const ConstructorInitializer &initializer,
              ResolvedConstructorInitializerInfo info);

  void recordResolvedSymbol(const Expr &expression, SymbolId symbol);

  void recordCompilerCapabilityType(const TypeRef &type,
                                    CompilerCapabilityTypeKind kind);

  SymbolId recordSymbol(SymbolRecord symbol);

  void setSymbolDocumentation(SymbolId symbol,
                              std::string documentationMarkdown);

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
  SemanticAnalysisSeal semanticAnalysisSeal;
  std::unordered_map<const Expr *, ConstantValue> constants;
  std::unordered_map<const Expr *, UnsafeOperationKind> unsafeOperations;
  std::unordered_map<const Expr *, DefinedFailureOperation> definedFailures;
  ProgramInitializationPlan programInitialization;
  std::optional<HostedProgramEntryPlan> hostedProgramEntry;
  std::unordered_set<const Expr *> programConstantSubstitutions;
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
  std::unordered_map<const StaticAssertDecl *, StaticAssertResult>
      staticAssertResults;
  std::unordered_map<const SwitchStmt *,
                     std::vector<std::vector<SemanticLoanId>>>
      switchArmLoanEnds;
  std::unordered_map<const StructuredBindingDecl *, StructuredBindingInfo>
      structuredBindings;
  std::unordered_map<const RangeForStmt *, ResolvedRangeIterationInfo>
      rangeIterations;
  std::unordered_map<const FunctionDecl *, FunctionInfo> functions;
  std::unordered_map<FunctionId, const FunctionDecl *> functionsById;
  std::unordered_map<const Lambda *, LambdaInfo> lambdas;
  std::unordered_map<LambdaId, const Lambda *> lambdasById;
  std::unordered_map<const ClassDecl *, ClassTypeInfo> classTypes;
  std::unordered_map<ClassId, const ClassDecl *> classTypesById;
  struct ExactClassSpecializationRecord {
    ClassId primary = 0;
    ClassId specialization = 0;
    std::vector<SemanticType> typeArguments;
    std::vector<CompileTimeValue> valueArguments;
  };
  std::vector<ExactClassSpecializationRecord> exactClassSpecializations;
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
  std::unordered_map<const Expr *, NativeFunctionConversionInfo>
      nativeFunctionConversions;
  std::unordered_map<const Expr *, FunctionItemSelectionInfo>
      functionItemSelections;
  std::unordered_set<const Call *> suppressedInheritedCalls;
  std::unordered_map<const PackFold *, ResolvedPackFoldInfo> packFolds;
  std::unordered_map<const Call *, ResolvedLambdaCallInfo> lambdaCalls;
  std::unordered_map<const Call *, DeferredCallableCallInfo>
      deferredCallableCalls;
  std::vector<PendingCallableForwarding> pendingCallableForwardings;
  std::unordered_map<const Expr *, ResolvedOperatorInfo> operators;
  std::unordered_set<const Expr *> suppressedInheritedOperators;
  std::unordered_map<const Expr *, ResolvedOperatorInfo> projectionOperators;
  std::unordered_map<const Expr *, ResolvedOperatorInfo> contextualConversions;
  std::unordered_set<const Expr *> contextualIntegerOperands;
  std::unordered_map<const ClassDecl *, ClassLifecycleInfo> classLifecycles;
  std::unordered_map<const Expr *, ResolvedConstructionInfo> constructions;
  std::unordered_map<const ArrayInitializer *, ResolvedCollectionInfo>
      collections;
  std::unordered_map<const ArrayInitializer *, ResolvedConstructionInfo>
      arrayElementConstructions;
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

} // namespace lang
