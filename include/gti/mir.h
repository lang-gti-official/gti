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
  DevirtualizeCall,
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
  ArithmeticRange,
  ExactReceiver,
  // The receiver is a caller-provided reference parameter whose referent
  // class is exact on every visible call of a closed function.
  ExactParameterReceiver,
  Count,
};

enum class MirSafetyCheckFamily : std::uint8_t {
  Bounds,
  ArithmeticOverflow,
  ArithmeticDomain,
  Count,
};

[[nodiscard]] constexpr std::string_view
mirSafetyCheckFamilyName(MirSafetyCheckFamily family) {
  switch (family) {
  case MirSafetyCheckFamily::Bounds:
    return "bounds";
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
  // Whole-program closed-world call-site facts (optimization/closed_world).
  bool closedWorld = false;

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
  // A constructor-owned direct base subobject; selection is its concrete
  // HirClassInstanceId. It is not a field symbol or an independent object.
  Base,
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
  bool foreignStorage = false;
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
  // A verifier-owned read-only loan for one fresh value argument, or a
  // read/mutable physical-shell loan for a fresh borrowed-carrier receiver.
  // The latter retains an independent Stored owner dependency. Backends erase
  // its representation; every path ends it immediately after invocation.
  CallArgument,
};

struct MirLoanDependency {
  MirPlaceId source = 0;
  MirLoanId parent = 0;
  std::vector<SymbolId> designatedFields;
  AccessMode access = AccessMode::ReadOnly;
  std::vector<BorrowDependencyCondition> conditions;

  friend bool operator==(const MirLoanDependency &,
                         const MirLoanDependency &) = default;
};

// Complete producer-attached borrow facts. Loan identity remains the authority
// for origin, access and liveness; the set records which typed alternative
// paths can carry those loans, including an explicitly proven empty set.
struct MirBorrowDependencySet {
  std::vector<MirLoanDependency> dependencies;
  std::vector<BorrowAlternativePath> possibleAlternatives;

  friend bool operator==(const MirBorrowDependencySet &,
                         const MirBorrowDependencySet &) = default;
};

// The evaluated reference address is selected from this exact payload
// extraction, not from the first of its possible external owner places.
// This descriptor does not grant a loan: independent dependency/liveness
// verification must establish every possible owner of the selected referent.
struct MirReferenceSelection {
  MirValueId value = 0;
  MirPlaceId carrier = 0;
  // Independently matched to the consumed carrier's complete dependency
  // group. Ending that shell does not end transferred external authority.
  MirLoanId transferredFrom = 0;
  std::vector<SymbolId> fields;
  std::vector<BorrowDependencyCondition> conditions;
  SemanticType reference = SemanticType::Unknown;

  friend bool operator==(const MirReferenceSelection &,
                         const MirReferenceSelection &) = default;
};

struct MirLoan {
  MirLoanId id = 0;
  SemanticLoanId semanticLoan = 0;
  MirLoanId parent = 0;
  // source/parent protect a reference's physical carrier shell. This
  // independent Stored loan protects its inherited external owners without
  // transferring ownership, suspending the dependency, or increasing access.
  MirLoanId storedDependency = 0;
  MirLoanKind kind = MirLoanKind::Local;
  MirPlaceId source = 0;
  // Each extra stored dependency retains its own provenance and permission.
  // Abstract entry dependencies designate a field rather than a concrete
  // caller place; concrete dependencies name their body-local source place.
  std::vector<MirLoanDependency> additionalDependencies;
  std::optional<MirReferenceSelection> referenceSelection;
  AccessMode access = AccessMode::ReadOnly;
  HirValueId producedBy = 0;
  std::vector<HirBindingId> carriers;
  SymbolId storedField = 0;
  // Exact nominal field path ending at the reference this loan designates.
  // Unlike storedField (a constructor storage target), this is provenance
  // metadata for a by-value carrier dependency and has no backend
  // representation.
  std::vector<SymbolId> designatedFields;
  // Exact active-alternative requirements for the primary dependency. These
  // are structural provenance, not runtime reads of inactive payloads.
  std::vector<BorrowDependencyCondition> conditions;
  // An owned carrier needs this dependency until cleanup, including read-only
  // dependencies used by source destructors. EndBorrow follows the exact Drop
  // or verified ownership transfer on every normal and failure edge. This
  // retention bit grants no mutable access or mutex-release authority.
  bool lifecycleBound = false;
  // A two-phase mutable call-result loan reserves one stable target place at
  // its producing call, but does not become exclusive until the unique
  // ActivateBorrow instruction immediately before the committing write.
  // Reads may overlap the reservation; writes, raw invalidation, escape, and
  // activation with a live overlapping loan are rejected by MIR verification.
  bool twoPhase = false;
  bool entry = false;
  bool escapes = false;

  [[nodiscard]] bool protectsSource(MirPlaceId place) const {
    if (source == place)
      return true;
    for (const auto &dependency : additionalDependencies)
      if (dependency.source != 0 && dependency.source == place)
        return true;
    return false;
  }

  [[nodiscard]] std::vector<MirPlaceId> additionalSources() const {
    std::vector<MirPlaceId> result;
    for (const auto &dependency : additionalDependencies)
      if (dependency.source != 0)
        result.push_back(dependency.source);
    return result;
  }

  [[nodiscard]] std::vector<std::vector<SymbolId>>
  additionalDesignatedFields() const {
    std::vector<std::vector<SymbolId>> result;
    for (const auto &dependency : additionalDependencies)
      if (!dependency.designatedFields.empty())
        result.push_back(dependency.designatedFields);
    return result;
  }

  friend bool operator==(const MirLoan &, const MirLoan &) = default;
};

enum class MirDropObligationKind {
  Binding,
  Value,
  PreparedParameter,
  // A completed constructor subobject transferred into `this`. Explicit
  // constructor stages arm it through their stage-completing Initialize or
  // Reparent. Declaration-field defaults executed by the selected constructor
  // follow that same rule; defaults completed by a separate verified field
  // initializer schedule enter the constructor body already active. Every
  // defined-failure edge drains the active set in reverse construction order,
  // and normal completion retires it by transfer to the caller.
  // Compiler-generated: it has no HIR obligation or full-expression identity.
  // Constructor bodies use the exact This-rooted field place; the owning
  // field-initializer schedule uses that field's exact binding-rooted place.
  ConstructionRollback,
  // Storage for one not-yet-constructed unique-owner pointee. This generated
  // obligation only deallocates bytes: it never invokes the pointee's cleanup.
  // AllocateObject arms it, construction failure drops it, and successful
  // construction transfers it into the ordinary owning result.
  ObjectAllocation,
  // A completed return operand in the caller-provided destination, still
  // owned by this invocation until its remaining cleanup succeeds. Reparent
  // arms it from the exact Value obligation; only the final Return may
  // transfer it out. Failure before publication drops it first.
  // Generated, with no HIR obligation or full-expression lifetime of its own.
  ReturnResult,
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
  // Exact pointee cleanup for an owning pointer to a lifecycle-guarded leaf
  // class. The owner may be empty; otherwise Drop invokes this class's source
  // destructor through the failure ABI before releasing the allocation.
  std::optional<HirClassInstanceId> uniquePointeeClass;
  // An owning pointer to a polymorphic class retains the complete allocated
  // object's cleanup selection across upcasts. Dispatch its cleanup through
  // that selection, not the statically named base's subobject recipe.
  bool polymorphicCleanup = false;
  // Selection of this child within its enclosing aggregate schedule. Root
  // descriptors have no selector; a field/base/capture child has exactly one.
  SymbolId uniqueOwnerField = 0;
  std::optional<HirClassInstanceId> uniqueOwnerBase;
  // Zero-based capture projection within an enclosing closure recipe.
  std::optional<std::size_t> captureIndex;
  // Active-alternative field selection within an enclosing payload enum.
  // Both selectors are present together; fields run in reverse source order.
  std::optional<std::size_t> payloadVariant;
  std::optional<std::size_t> payloadField;
  // A finite back-edge to an enclosing class recipe with this exact
  // classInstance/type. It has no destructor or children of its own. Complete
  // root recipes still contain every reachable source destructor, so their
  // effect target inventory does not need to chase a cyclic native type.
  bool recursiveCleanup = false;
  // Ordered complete active-subobject cleanup. A class runs its guarded source
  // destructor, if present, then children in reverse field order, then base
  // order. A closure destroys active captures in reverse capture order.
  // A unique owner of an aggregate has
  // one unprojected pointee child, followed by allocation release. Source
  // containers use ordinary destructor bodies to schedule element lifetime
  // ends through the general object-lifetime operations. A first
  // failure drains remaining children; a second failure terminates immediately
  // with both records. Entering during failure returns on the first new
  // failure.
  std::vector<MirDropType> aggregate;
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
  AllocateObject,
  // Invokes a selected base constructor in place. Completion arms only the
  // base's rollback obligation, never a standalone class result value.
  InitializeBase,
  // Drains a represented destination before its paired Assign commits. The
  // enclosing representation stays live but disarmed/empty, including on
  // failure. RHS ownership is untouched until the success-path Assign.
  CleanupBeforeAssign,
  // Compile-time-only permission transitions for the exact read-only endpoint
  // of a structural mutable range. They do not start/end a lifetime or release
  // resources. Verification binds them to the range inventory and both Invoke
  // continuations before a backend may erase them.
  ParkRangeCursor,
  RestoreRangeCursor,
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
  Closure,
  NativeCallback,
  FunctionItem,
  PayloadConstruct,
  PayloadExtract,
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

// One consumed operand of a synchronous structured task group. A borrowed
// task retains its existing stored loan through the whole operation, including
// worker cleanup and join-all before either continuation. No handle escapes.
struct MirScopedThreadTask {
  MirThreadTaskAdapterId adapter = 0;
  MirLoanId dependency = 0;

  friend bool operator==(const MirScopedThreadTask &,
                         const MirScopedThreadTask &) = default;
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
  // backend bind a field/base initialization to its exact MIR schedule without
  // reopening AST expressions.
  std::size_t constructorInitializer = 0;
  std::optional<MirCallInputRole> callInputRole;
  std::size_t callInputIndex = 0;
  HirCallInputKind callInputKind = HirCallInputKind::Value;
  // A retained pointee invocation consumes availability of this owning field,
  // not its accessor-result loan. Physical owner cleanup remains active.
  std::optional<MirPlaceId> retainedOwnerPlace;
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
  // Preserve the frontend-selected origins; emission must not infer them.
  std::vector<std::size_t> borrowArguments;
  std::vector<BorrowedFieldOrigin> borrowFields;
  // A construction or call publishes dynamic text arguments into direct text
  // fields under its result loan. Its borrow facts describe this call, not
  // the target's summary.
  bool dependentTextPublication = false;
  // Semantics analyzed this call before its callee's text relays were final
  // (recursion), so each of its text arguments must be ownerless.
  bool provisionalTextRelays = false;
  std::optional<BorrowOriginPlace> borrowPlace;
  bool restoresReferenceResult = false;
  std::vector<RestoredBorrowField> restoredBorrowFields;
  MirOperation operation = MirOperation::None;
  std::optional<Literal> literal;
  MirLiteralProvenance literalProvenance;
  bool programConstantSubstitution = false;
  std::optional<EnumId> enumOwner;
  std::optional<EnumConstant> enumValue;
  std::optional<std::size_t> enumVariant;
  std::optional<std::size_t> payloadIndex;
  std::optional<PayloadExtractionKind> payloadExtractionKind;
  IntrinsicKind intrinsic = IntrinsicKind::None;
  // Concrete queried representation; only ObjectSize/ObjectAlignment may
  // carry this field. It is not a runtime argument or a borrow origin.
  std::optional<SemanticType> objectLayoutType;
  bool neverReturns = false;
  bool noNormalBorrowResult = false;
  bool tracksCaller = false;
  std::optional<CallerLocationOrigin> callerLocation;
  FailureSiteId callerLocationSite = 0;
  bool forwardsCallerLocation = false;
  std::optional<SourceLocationLiteral> sourceLocation;
  SynchronizationOperation synchronization;
  DefinedFailureOperation definedFailure;
  // Exact contained element cleanup for general object destruction and
  // explicit optional-slot destruction. The operation retires the slot before
  // executing this recipe; a destructor failure propagates its original record
  // through Invoke.
  std::optional<MirDropType> elementCleanup;
  std::optional<MirDropType> replacementCleanup;
  // Symmetric exact instruction pair: CleanupBeforeAssign <-> Assign.
  MirInstructionId replacementPair = 0;
  std::vector<FailureSiteId> localFailureSites;
  std::vector<MirSafetyDischarge> safetyDischarges;
  CallDispatch dispatch = CallDispatch::Static;
  SemanticType dispatchOwner = SemanticType::Unknown;
  std::optional<HirFunctionInstanceId> functionTarget;
  std::optional<HirFunctionInstanceId> functionPointerFunctionTarget;
  std::optional<HirLambdaId> functionPointerLambdaTarget;
  std::optional<SemanticType> nativeCalleeType;
  std::optional<MirNativeCallbackAdapterId> nativeCallbackAdapter;
  std::optional<MirThreadTaskAdapterId> threadTaskAdapter;
  std::vector<MirScopedThreadTask> scopedThreadTasks;
  std::optional<HirConstructorInstanceId> constructorTarget;
  // Body-local ObjectAllocation obligation consumed by unique construction,
  // or established by AllocateObject. It is not a source-visible owner value.
  MirDropObligationId objectAllocation = 0;
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

// Exact enum instances come from typed results/source operands, not solely
// from the generic primary declaration identity.
[[nodiscard]] inline SemanticType
mirInstructionEnumType(const MirInstruction &instruction) {
  SemanticType type = instruction.info.type;
  if (instruction.operation == MirOperation::PayloadExtract &&
      !instruction.operands.empty())
    type = instruction.operands.front().type;
  while (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
    SemanticType referent = type.arguments.front();
    type = std::move(referent);
  }
  return type;
}

[[nodiscard]] bool hasMirSafetyDischarge(const MirInstruction &instruction,
                                         MirSafetyDischarge discharge);
// Shared verifier/backend/failure-proof admission of the native indirect call
// family. The semantic signature remains authoritative; this independently
// checks its ABI shape and ordered callee/argument staging against MIR.
class MirProgram;
struct MirBody;
[[nodiscard]] bool isMirNativeIndirectCall(const MirProgram &program,
                                           const MirBody &body,
                                           const MirInstruction &instruction);
// Shared verifier/backend admission of a static member call that presents an
// interface-typed receiver at its target's owner: `dispatchOwner` names that
// owner, which directly implements the receiver's interface. Semantics never
// selects such a member; only the exact-parameter receiver refinement creates
// it, and optimization coherence replays that refinement's closed-world proof.
[[nodiscard]] bool
isMirExactReceiverPresentation(const MirProgram &program,
                               const MirInstruction &instruction,
                               const SemanticType &receiverType);
[[nodiscard]] bool
isMirFailureOutcomeDischarged(const MirInstruction &instruction,
                              std::size_t origin,
                              DefinedFailureOutcome outcome);
[[nodiscard]] bool
allMirLocalFailuresDischarged(const MirInstruction &instruction);

[[nodiscard]] MirInstruction
makeMirInstructionTombstone(const MirInstruction &instruction);

struct MirBody;

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
  // Enters a shared failure-cleanup continuation. The source block is a
  // primary cleanup block of any active record; `target` is the entry of the
  // continuation named by `failureRecord`, which then runs with that
  // continuation as its active record and propagates the incoming failure
  // unchanged.
  ContinueFailure,
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

// A producer record binds the failure of exactly one Invoke at its parameter
// block. A continuation record names a cleanup suffix shared by several
// failure paths: it has no producer, its parameter block is entered only by
// `ContinueFailure` edges from primary cleanup blocks, and it carries
// whichever producer's failure entered it to the endpoint unchanged. Failure
// provenance therefore stays with producers; continuations own only the
// cleanup schedule they share.
enum class MirFailureRecordKind {
  Producer,
  Continuation,
};

struct MirFailureRecord {
  MirFailureRecordId id = 0;
  MirFailureRecordKind kind = MirFailureRecordKind::Producer;
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
  std::optional<MirBorrowDependencySet> borrowDependencies;

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

struct MirRangeEndpointParking {
  MirLoanId cursor = 0;
  HirValueId endValue = 0;
  MirInstructionId beginCall = 0;
  MirInstructionId endCall = 0;
  MirInstructionId endReceiver = 0;
  MirInstructionId park = 0;
  MirInstructionId restore = 0;
  std::vector<MirInstructionId> failureRestores;

  friend bool operator==(const MirRangeEndpointParking &,
                         const MirRangeEndpointParking &) = default;
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
  MirPlaceId elementOwner = 0;
  HirStatementId elementOwnerStatement = 0;
  MirLoanId rangeLoan = 0;
  MirLoanId elementLoan = 0;
  MirLoanId elementOwnerLoan = 0;
  MirValueId elementAccessValue = 0;
  HirValueId elementAccessSource = 0;
  FunctionId dereferenceFunction = 0;
  MirDropObligationId elementOwnerDrop = 0;
  MirDropObligationId elementSourceDrop = 0;
  MirDropObligationId ownerDrop = 0;
  MirBlockId conditionEntryBlock = 0;
  MirBlockId conditionBlock = 0;
  MirBlockId bodyBlock = 0;
  MirBlockId incrementBlock = 0;
  MirBlockId cleanupBlock = 0;
  MirBlockId exitBlock = 0;
  std::uint64_t arrayLength = 0;
  bool ownsRangeTemporary = false;
  bool materializesElementValue = false;
  std::optional<MirRangeEndpointParking> endpointParking;

  friend bool operator==(const MirRangeIteration &,
                         const MirRangeIteration &) = default;
};

struct MirBody {
  MirBodyKind kind = MirBodyKind::Function;
  // Only a tracked source function owns an incoming caller-location parameter.
  bool hasCallerLocation = false;
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

// An input whose enclosing invocation never executes may have no SSA consumer
// only when every remaining normal path in this full expression reaches an
// explicit terminal call. This query does not replace the independent loan and
// drop checks on failure edges, or declaration-contract verification.
[[nodiscard]] bool mirCallInputAbandonedByTerminal(const MirBody &body,
                                                   const MirInstruction &input);

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

// An explicit move of a whole binding publishes its value into one exact
// value-rooted temporary, whose obligation the enclosing full expression owns.
[[nodiscard]] MirTemporaryMaterialization
findMirMovedTemporaryMaterialization(const MirBody &body, const MirPlace &place,
                                     HirValueId sourceValue);

// An uncarried borrowed class temporary keeps its exact producer's Stored
// owner dependency until its full-expression value obligation is retired.
// This derives lifetime authority from existing identities, not a second
// cleanup flag. The published fullExpression id may still be pending while
// lowering; hirFullExpression must already identify the source boundary.
struct MirTemporaryCarrierDependency {
  MirTemporaryMaterialization materialization;
  const MirLoan *loan = nullptr;
  const MirDropObligation *drop = nullptr;

  [[nodiscard]] explicit operator bool() const {
    return materialization && loan != nullptr && drop != nullptr;
  }
};

[[nodiscard]] MirTemporaryCarrierDependency
findMirTemporaryCarrierDependency(const MirBody &body, const MirPlace &place);

// Includes the loan itself, physical-parent/stored-owner edges, and the
// independent input authorities retained by composed stored dependencies.
[[nodiscard]] bool mirLoanRetainsDependency(const MirBody &body, MirLoanId loan,
                                            MirLoanId dependency);

[[nodiscard]] bool supportsMirFailureControlFlow(MirBodyKind kind);
// Body-level refinement: a constructor whose lowering silently transferred a
// subobject into `this` without arming rollback routes no defined-failure
// edges at all.
[[nodiscard]] bool mirBodyRoutesFailureEdges(const MirBody &body);
[[nodiscard]] bool
requiresMirFailureControlFlow(const MirInstruction &instruction);
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
  AccessMode access = AccessMode::ReadOnly;
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

struct MirClassTypeAlias {
  TypeAliasId declaration = 0;
  SemanticType type = SemanticType::Unknown;

  friend bool operator==(const MirClassTypeAlias &,
                         const MirClassTypeAlias &) = default;
};

struct MirClassInstance {
  HirClassInstanceId id = 0;
  SourceUnitId sourceUnit = 0;
  ClassId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SemanticTypeTraits traits;
  ConcurrencyCapabilityPolicy transferPolicy =
      ConcurrencyCapabilityPolicy::Structural;
  ConcurrencyCapabilityPolicy sharePolicy =
      ConcurrencyCapabilityPolicy::Structural;
  std::optional<SemanticType> transferCondition;
  std::optional<SemanticType> shareCondition;
  CompilerCapabilityTypeKind compilerCapability =
      CompilerCapabilityTypeKind::None;
  bool lifecycleBoundBorrowCarrier = false;
  bool mutexGuardBorrowCarrier = false;
  ClassKind kind = ClassKind::Class;
  std::vector<HirBaseInstance> bases;
  std::vector<HirBaseInstance> structuralBases;
  bool abstract = false;
  bool polymorphic = false;
  bool cAbiRecord = false;
  bool cOpaqueHandle = false;
  std::optional<MirCAbiRecordLayout> cAbiLayout;
  std::optional<MirUnionLayout> unionLayout;
  SpecialMemberStatus defaultConstructor = SpecialMemberStatus::Deleted;
  bool generatedDefaultRequiresSchedule = false;
  SpecialMemberStatus copyConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveConstructor = SpecialMemberStatus::Deleted;
  SpecialMemberStatus copyAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus moveAssignment = SpecialMemberStatus::Deleted;
  SpecialMemberStatus destructorStatus = SpecialMemberStatus::Generated;
  std::optional<HirDestructorInstanceId> destructor;
  bool requiresActiveDropState = false;
  bool requiresActiveCleanup = false;
  std::vector<MirClassFieldInfo> declaredFields;
  std::vector<MirClassTypeAlias> typeAliases;
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
  std::optional<SemanticType> nativeCalleeType;

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
  std::vector<std::size_t> ownerlessTextParameters;
  std::vector<OwnerlessTextField> ownerlessTextFields;
  // Formals a returned fresh value holds in a direct text field (L-OWN-02).
  std::vector<PublishedTextField> publishedTextFields;
  // The returned value views the referent of read-only reference formal
  // returnBorrowParameter at the one returnBorrowFields path (L-OWN-02).
  bool dependentTextReturn = false;
  std::vector<HirBindingId> parameterBindings;
  ProgramEntryKind entryKind = ProgramEntryKind::None;
  std::optional<HirFunctionInstanceId> entryArgumentAppendTarget;
  bool staticMember = false;
  ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
  std::optional<OverloadedOperator> overloadedOperator;
  bool constexprFunction = false;
  bool requiresUnsafe = false;
  bool neverReturns = false;
  bool tracksCaller = false;
  CThreadRuntimeOperation cThreadOperation = CThreadRuntimeOperation::None;
  bool threadConfinedCallback = false;
  ProcessMemoryRuntimeOperation processMemoryOperation =
      ProcessMemoryRuntimeOperation::None;
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  // Resolved source intent supplied by semantics, not inferred from this body.
  bool explicitReturnBorrowOrigin = false;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  std::vector<std::size_t> returnBorrowParameters;
  std::vector<BorrowedFieldOrigin> returnBorrowFields;
  std::optional<FunctionBorrowDependencySummary> completeReturnBorrow;
  std::optional<BorrowOriginPlace> returnBorrowPlace;
  bool restoresReferenceResult = false;
  std::vector<RestoredBorrowField> restoredBorrowFields;
  std::optional<OwnedPointeeProjection> ownedPointeeProjection;
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
  CThreadCallbackKind cThreadCallback = CThreadCallbackKind::None;
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
  bool scoped = false;
  std::size_t scopeArgument = 0;

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
  bool constexprConstructor = false;
  bool requiresUnsafe = false;
  bool generatedDefault = false;
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
  std::vector<std::size_t> borrowParameters;
  std::optional<FunctionBorrowDependencySummary> completeBorrow;
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
  CallableInvocationCapability capability = CallableInvocationCapability::Read;
  std::vector<SymbolId> captureSymbols;
  std::vector<bool> captureRequiresActiveCleanup;
  // A reference result's owner contract: the parameter whose entry loan the
  // escaping Return loan must reach. Captures never own a result.
  BorrowOriginKind returnBorrowOrigin = BorrowOriginKind::None;
  std::size_t returnBorrowParameter = 0;
  AccessMode returnBorrowAccess = AccessMode::ReadOnly;
  MirEffectSummary effectSummary;
  MirBody body;

  friend bool operator==(const MirLambdaInstance &,
                         const MirLambdaInstance &) = default;
};

// Pointer-free public-root authority for one host embedding export. These
// rows are selected in semantics and copied through HIR; MIR never reopens
// source-level name lookup when establishing an ABI root.
struct MirEmbeddingExport {
  FunctionId declaration = 0;
  HirFunctionInstanceId function = 0;
  std::string qualifiedName;
  std::string symbol;
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  SourceSpan requestSource;

  friend bool operator==(const MirEmbeddingExport &,
                         const MirEmbeddingExport &) = default;
};

struct MirEmbeddingPlan {
  std::uint32_t abiVersion = embeddingAbiVersionV1;
  std::vector<MirEmbeddingExport> exports;

  friend bool operator==(const MirEmbeddingPlan &,
                         const MirEmbeddingPlan &) = default;
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

// A lexical binding evaluated inside one storage initializer, never persistent
// program storage. Retain its exact source and place identity independently of
// the operation shapes emitted for its initialization or cleanup. A structured
// name's place is its published projection of a separate hidden local owner;
// binding remains the name's semantic identity, not the physical owner's ID.
struct MirProgramInitializationLocal {
  HirBindingId binding = 0;
  SymbolId symbol = 0;
  MirPlaceId place = 0;

  friend bool operator==(const MirProgramInitializationLocal &,
                         const MirProgramInitializationLocal &) = default;
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
  std::vector<MirProgramInitializationLocal> locals;
  std::vector<HirFullExpressionId> nestedFullExpressions;

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
  BeginCThreadRuntime,
  EndCThreadRuntime,
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
  HirFunctionInstanceId cThreadBegin = 0;
  HirFunctionInstanceId cThreadEnd = 0;
  std::vector<MirHostedStartupOperation> operations;

  [[nodiscard]] const MirHostedStartupOperation *
  findOperation(MirHostedStartupOperationId id) const {
    return id == 0 || id > operations.size() ? nullptr : &operations[id - 1];
  }

  friend bool operator==(const MirHostedStartupPlan &,
                         const MirHostedStartupPlan &) = default;
};

// Exact numeric representation selected by semantics for a payload-free enum.
struct MirIntegralEnum {
  EnumId declaration = 0;
  SemanticType type = SemanticType::Unknown;
  SourceUnitId sourceUnit = 0;
  SemanticType underlyingType = SemanticType::Unknown;
  friend bool operator==(const MirIntegralEnum &,
                         const MirIntegralEnum &) = default;
};

// Pointer-free lifecycle authority for a closed payload enum. Variant rows
// retain the exact ordered field types and active-cleanup facts that MIR
// construction, extraction, ownership flow, and backends are allowed to use.
// Names remain declaration/lowered-program presentation metadata.
struct MirPayloadEnumVariant {
  std::size_t variantIndex = 0;
  std::vector<SemanticType> payloadTypes;
  std::vector<SymbolId> payloadSymbols;
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
  [[nodiscard]] const std::vector<ForeignStorageInfo> &
  foreignStorageDeclarations() const {
    return foreignStorage_;
  }
  [[nodiscard]] const ForeignStorageInfo *
  findForeignStorage(SymbolId symbol) const {
    const auto found = std::find_if(
        foreignStorage_.begin(), foreignStorage_.end(),
        [symbol](const auto &storage) { return storage.symbol == symbol; });
    return found == foreignStorage_.end() ? nullptr : &*found;
  }

  [[nodiscard]] bool valid() const { return valid_; }
  [[nodiscard]] const TargetInfo &target() const { return target_; }
  [[nodiscard]] ExecutionProfile executionProfile() const {
    return executionProfile_;
  }
  [[nodiscard]] ProgramKind programKind() const { return programKind_; }
  [[nodiscard]] bool requiresCThreadRuntime() const {
    if (executionProfile_ != ExecutionProfile::Concurrent) {
      return false;
    }
    for (const auto &function : functions) {
      if (requiresCThreadConcurrency(function.cThreadOperation)) {
        return true;
      }
    }
    for (const auto &adapter : nativeCallbacks) {
      if (adapter.cThreadCallback != CThreadCallbackKind::None) {
        return true;
      }
    }
    return false;
  }
  [[nodiscard]] const MirBody &module() const { return moduleBody; }

  [[nodiscard]] const std::optional<MirEmbeddingPlan> &embeddingPlan() const {
    return embeddingPlan_;
  }

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

  [[nodiscard]] const std::vector<MirIntegralEnum> &integralEnums() const {
    return integralEnums_;
  }

  [[nodiscard]] const MirIntegralEnum *findIntegralEnum(EnumId id) const {
    return findIntegralEnum(SemanticType::enumType(id));
  }

  [[nodiscard]] const MirIntegralEnum *
  findIntegralEnum(const SemanticType &type) const {
    const auto found =
        std::find_if(integralEnums_.begin(), integralEnums_.end(),
                     [&](const MirIntegralEnum &candidate) {
                       return candidate.type == type;
                     });
    return found == integralEnums_.end() ? nullptr : &*found;
  }

  [[nodiscard]] const MirPayloadEnum *findPayloadEnum(EnumId id) const {
    return findPayloadEnum(SemanticType::enumType(id));
  }

  [[nodiscard]] const MirPayloadEnum *
  findPayloadEnum(const SemanticType &type) const {
    const auto found = std::find_if(payloadEnums_.begin(), payloadEnums_.end(),
                                    [&](const MirPayloadEnum &candidate) {
                                      return candidate.type == type;
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

  // A closure type names its semantic lambda, whose numbering is separate
  // from the instance ids above; the two coincide only when every semantic
  // lambda is instantiated. Resolve the exact instance by its closure type.
  [[nodiscard]] const MirLambdaInstance *
  findLambdaForType(const SemanticType &type) const {
    if (type.kind != SemanticType::Lambda)
      return nullptr;
    for (const MirLambdaInstance &instance : lambdas)
      if (instance.type == type)
        return &instance;
    return nullptr;
  }

  [[nodiscard]] std::size_t blockCount() const;

  friend bool operator==(const MirProgram &, const MirProgram &) = default;

private:
  std::vector<ForeignStorageInfo> foreignStorage_;
  friend class MirLowerer;
  friend struct MirProgramTestAccess;

  bool valid_ = true;
  TargetInfo target_ = TargetInfo::host();
  ExecutionProfile executionProfile_ = ExecutionProfile::SingleThreaded;
  ProgramKind programKind_ = ProgramKind::Executable;
  FailureMetadata failureMetadata_;
  std::optional<MirEmbeddingPlan> embeddingPlan_;
  MirProgramInitializationPlan programInitialization;
  std::optional<MirHostedStartupPlan> hostedStartupPlan_;
  std::optional<MirBody> hostedStartupBody;
  MirBody moduleBody;
  std::vector<MirPayloadEnum> payloadEnums_;
  std::vector<MirIntegralEnum> integralEnums_;
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

// A constructor record can publish a field initializer that the body leaves
// unstaged: no Initialize instruction carries that initializer, so the field
// is written from the MIR value defined by the initializer's single HIR
// argument. Returns that initializer when `instruction` defines such a value
// in constructor `owner`'s body, or nullptr when an explicit Initialize owns
// the field write, the initializer stores a reference, the body is not a
// constructor, or the record names no such argument. The record is a consumer
// of that value even when the body records no use of it, so a verified
// rewrite must not remove the instruction as unused. Lowering, optimization
// and the backend share this one rule.
[[nodiscard]] const MirConstructorInitializer *
mirRecordPublishedConstructorInitializer(const MirProgram &program,
                                         const MirBody &body, std::size_t owner,
                                         const MirInstruction &instruction);

// These exact producers and stable places are consumed by the independently
// verified range inventory, not just ordinary SSA uses. Canonical rewrites
// must preserve them until they can repair and replay the corresponding range
// publication contract. Ordinary computations inside a range body are not
// protected merely because they occur in a loop.
[[nodiscard]] const MirRangeIteration *
mirRangePublishedInstruction(const MirBody &body,
                             const MirInstruction &instruction);
[[nodiscard]] const MirRangeIteration *
mirRangePublishedElementOwner(const MirBody &body, MirPlaceId place);

// Complete protected-place set, preserving deterministic first-seen order.
// Missing/malformed loan or dependency identities yield an empty set so
// consumers cannot accidentally treat incomplete provenance as disjoint.
[[nodiscard]] std::vector<MirPlaceId>
mirLoanProtectedSources(const MirBody &body, MirLoanId loan);

// Conservative same-block lifecycle proof. A moved-from shell still needs
// retirement, but cannot invoke its source destructor. Zero means block end.
[[nodiscard]] bool
mirDropFollowsDefiniteMove(const MirBlock &block,
                           MirDropObligationId obligation,
                           MirInstructionId beforeInstruction = 0);

// The same proof when the block itself leaves the obligation untouched: it
// follows invoke edges, from an invoke's success or failure continuation back
// to the invoking block, until the obligation's last event. Any other incoming
// edge, a merge or the entry stops it, so a partial CFG during lowering gives
// the complete CFG's answer. Lowering, verification and the backend share
// this one rule.
[[nodiscard]] bool
mirDropFollowsDefiniteMove(const MirBody &body, const MirBlock &block,
                           MirDropObligationId obligation,
                           MirInstructionId beforeInstruction = 0);

// Each block's incoming edge count and, when its only incoming edge is an
// invoke edge, the invoking block. Lowering records each terminator as it is
// set; a complete body builds the same table at once.
struct MirInvokeContinuations {
  std::vector<std::uint32_t> incomingEdges;
  std::vector<MirBlockId> invoker;
  std::vector<bool> successEdge;

  void record(const MirBlock &block);
};

[[nodiscard]] MirInvokeContinuations
mirInvokeContinuations(const MirBody &body);

// The same proof against a prepared continuation table, for callers that
// query many drops of one body.
[[nodiscard]] bool mirDropFollowsDefiniteMove(
    const MirBody &body, const MirInvokeContinuations &continuations,
    const MirBlock &block, MirDropObligationId obligation,
    MirInstructionId beforeInstruction = 0);

} // namespace lang
