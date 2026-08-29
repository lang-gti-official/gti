#pragma once

#include "gti/failure_metadata.h"
#include "gti/hir.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
using MirThreadTaskAdapterId = std::size_t;

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

enum class MirTransformKind {
  ReplaceInstruction,
  RemoveInstruction,
  ReplaceTerminator,
  RemoveBlock,
  PromotePlace,
  DischargeSafetyCheck,
  Count,
};

enum class MirTransformProofKind {
  IdentityFold,
  ComputeFold,
  SparseConstant,
  BranchFold,
  UnreachableBlock,
  DeadUnusedComputation,
  ScalarPromotion,
  BoundsRange,
  StorageInitialization,
  ArithmeticRange,
  Count,
};

enum class MirSafetyCheckFamily : std::uint8_t {
  Bounds,
  StorageState,
  ArithmeticOverflow,
  ArithmeticDomain,
  Count,
};

[[nodiscard]] constexpr std::string_view
mirSafetyCheckFamilyName(MirSafetyCheckFamily family) {
  switch (family) {
  case MirSafetyCheckFamily::Bounds:
    return "bounds";
  case MirSafetyCheckFamily::StorageState:
    return "storage-state";
  case MirSafetyCheckFamily::ArithmeticOverflow:
    return "arithmetic-overflow";
  case MirSafetyCheckFamily::ArithmeticDomain:
    return "arithmetic-domain";
  case MirSafetyCheckFamily::Count:
    return "invalid";
  }
  return "invalid";
}

// A backend may omit exactly this source-defined failure outcome. The
// optimizer attaches discharges only through the controlled MIR editor, and
// optimization coherence independently re-derives every attached outcome.
// Retaining the outcome identity, rather than a broad "cannot fail" bit,
// permits an instruction with unrelated failure modes to stay checked.
struct MirSafetyDischarge {
  MirSafetyCheckFamily family = MirSafetyCheckFamily::Count;
  std::size_t origin = 0;
  DefinedFailureOutcome outcome;

  friend bool operator==(const MirSafetyDischarge &,
                         const MirSafetyDischarge &) = default;
};

// Analyses whose exact facts justify one optimizer transformation. These are
// proof inputs, not cache-preservation claims; the optimizer's analysis manager
// owns invalidation separately.
struct MirTransformRequiredAnalyses {
  bool valueUses = false;
  bool effects = false;
  bool reachability = false;
  bool dominance = false;
  bool scalarPromotion = false;
  bool safetyChecks = false;
  bool sparseConstants = false;

  friend bool operator==(const MirTransformRequiredAnalyses &,
                         const MirTransformRequiredAnalyses &) = default;
};

// Stable, deterministic evidence for one source-MIR to optimized-MIR edit.
// The verifier treats this record as an untrusted claim and independently
// replays it against canonical source MIR before LoweredProgram construction.
// Backends never receive these records.
struct MirTransformRecord {
  std::string pass;
  MirTransformKind kind = MirTransformKind::ReplaceInstruction;
  MirTransformProofKind proof = MirTransformProofKind::IdentityFold;
  MirBodyAddress body;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirPlaceId place = 0;
  HirValueId sourceValue = 0;
  HirStatementId sourceStatement = 0;
  // Replacement payloads are untrusted claims. Coherence replay derives the
  // same literal independently before applying an instruction replacement.
  std::optional<Literal> replacementLiteral;
  std::optional<MirSafetyDischarge> safetyDischarge;
  std::vector<MirBlockId> relatedBlocks;
  std::vector<MirInstructionId> relatedInstructions;
  std::vector<MirValueId> relatedValues;
  std::vector<MirPlaceId> relatedPlaces;
  MirTransformRequiredAnalyses requiredAnalyses;

  friend bool operator==(const MirTransformRecord &,
                         const MirTransformRecord &) = default;
};

[[nodiscard]] bool mirTransformRecordLess(const MirTransformRecord &left,
                                          const MirTransformRecord &right);

// Conservative instruction and callable-body effects. A false field is a
// proof obligation: verification independently derives the least summary that
// the current MIR can justify and rejects an under-approximation. True fields
// may conservatively over-approximate behavior.
struct MirEffectTraits {
  bool readsPlace = false;
  bool writesPlace = false;
  bool readsUnknownMemory = false;
  bool writesUnknownMemory = false;
  bool allocates = false;
  bool invokesRuntime = false;
  bool mayTrap = false;
  bool startsLoan = false;
  bool endsLoan = false;
  bool dependsOnLoan = false;
  bool copiesValue = false;
  bool movesValue = false;
  bool initializesValue = false;
  bool dropsValue = false;
  bool invokesUserCode = false;
  bool targetDependent = false;
  bool maySynchronize = false;
  bool speculatable = false;
  bool removableWhenUnused = false;
  bool reorderable = false;

  friend bool operator==(const MirEffectTraits &,
                         const MirEffectTraits &) = default;
};

struct MirEffectSummary {
  MirEffectTraits traits;
  bool receiverEscapes = false;
  std::vector<bool> argumentsEscape;

  friend bool operator==(const MirEffectSummary &,
                         const MirEffectSummary &) = default;
};

struct MirProgramEffectSummaries {
  std::vector<MirEffectSummary> functions;
  std::vector<MirEffectSummary> constructors;
  std::vector<MirEffectSummary> destructors;
  std::vector<MirEffectSummary> lambdas;

  friend bool operator==(const MirProgramEffectSummaries &,
                         const MirProgramEffectSummaries &) = default;
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
  // A Loan operand may additionally retain the exact runtime address source
  // for reference initialization; the loan remains provenance authority.
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
  // A verifier-owned read-only loan from one materialized fresh value into
  // one ordered call argument. Backends erase its representation, and every
  // path ends it immediately after that invocation.
  CallArgument,
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
  // Exact stored-reference field whose referent this loan designates. Unlike
  // storedField (a constructor storage target), this is provenance metadata
  // for a by-value carrier dependency and has no backend representation.
  SymbolId designatedField = 0;
  // True only for a stored exclusive dependency whose carrier destructor
  // releases the protected resource. Cleanup must Drop that carrier before
  // ending this loan on every normal and failure edge.
  bool lifecycleBound = false;
  // A two-phase mutable call-result loan reserves one stable target place at
  // its producing call, but does not become exclusive until the unique
  // ActivateBorrow instruction immediately before the committing write.
  // Reads may overlap the reservation; writes, raw invalidation, escape, and
  // activation with a live overlapping loan are rejected by MIR verification.
  bool twoPhase = false;
  bool entry = false;
  bool escapes = false;

  friend bool operator==(const MirLoan &, const MirLoan &) = default;
};

enum class MirDropObligationKind {
  Binding,
  Value,
  PreparedParameter,
  // A completed constructor subobject transferred into `this`. Explicit
  // constructor stages arm it through their stage-completing Initialize;
  // completed in-class initializers enter the constructor body already active
  // through the owning class's verified field-initializer schedule. Every
  // defined-failure edge drains the active set in reverse construction order,
  // and normal completion retires it by transfer to the caller.
  // Compiler-generated: it has no HIR obligation or full-expression identity.
  // Constructor bodies use the exact This-rooted field place; the owning
  // field-initializer schedule uses that field's exact binding-rooted place.
  ConstructionRollback,
};

// A homogeneous fixed-array drop walks these extents in reverse lexicographic
// order and applies the leaf cleanup descriptor to every element. Keeping the
// concrete shape in MIR avoids asking a backend to rediscover aggregate
// lifetime semantics from its chosen array representation.
struct MirFixedArrayDropSchedule {
  std::vector<std::size_t> extents;
  SemanticType leafType = SemanticType::Unknown;
  std::optional<HirClassInstanceId> leafClassInstance;

  friend bool operator==(const MirFixedArrayDropSchedule &,
                         const MirFixedArrayDropSchedule &) = default;
};

struct MirDropType {
  SemanticType type = SemanticType::Unknown;
  std::optional<HirClassInstanceId> classInstance;
  std::optional<HirLambdaId> lambdaInstance;
  std::optional<HirDestructorInstanceId> destructor;
  std::optional<MirFixedArrayDropSchedule> fixedArray;
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
  ActivateBorrow,
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
  FunctionItem,
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
  SparseConstant,
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
// instruction's literal. A sparse constant retains the original operation and
// the complete SCCP evidence value set; optimization coherence reruns SCCP on
// canonical MIR before accepting it.
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
  // Destructive optimizer removal preserves the stable instruction and
  // result identities as an inert slot. A tombstone has one canonical shape,
  // is never executable, and is admitted only by MIR verification/coherence.
  bool tombstone = false;
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
  std::vector<MirSafetyDischarge> safetyDischarges;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<MirNativeCallbackAdapterId> nativeCallbackAdapter;
  std::optional<MirThreadTaskAdapterId> threadTaskAdapter;
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
  // A control-flow expression may end the same source full expression on a
  // mutually exclusive early-return edge. The ordinary continuation retains
  // the one primary boundary; every early endpoint is marked explicitly so
  // verification can prove path separation and exact cleanup independently.
  bool earlyExitFullExpressionEnd = false;
  std::size_t cleanupBoundaryEnd = 0;

  friend bool operator==(const MirInstruction &,
                         const MirInstruction &) = default;
};

[[nodiscard]] bool hasMirSafetyDischarge(const MirInstruction &instruction,
                                         MirSafetyDischarge discharge);
[[nodiscard]] bool
isMirFailureOutcomeDischarged(const MirInstruction &instruction,
                              std::size_t origin,
                              DefinedFailureOutcome outcome);
[[nodiscard]] bool
allMirLocalFailuresDischarged(const MirInstruction &instruction);

[[nodiscard]] MirInstruction
makeMirInstructionTombstone(const MirInstruction &instruction);

struct MirScalarPromotionRewrite {
  MirPlaceId place = 0;
  std::vector<MirBlockId> parameterBlocks;
  std::vector<MirInstructionId> replacedInstructions;
  std::vector<MirInstructionId> removedInstructions;
  std::vector<MirValueId> createdValues;

  friend bool operator==(const MirScalarPromotionRewrite &,
                         const MirScalarPromotionRewrite &) = default;
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
  std::vector<MirOperand> arguments;

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
  std::vector<MirOperand> targetArguments;
  std::vector<MirOperand> elseArguments;
  std::vector<MirSwitchTarget> switchTargets;
  std::vector<MirLifecycleEvent> successLifecycle;
  MirTerminatorProvenance provenance;

  friend bool operator==(const MirTerminator &,
                         const MirTerminator &) = default;
};

struct MirBlockParameter {
  MirValueId value = 0;
  SemanticType type = SemanticType::Unknown;
  HirValueId sourceValue = 0;
  HirStatementId sourceStatement = 0;

  friend bool operator==(const MirBlockParameter &,
                         const MirBlockParameter &) = default;
};

// The first block-argument vocabulary is intentionally confined to scalar,
// non-owning values. Class values, owners, references, arrays, closures, and
// storage records retain their existing place/lifecycle representation until
// a client can preserve those semantics across CFG edges.
[[nodiscard]] bool isMirBlockParameterType(const SemanticType &type);

struct MirBlock {
  MirBlockId id = 0;
  ProgramInitializationStepId programInitializationStep = 0;
  MirFailureRecordId failureParameter = 0;
  MirFailureRecordId activeFailure = 0;
  std::vector<MirBlockParameter> parameters;
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
  EdgeArgument,
  PlaceRoot,
  PlaceIndex,
};

struct MirValueUse {
  MirValueId value = 0;
  MirValueUseKind kind = MirValueUseKind::InstructionOperand;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirPlaceId place = 0;
  MirBlockId edgeTarget = 0;
  std::size_t edgeIndex = 0;
  std::size_t operandIndex = 0;

  friend bool operator==(const MirValueUse &, const MirValueUse &) = default;
};

struct MirRangeIteration {
  std::size_t id = 0;
  HirStatementId hirStatement = 0;
  HirStatementId coreLoop = 0;
  RangeIterationStrategy strategy = RangeIterationStrategy::Structural;
  SemanticType rangeType = SemanticType::Unknown;
  SemanticType iteratorType = SemanticType::Unknown;
  SemanticType sentinelType = SemanticType::Unknown;
  SemanticType elementType = SemanticType::Unknown;
  AccessMode elementAccess = AccessMode::ReadOnly;
  MirPlaceId owner = 0;
  MirPlaceId rangeCarrier = 0;
  MirPlaceId iterator = 0;
  MirPlaceId sentinel = 0;
  MirPlaceId element = 0;
  MirLoanId rangeLoan = 0;
  MirLoanId elementLoan = 0;
  MirValueId expectedPayloadExtraction = 0;
  MirDropObligationId ownerDrop = 0;
  MirBlockId conditionEntryBlock = 0;
  MirBlockId conditionBlock = 0;
  MirBlockId bodyBlock = 0;
  MirBlockId incrementBlock = 0;
  MirBlockId cleanupBlock = 0;
  MirBlockId exitBlock = 0;
  std::uint64_t arrayLength = 0;
  bool ownsRangeTemporary = false;
  bool expectedPayloadProjection = false;

  friend bool operator==(const MirRangeIteration &,
                         const MirRangeIteration &) = default;
};

struct MirBody {
  MirBodyKind kind = MirBodyKind::Function;
  // Source-defined standard-library wrappers are part of the complete MIR
  // declaration inventory even when their public surface is unavailable in
  // the selected execution profile. Lowering sets this bit only on a
  // compiler-trusted body that actually contains an exact private
  // synchronization intrinsic. Program verification treats it as a narrow
  // exemption for that dormant wrapper body, never as executable profile
  // authority.
  bool compilerTrustedSynchronization = false;
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
  std::vector<MirRangeIteration> rangeIterations;
  std::vector<MirValue> values;
  std::vector<std::vector<MirValueUse>> valueUses;

  [[nodiscard]] const MirBlock *findBlock(MirBlockId id) const {
    return id == 0 || id > blocks.size() ? nullptr : &blocks[id - 1];
  }

  [[nodiscard]] MirBlock *findBlock(MirBlockId id) {
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

// A source temporary is either still rooted directly in its SSA value or has
// been promoted into one hidden slot so it remains nameable across a CFG
// merge. This query accepts only those two exact forms and returns the unique
// MIR producer that materializes the source identity.
struct MirTemporaryMaterialization {
  const MirValue *value = nullptr;
  const MirInstruction *producer = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return value != nullptr && producer != nullptr;
  }
};

[[nodiscard]] MirTemporaryMaterialization
findMirTemporaryMaterialization(const MirBody &body, const MirPlace &place,
                                HirValueId sourceValue);

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

struct MirCAbiRecordFieldLayout {
  SymbolId field = 0;
  SemanticType type = SemanticType::Unknown;
  std::uint64_t offsetBytes = 0;
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;

  friend bool operator==(const MirCAbiRecordFieldLayout &,
                         const MirCAbiRecordFieldLayout &) = default;
};

struct MirCAbiRecordLayout {
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
  std::vector<MirCAbiRecordFieldLayout> fields;

  friend bool operator==(const MirCAbiRecordLayout &,
                         const MirCAbiRecordLayout &) = default;
};

struct MirUnionFieldLayout {
  SymbolId field = 0;
  SemanticType type = SemanticType::Unknown;
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;

  friend bool operator==(const MirUnionFieldLayout &,
                         const MirUnionFieldLayout &) = default;
};

struct MirUnionLayout {
  std::uint64_t sizeBytes = 0;
  std::uint32_t abiAlignmentBytes = 0;
  std::vector<MirUnionFieldLayout> fields;

  friend bool operator==(const MirUnionLayout &,
                         const MirUnionLayout &) = default;
};

struct MirClassInstance {
  HirClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  CompilerCapabilityTypeKind compilerCapability =
      CompilerCapabilityTypeKind::None;
  bool lifecycleBoundBorrowCarrier = false;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  std::vector<HirBaseInstance> structuralBases;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  std::optional<MirCAbiRecordLayout> cAbiLayout;
  std::optional<MirUnionLayout> unionLayout;
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
  std::vector<CompileTimeValue> valueArguments;
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
  MirEffectSummary effectSummary;
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

struct MirThreadTaskAdapter {
  MirThreadTaskAdapterId id = 0;
  SemanticType taskType = SemanticType::Unknown;
  SemanticType resultType = SemanticType::Unknown;
  HirValueId spawnValue = 0;
  MirBodyAddress sourceBody;
  MirInstructionId spawnInstruction = 0;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirLambdaId> lambdaTarget;
  CallableInvocationCapability targetInvocation =
      CallableInvocationCapability::Read;
  CallableInvocationCapability invocation = CallableInvocationCapability::Once;
  bool catchesWorkerFailure = true;
  bool catchesNativeExceptions = true;

  friend bool operator==(const MirThreadTaskAdapter &,
                         const MirThreadTaskAdapter &) = default;
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
  // Constructor-local generic arguments are part of the concrete instance
  // identity just as owner arguments are part of the class instance. Keep
  // them explicit in MIR so verification and later phases never have to
  // reconstruct deduction from substituted parameter types.
  std::vector<SemanticType> typeArguments;
  std::vector<CompileTimeValue> valueArguments;
  std::vector<SemanticType> parameterTypes;
  std::vector<HirBindingId> parameterBindings;
  BorrowOriginKind borrowOrigin = BorrowOriginKind::None;
  std::size_t borrowParameter = 0;
  AccessMode borrowAccess = AccessMode::ReadOnly;
  MirDefinitionKind definitionKind = MirDefinitionKind::Declaration;
  bool mayRaiseDefinedFailure = true;
  MirEffectSummary effectSummary;
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
  MirEffectSummary effectSummary;
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
  MirEffectSummary effectSummary;
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

// Pointer-free lifecycle authority for a closed payload enum. Variant rows
// retain the exact ordered field types and active-cleanup facts that MIR
// construction, extraction, ownership flow, and backends are allowed to use.
// Names remain declaration/lowered-program presentation metadata.
struct MirPayloadEnumVariant {
  std::size_t variantIndex = 0;
  std::vector<SemanticType> payloadTypes;
  std::vector<bool> payloadRequiresActiveCleanup;

  friend bool operator==(const MirPayloadEnumVariant &,
                         const MirPayloadEnumVariant &) = default;
};

struct MirPayloadEnum {
  EnumId declaration = 0;
  SourceUnitId sourceUnit = 0;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits{};
  std::vector<MirPayloadEnumVariant> variants;
  bool requiresActiveCleanup = false;

  friend bool operator==(const MirPayloadEnum &,
                         const MirPayloadEnum &) = default;
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

  [[nodiscard]] const std::vector<MirPayloadEnum> &payloadEnums() const {
    return payloadEnums_;
  }

  [[nodiscard]] const MirPayloadEnum *findPayloadEnum(EnumId id) const {
    const auto found = std::find_if(payloadEnums_.begin(), payloadEnums_.end(),
                                    [&](const MirPayloadEnum &candidate) {
                                      return candidate.declaration == id;
                                    });
    return found == payloadEnums_.end() ? nullptr : &*found;
  }

  [[nodiscard]] const std::vector<MirFunctionInstance> &
  functionInstances() const {
    return functions;
  }

  [[nodiscard]] const std::vector<MirNativeCallbackAdapter> &
  nativeCallbackAdapters() const {
    return nativeCallbacks;
  }

  [[nodiscard]] const std::vector<MirThreadTaskAdapter> &
  threadTaskAdapters() const {
    return threadTasks;
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

  [[nodiscard]] const MirThreadTaskAdapter *
  findThreadTaskAdapter(MirThreadTaskAdapterId id) const {
    return id == 0 || id > threadTasks.size() ? nullptr : &threadTasks[id - 1];
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
  friend struct MirProgramTestAccess;

  bool valid_ = true;
  ExecutionProfile executionProfile_ = ExecutionProfile::SingleThreaded;
  FailureMetadata failureMetadata_;
  MirProgramInitializationPlan programInitialization;
  std::optional<MirHostedStartupPlan> hostedStartupPlan_;
  std::optional<MirBody> hostedStartupBody;
  MirBody moduleBody;
  std::vector<MirPayloadEnum> payloadEnums_;
  std::vector<MirClassInstance> classes;
  std::vector<MirFunctionInstance> functions;
  std::vector<MirNativeCallbackAdapter> nativeCallbacks;
  std::vector<MirThreadTaskAdapter> threadTasks;
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

} // namespace lang
