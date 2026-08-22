#pragma once

#include "gti/failure_metadata.h"
#include "gti/hir.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

using MirBlockId = std::size_t;
using MirPlaceId = std::size_t;
using MirLoanId = std::size_t;
using MirInstructionId = std::size_t;
using MirValueId = std::size_t;
using MirTemporaryId = std::size_t;
using MirDropObligationId = std::size_t;
using MirFailureRecordId = std::size_t;
using MirHostedStartupOperationId = std::size_t;
using MirNativeCallbackAdapterId = std::size_t;

struct MirFullExpression {
  HirFullExpressionId id = 0;
  HirFullExpressionId hirExpression = 0;
  HirStatementId statement = 0;
  std::size_t constructorInitializer = 0;
  std::vector<HirValueId> roots;

  friend bool operator==(const MirFullExpression &,
                         const MirFullExpression &) = default;
};

enum class MirCleanupBoundaryKind {
  Normal,
  Failure,
  Count,
};

struct MirCleanupBoundary {
  std::size_t id = 0;
  MirHostedStartupOperationId hostedStartupOperation = 0;
  MirCleanupBoundaryKind kind = MirCleanupBoundaryKind::Normal;
  std::vector<MirDropObligationId> obligations;

  friend bool operator==(const MirCleanupBoundary &,
                         const MirCleanupBoundary &) = default;
};

enum class MirBodyKind {
  Module,
  FieldInitializers,
  StaticFieldInitializers,
  Function,
  Constructor,
  Destructor,
  Lambda,
  HostedStartup,
};

// Stable program-local address of one MIR body. `owner` is zero only for the
// module body. HostedStartup uses the exact nonzero entry function instance;
// every other kind uses its corresponding concrete instance ID.
struct MirBodyAddress {
  MirBodyKind kind = MirBodyKind::Module;
  std::size_t owner = 0;

  friend bool operator==(const MirBodyAddress &,
                         const MirBodyAddress &) = default;
};

enum class MirPlaceRootKind {
  Binding,
  Symbol,
  This,
  Temporary,
  Value,
  Loan,
};

enum class MirProjectionKind {
  Field,
  Index,
  Dereference,
  RawIndex,
  RawDereference,
  PackElement,
};

struct MirPlaceProjection {
  MirProjectionKind kind = MirProjectionKind::Field;
  SymbolId field = 0;
  MirValueId index = 0;
  std::optional<std::uint64_t> constantIndex;
  std::size_t selection = 0;

  friend bool operator==(const MirPlaceProjection &,
                         const MirPlaceProjection &) = default;
};

struct MirPlace {
  MirPlaceId id = 0;
  MirHostedStartupOperationId hostedStartupOperation = 0;
  MirPlaceRootKind root = MirPlaceRootKind::Value;
  HirBindingId binding = 0;
  SymbolId symbol = 0;
  std::size_t capture = 0;
  MirTemporaryId temporary = 0;
  MirValueId value = 0;
  MirLoanId loan = 0;
  std::vector<MirPlaceProjection> projections;
  SemanticType type = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  SemanticTypeTraits traits{};
  HirValueId sourceValue = 0;
  std::optional<PlaceKey> key;
  bool initiallyAvailable = false;

  friend bool operator==(const MirPlace &, const MirPlace &) = default;
};

enum class MirOperandKind {
  Value,
  Constant,
  Address,
  Copy,
  Move,
  BorrowRead,
  BorrowWrite,
  Loan,
};

struct MirOperand {
  MirOperandKind kind = MirOperandKind::Value;
  MirValueId value = 0;
  MirPlaceId place = 0;
  MirLoanId loan = 0;
  std::optional<Literal> literal;
  SemanticType type = SemanticType::Unknown;

  friend bool operator==(const MirOperand &, const MirOperand &) = default;
};

enum class MirLoanKind {
  Local,
  CallResult,
  Stored,
  Return,
  Parameter,
};

struct MirLoan {
  MirLoanId id = 0;
  SemanticLoanId semanticLoan = 0;
  MirLoanId parent = 0;
  MirLoanKind kind = MirLoanKind::Local;
  MirPlaceId source = 0;
  AccessMode access = AccessMode::ReadOnly;
  HirValueId producedBy = 0;
  std::vector<HirBindingId> carriers;
  SymbolId storedField = 0;
  bool entry = false;
  bool escapes = false;

  friend bool operator==(const MirLoan &, const MirLoan &) = default;
};

enum class MirDropObligationKind {
  Binding,
  Value,
  PreparedParameter,
  // A completed constructor subobject transferred into `this`. Armed by the
  // stage-completing Initialize, drained in reverse stage order on every
  // defined-failure edge of the constructor body, and retired by transfer
  // to the caller on normal completion. Compiler-generated: it has no HIR
  // obligation or full-expression identity, and its place is the exact
  // This-rooted field place of its stage.
  ConstructionRollback,
};

struct MirDropType {
  SemanticType type = SemanticType::Unknown;
  std::optional<HirClassInstanceId> classInstance;
  std::optional<HirLambdaId> lambdaInstance;
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveCleanup = false;

  friend bool operator==(const MirDropType &, const MirDropType &) = default;
};

struct MirDropObligation {
  MirDropObligationId id = 0;
  MirHostedStartupOperationId hostedStartupOperation = 0;
  HirDropObligationId hirObligation = 0;
  std::size_t constructionOrder = 0;
  MirDropObligationKind kind = MirDropObligationKind::Value;
  MirPlaceId place = 0;
  HirBindingId binding = 0;
  HirValueId value = 0;
  MirValueId generatedValue = 0;
  HirFullExpressionId hirFullExpression = 0;
  HirFullExpressionId fullExpression = 0;
  MirDropType dropType;
  bool initiallyActive = false;

  friend bool operator==(const MirDropObligation &,
                         const MirDropObligation &) = default;
};

enum class MirLifecycleEventKind {
  Initialize,
  Move,
  Reparent,
  Replace,
  TransferOut,
  Drop,
};

struct MirLifecycleEvent {
  MirLifecycleEventKind kind = MirLifecycleEventKind::Initialize;
  MirDropObligationId source = 0;
  MirDropObligationId target = 0;
  bool conditional = false;
  bool failureCleanup = false;

  friend bool operator==(const MirLifecycleEvent &,
                         const MirLifecycleEvent &) = default;
};

enum class MirInstructionKind {
  Compute,
  Load,
  Initialize,
  Assign,
  Modify,
  Move,
  Borrow,
  CallInput,
  Call,
  Construct,
  Drop,
  EndBorrow,
  Lifecycle,
  CallBody,
  Count,
};

enum class MirCallInputRole {
  Receiver,
  Argument,
};

enum class MirOperation {
  None,
  Literal,
  EnumConstant,
  Aggregate,
  Index,
  Identity,
  Convert,
  ExpectedHasValue,
  Closure,
  NativeCallback,
  PayloadConstruct,
  PayloadExtract,
  Unexpected,
  AddressOf,
  PointerAdd,
  PointerSubtract,
  PointerDifference,
  Comma,
  Add,
  Subtract,
  Multiply,
  Divide,
  Remainder,
  BitwiseAnd,
  BitwiseOr,
  BitwiseXor,
  ShiftLeft,
  ShiftRight,
  Equal,
  NotEqual,
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  Positive,
  Negate,
  LogicalNot,
  BitwiseNot,
  Assign,
  AddAssign,
  SubtractAssign,
  MultiplyAssign,
  DivideAssign,
  RemainderAssign,
  BitwiseAndAssign,
  BitwiseOrAssign,
  BitwiseXorAssign,
  ShiftLeftAssign,
  ShiftRightAssign,
  PreIncrement,
  PreDecrement,
  PostIncrement,
  PostDecrement,
  Count,
};

enum class MirLiteralProvenanceKind {
  None,
  Source,
  IdentityFold,
  ComputeFold,
  Count,
};

// Every MIR Compute/Literal instruction says whether it came directly from
// lowering or from a verified optimizer rewrite. A rewrite retains its
// original MIR input rather than asking a backend to recover transformation
// authority from HIR. The provenance is proof-only metadata. An identity
// fold traces its dominating source value through same-typed identities to
// the exact literal. A compute fold retains the folded operation and its
// exact operand values, each of which must dominate the rewritten
// instruction and carry a literal the operation re-evaluates to this
// instruction's literal.
struct MirLiteralProvenance {
  MirLiteralProvenanceKind kind = MirLiteralProvenanceKind::None;
  MirValueId sourceValue = 0;
  MirOperation sourceOperation = MirOperation::Literal;
  std::vector<MirValueId> sourceValues;

  friend bool operator==(const MirLiteralProvenance &,
                         const MirLiteralProvenance &) = default;
};

struct MirInstruction {
  MirInstructionId id = 0;
  MirInstructionKind kind = MirInstructionKind::Compute;
  MirHostedStartupOperationId hostedStartupOperation = 0;
  HirValueId hirValue = 0;
  HirStatementId hirStatement = 0;
  HirValueId callSite = 0;
  // One-based source constructor-initializer stage. This is zero for every
  // instruction outside a constructor prologue and lets verification and a
  // backend bind a field initialization to its exact MIR schedule without
  // reopening AST expressions.
  std::size_t constructorInitializer = 0;
  std::optional<MirCallInputRole> callInputRole;
  std::size_t callInputIndex = 0;
  HirCallInputKind callInputKind = HirCallInputKind::Value;
  bool defaultArgument = false;
  std::optional<MirDropObligationId> preparedParameterDrop;
  std::optional<MirDropObligationId> successResultDrop;
  // A failure-capable call or construction whose result directly initializes
  // this binding publishes the value only on its Invoke success successor.
  // The successor must contain the unique matching Initialize instruction.
  std::optional<MirPlaceId> successResultDestination;
  UnsafeOperationKind unsafeOperation = UnsafeOperationKind::None;
  bool rawMemoryAccess = false;
  std::optional<MirValueId> result;
  std::optional<MirPlaceId> destination;
  std::optional<MirOperand> receiver;
  std::vector<MirOperand> operands;
  std::vector<SemanticType> parameterTypes;
  std::vector<SemanticType> closureCaptureTypes;
  std::vector<LambdaCaptureMode> closureCaptureModes;
  std::optional<MirLoanId> loan;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowArgument = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  std::optional<BorrowOriginPlace> borrowPlace;
  MirOperation operation = MirOperation::None;
  std::optional<Literal> literal;
  MirLiteralProvenance literalProvenance;
  bool programConstantSubstitution = false;
  std::optional<EnumId> enumOwner;
  std::optional<EnumConstant> enumValue;
  std::optional<std::size_t> enumVariant;
  std::optional<std::size_t> payloadIndex;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  SynchronizationOperation synchronization;
  DefinedFailureOperation definedFailure;
  std::vector<FailureSiteId> localFailureSites;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<MirNativeCallbackAdapterId> nativeCallbackAdapter;
  std::optional<HirConstructorInstanceId> constructorTarget;
  std::optional<MirBodyAddress> bodyTarget;
  ConstructorKind constructorKind = ConstructorKind::Ordinary;
  std::optional<HirLambdaId> lambdaTarget;
  std::vector<CallableArgumentBoundary> callableArguments;
  std::optional<CallableBoundary> callableBoundary;
  std::optional<CallableInvocationCapability> callableInvocation;
  ExpressionInfo info;
  std::optional<OwnershipEvent> ownership;
  std::vector<MirLifecycleEvent> lifecycle;
  HirFullExpressionId fullExpressionEnd = 0;
  std::size_t cleanupBoundaryEnd = 0;

  friend bool operator==(const MirInstruction &,
                         const MirInstruction &) = default;
};

struct MirProgramConstantSubstitution {
  HirValueId hirValue = 0;
  ConstantValue constant;

  friend bool operator==(const MirProgramConstantSubstitution &,
                         const MirProgramConstantSubstitution &) = default;
};

enum class MirTerminatorKind {
  None,
  Goto,
  Branch,
  Switch,
  Invoke,
  Return,
  PropagateFailure,
  Unreachable,
  Exit,
  ContainFailure,
  TerminateCleanupFailure,
};

struct MirSwitchTarget {
  std::optional<SwitchCaseValue> value;
  MirBlockId target = 0;

  friend bool operator==(const MirSwitchTarget &,
                         const MirSwitchTarget &) = default;
};

enum class MirTerminatorProvenanceKind {
  None,
  BranchFold,
  Count,
};

// A rewritten terminator retains replayable proof exactly like a rewritten
// literal. A branch fold keeps the folded branch's condition value: the
// verifier re-reads that value's dominating literal and re-selects the
// taken target, so a Goto produced by the optimizer can never disagree
// with the branch it replaced.
struct MirTerminatorProvenance {
  MirTerminatorProvenanceKind kind = MirTerminatorProvenanceKind::None;
  MirValueId foldSourceValue = 0;

  friend bool operator==(const MirTerminatorProvenance &,
                         const MirTerminatorProvenance &) = default;
};

struct MirTerminator {
  MirTerminatorKind kind = MirTerminatorKind::None;
  MirHostedStartupOperationId hostedStartupOperation = 0;
  HirValueId hirValue = 0;
  HirStatementId hirStatement = 0;
  std::optional<MirOperand> value;
  std::optional<MirLoanId> returnLoan;
  MirInstructionId invokeInstruction = 0;
  MirFailureRecordId failureRecord = 0;
  MirBlockId target = 0;
  MirBlockId elseTarget = 0;
  std::vector<MirSwitchTarget> switchTargets;
  std::vector<MirLifecycleEvent> successLifecycle;
  MirTerminatorProvenance provenance;

  friend bool operator==(const MirTerminator &,
                         const MirTerminator &) = default;
};

struct MirBlock {
  MirBlockId id = 0;
  ProgramInitializationStepId programInitializationStep = 0;
  MirFailureRecordId failureParameter = 0;
  MirFailureRecordId activeFailure = 0;
  std::vector<MirInstruction> instructions;
  MirTerminator terminator;
  bool reachable = false;

  friend bool operator==(const MirBlock &, const MirBlock &) = default;
};

struct MirFailureRecord {
  MirFailureRecordId id = 0;
  MirHostedStartupOperationId hostedStartupOperation = 0;
  MirBlockId producerBlock = 0;
  MirInstructionId producerInstruction = 0;
  MirBlockId parameterBlock = 0;

  friend bool operator==(const MirFailureRecord &,
                         const MirFailureRecord &) = default;
};

struct MirValue {
  MirValueId id = 0;
  MirHostedStartupOperationId hostedStartupOperation = 0;
  HirValueId sourceValue = 0;
  ExpressionInfo info;
  MirBlockId definitionBlock = 0;
  MirInstructionId definition = 0;

  friend bool operator==(const MirValue &, const MirValue &) = default;
};

enum class MirValueUseKind {
  InstructionOperand,
  InstructionReceiver,
  Terminator,
  PlaceRoot,
  PlaceIndex,
};

struct MirValueUse {
  MirValueId value = 0;
  MirValueUseKind kind = MirValueUseKind::InstructionOperand;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirPlaceId place = 0;
  std::size_t operandIndex = 0;

  friend bool operator==(const MirValueUse &, const MirValueUse &) = default;
};

struct MirBody {
  MirBodyKind kind = MirBodyKind::Function;
  PlaceDomain placeDomain;
  MirBlockId entry = 0;
  SemanticType returnType = SemanticType::Void;
  std::vector<MirBlock> blocks;
  std::vector<MirPlace> places;
  std::vector<MirLoan> loans;
  std::vector<MirFullExpression> fullExpressions;
  std::vector<MirCleanupBoundary> cleanupBoundaries;
  std::vector<MirDropObligation> dropObligations;
  std::vector<MirFailureRecord> failureRecords;
  std::vector<MirProgramConstantSubstitution> programConstantSubstitutions;
  std::vector<MirValue> values;
  std::vector<std::vector<MirValueUse>> valueUses;

  [[nodiscard]] const MirBlock *findBlock(MirBlockId id) const {
    return id == 0 || id > blocks.size() ? nullptr : &blocks[id - 1];
  }

  [[nodiscard]] const MirPlace *findPlace(MirPlaceId id) const {
    return id == 0 || id > places.size() ? nullptr : &places[id - 1];
  }

  [[nodiscard]] const MirLoan *findLoan(MirLoanId id) const {
    return id == 0 || id > loans.size() ? nullptr : &loans[id - 1];
  }

  [[nodiscard]] const MirDropObligation *
  findDropObligation(MirDropObligationId id) const {
    return id == 0 || id > dropObligations.size() ? nullptr
                                                  : &dropObligations[id - 1];
  }

  [[nodiscard]] const MirFailureRecord *
  findFailureRecord(MirFailureRecordId id) const {
    return id == 0 || id > failureRecords.size() ? nullptr
                                                 : &failureRecords[id - 1];
  }

  [[nodiscard]] const MirValue *findValue(MirValueId id) const {
    return id == 0 || id > values.size() ? nullptr : &values[id - 1];
  }

  [[nodiscard]] const std::vector<MirValueUse> &usesOf(MirValueId id) const;

  [[nodiscard]] std::size_t instructionCount() const;

  friend bool operator==(const MirBody &, const MirBody &) = default;
};

struct MirVerificationError {
  MirBodyKind bodyKind = MirBodyKind::Function;
  std::size_t owner = 0;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::string message;
};

struct MirVerificationResult {
  std::vector<MirVerificationError> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

void rebuildMirReachability(MirBody &body);
[[nodiscard]] bool rebuildMirValueUses(MirBody &body);
[[nodiscard]] bool supportsMirFailureControlFlow(MirBodyKind kind);
// Body-level refinement: a constructor whose lowering silently transferred a
// subobject into `this` without arming rollback routes no defined-failure
// edges at all.
[[nodiscard]] bool mirBodyRoutesFailureEdges(const MirBody &body);
enum class MirFailureControlFlowPosition {
  None,
  FullExpressionRoot,
  PreparedCallArgumentRoot,
};
[[nodiscard]] bool
requiresMirFailureControlFlow(const MirInstruction &instruction,
                              MirFailureControlFlowPosition position);
[[nodiscard]] MirVerificationResult verifyMirBody(const MirBody &body,
                                                  std::size_t owner = 0);

struct MirFieldDrop {
  HirBindingId field = 0;
  SymbolId symbol = 0;
  SemanticType type = SemanticType::Unknown;
  bool requiresActiveCleanup = false;

  friend bool operator==(const MirFieldDrop &, const MirFieldDrop &) = default;
};

struct MirClassFieldLifecycle {
  HirBindingId field = 0;
  SymbolId symbol = 0;
  SemanticType type = SemanticType::Unknown;
  DropKind dropKind = DropKind::Trivial;
  bool requiresActiveCleanup = false;

  friend bool operator==(const MirClassFieldLifecycle &,
                         const MirClassFieldLifecycle &) = default;
};

struct MirClassFieldInfo {
  HirBindingId field = 0;
  SymbolId symbol = 0;
  SemanticType type = SemanticType::Unknown;
  DropKind dropKind = DropKind::Trivial;
  bool requiresActiveCleanup = false;

  friend bool operator==(const MirClassFieldInfo &,
                         const MirClassFieldInfo &) = default;
};

struct MirClassInstance {
  HirClassInstanceId id = 0;
  ClassId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  std::vector<HirBaseInstance> structuralBases;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  std::optional<CAbiRecordLayout> cAbiLayout;
  std::optional<UnionLayout> unionLayout;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus destructorStatus = SpecialMemberStatus::Generated;
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveDropState = false;
  bool requiresActiveCleanup = false;
  std::vector<MirClassFieldInfo> declaredFields;
  std::vector<MirClassFieldLifecycle> fields;
  MirBody fieldInitializers;
  MirBody staticFieldInitializers;
  std::vector<MirFieldDrop> fieldDropOrder;

  friend bool operator==(const MirClassInstance &,
                         const MirClassInstance &) = default;
};

struct MirCallableSignature {
  SemanticType returnType = SemanticType::Void;
  std::vector<SemanticType> parameterTypes;
  CallableInvocationCapability requiredCapability =
      CallableInvocationCapability::Read;
  std::optional<CallableInvocationCapability> selectedCapability;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirLambdaId> lambdaTarget;

  friend bool operator==(const MirCallableSignature &,
                         const MirCallableSignature &) = default;
};

struct MirCallableForwarding {
  std::size_t parameterIndex = 0;
  std::optional<HirFunctionInstanceId> functionTarget;

  friend bool operator==(const MirCallableForwarding &,
                         const MirCallableForwarding &) = default;
};

struct MirCallableParameter {
  std::size_t parameterIndex = 0;
  SemanticType callableType = SemanticType::Unknown;
  AccessMode access = AccessMode::ReadOnly;
  CallableBoundary boundary = CallableBoundary::Confined;
  std::optional<CallableOwnedTransport> ownedTransport;
  std::vector<MirCallableSignature> signatures;
  std::vector<MirCallableForwarding> forwardings;

  friend bool operator==(const MirCallableParameter &,
                         const MirCallableParameter &) = default;
};

enum class MirDefinitionKind {
  Source,
  RuntimeBinding,
  Declaration,
};

struct MirFunctionInstance {
  HirFunctionInstanceId id = 0;
  FunctionId declaration = 0;
  // The instance's substituted generic arguments in declaration order;
  // empty for non-generic declarations. Emission spells them explicitly
  // where C++ deduction cannot recover them from the parameter types.
  std::vector<SemanticType> typeArguments;
  std::optional<HirClassInstanceId> owner;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  ProgramEntryKind entryKind = ProgramEntryKind::None;
  std::optional<HirFunctionInstanceId> entryArgumentAppendTarget;
  bool staticMember = false;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  std::optional<OverloadedOperator> overloadedOperator;
  bool constexprFunction = false;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::optional<BorrowOriginPlace> returnBorrowPlace;
  LanguageLinkage linkage = LanguageLinkage::Gti;
  std::string externalSymbol;
  std::optional<std::size_t> cArrayCountParameter;
  bool virtualMethod = false;
  bool pureVirtual = false;
  bool overrideMethod = false;
  std::vector<FunctionId> virtualRoots;
  std::vector<MirCallableParameter> callableParameters;
  using DefinitionKind = MirDefinitionKind;
  DefinitionKind definitionKind = DefinitionKind::Declaration;
  // MIR-owned defined-failure propagation dimension. Verification proves
  // every false value from the complete acyclic scalar/static-call graph.
  // This does not summarize allocation, user-code, synchronization, or other
  // O-MIR-02 effect dimensions.
  bool mayRaiseDefinedFailure = true;
  MirBody body;

  friend bool operator==(const MirFunctionInstance &,
                         const MirFunctionInstance &) = default;
};

enum class MirNativeCallbackFailurePolicy {
  TerminateInvocation,
};

struct MirNativeCallbackAdapter {
  MirNativeCallbackAdapterId id = 0;
  HirFunctionInstanceId target = 0;
  SemanticType type = SemanticType::Unknown;
  bool targetMayRaiseDefinedFailure = true;
  MirNativeCallbackFailurePolicy failurePolicy =
      MirNativeCallbackFailurePolicy::TerminateInvocation;
  bool catchesNativeExceptions = true;

  friend bool operator==(const MirNativeCallbackAdapter &,
                         const MirNativeCallbackAdapter &) = default;
};

struct MirConstructorInitializer {
  ConstructorInitializerTargetKind kind =
      ConstructorInitializerTargetKind::Field;
  SemanticType targetType = SemanticType::Unknown;
  SymbolId field = 0;
  std::optional<HirClassInstanceId> base;
  std::optional<HirConstructorInstanceId> constructorTarget;
  std::vector<HirValueId> arguments;
  std::size_t explicitArgumentCount = 0;
  bool storesReference = false;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  bool generatedDefault = false;
  std::optional<std::size_t> ownedParameter;

  friend bool operator==(const MirConstructorInitializer &,
                         const MirConstructorInitializer &) = default;
};

struct MirConstructorInstance {
  HirConstructorInstanceId id = 0;
  HirClassInstanceId owner = 0;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowParameter = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  MirDefinitionKind definitionKind = MirDefinitionKind::Declaration;
  bool mayRaiseDefinedFailure = true;
  std::vector<MirConstructorInitializer> initializers;
  MirBody body;

  friend bool operator==(const MirConstructorInstance &,
                         const MirConstructorInstance &) = default;
};

struct MirDestructorInstance {
  HirDestructorInstanceId id = 0;
  HirClassInstanceId owner = 0;
  MirDefinitionKind definitionKind = MirDefinitionKind::Declaration;
  bool mayRaiseDefinedFailure = true;
  MirBody body;

  friend bool operator==(const MirDestructorInstance &,
                         const MirDestructorInstance &) = default;
};

struct MirLambdaInstance {
  HirLambdaId id = 0;
  LambdaId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  std::vector<SemanticType> captureTypes;
  std::vector<LambdaCaptureMode> captureModes;
  std::vector<SymbolId> captureSymbols;
  std::vector<bool> captureRequiresActiveCleanup;
  MirBody body;

  friend bool operator==(const MirLambdaInstance &,
                         const MirLambdaInstance &) = default;
};

struct MirProgramInitializationUnit {
  SourceUnitId sourceUnit = 0;
  std::vector<ProgramInitializationStepId> steps;

  friend bool operator==(const MirProgramInitializationUnit &,
                         const MirProgramInitializationUnit &) = default;
};

enum class MirProgramDataInitializationKind {
  None,
  ImplicitZero,
  Constant,
  Count,
};

// Pointer-free identity for one program-wide storage step in the merged
// Module/0 body. Data-only steps retain an explicit implicit-zero/constant
// provenance discriminator and the exact constant payload when applicable.
// Executable steps instead retain the exact HIR statement/root and published
// MIR full-expression identities.
struct MirProgramInitializationStep {
  ProgramInitializationStepId id = 0;
  SourceUnitId sourceUnit = 0;
  ProgramStorageKind storageKind = ProgramStorageKind::NamespaceGlobal;
  ProgramInitializationStepRole role =
      ProgramInitializationStepRole::Initializer;
  SymbolId symbol = 0;
  HirClassInstanceId ownerClass = 0;
  bool requiresActiveCleanup = false;
  HirBindingId binding = 0;
  MirPlaceId storagePlace = 0;
  MirBlockId entryBlock = 0;
  MirInstructionId storageInitialization = 0;
  MirProgramDataInitializationKind dataInitialization =
      MirProgramDataInitializationKind::None;
  std::optional<ConstantValue> dataConstant;
  HirStatementId statement = 0;
  HirValueId initializer = 0;
  HirFullExpressionId fullExpression = 0;

  friend bool operator==(const MirProgramInitializationStep &,
                         const MirProgramInitializationStep &) = default;
};

struct MirProgramInitializationPlan {
  std::vector<MirProgramInitializationUnit> units;
  std::vector<MirProgramInitializationStep> steps;

  [[nodiscard]] const MirProgramInitializationStep *
  findStep(ProgramInitializationStepId id) const {
    return id == 0 || id > steps.size() ? nullptr : &steps[id - 1];
  }

  [[nodiscard]] const MirProgramInitializationStep *
  findStepForSymbol(SymbolId symbol) const {
    const auto found =
        std::find_if(steps.begin(), steps.end(), [symbol](const auto &step) {
          return step.symbol == symbol;
        });
    return found == steps.end() ? nullptr : &*found;
  }

  friend bool operator==(const MirProgramInitializationPlan &,
                         const MirProgramInitializationPlan &) = default;
};

enum class MirHostedStartupExitPolicy {
  ImmediateExit70,
  Count,
};

enum class MirHostedStartupFailureBehavior {
  None,
  Detect,
  Propagate,
  Count,
};

enum class MirHostedStartupOperationKind {
  ValidateArgumentCount,
  ConvertArgumentCount,
  CallProgramInitialization,
  ConstructArgumentVector,
  InitializeArgumentIndex,
  EnterArgumentLoop,
  LoadArgumentIndex,
  TestArgumentIndex,
  BranchArgumentLoop,
  ReadArgumentView,
  PrepareStringConstructorArgument,
  ConstructArgumentString,
  PrepareAppendReceiver,
  PrepareAppendArgumentMove,
  CallAppend,
  AdvanceArgumentIndex,
  ContinueArgumentLoop,
  PrepareEntryCount,
  PrepareEntryArgumentsMove,
  CallEntry,
  ReturnEntry,
  RouteOperationFailure,
  DropFailureCleanup,
  RouteCleanupFailure,
  EndFailureCleanup,
  ContainFailure,
  TerminateCleanupFailure,
  Count,
};

struct MirHostedStartupSourceAnchor {
  SourceUnitId sourceUnit = 0;
  std::size_t start = 0;
  std::size_t end = 0;
  int line = 1;

  friend bool operator==(const MirHostedStartupSourceAnchor &,
                         const MirHostedStartupSourceAnchor &) = default;
};

struct MirHostedStartupOperation {
  MirHostedStartupOperationId id = 0;
  MirHostedStartupOperationKind kind =
      MirHostedStartupOperationKind::ValidateArgumentCount;
  MirHostedStartupFailureBehavior failureBehavior =
      MirHostedStartupFailureBehavior::None;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirPlaceId place = 0;
  MirValueId value = 0;
  MirDropObligationId dropObligation = 0;
  MirFailureRecordId failureRecord = 0;
  std::size_t cleanupBoundary = 0;
  bool terminator = false;

  friend bool operator==(const MirHostedStartupOperation &,
                         const MirHostedStartupOperation &) = default;
};

// Pointer-free authority for the compiler-generated hosted boundary. The
// operation rows are dense and close over every generated place, value, drop,
// instruction, and terminator in `HostedStartup/<entry>`; source HIR
// identities never appear in that body. Failure cleanup and terminal
// containment remain a later MIR capability even though
// detection/propagation intent is retained here.
struct MirHostedStartupPlan {
  ProgramEntryKind kind = ProgramEntryKind::None;
  HirFunctionInstanceId entry = 0;
  HirFunctionInstanceId appendFunction = 0;
  HirConstructorInstanceId vectorConstructor = 0;
  HirConstructorInstanceId stringConstructor = 0;
  MirHostedStartupSourceAnchor sourceAnchor;
  MirBodyAddress programInitializationTarget;
  MirHostedStartupExitPolicy exitPolicy =
      MirHostedStartupExitPolicy::ImmediateExit70;
  MirPlaceId argumentIndexPlace = 0;
  MirPlaceId argumentVectorPlace = 0;
  MirValueId stabilizedCount = 0;
  MirValueId argumentVector = 0;
  MirValueId entryResult = 0;
  std::vector<MirHostedStartupOperation> operations;

  [[nodiscard]] const MirHostedStartupOperation *
  findOperation(MirHostedStartupOperationId id) const {
    return id == 0 || id > operations.size() ? nullptr : &operations[id - 1];
  }

  friend bool operator==(const MirHostedStartupPlan &,
                         const MirHostedStartupPlan &) = default;
};

class MirProgram {
public:
  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] ExecutionProfile executionProfile() const {
    return executionProfile_;
  }
  [[nodiscard]] const MirBody &module() const { return moduleBody; }

  [[nodiscard]] const MirProgramInitializationPlan &
  programInitializationPlan() const {
    return programInitialization;
  }

  [[nodiscard]] const std::optional<MirHostedStartupPlan> &
  hostedStartupPlan() const {
    return hostedStartupPlan_;
  }

  [[nodiscard]] const MirBody *hostedStartup() const {
    return hostedStartupBody ? &*hostedStartupBody : nullptr;
  }

  [[nodiscard]] const FailureMetadata &failureMetadata() const {
    return failureMetadata_;
  }

  [[nodiscard]] const std::vector<MirClassInstance> &classInstances() const {
    return classes;
  }

  [[nodiscard]] const std::vector<MirFunctionInstance> &
  functionInstances() const {
    return functions;
  }

  [[nodiscard]] const std::vector<MirNativeCallbackAdapter> &
  nativeCallbackAdapters() const {
    return nativeCallbacks;
  }

  [[nodiscard]] const std::vector<MirConstructorInstance> &
  constructorInstances() const {
    return constructors;
  }

  [[nodiscard]] const std::vector<MirDestructorInstance> &
  destructorInstances() const {
    return destructors;
  }

  [[nodiscard]] const std::vector<MirLambdaInstance> &lambdaInstances() const {
    return lambdas;
  }

  [[nodiscard]] const MirFunctionInstance *
  findFunctionInstance(HirFunctionInstanceId id) const {
    return id == 0 || id > functions.size() ? nullptr : &functions[id - 1];
  }

  [[nodiscard]] const MirNativeCallbackAdapter *
  findNativeCallbackAdapter(MirNativeCallbackAdapterId id) const {
    return id == 0 || id > nativeCallbacks.size() ? nullptr
                                                  : &nativeCallbacks[id - 1];
  }

  [[nodiscard]] const MirClassInstance *
  findClassInstance(HirClassInstanceId id) const {
    return id == 0 || id > classes.size() ? nullptr : &classes[id - 1];
  }

  [[nodiscard]] const MirConstructorInstance *
  findConstructorInstance(HirConstructorInstanceId id) const {
    return id == 0 || id > constructors.size() ? nullptr
                                               : &constructors[id - 1];
  }

  [[nodiscard]] const MirDestructorInstance *
  findDestructorInstance(HirDestructorInstanceId id) const {
    return id == 0 || id > destructors.size() ? nullptr : &destructors[id - 1];
  }

  [[nodiscard]] const MirLambdaInstance *findLambda(HirLambdaId id) const {
    return id == 0 || id > lambdas.size() ? nullptr : &lambdas[id - 1];
  }

  [[nodiscard]] std::size_t blockCount() const;

  friend bool operator==(const MirProgram &, const MirProgram &) = default;

private:
  friend class MirLowerer;
  friend MirVerificationResult
  verifyMirOptimizationCoherence(const MirProgram &source,
                                 const MirProgram &optimized);

  bool valid_ = true;
  ExecutionProfile executionProfile_ = ExecutionProfile::SingleThreaded;
  FailureMetadata failureMetadata_;
  MirProgramInitializationPlan programInitialization;
  std::optional<MirHostedStartupPlan> hostedStartupPlan_;
  std::optional<MirBody> hostedStartupBody;
  MirBody moduleBody;
  std::vector<MirClassInstance> classes;
  std::vector<MirFunctionInstance> functions;
  std::vector<MirNativeCallbackAdapter> nativeCallbacks;
  std::vector<MirConstructorInstance> constructors;
  std::vector<MirDestructorInstance> destructors;
  std::vector<MirLambdaInstance> lambdas;
};

// Returns every body exactly once in deterministic program order: module,
// each class's field/static initializers, functions, constructors,
// destructors, lambdas, then the optional hosted-startup body. An address not
// owned by the program resolves to nullptr; only Module uses owner zero and
// HostedStartup uses the exact nonzero entry function instance.
[[nodiscard]] std::vector<MirBodyAddress>
enumerateMirBodyAddresses(const MirProgram &program);
[[nodiscard]] const MirBody *findMirBody(const MirProgram &program,
                                         MirBodyAddress address);
[[nodiscard]] MirBody *findMirBody(MirProgram &program, MirBodyAddress address);

[[nodiscard]] MirVerificationResult verifyMirProgram(const MirProgram &program);

// One literal operand of a compute fold, carried with the type that fixes
// its evaluation domain.
struct MirComputeFoldOperand {
  Literal literal;
  SemanticType type;
};

// The single evaluation authority for MIR compute folds: the optimizer
// derives a fold through it and the verifier replays the same call, so a
// folded literal can never disagree with its proof. Returns nothing for
// any operation, operand shape, or domain outside the folded vocabulary
// (currently the boolean-producing comparisons and logical not).
[[nodiscard]] std::optional<Literal>
evaluateMirComputeFold(MirOperation operation,
                       const std::vector<MirComputeFoldOperand> &operands,
                       const SemanticType &resultType);

// Verifies that `optimized` is exactly `source` plus rewrites admitted by the
// controlled MIR editor. The current rewrite vocabulary contains only the
// verifier-proven Compute/Identity -> Compute/Literal identity fold; no CFG,
// operand, call, lifecycle, or metadata rewrite is authorized.
[[nodiscard]] MirVerificationResult
verifyMirOptimizationCoherence(const MirProgram &source,
                               const MirProgram &optimized);

struct MirDefinedFailureEffects {
  std::vector<bool> functions;
  std::vector<bool> constructors;
  std::vector<bool> destructors;

  friend bool operator==(const MirDefinedFailureEffects &,
                         const MirDefinedFailureEffects &) = default;
};

// Failure-dimension slice of O-MIR-02. Each result is indexed by its concrete
// instance ID and remains conservative (`true`) unless an acyclic, closed,
// bounded scalar/static-call/construction/normal-cleanup graph is proved from
// MIR alone.
[[nodiscard]] MirDefinedFailureEffects
deriveMirDefinedFailureEffects(const MirProgram &program);

// Proves that moving a value cannot execute a GTI defined-failure edge. For a
// class this requires an exact concrete MIR instance, a generated move member,
// and recursively failure-free structural state; declared move bodies remain
// outside the proof.
[[nodiscard]] bool mirTypeMoveIsDefinedFailureFree(const MirProgram &program,
                                                   const SemanticType &type);

// Compatibility accessor for clients of MIR v20's function-only slice.
[[nodiscard]] std::vector<bool>
deriveMirScalarDefinedFailureEffects(const MirProgram &program);

struct MirLoweringResult {
  MirProgram program;

  [[nodiscard]] bool valid() const { return program.valid(); }
};

class MirLowerer {
public:
  [[nodiscard]] MirLoweringResult
  lower(const HirProgram &source, const FailureMetadata &failureMetadata) const;

private:
  [[nodiscard]] static MirLoweringResult
  lowerProgram(const HirProgram &source, const FailureMetadata &failureMetadata,
               const MirDefinedFailureEffects &definedFailureEffects,
               bool includeProgramInitialization);

  [[nodiscard]] static MirBody lowerBody(
      const HirProgram &program, const FailureMetadata &failureMetadata,
      const HirBody &body, MirBodyKind kind, SemanticType returnType,
      const std::vector<HirValueId> &prologueValues,
      const MirDefinedFailureEffects &definedFailureEffects, bool &valid,
      bool implicitZeroReturn = false,
      const std::vector<HirConstructorInitializer> *initializers = nullptr,
      const HirFunctionInstance *function = nullptr,
      const HirConstructorInstance *constructor = nullptr,
      const HirLambda *lambda = nullptr,
      const HirProgramInitializationPlan *programInitialization = nullptr,
      MirProgramInitializationPlan *loweredProgramInitialization = nullptr);

  [[nodiscard]] static bool
  lowerHostedStartup(const HirProgram &source,
                     const FailureMetadata &failureMetadata,
                     MirProgram &program);
};

} // namespace lang
