#pragma once

#include "gti/concurrency.h"
#include "gti/process_memory_runtime.h"
#include "gti/program_request.h"
#include "gti/semantic_core.h"

#include <functional>

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
  RawDereference,
  RawIndex,
  RawMember,
  UnionMember,
  PointerArithmetic,
  ForeignPointerCall,
  DeclaredUnsafeCall,
  ObjectLifetime,
};

struct SemanticLoanEndpoint {
  SemanticLoanEndKind kind = SemanticLoanEndKind::AfterStatement;
  const Stmt *statement = nullptr;
  std::size_t switchArm = 0;
};

using SemanticLoanPlaceProjectionKind = PlaceProjectionKind;
using SemanticLoanPlaceProjection = PlaceProjection;
using SemanticLoanPlace = PlaceKey;

// One field of a composed borrowed value protects its own independent owner.
// The first dependency remains in the scalar loan fields for the ordinary
// one-owner reference path; these records contain the additional dependencies.
struct SemanticLoanDependency {
  SemanticLoanPlace place;
  std::vector<SymbolId> designatedFields;
  AccessMode access = AccessMode::ReadOnly;
  bool protectsStorage = false;

  friend bool operator==(const SemanticLoanDependency &,
                         const SemanticLoanDependency &) = default;
};

struct SemanticLoanInfo {
  SemanticLoanId id = 0;
  SemanticLoanKind kind = SemanticLoanKind::Reference;
  const Expr *origin = nullptr;
  SymbolId owner = 0;
  bool receiverOrigin = false;
  SemanticLoanId parent = 0;
  // A reference to a borrowed-state carrier protects the carrier's physical
  // storage separately from the external owners retained by this StoredValue
  // loan. Unlike parent, this edge neither denotes containment nor suspends
  // the dependency's access permission.
  SemanticLoanId storedDependency = 0;
  bool entry = false;
  // Receiver-only abstract entry dependencies have no source binding. This
  // identity keeps them in their declaring body when one model holds many
  // functions and destructors; it is consumed during HIR lowering, never by
  // the backend.
  const Stmt *entryDeclaration = nullptr;
  SemanticLoanPlace place;
  AccessMode access = AccessMode::ReadOnly;
  bool protectsStorage = false;
  // A lifecycle-bound owner dependency remains active until its one carrier
  // is destroyed. It cannot use ordinary last-use loan shortening because
  // the carrier's destructor performs the operation that releases the
  // protected resource.
  bool lifecycleBound = false;
  // For a StoredValue parameter loan, this exact nominal field path ends at
  // the reference designating its external referent. Empty for other loans.
  std::vector<SymbolId> designatedFields;
  std::vector<SemanticLoanDependency> additionalDependencies;
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
  // Synthetic per-iteration staging: a reference alias for place yields, or
  // an independent lexical value for value yields. Its exact producer and
  // declaration identity remain part of the range contract.
  SymbolId elementOwner = 0;
  SemanticLoanId rangeLoan = 0;
  SemanticLoanId elementLoan = 0;
  SemanticLoanId elementOwnerLoan = 0;
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
  // The selected zero-argument read-only endpoint returns a value with no
  // retained reference or stored-owner dependency. This permits MIR to park a
  // fresh hidden mutable cursor while evaluating that exact endpoint.
  bool nonBorrowingEndpoint = false;
  bool materializesElementValue = false;
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
  // Direct arguments of open callables and arms of inferred conditional
  // values may defer their result. Concrete semantics resolves an exact
  // non-void, non-borrowing value before HIR.
  bool inferResultFromInstantiation = false;
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

// A type-level prerequisite for a confined, join-all worker boundary. This is
// not permission to spawn: the invocation must additionally retain the actual
// origin loans and prove joining before any normal/failure continuation. A
// successful query never changes the type's ordinary transfer/share traits.
struct ScopedThreadTaskDependency {
  std::vector<SymbolId> fields;
  SemanticType reference = SemanticType::Unknown;

  friend bool operator==(const ScopedThreadTaskDependency &,
                         const ScopedThreadTaskDependency &) = default;
};

struct ScopedThreadTaskShape {
  SemanticType taskType = SemanticType::Unknown;
  ResolvedThreadTaskEntry entry;
  std::vector<ScopedThreadTaskDependency> dependencies;

  friend bool operator==(const ScopedThreadTaskShape &,
                         const ScopedThreadTaskShape &) = default;
};

// One consumed task of a synchronous join-all invocation. The dependencies
// name the existing checked places, not a second concurrency provenance map.
// Their endpoint is the enclosing call's completion (including failure).
struct ResolvedScopedThreadTask {
  ScopedThreadTaskShape shape;
  std::vector<SemanticLoanDependency> dependencies;

  friend bool operator==(const ResolvedScopedThreadTask &,
                         const ResolvedScopedThreadTask &) = default;
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
  UniqueOwnerSwap,
  UniqueOwnerUpcast,
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
  OptionalStorageHasValue,
  OptionalStorageConstruct,
  OptionalStorageRead,
  OptionalStorageReadMut,
  OptionalStorageReset,
  PolymorphicStorageHasValue,
  PolymorphicStorageConstruct,
  PolymorphicStorageRead,
  PolymorphicStorageReadMut,
  PolymorphicStorageReset,
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
  AtomicWait,
  AtomicNotifyOne,
  AtomicNotifyAll,
  MutexLock,
  MutexUnlock,
  MutexGuardActive,
  MutexGuardRead,
  MutexGuardReadMut,
  MutexLockFailure,
  ExactTypeParameterConstruction,
  RequireKey,
  RequireContainerArgument,
  Panic,
  SharedOwnerEmpty,
  SharedOwnerFromUnique,
  SharedOwnerClone,
  SharedOwnerBorrow,
  SharedOwnerIsNull,
  SharedOwnerUseCount,
  SharedOwnerSwap,
  SharedOwnerUpcast,
  SourceFileName,
  SourceFunctionName,
  SourceLine,
  SourceColumn,
  // Compile-time reflection queries (std/meta). Folded to program constants
  // in semantics whenever their generic arguments are concrete; they never
  // execute.
  MetaFieldCount,
  MetaFieldName,
  MetaFieldIsPublic,
  MetaFieldHasDefault,
  MetaTypeName,
  MetaTypeKind,
  MetaFieldAttributeCount,
  MetaFieldHasAttribute,
  MetaTypeAttributeCount,
  MetaTypeHasAttribute,
  // <T, Trait>: 0 payload enum, 1 generic instance, 2 reflectable,
  // 3 signed integral.
  MetaTypeTrait,
  MetaEnumeratorCount,
  MetaEnumeratorName,
  // <E, I>: the enumerator as a constant of E (integral enums).
  MetaEnumeratorValue,
  MetaPayloadCount,
  MetaPayloadName,
  MetaBaseCount,
  MetaArgumentCount,
  // Callable metadata: methods (including operators and static methods) in
  // declaration order, constructors in declaration order, their parameters.
  MetaMethodCount,
  MetaMethodName,
  // <T, I, Trait>: 0 static, 1 operator, 2 constexpr, 3 virtual, 4 mutable
  // receiver, 5 public.
  MetaMethodTrait,
  MetaMethodParameterCount,
  MetaParameterName,
  MetaConstructorCount,
  MetaConstructorParameterCount,
  MetaConstructorParameterName,
  // <T, Kind, I>: 0 type, 1 field, 2 enumerator, 3 method, 4 constructor.
  MetaDeclarationLine,
  MetaDeclarationFile,
  // <T>: the qualified name of T's primary declaration, without arguments.
  MetaTypePrimaryName,
  // Attribute value access: semantics records the attribute's constant on the
  // call and aliases it to a construction of the attribute struct.
  MetaFieldAttribute,
  MetaTypeAttribute,
  // Declared default value: semantics aliases the call to a clone of the
  // field's default initializer.
  MetaFieldDefault,
  // Reflected field access: semantics analyses the field projection it
  // denotes and aliases the call to it.
  MetaFieldGet,
  MetaFieldGetMut,
  // Structural construction: semantics selects the subject's designated
  // constructor for the call's arguments, so the call is a construction.
  MetaConstruct,
  SimdAdd,
  SimdSubtract,
  SimdMultiply,
  SimdDivide,
  SimdEqual,
  SimdLess,
  // Sealed initialized-place transfers: all owned inputs are prepared before
  // entry; eligible generated relocation cannot fail or invoke user code.
  RestoringExchange,
  RestoringSwap,
  RestoringIndexedSwap,
  EnumUnderlyingValue,
  RawBorrow,
  RawBorrowMut,
  RawPointerConversion,
  ObjectSize,
  ObjectAlignment,
  ObjectDestroy,
  ObjectConstruct,
  ObjectRelocate,
  ObjectRelocateRange,
  RequireVacantStorage,
  RequireLiveStorage,
  RequireRelocationCapacity,
  RequireRelocationSource,
  RequireRelocationDestination,
  TextViewFromAddress,
  CStringFromAddress,
  RequireAllocation,
  ThreadScope,
  Count,
};

// These checks consume a source-computed condition, not a storage object.
// The declaration-bound intrinsic selects the failure family; neither MIR
// nor a backend needs to inspect a container or rediscover its slot policy.
[[nodiscard]] inline std::optional<DefinedFailureOutcome>
intrinsicBooleanFailureOutcome(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::RequireAllocation:
    return DefinedFailureOutcome{DefinedFailureCode::AllocationFailure,
                                 DefinedFailureDetail::PrivateStorage};
  case IntrinsicKind::RequireKey:
    return DefinedFailureOutcome{DefinedFailureCode::KeyNotFound,
                                 DefinedFailureDetail::KeyLookup};
  case IntrinsicKind::RequireContainerArgument:
    return DefinedFailureOutcome{DefinedFailureCode::InvalidContainerArgument,
                                 DefinedFailureDetail::ContainerArgument};
  case IntrinsicKind::RequireVacantStorage:
    return DefinedFailureOutcome{DefinedFailureCode::InvalidStorageState,
                                 DefinedFailureDetail::DuplicateConstruction};
  case IntrinsicKind::RequireLiveStorage:
    return DefinedFailureOutcome{DefinedFailureCode::InvalidStorageState,
                                 DefinedFailureDetail::UninitializedAccess};
  case IntrinsicKind::RequireRelocationCapacity:
    return DefinedFailureOutcome{DefinedFailureCode::InvalidStorageState,
                                 DefinedFailureDetail::RelocationCapacity};
  case IntrinsicKind::RequireRelocationSource:
    return DefinedFailureOutcome{DefinedFailureCode::InvalidStorageState,
                                 DefinedFailureDetail::InvalidRelocationSource};
  case IntrinsicKind::RequireRelocationDestination:
    return DefinedFailureOutcome{
        DefinedFailureCode::InvalidStorageState,
        DefinedFailureDetail::OccupiedRelocationDestination};
  default:
    return std::nullopt;
  }
}

[[nodiscard]] constexpr bool simdIntrinsic(IntrinsicKind intrinsic) {
  return intrinsic >= IntrinsicKind::SimdAdd &&
         intrinsic <= IntrinsicKind::SimdLess;
}

[[nodiscard]] constexpr bool simdComparisonIntrinsic(IntrinsicKind intrinsic) {
  return intrinsic == IntrinsicKind::SimdEqual ||
         intrinsic == IntrinsicKind::SimdLess;
}

// A compiler-resolved source call site. These facts are language meaning, not
// a backend spelling: HIR captures them while expanding caller-side defaults,
// MIR preserves and verifies them, and each backend only chooses a concrete
// representation for the already-resolved values.
struct SourceLocationLiteral {
  std::string file;
  std::string function;
  std::uint32_t line = 0;
  std::uint32_t column = 0;

  friend bool operator==(const SourceLocationLiteral &,
                         const SourceLocationLiteral &) = default;
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
  TextView,
  CString,
  OptionalStorage,
  PolymorphicStorage,
  ThreadHandle,
  AtomicStorage,
  MutexStorage,
  MutexLockToken,
  SharedOwner,
  UninitializedStorage,
  NativeCScalar,
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

// One returned reference-bearing path and the exact caller-relative field
// path supplying it. Empty resultFields denotes a reference return itself.
// The semantic declaration owns this contract; HIR/MIR preserve it verbatim
// and verification checks it against executable return and call publications.
struct BorrowedFieldOrigin {
  std::vector<SymbolId> resultFields;
  BorrowOriginKind origin = BorrowOriginKind::None;
  std::size_t parameter = 0;
  std::vector<SymbolId> sourceFields;
  AccessMode access = AccessMode::ReadOnly;

  friend bool operator==(const BorrowedFieldOrigin &,
                         const BorrowedFieldOrigin &) = default;
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

// A source-derived precondition for turning an unknown raw input into a
// checked borrow. These are not loans: passing a pointer does not retain it.
// Formal indices survive concrete generic substitution and wrapper calls.
struct RawBorrowContract {
  std::size_t pointerParameter = 0;
  bool pointerReceiver = false;
  std::optional<std::size_t> ownerParameter;
  bool ownerReceiver = false;
  std::vector<PlaceProjection> ownerProjections;
  AccessMode access = AccessMode::ReadOnly;

  friend bool operator==(const RawBorrowContract &,
                         const RawBorrowContract &) = default;
};

struct FunctionInfo {
  FunctionId id = 0;
  SourceUnitId sourceUnit = 0;
  const FunctionDecl *declaration = nullptr;
  // The declaration owns the API; a separate definition owns executable
  // syntax, parameter bindings and definition-site lookup.
  const FunctionDecl *definition = nullptr;
  [[nodiscard]] const FunctionDecl *bodyDeclaration() const {
    return definition != nullptr ? definition : declaration;
  }
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
  bool neverReturns = false;
  bool tracksCaller = false;
  bool discardableResult = false;
  CThreadRuntimeOperation cThreadOperation = CThreadRuntimeOperation::None;
  ProcessMemoryRuntimeOperation processMemoryOperation =
      ProcessMemoryRuntimeOperation::None;
  // Derived from resolved calls, after all source bodies have been analyzed.
  bool requiresCThreadRuntime = false;
  std::vector<RawBorrowContract> rawBorrowContracts;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  std::optional<std::size_t> cArrayCountParameter;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  bool explicitReturnBorrowOrigin = false;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::vector<std::size_t> returnBorrowParameters;
  std::vector<BorrowedFieldOrigin> returnBorrowFields;
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

// One semantically selected host export. This is the first point where a
// manifest spelling becomes an exact GTI declaration and signature. Rows are
// canonicalized by public symbol so later phases never repeat name lookup or
// ABI eligibility decisions.
struct SemanticEmbeddingExport {
  FunctionId function = 0;
  std::string qualifiedName;
  std::string symbol;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  SourceSpan requestSource;

  friend bool operator==(const SemanticEmbeddingExport &,
                         const SemanticEmbeddingExport &) = default;
};

struct SemanticEmbeddingPlan {
  std::uint32_t abiVersion = embeddingAbiVersionV1;
  std::vector<SemanticEmbeddingExport> exports;

  friend bool operator==(const SemanticEmbeddingPlan &,
                         const SemanticEmbeddingPlan &) = default;
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
  // Exact concrete instance proven by ordinary body analysis to perform no
  // raw memory/address-arithmetic access. Unknown remains conservative.
  SemanticType rawAccessFreeType = SemanticType::Unknown;
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
  // Lexical ownership, not an enclosing object or lifetime dependency.
  ClassId enclosingClass = 0;
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
  // Active source declaration order; instance analysis resolves each alias in
  // the exact owner scope before HIR publishes a concrete class catalog.
  std::vector<const TypeAliasDecl *> typeAliases;
  std::optional<StoredReferenceInfo> storedReference;
  bool lifecycleBoundBorrowCarrier = false;
  // Separate trusted lock authority from general dependency retention.
  bool mutexGuardBorrowCarrier = false;
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
  std::optional<SemanticType> transferCondition;
  std::optional<SemanticType> shareCondition;
  bool compilerPrivate = false;
  // `[[reflect_private]]`: reflected access reaches private instance fields
  // from anywhere, so the representation must not hide them either.
  bool reflectPrivate = false;
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
  std::vector<GenericParameterInfo> genericParameters;
  SemanticType underlyingType = SemanticType::Int32;
  std::vector<EnumeratorInfo> enumerators;
  bool payload = false;
  bool compilerPrivate = false;
};

struct ResolvedEnumeratorInfo {
  EnumId owner = 0;
  SemanticType type = SemanticType::Unknown;
  const EnumeratorDecl *declaration = nullptr;
  EnumConstant value;
  std::size_t variantIndex = 0;
};

struct ResolvedPayloadConstructionInfo {
  EnumId owner = 0;
  SemanticType type = SemanticType::Unknown;
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
  SemanticType type = SemanticType::Unknown;
  const EnumeratorDecl *declaration = nullptr;
  std::size_t variantIndex = 0;
  std::vector<PayloadBindingInfo> bindings;
};

struct TypeAliasInfo {
  TypeAliasId id = 0;
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
  // Distinct formal inputs retained by a composed read-only carrier. Includes
  // the primary borrowParameter when there is more than one input.
  std::vector<std::size_t> borrowParameters;
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
  // Exact source base selected by the bounded generated-constructor schedule.
  // Zero means this base-only family is not selected; field-default schedules
  // are selected independently below.
  ConstructorId generatedDefaultBaseConstructor = 0;
  // Declaration defaults execute in one selected constructor body, so failure
  // can roll back earlier completed fields before leaving construction.
  bool generatedDefaultFieldInitializers = false;
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
  std::vector<std::size_t> borrowArguments;
  std::vector<BorrowedFieldOrigin> borrowFields;
  bool generatedDefault = false;
  ConstructorKind kind = ConstructorKind::Ordinary;
  // Exact same-type fresh-result initialization names the existing result
  // object. It selects no copy/move constructor and creates no second lifetime.
  bool directInitialization = false;
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
  // A native indirect target is a value with this exact C signature, never
  // a selected GTI function or a callback-adapter declaration.
  std::optional<SemanticType> nativeCalleeType;
  std::optional<CallableBoundary> callableBoundary;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<const Expr *> defaultArguments;
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<AppliedConceptRequirement> requirements;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  bool neverReturns = false;
  bool tracksCaller = false;
  SynchronizationOperation synchronization;
  std::optional<ResolvedThreadTaskEntry> threadTaskEntry;
  std::vector<ResolvedScopedThreadTask> scopedThreadTasks;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::vector<std::size_t> borrowArguments;
  std::vector<BorrowedFieldOrigin> borrowFields;
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
  // Trusted builtin-member forwarding normalizes the written receiver into
  // the first ordinary source-function argument. The AST still owns it; all
  // consumers must use resolvedCallArgument/count rather than written indices.
  const ExprPtr *argumentReceiver = nullptr;
};

[[nodiscard]] inline bool
validResolvedCallArguments(const Call &call, const ResolvedCallInfo *resolved) {
  if (resolved == nullptr || resolved->argumentReceiver == nullptr)
    return true;
  const auto *member = exprAs<Get>(call.callee().get());
  return member != nullptr && resolved->argumentReceiver == &member->object() &&
         resolved->function != 0 && resolved->declaration != nullptr &&
         resolved->intrinsic == IntrinsicKind::None &&
         resolved->dispatch == CallDispatch::Static &&
         resolved->receiverMode == CallReceiverMode::None;
}

[[nodiscard]] inline std::size_t
resolvedCallArgumentCount(const Call &call, const ResolvedCallInfo *resolved) {
  return validResolvedCallArguments(call, resolved)
             ? call.arguments().size() + (resolved != nullptr &&
                                          resolved->argumentReceiver != nullptr)
             : 0;
}

[[nodiscard]] inline const ExprPtr *
resolvedCallArgument(const Call &call, const ResolvedCallInfo *resolved,
                     std::size_t index) {
  if (!validResolvedCallArguments(call, resolved))
    return nullptr;
  if (resolved != nullptr && resolved->argumentReceiver != nullptr) {
    if (index == 0)
      return resolved->argumentReceiver;
    --index;
  }
  return index < call.arguments().size() ? &call.arguments()[index] : nullptr;
}

// Non-owning argument view shared by effects/provenance queries. Neither
// normalization nor a query copies/reanalyzes a source expression.
class ResolvedCallArguments {
public:
  ResolvedCallArguments() = default;
  ResolvedCallArguments(std::span<const ExprPtr> arguments)
      : written_(arguments) {}
  ResolvedCallArguments(const ExprList &arguments) : written_(arguments) {}
  ResolvedCallArguments(const Call &call, const ResolvedCallInfo *resolved)
      : call_(&call), resolved_(resolved) {}
  [[nodiscard]] std::size_t size() const {
    return call_ ? resolvedCallArgumentCount(*call_, resolved_)
                 : written_.size();
  }
  [[nodiscard]] const ExprPtr &operator[](std::size_t index) const {
    return call_ ? *resolvedCallArgument(*call_, resolved_, index)
                 : written_[index];
  }

private:
  const Call *call_ = nullptr;
  const ResolvedCallInfo *resolved_ = nullptr;
  std::span<const ExprPtr> written_;
};

// A lexical unsafe raw dereference returned under an explicit declaration
// contract. The owner is a resolved checked input, never inferred from the
// pointer's representation. HIR lowers this to the shared raw-borrow operation.
struct RawReferenceAssertionInfo {
  PlaceKey owner;
  SemanticType ownerReference = SemanticType::Unknown;
};

enum class ExpectedCombinatorKind {
  TransformError,
  Transform,
  AndThen,
  Expect
};

// One exact consuming expected<T, E> mapper. Expected is a
// language type, but the mapped alternative is still computed by an ordinary
// GTI function or lambda whose exact identity remains authoritative through
// lowering. Mapping selects exactly one mapper/lambda and retains its type.
// Expect instead retains the exact message type and selects the shared Panic
// intrinsic on its failure arm; it has no mapper target.
struct ExpectedCombinatorInfo {
  ExpectedCombinatorKind kind = ExpectedCombinatorKind::TransformError;
  SemanticType sourceType = SemanticType::Unknown;
  SemanticType resultType = SemanticType::Unknown;
  SemanticType mapperType = SemanticType::Unknown;
  FunctionId mapper = 0;
  LambdaId lambda = 0;
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
  bool inferResultFromInstantiation = false;
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
  std::vector<std::size_t> borrowArguments;
  std::vector<BorrowedFieldOrigin> borrowFields;
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
  // A source-only name inside an unevaluated member requirement. It never
  // denotes runtime storage and may have a concept-dependent type.
  RequirementParameter,
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
  // The expansion id of the declaring token (Token::expansion).
  std::uint32_t expansion = 0;
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

  // Identity ceiling, including the reserved prefix of an instance delta.
  [[nodiscard]] SymbolId symbolIdentityCeiling() const;

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
    std::uint32_t expansion = 0;

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
      result ^= std::hash<std::uint32_t>{}(key.expansion) + 0x9e3779b9U +
                (result << 6U) + (result >> 2U);
      return result;
    }
  };

  static int priority(SemanticOccurrenceKind kind);

  void clear();

  // Turns this database into an instance-analysis delta over baseDatabase:
  // lookups fall back to the base, and new symbol identities continue after
  // the base's so instance records never collide with base SymbolIds.
  void beginInstanceDelta(const SemanticDatabase &baseDatabase,
                          SymbolId reservedIdentityCeiling);

  void rebase(const SemanticDatabase *baseDatabase);

  void reserveIdentities(SymbolId ceiling);

  SymbolId recordSymbol(SymbolRecord symbol);

  void setSymbolDocumentation(SymbolId symbol,
                              std::string documentationMarkdown);

  [[nodiscard]] SymbolId
  symbolForDeclaration(SourceUnitId sourceUnit, const SourceSpan &span,
                       std::string_view generatedName = {},
                       std::uint32_t expansion = 0) const;

  void record(SemanticOccurrence occurrence);

  void setToolingOccurrencesEnabled(bool enabled);

  void finalize();
  void finalize(const std::unordered_set<SourceUnitId> &units);
  static void finalizeUnit(std::vector<SemanticOccurrence> &unitOccurrences);

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

// One element of an expanded `constexpr for`: the element type the binding
// was initialized with and the analysed copy of `{ binding; body }`.
struct ConstexprForIteration {
  SemanticType element;
  StmtPtr block;
};

struct ConstexprForExpansion {
  std::vector<ConstexprForIteration> iterations;
};

// One `[[ns::name(args)]]` application on a field or class-like declaration,
// evaluated to the constant object its arguments denote. The compiler gives
// it no meaning; libraries read it through <std/meta>.
struct ResolvedDeclarationAttribute {
  ClassId type = 0;
  SemanticType exactType = SemanticType::Unknown;
  ConstantValue value;
  SourceSpan span;
  const DeclarationAttribute *syntax = nullptr;
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

// Immutable selected-construction provenance. HIR may carry or copy a proof,
// but cannot manufacture one for a different source expression. Concrete
// instance models issue the same proof even when the primary skipped an arm.
class SemanticDirectInitializationProof {
public:
  SemanticDirectInitializationProof(const SemanticDirectInitializationProof &) =
      default;
  SemanticDirectInitializationProof(SemanticDirectInitializationProof &&) =
      default;
  SemanticDirectInitializationProof &
  operator=(const SemanticDirectInitializationProof &) = default;
  SemanticDirectInitializationProof &
  operator=(SemanticDirectInitializationProof &&) = default;

  [[nodiscard]] const Expr *source() const { return source_; }
  [[nodiscard]] const Expr *producer() const { return producer_; }
  [[nodiscard]] const SemanticType &type() const { return type_; }
  [[nodiscard]] std::uint64_t snapshot() const { return snapshot_; }

private:
  friend class SemanticModel;
  SemanticDirectInitializationProof(const Expr &source, const Expr &producer,
                                    SemanticType type, std::uint64_t snapshot)
      : source_(&source), producer_(&producer), type_(std::move(type)),
        snapshot_(snapshot) {}

  const Expr *source_;
  const Expr *producer_;
  SemanticType type_;
  std::uint64_t snapshot_;
};

struct SemanticFullExpression {
  std::size_t order = 0;
  const Stmt *statement = nullptr;
  const ConstructorInitializer *constructorInitializer = nullptr;
  std::vector<const Expr *> roots;
};

struct ImplicitReturnMoveInfo {
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  AccessMode borrowAccess = AccessMode::ReadOnly;
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
  // Source-free empty construction anchors are owned across immutable copies
  // and instance deltas, just like semantic-generated constructor syntax.
  [[nodiscard]] const ExprPtr &
  implicitFieldInitializer(const VariableDecl &field) const;
  [[nodiscard]] const ExprPtr &
  resolvedFieldInitializer(const VariableDecl &field) const;
  void retainImplicitFieldInitializer(const VariableDecl &field);
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

  [[nodiscard]] const CallerLocationOrigin *
  findCallerLocation(const Expr &expression) const;

  [[nodiscard]] const RawReferenceAssertionInfo *
  findRawReferenceAssertion(const Expr &expression) const;

  [[nodiscard]] const ProgramInitializationPlan &
  programInitializationPlan() const;

  [[nodiscard]] const std::optional<HostedProgramEntryPlan> &
  hostedProgramEntryPlan() const;

  [[nodiscard]] ProgramKind programKind() const;

  [[nodiscard]] const std::optional<SemanticEmbeddingPlan> &
  embeddingPlan() const;
  [[nodiscard]] bool hasNativeFunctionConversions() const;

  [[nodiscard]] bool
  isProgramConstantSubstitution(const Expr &expression) const;

  [[nodiscard]] const PlaceKey *findPlace(const Expr &expression) const;

  [[nodiscard]] const OwnershipEvent *
  findOwnershipEvent(const Expr &expression) const;

  [[nodiscard]] const ImplicitReturnMoveInfo *
  findImplicitReturnMove(const Expr &expression) const;

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

  // The expansion of a `constexpr for`; nullptr while the primary analysis
  // deferred it to concrete instances or when it was never analysed.
  [[nodiscard]] const ConstexprForExpansion *
  findConstexprForExpansion(const ConstexprForStmt &statement) const;
  // The declared attributes of a field or class-like declaration; nullptr
  // when none were recorded.
  [[nodiscard]] const std::vector<ResolvedDeclarationAttribute> *
  findDeclarationAttributes(const Stmt &declaration) const;

  // An expression that semantic analysis synthesized to stand for another
  // (reflected field access analysed as the field projection it denotes).
  // Lowering and classification follow the alias; the original keeps its
  // own records for the passes that read the call shape.
  [[nodiscard]] const Expr *findExpressionAlias(const Expr &expression) const;

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

  // Primary body facts are required before concrete HIR discovery may use a
  // function definition. Invalid primary analysis never reaches HIR.
  void recordAnalyzedFunctionBody(const FunctionDecl &body) {
    analyzedFunctionBodies.insert(&body);
  }
  [[nodiscard]] bool hasAnalyzedFunctionBody(const FunctionDecl &body) const {
    return analyzedFunctionBodies.contains(&body) ||
           (base != nullptr && base->hasAnalyzedFunctionBody(body));
  }
  // A late demand that could not stay equivalent to in-pass analysis
  // withdraws its bodies, so concrete discovery requests them instead.
  void forgetAnalyzedFunctionBody(const FunctionDecl &body) {
    analyzedFunctionBodies.erase(&body);
  }

  // While a journal is set, every recorded call resolution records its call.
  // Late demand scopes it to finalize exactly the calls one batch recorded.
  std::unordered_set<const Call *> *
  setCallJournal(std::unordered_set<const Call *> *journal) {
    auto *previous = callJournal;
    callJournal = journal;
    return previous;
  }

  // Continues symbol identities above an instance-delta ceiling (late
  // demand), so new base symbols never collide with concrete-only symbols of
  // an existing delta. The skipped identities hold inert placeholder records
  // (source unit 0) that no declaration resolves to.
  void reserveSymbolIdentities(SymbolId ceiling);

  // While a sink is set, every successful function lookup records its identity.
  // Body analysis scopes this side channel; completed snapshots retain no sink.
  std::unordered_set<FunctionId> *
  setFunctionDemandSink(std::unordered_set<FunctionId> *sink) const {
    auto *previous = functionDemandSink;
    functionDemandSink = sink;
    return previous;
  }

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

  // Exact declaration-owned signatures retained by semantic substitution.
  // Snapshot consumers do not re-run generic substitution or infer it from HIR.
  [[nodiscard]] std::optional<EnumTypeInfo>
  enumTypeFor(const SemanticType &type) const;

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
  [[nodiscard]] const ExpectedCombinatorInfo *
  findExpectedCombinator(const Call &call) const;
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

  // A fresh class result is a newly produced object, not a value-category
  // transfer from an existing place (notably the Move intrinsic).
  [[nodiscard]] bool isFreshClassResult(const Expr &expression) const;
  // A comma expression with no selected operator.
  [[nodiscard]] const Binary *builtinComma(const Expr &expression) const;
  // A class or Expected value grouping or comma, or a selected fresh-result
  // construction wrapper.
  [[nodiscard]] const Expr *
  directInitializationSource(const Expr &expression) const;

  [[nodiscard]] std::optional<SemanticDirectInitializationProof>
  directInitializationProof(const Expr &expression) const;

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

  void setProgramKind(ProgramKind kind);

  void setEmbeddingPlan(std::optional<SemanticEmbeddingPlan> plan);

  void recordProgramConstantSubstitution(const Expr &expression);
  void clearProgramConstantSubstitution(const Expr &expression);

  // Turns this model into an instance-analysis delta over baseModel: reads
  // fall back to the base while writes stay local, so concrete instance
  // reanalysis records only what it produces instead of copying the whole
  // program's model. Loan tables deliberately do not fall back - instance
  // analysis restarts loan identities, mirroring the clearLoans() semantics
  // the previous whole-model copy relied on.
  void beginInstanceDelta(const SemanticModel &baseModel,
                          SymbolId reservedIdentityCeiling = 0);

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

  void recordCallerLocation(const Expr &expression,
                            CallerLocationOrigin origin);

  void recordRawReferenceAssertion(const Expr &expression,
                                   RawReferenceAssertionInfo info);

  void recordPlace(const Expr &expression, PlaceKey place);

  void recordOwnershipEvent(const Expr &expression, OwnershipEvent event);

  void recordImplicitReturnMove(const Expr &expression,
                                ImplicitReturnMoveInfo info);

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

  void recordConstexprBranch(const IfStmt &statement,
                             std::optional<bool> thenBranch);

  void
  recordConstexprForExpansion(const ConstexprForStmt &statement,
                              std::optional<ConstexprForExpansion> expansion);
  void recordDeclarationAttributes(
      const Stmt &declaration,
      std::vector<ResolvedDeclarationAttribute> attributes);

  const Expr &recordExpressionAlias(const Expr &expression,
                                    std::shared_ptr<const Expr> alias);

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
  void bindFunctionDefinition(const FunctionDecl &declaration,
                              const FunctionDecl &definition);

  void record(const Lambda &declaration, LambdaInfo info);

  void recordClassType(const ClassDecl &declaration, ClassTypeInfo info);

  void
  recordExactClassSpecialization(ClassId primary, ClassId specialization,
                                 std::vector<SemanticType> typeArguments,
                                 std::vector<CompileTimeValue> valueArguments);

  void record(const TypeAliasDecl &declaration, TypeAliasInfo info);

  void recordEnumType(const EnumDecl &declaration, EnumTypeInfo info);

  void recordEnumInstanceType(SemanticType type, EnumTypeInfo info);

  void record(const QualifiedName &expression, ResolvedEnumeratorInfo info);

  void recordSwitchCase(const Expr &expression, SwitchCaseValue value);

  void recordExhaustiveSwitch(const SwitchStmt &statement);

  void recordPayloadConstruction(const Call &call,
                                 ResolvedPayloadConstructionInfo info);

  void recordPayloadPattern(const Expr &expression,
                            ResolvedPayloadPatternInfo info);

  void record(const Call &call, ResolvedCallInfo info);
  void recordExpectedCombinator(const Call &call, ExpectedCombinatorInfo info);
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
                       std::string_view generatedName = {},
                       std::uint32_t expansion = 0) const;

  // AST copies made by compile-time expansion. They are referenced by HIR
  // and by instance-analysis deltas, so the model that outlives both owns
  // them.
  void
  adoptExpansionArena(std::vector<StmtPtr> statements,
                      std::vector<std::shared_ptr<const Expr>> expressions);

  void recordOccurrence(SemanticOccurrence occurrence);

  // Occurrences answer editor position queries (hover, definition, semantic
  // tokens) and are consumed only by language queries and the LSP. Symbols
  // stay recorded either way because HIR and the emitter resolve member
  // identity through them. A compile-only consumer disables occurrences so
  // analysis does not build, sort, and retain a table nothing will read.
  void setToolingOccurrencesEnabled(bool enabled);

  void recordCompletion(SemanticCompletionContext context);

  void finalizeOccurrences();
  // One late-demand batch: only the units whose occurrences it extended.
  void finalizeOccurrences(const std::unordered_set<SourceUnitId> &units);
  [[nodiscard]] std::unordered_map<SourceUnitId, std::size_t>
  occurrenceCounts() const;

  void finalizeCallableArguments(ResolvedCallInfo &call) const;

  void finalizeCallableArguments();
  // One late-demand batch: the calls it recorded and the occurrences of the
  // units it extended.
  void finalizeCallableArguments(const std::unordered_set<const Call *> &calls,
                                 const std::unordered_set<SourceUnitId> &units);

  void finalizeCallableForwardings();

  [[nodiscard]] const FunctionDecl &
  canonicalFunctionDeclaration(const FunctionDecl &declaration) const;

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
  std::unordered_map<const Expr *, CallerLocationOrigin> callerLocations;
  ProgramInitializationPlan programInitialization;
  std::optional<HostedProgramEntryPlan> hostedProgramEntry;
  ProgramKind programKind_ = ProgramKind::Executable;
  std::optional<SemanticEmbeddingPlan> embeddingPlan_;
  std::unordered_set<const Expr *> programConstantSubstitutions;
  std::unordered_map<const Expr *, PlaceKey> places;
  std::unordered_map<const Expr *, OwnershipEvent> ownershipEvents;
  std::unordered_map<const Expr *, ImplicitReturnMoveInfo> implicitReturnMoves;
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
  // nullopt is an explicitly deferred primary selection, not a false branch.
  std::unordered_map<const IfStmt *, std::optional<bool>> constexprBranches;
  std::unordered_map<const ConstexprForStmt *,
                     std::optional<ConstexprForExpansion>>
      constexprForExpansions;
  std::unordered_map<const Stmt *, std::vector<ResolvedDeclarationAttribute>>
      declarationAttributes;
  std::unordered_map<const Expr *, std::shared_ptr<const Expr>>
      expressionAliases;
  std::vector<StmtPtr> expansionArena;
  std::vector<std::shared_ptr<const Expr>> expressionArena;
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
  std::unordered_map<const FunctionDecl *, const FunctionDecl *>
      functionDeclarations;
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
  std::vector<std::pair<SemanticType, EnumTypeInfo>> enumInstanceTypes;
  std::unordered_map<EnumId, const EnumDecl *> enumTypesById;
  std::unordered_map<const QualifiedName *, ResolvedEnumeratorInfo> enumerators;
  std::unordered_map<const Expr *, SwitchCaseValue> switchCases;
  std::unordered_set<const SwitchStmt *> exhaustiveSwitches;
  std::unordered_map<const Call *, ResolvedPayloadConstructionInfo>
      payloadConstructions;
  std::unordered_map<const Expr *, ResolvedPayloadPatternInfo> payloadPatterns;
  std::unordered_map<const Call *, ResolvedCallInfo> calls;
  std::unordered_map<const Expr *, RawReferenceAssertionInfo>
      rawReferenceAssertions;
  std::unordered_map<const Call *, ExpectedCombinatorInfo> expectedCombinators;
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
  std::unordered_map<const VariableDecl *, std::shared_ptr<const ExprPtr>>
      implicitFieldInitializers;
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
  std::unordered_set<const FunctionDecl *> analyzedFunctionBodies;
  mutable std::unordered_set<FunctionId> *functionDemandSink = nullptr;
  std::unordered_set<const Call *> *callJournal = nullptr;
};

class SemanticTypePrinter {
public:
  using GenericParameterLookup =
      std::function<const GenericParameterInfo *(GenericParameterId)>;
  explicit SemanticTypePrinter(const SemanticModel &semantics);

  [[nodiscard]] std::string print(const SemanticType &type) const;
  // Registration may supply declaration-owned parameters not yet published
  // in the completed model. The lookup only supplies spelling and is not kept.
  [[nodiscard]] std::string print(const SemanticType &type,
                                  const GenericParameterLookup &lookup) const;

private:
  const SemanticModel &semantics;
};

struct SemanticInstanceAnalysis {
  SemanticModel model;
  std::vector<SemanticDiagnostic> diagnostics;

  [[nodiscard]] bool valid() const { return !hasErrorDiagnostics(diagnostics); }
};

// Shared semantic substitution kernel. Consumers preserve declaration-bound
// parameter identities; an optional semantic catalog resolves exact nominal
// specializations after substitution, including nested type arguments.
[[nodiscard]] SemanticType
substituteSemanticType(const SemanticType &type,
                       const GenericSubstitution &substitution,
                       const SemanticModel *model = nullptr);

} // namespace lang
