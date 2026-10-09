#pragma once

#include "cpp_mir_body_emission_plan.h"
#include "gti/mir.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

struct LoweredBody;

// Spell one verified cleanup recipe against an existing live representation.
// The caller owns ending that representation's lifetime. Collection recipes
// supply this same operation recursively to their reverse-live-element loop.
[[nodiscard]] std::string
cppMirCleanupValueExpression(const MirProgram &program, const MirDropType &type,
                             std::string_view value, std::string_view record,
                             std::string_view unwinding);

// This is the private, pointer-free representation input to the generic MIR
// body emitter. Production construction belongs to the one sealed C++
// representation-snapshot builder. Tests may construct rows directly, but no
// row is language or execution authority: every row is checked against MIR.
enum class CppMirTypeRepresentationKind {
  Void,
  Scalar,
  StringView,
  NullPointer,
  RawPointer,
  Reference,
  FixedArray,
  Class,
  Enum,
  Function,
  FunctionItem,
  Lambda,
  UniqueOwner,
  SharedPointer,
  Storage,
  Expected,
  Unexpected,
  Meta,
  Count,
};

struct CppMirTypeRepresentation {
  SemanticType type = SemanticType::Unknown;
  CppMirTypeRepresentationKind kind = CppMirTypeRepresentationKind::Scalar;
  std::string spelling;
  // A class template's qualified primary name, separate from its concrete
  // argument spelling. MIR body emission uses this only when a semantic class
  // contains an otherwise-unnameable closure type and composes the exact
  // body-local closure alias into the template-id.
  std::string templateNameSpelling;
  // The spelling was resolved in this body's native template scope, rather
  // than copied from the program-wide (potentially unnameable) type table.
  bool bodyTypeOverlay = false;
  // A Class row's semantic lifecycle proves the prelude-declaration shape
  // compiles: a usable default constructor and move assignment let the
  // value declare value-initialized and receive its construction by
  // assignment (the 0.215 boundary rule, carried here because the MIR
  // program records no special-member status).
  bool boundaryConstructible = false;
  // A usable copy constructor lets a Load publish directly into an
  // uninitialized lifetime slot.  Keep this separate from assignment:
  // borrow-carrying forward iterators deliberately support independent
  // cursor construction while forbidding reseating assignment.
  bool copyConstructible = false;
  // The 0.286 copy-assignment proof: both copy construction and copy
  // assignment are usable, so an already initialized destination may be
  // replaced from a copied place.
  bool copyable = false;

  friend bool operator==(const CppMirTypeRepresentation &,
                         const CppMirTypeRepresentation &) = default;
};

struct CppMirBodyNameRepresentation {
  MirBodyAddress address;
  std::string spelling;
  // The status ABI retains an unused void* result channel from a symbolic
  // result declaration/virtual family. It never denotes a MIR runtime value.
  bool voidFailureResultParameter = false;
  // A symbolic virtual result retains an explicit output channel across its
  // family. Ordinary direct-return calls pass nullptr; byte transport supplies
  // its verified destination instead. This is representation, not a MIR value.
  bool conditionalResultParameter = false;
  // A generic parameter occurs in no parameter type, but the instance is
  // closure-bearing, so its arguments cannot be spelled program-wide. Each
  // call site spells them with its own closure aliases.
  bool callSiteTemplateArguments = false;

  friend bool operator==(const CppMirBodyNameRepresentation &,
                         const CppMirBodyNameRepresentation &) = default;
};

enum class CppMirSymbolRepresentationKind {
  Storage,
  Field,
  Capture,
  Count,
};

struct CppMirSymbolRepresentation {
  CppMirSymbolRepresentationKind kind = CppMirSymbolRepresentationKind::Storage;
  // Class-instance ID for a field, lambda-instance ID for a capture, and zero
  // for namespace storage. A static-field storage row may retain its class
  // owner while still using the Storage kind.
  std::size_t owner = 0;
  SymbolId symbol = 0;
  std::size_t ordinal = 0;
  SemanticType type = SemanticType::Unknown;
  std::string spelling;
  // A Field row may also carry the source declaration's dependent type
  // spelling. Concrete MIR instances still prove the exact field type; this
  // copied representation is used only when their shared C++ template
  // definition must spell a direct initializer in terms of T/N parameters.
  std::string declarationTypeSpelling;
  // Expression access unwraps a conditional byte-preserving field cell. Keep
  // this separate from the actual member declaration/initializer name.
  bool bytePreservingFieldCell = false;
  // A runtime-initialized owning program storage row names a backend lifetime
  // slot rather than an already-live semantic value. Module/0 constructs the
  // slot, while ordinary source reads and writes access its engaged value.
  // This is a C++ representation fact only; MIR continues to name the same
  // program storage place and owns initialization order.
  bool runtimeLifetimeSlot = false;

  friend bool operator==(const CppMirSymbolRepresentation &,
                         const CppMirSymbolRepresentation &) = default;
};

struct CppMirPayloadVariantRepresentation {
  std::size_t index = 0;
  std::string spelling;
  std::vector<SemanticType> fieldTypes;
  std::vector<bool> fieldRequiresActiveCleanup;

  friend bool operator==(const CppMirPayloadVariantRepresentation &,
                         const CppMirPayloadVariantRepresentation &) = default;
};

struct CppMirEnumRepresentation {
  EnumId owner = 0;
  SemanticType type = SemanticType::Unknown;
  std::string spelling;
  SemanticType underlyingType = SemanticType::Unknown;
  SemanticTypeTraits traits{};
  bool requiresActiveCleanup = false;
  std::vector<CppMirPayloadVariantRepresentation> payloadVariants;

  friend bool operator==(const CppMirEnumRepresentation &,
                         const CppMirEnumRepresentation &) = default;
};

// One exact source constructor that the C++ backend may expose through its
// terminally-contained constructor tag. The row is target representation,
// not executable authority: initializer selection still proves the matching
// Construct/Invoke schedule and admits the constructor's failure-form MIR
// body before using it.
struct CppMirContainedConstructorRepresentation {
  HirConstructorInstanceId constructor = 0;
  SemanticType ownerType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
  std::string tagSpelling;
  std::string stateSpelling;

  friend bool
  operator==(const CppMirContainedConstructorRepresentation &,
             const CppMirContainedConstructorRepresentation &) = default;
};

// A capability row names a sealed C++ representation helper or adapter
// family. It never asserts that MIR has a missing schedule: the analysis below
// reports MissingMirAuthority even when a matching row is present.
enum class CppMirEmissionCapabilityKind {
  Aggregate,
  Borrow,
  Bounds,
  CallableDispatch,
  Closure,
  DefinedFailure,
  Expected,
  HostedEntry,
  Intrinsic,
  LifetimeStorage,
  NativeInterop,
  Payload,
  ProgramInitialization,
  RawMemory,
  Synchronization,
  VirtualDispatch,
  Count,
};

struct CppMirEmissionCapabilityRepresentation {
  CppMirEmissionCapabilityKind kind = CppMirEmissionCapabilityKind::Aggregate;
  std::string spelling;

  friend bool
  operator==(const CppMirEmissionCapabilityRepresentation &,
             const CppMirEmissionCapabilityRepresentation &) = default;
};

struct CppMirBodyEmissionMapRows {
  std::vector<CppMirTypeRepresentation> types;
  std::vector<CppMirBodyNameRepresentation> bodies;
  std::vector<CppMirSymbolRepresentation> symbols;
  std::vector<CppMirEnumRepresentation> enums;
  std::vector<CppMirContainedConstructorRepresentation> containedConstructors;
  std::vector<CppMirEmissionCapabilityRepresentation> capabilities;
};

class CppMirBodyEmissionMap {
public:
  CppMirBodyEmissionMap() = default;
  explicit CppMirBodyEmissionMap(CppMirBodyEmissionMapRows rows);

  [[nodiscard]] const std::vector<CppMirTypeRepresentation> &types() const {
    return types_;
  }
  [[nodiscard]] const std::vector<CppMirBodyNameRepresentation> &
  bodies() const {
    return bodies_;
  }
  [[nodiscard]] const std::vector<CppMirSymbolRepresentation> &symbols() const {
    return symbols_;
  }
  [[nodiscard]] const std::vector<CppMirEnumRepresentation> &enums() const {
    return enums_;
  }
  [[nodiscard]]
  const std::vector<CppMirContainedConstructorRepresentation> &
  containedConstructors() const {
    return containedConstructors_;
  }
  [[nodiscard]] const std::vector<CppMirEmissionCapabilityRepresentation> &
  capabilities() const {
    return capabilities_;
  }

private:
  std::vector<CppMirTypeRepresentation> types_;
  std::vector<CppMirBodyNameRepresentation> bodies_;
  std::vector<CppMirSymbolRepresentation> symbols_;
  std::vector<CppMirEnumRepresentation> enums_;
  std::vector<CppMirContainedConstructorRepresentation> containedConstructors_;
  std::vector<CppMirEmissionCapabilityRepresentation> capabilities_;
};

// A concrete variadic instance retains one semantic TypePack parameter in
// MIR, while the native ABI receives one parameter per concrete pack element.
// Only a final, concrete pack has this representation; symbolic or misplaced
// packs stay unavailable to per-instance MIR emission.
[[nodiscard]] std::optional<std::vector<SemanticType>>
cppMirFlattenConcreteParameterTypes(
    std::span<const SemanticType> parameterTypes);

// Exhaustive enum-level encoding classification. RepresentedByMir says the
// core record has a complete backend-readable shape, not that every contextual
// ordering/failure/lifecycle proof has landed. NeedsCopiedRepresentation says
// execution additionally needs a checked row from CppMirBodyEmissionMap.
enum class CppMirEmissionEncoding {
  RepresentedByMir,
  NeedsCopiedRepresentation,
  MissingMirAuthority,
  Invalid,
};

// The exact representation-row kind the emission analysis expects for a
// semantic type, or nullopt for Unknown. The production rows builder mirrors
// this mapping so a built row can never be structurally stale against the
// analysis.
[[nodiscard]] std::optional<CppMirTypeRepresentationKind>
cppMirExpectedTypeRepresentation(const SemanticType &type);

// Expected with a class payload uses uninitialized result storage in the
// transformed ABI. Native boundary adapters must share body emission's exact
// move/representation eligibility rather than default-constructing a payload.
[[nodiscard]] bool
cppMirExpectedClassPlacementResult(const MirProgram &program,
                                   const CppMirBodyEmissionMap &representations,
                                   const SemanticType &type);

// Lexical fixed arrays (including cleanup-free move-only elements) cannot
// assume default/copy construction at an ABI boundary. Share their MIR-derived
// placement contract with adapters.
[[nodiscard]] bool cppMirOwningArrayPlacementResult(const MirProgram &program,
                                                    const SemanticType &type);

// The single spelling authority for a string-view literal: the exact
// `std::string_view{"…", N}` braced form both backends spell, with the
// data escaped byte-for-byte (embedded NULs included, so the explicit
// length is load-bearing).
[[nodiscard]] std::string
cppMirStringViewLiteralSpelling(std::string_view value);

// Single authority for the byte-exact floating literal both backends
// emit: std::bit_cast over the literal's stored bits, so text can never
// drift from the frontend's binary value.
[[nodiscard]] std::string cppMirBinaryFloatLiteralSpelling(BinaryFloat value);

// The single spelling authority for the shipped checked-operation helper
// family (`mir_failure_status_v1`): the fully qualified `mir_checked_*_v1`
// helper a backend spells for a checked MIR operation, and an empty view
// for every other operation.
[[nodiscard]] std::string_view
cppMirCheckedOperationHelperSpelling(MirOperation operation);

// The status-returning compound assignment helper family
// (`mir_checked_compound_*_v1`); empty for non-compound operations.
[[nodiscard]] std::string_view
cppMirCompoundCheckedHelperSpelling(MirOperation operation);

// The native compound token used only after verified MIR has discharged every
// local failure outcome and C++ has the same successful-path semantics. Empty
// for operations such as remainder and shifts whose defined GTI result is not
// fully represented by the corresponding C++ operator.
[[nodiscard]] std::string_view
cppMirCompoundOperatorSpelling(MirOperation operation);

// The single spelling authority for the shipped integer-arithmetic helper
// family (`gti_internal_backend_helpers_v1`): the fully qualified helper a
// backend spells for one of the nine wrapping/saturating/checked intrinsic
// kinds, and an empty view for every other kind. The general text
// vocabulary admits only the six wrapping/saturating kinds (a checked
// variant produces an `Expected` result outside the scalar vocabulary);
// the compatibility emitter also spells the checked template form.
[[nodiscard]] std::string_view
cppIntegerArithmeticIntrinsicSpelling(IntrinsicKind intrinsic);

[[nodiscard]] CppMirEmissionEncoding
classifyCppMirInstructionKind(MirInstructionKind kind);
[[nodiscard]] CppMirEmissionEncoding
classifyCppMirOperation(MirOperation operation);
[[nodiscard]] CppMirEmissionEncoding
classifyCppMirOperandKind(MirOperandKind kind);
[[nodiscard]] CppMirEmissionEncoding
classifyCppMirPlaceRootKind(MirPlaceRootKind kind);
[[nodiscard]] CppMirEmissionEncoding
classifyCppMirProjectionKind(MirProjectionKind kind);
[[nodiscard]] CppMirEmissionEncoding
classifyCppMirTerminatorKind(MirTerminatorKind kind);
[[nodiscard]] CppMirEmissionEncoding classifyCppMirBodyKind(MirBodyKind kind);

// Fail-closed generic body-emission front gate and general per-instance text
// step (ADR 016). This class deliberately has no Program, AST, HirBody,
// SemanticModel, or OptimizationResult input: analysis checks MIR against the
// copied representation rows, and text emission consumes only MIR and those
// rows. Text emission runs only after this analysis is Ready; current known
// gaps are therefore explicit rather than delegated to the compatibility
// emitter.
// Single authority for the no-argument hosted-startup adapter schedule,
// shared by the emission analysis and the adapter emission.
[[nodiscard]] bool
cppMirHostedStartupNoArgumentsSchedule(const MirProgram &program);

// The failure-free no-argument hosted schedule: CallEntry with no failure
// behavior, then ReturnEntry — the bare call/return adapter of an entry
// that cannot raise.
[[nodiscard]] bool
cppMirHostedStartupFailureFreeSchedule(const MirProgram &program);

// Single authority for the owned-arguments hosted-startup marshaling
// schedule: count validation and conversion, argument-vector construction,
// the per-argument view/string/append loop with its failure-cleanup
// envelope, and the entry call, each under immediate-exit-70 containment.
// The emitted argc/argv main is then that body's complete emission.
[[nodiscard]] bool
cppMirHostedStartupOwnedArgumentsSchedule(const MirProgram &program);

// True when Module/0 can propagate a defined failure to HostedStartup. This
// is derived from the verified module instructions and is shared by module
// text selection and the hosted adapter; the backend must not infer it from
// source initializers or emitted C++.
[[nodiscard]] bool
cppMirModuleMayRaiseDefinedFailure(const MirProgram &program);

// Single naming authority for the transformed failure sibling (ADR 017),
// shared by the compatibility signature/wrapper emission and the verified
// caller spelling. A plain member name carries the __gti_mir_failure
// suffix directly. A structural operator bridge spells a real C++
// operator name, which cannot carry a suffix, so its sibling derives a
// mangled token name; an operator outside the token map returns empty and
// the body keeps the compatibility route.
[[nodiscard]] std::string
cppMirFailureSiblingSpelling(std::string_view memberSpelling);

// Resolves a virtual function instance to the pure interface contract whose
// transformed status ABI it must share. The bounded cutover surface accepts
// only exact GTI interface overrides: return and parameter types, receiver
// mutability, operator identity, and concrete base ancestry must all agree.
// Non-virtual calls and virtual families without that exact contract return
// nullptr and remain on the compatibility path.
[[nodiscard]] const MirFunctionInstance *
cppMirVirtualFailureContractRoot(const MirProgram &program,
                                 const MirFunctionInstance &function);

// Compiler-private tag used to select the explicit-data constructor failure
// overload. Keeping the spelling in the shared backend authority prevents a
// caller and declaration from drifting to different generated C++ types.
[[nodiscard]] std::string
cppMirFailureConstructorTagSpelling(HirConstructorInstanceId constructor);

// Compiler-private tag used by an in-class initializer to select the exact
// terminal-containment overload paired with a failure-form constructor.
[[nodiscard]] std::string
cppMirContainedConstructorTagSpelling(HirConstructorInstanceId constructor);

// The reference-field initializer schedule (ADR 018): each
// stores-reference constructor initializer pairs bijectively with one
// Stored loan on the same field whose source is the dereference carrier of
// one reference parameter. The C++ member-initializer list binds the field
// to that parameter and no loan pointer ever materializes; any other
// Stored-loan shape returns nullopt and the body declines fail-closed.
struct CppMirStoredReferenceBinding {
  std::size_t initializer = 0;
  SymbolId field = 0;
  std::size_t parameter = 0;
};
[[nodiscard]] std::optional<std::vector<CppMirStoredReferenceBinding>>
cppMirStoredReferenceBindings(const MirConstructorInstance &constructor);

// True when a constructor and every constructor selected by its initializer
// metadata can never report a recoverable failure to its caller. Terminal
// failure inside a plain helper is permitted: it does not return and therefore
// cannot take a caller's failure edge. Cycles and missing targets fail closed.
[[nodiscard]] bool
cppMirConstructorStatusCannotFail(const MirProgram &program,
                                  HirConstructorInstanceId constructor);

// True when an exact field-publication proof still requires in-place body
// construction after native initializer bindings claim their own values.
// That remaining schedule has only a status-ABI spelling even if its
// constructor cannot report failure; no-failure is not a plain-body witness.
[[nodiscard]] bool cppMirConstructorRequiresFieldPlacement(
    const MirProgram &program, const MirConstructorInstance &constructor);

// True when this concrete MIR body has no status-producing failure record or
// routing terminator.  The C++ adapter layer uses this exact proof before
// exposing an ordinary wrapper for a conservatively may-raise source member.
[[nodiscard]] bool cppMirBodyStatusCannotFail(const MirBody &body);

// True only when one verified body consumes this exact source member as the
// inline class-valued producer of a later operation. The C++ declaration
// emitter uses this to publish the narrow failure-free boundary wrapper needed
// by that use, without manufacturing ordinary entry points for unrelated
// failure-form members.
[[nodiscard]] bool cppMirNeedsFailureFreeBoundaryWrapper(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    HirFunctionInstanceId function);

// True only when one verified body consumes this exact source member through
// the class-subscript place compatibility path. That path has no call result
// on which to encode a failure edge, so it needs an ordinary member boundary
// that invokes the admitted failure sibling and contains failure terminally.
[[nodiscard]] bool cppMirNeedsTerminalSubscriptBoundaryWrapper(
    const MirProgram &program, const CppMirBodyEmissionMap &representations,
    HirFunctionInstanceId function);

// True when the concrete class has a compiler-proven, observation-free empty
// representation that the C++ backend may use while a failure-form
// constructor publishes its MIR field schedule. This is representation-only:
// it does not make a default constructor available to GTI source.
[[nodiscard]] bool
cppMirFailureConstructorEmptyStateEligible(const MirProgram &program,
                                           HirClassInstanceId instance);

[[nodiscard]] bool
cppMirFailureConstructorEmptyStateTypeEligible(const MirProgram &program,
                                               const SemanticType &type);

// Unlike an empty value of the whole class, empty fields are also valid while
// a source destructor's lifecycle guard is disarmed during construction.
// This proof permits body-ordered parameter publication, not a new empty
// constructor for a user-destructor-bearing class.
[[nodiscard]] bool
cppMirFailureConstructorEmptyFieldsEligible(const MirProgram &program,
                                            HirClassInstanceId instance);

// An exact selected base initializer. Native fusion preserves observation-free
// scalar parameter copy-loads/literals. A prepared binding instead selects
// the admitted executable pre-base partition below; it never reconstructs
// checked or effectful source expressions in the native initializer list.
struct CppMirConstructedFieldInitializerArgument {
  SemanticType type = SemanticType::Unknown;
  std::optional<std::size_t> parameter;
  std::optional<Literal> literal;
  bool moved = false;
};
struct CppMirBaseParameterInitializerBinding {
  std::size_t initializer = 0;
  SemanticType baseType = SemanticType::Unknown;
  HirClassInstanceId base = 0;
  HirConstructorInstanceId constructor = 0;
  std::vector<std::size_t> parameters;
  std::vector<CppMirConstructedFieldInitializerArgument> arguments;
  bool prepared = false;
};
// Exact pre-base MIR partition. No source expressions are reconstructed by
// its representation writer; scalar values and places cross the native
// delegation boundary through a pointer-free preparation carrier.
struct CppMirBasePreparation {
  MirInstructionId stage = 0;
  MirBlockId stageBlock = 0;
  std::vector<MirBlockId> blocks;
  std::vector<MirInstructionId> instructions;
  std::vector<MirValueId> values;
  std::vector<MirValueId> carriedValues;
  std::vector<MirPlaceId> places;
};
[[nodiscard]] bool
cppMirBaseGateEligible(const MirProgram &program,
                       const MirConstructorInstance &constructor);
[[nodiscard]] std::optional<CppMirBasePreparation>
cppMirBasePreparation(const MirProgram &program,
                      const MirConstructorInstance &constructor);
[[nodiscard]]
std::optional<std::vector<CppMirBaseParameterInitializerBinding>>
cppMirBaseParameterInitializerBindings(
    const MirProgram &program, const MirConstructorInstance &constructor);

// A scalar or passive generated-copy class field initializer whose argument is
// one exact copy-load of a constructor parameter. The native
// member-initializer list may spell the parameter directly after proving the
// paired MIR Initialize for either scalar or passive class fields. Every other
// expression shape remains in the MIR body schedule.
struct CppMirCopyParameterFieldBinding {
  std::size_t initializer = 0;
  SymbolId field = 0;
  std::size_t parameter = 0;
  MirPlaceId sourcePlace = 0;
  MirValueId loadedValue = 0;
  MirInstructionId loadInstruction = 0;
  MirInstructionId initializeInstruction = 0;
  MirInstructionId boundaryInstruction = 0;
};
[[nodiscard]]
std::optional<std::vector<CppMirCopyParameterFieldBinding>>
cppMirCopyParameterFieldBindings(const MirProgram &program,
                                 const MirConstructorInstance &constructor);

// An explicit class-typed field initializer whose value is one exact ordinary
// `Construct` of the field's own type from copy-loads of this constructor's
// scalar parameters or exact source scalar literals. C++17
// guaranteed elision makes `field(Type(argument...))` in the native
// member-initializer list the exact representation of MIR's
// construct-then-publish schedule: the field is constructed once, in place,
// with no intermediate object and no invented copy or move.
//
// The main proof is deliberately whole-constructor. Because C++ initializes
// members in declaration order, one hoisted initializer could otherwise be
// reordered against a sibling that still publishes from the body. This family
// therefore claims a schedule only when every declared field has its own
// explicit initializer in declaration order and the entry block contains
// nothing but the instructions named below.
//
// A cleanup-free field retains its exact Initialize and This-rooted field
// place; only its temporary slot and drop identities are zero. Scalar literal
// and substituted scalar default siblings share this declaration-ordered
// schedule. An active-cleanup field additionally requires the produced value's
// `Value` obligation, its `Reparent` into the field's `ConstructionRollback`
// obligation, and one unconditional transfer-out at normal completion. Every
// other initializer-expression shape stays in the MIR body schedule.
// A separate exact prefix admits one cleanup-bound borrowed child field from
// read-only reference formals and a non-failing selected constructor. Its
// Value-to-ConstructionRollback publication and dependency transfer remain
// mandatory; later body/failure cleanup is not part of the fused prefix.
struct CppMirConstructedFieldInitializerBinding {
  std::size_t initializer = 0;
  SymbolId field = 0;
  SemanticType fieldType = SemanticType::Unknown;
  HirConstructorInstanceId constructor = 0;
  std::vector<CppMirConstructedFieldInitializerArgument> arguments;
  MirValueId constructedValue = 0;
  MirInstructionId constructInstruction = 0;
  std::vector<MirInstructionId> argumentInstructions;
  MirInstructionId initializeInstruction = 0;
  MirInstructionId boundaryInstruction = 0;
  MirPlaceId slotPlace = 0;
  MirPlaceId fieldPlace = 0;
  MirDropObligationId valueDrop = 0;
  MirDropObligationId fieldDrop = 0;
  // The first native field uses its selected status-tag constructor. The
  // enclosing body preserves that status and executes the original MIR Invoke.
  bool failureStatus = false;
};
// Returns the complete claimed schedule, or an empty vector when this
// constructor is outside the family. The family is purely additive: an
// unclaimed constructor keeps whatever route it already had.
[[nodiscard]]
std::vector<CppMirConstructedFieldInitializerBinding>
cppMirConstructedFieldInitializerBindings(
    const MirProgram &program, const MirConstructorInstance &constructor);

// The owned-parameter initializer schedule: each `field(std::move(parameter))`
// metadata row is tied bijectively to the exact parameter place, ownership
// Move, optional explicit This.field publication, the exact value-to-field
// rollback transfer for an active field, and, for a lexical value, its
// parameter Drop. A concrete passive specialization has no lifecycle/drop
// records; the zero identities below represent that proven absence. A backend
// may publish the field in its native member-initializer list and erase only
// the records named by this proof. A malformed or partially claimed schedule
// returns nullopt so constructor admission fails closed.
struct CppMirOwnedParameterFieldBinding {
  std::size_t initializer = 0;
  SymbolId field = 0;
  std::size_t parameter = 0;
  MirPlaceId sourcePlace = 0;
  MirValueId movedValue = 0;
  MirInstructionId moveInstruction = 0;
  MirInstructionId initializeInstruction = 0;
  MirInstructionId fieldTransferInstruction = 0;
  MirInstructionId dropInstruction = 0;
  MirDropObligationId parameterDrop = 0;
  MirDropObligationId valueDrop = 0;
  MirDropObligationId fieldDrop = 0;
};
[[nodiscard]]
std::optional<std::vector<CppMirOwnedParameterFieldBinding>>
cppMirOwnedParameterFieldBindings(const MirProgram &program,
                                  const MirConstructorInstance &constructor);

// Optional per-caller instrumentation. It observes validation without changing
// admission and has no process-wide cache or lifetime beyond the caller.
struct CppMirBodyEmissionMetrics {
  std::size_t programVerifications = 0;
};

class CppMirBodyEmitter {
public:
  CppMirBodyEmitter(const MirProgram &program,
                    const CppMirBodyEmissionMap &representations,
                    CppMirBodyEmissionMetrics *metrics = nullptr,
                    std::span<const LoweredBody> debugBodies = {})
      : program_(program), representations_(representations), metrics_(metrics),
        debugBodies_(debugBodies) {}

  [[nodiscard]] CppMirBodyEmissionAnalysis
  analyze(MirBodyAddress address) const;
  [[nodiscard]] CppMirBodyEmissionPlan buildPlan(MirBodyAddress address) const;
  [[nodiscard]] CppMirProgramEmissionPlan buildProgramPlan() const;
  // Verifies both MIR/representation coherence and complete text coverage for
  // every core body. A Ready result is the backend preflight guarantee that no
  // executable body needs an AST/HIR emission path.
  [[nodiscard]] CppMirProgramEmissionAnalysis analyzeProgram() const;

  // General per-instance body text for the scalar vocabulary, including the
  // private Module/0 runtime-initialization function. `familyLabel` is the
  // production verified-MIR marker label and `indentation` is the caller's
  // two-space indentation depth at the body's opening brace.
  // Analysis runs first; a non-Ready body returns its issues and no text. A
  // Ready body whose construct falls outside the ported vocabulary is
  // emission drift and throws, matching the transitional emitter's
  // fail-closed behavior.
  [[nodiscard]] CppMirBodyEmissionText
  emitBodyText(MirBodyAddress address, std::string_view familyLabel,
               std::size_t indentation) const;
  [[nodiscard]] CppMirBodyEmissionText
  emitBodyText(const CppMirBodyEmissionPlan &plan, std::string_view familyLabel,
               std::size_t indentation) const;

  // Non-throwing vocabulary probe for the general bb-loop text form: true
  // exactly when every place, value, instruction, and terminator of the
  // addressed function body is inside the text step's ported vocabulary and
  // every consulted representation row is present. Admission combines this
  // with a Ready analysis; the pair is the single authority on what the
  // general emitter can publish, so selection never re-models emission.
  [[nodiscard]] bool supportsBodyText(MirBodyAddress address) const;

  // The failure form of the vocabulary (ADR 017): the body emits under the
  // transformed private ABI — checked operations produce
  // `mir_failure_status_v1` results, `Invoke` branches on the status and
  // writes the exact MIR failure record on the failure edge,
  // `PropagateFailure` returns false after its cleanup block, and `Return`
  // publishes through the out-parameter before returning true. The current
  // slice admits leaf bodies only: no calls, a non-void scalar result, and
  // every failure detector carrying exactly one site and origin.
  [[nodiscard]] bool supportsFailureBodyText(MirBodyAddress address) const;

  // True exactly when the addressed function body is the shell of a
  // non-source definition: a native or intrinsic declaration whose C++
  // surface is carried entirely by the shipped runtime headers and helper
  // families, so the body's complete emission is the absence of a
  // definition. The shape is verified against the real body — one
  // reachable block, no instructions, no loans, drops, cleanup
  // boundaries, or failure records, and a Return or Unreachable
  // terminator.
  [[nodiscard]] bool boundaryDeclarationBody(MirBodyAddress address) const;

  // True exactly when the program's hosted-startup plan is the
  // no-argument adapter schedule the backend emits: one CallEntry with
  // propagated failure, RouteOperationFailure, ContainFailure, and
  // ReturnEntry under the immediate-exit-70 policy. The emitted program
  // entry adapter is then that body's complete authorized emission.
  [[nodiscard]] bool hostedStartupNoArgumentsSchedule() const {
    return cppMirHostedStartupNoArgumentsSchedule(program_);
  }

  [[nodiscard]] CppMirBodyEmissionText
  emitFailureBodyText(MirBodyAddress address, std::string_view familyLabel,
                      std::size_t indentation) const;
  [[nodiscard]] CppMirBodyEmissionText
  emitFailureBodyText(const CppMirBodyEmissionPlan &plan,
                      std::string_view familyLabel,
                      std::size_t indentation) const;

  // Extracts the passive initializer schedule of a FieldInitializers,
  // StaticFieldInitializers, or Module body so the emission site can spell
  // in-class initializers (and verified-empty markers) from MIR.
  [[nodiscard]] CppMirInitializerScheduleText
  initializerSchedule(MirBodyAddress address) const;

  // True only for the bounded ordinary-constructor composition where C++ may
  // construct a state-bearing base directly: the derived fields and body are
  // failure-free, every concrete base uses an exact zero-argument source
  // constructor, and that base constructor plus its field schedule are both
  // independently spellable from verified MIR.
  [[nodiscard]] bool supportsNativeContainedBaseConstruction(
      HirConstructorInstanceId constructor) const;

private:
  [[nodiscard]] CppMirBodyEmissionPlan
  buildPlanFromAnalysis(CppMirBodyEmissionAnalysis analysis) const;
  [[nodiscard]] bool supportsBodyTextImpl(MirBodyAddress address,
                                          bool failureForm) const;

  const MirProgram &program_;
  const CppMirBodyEmissionMap &representations_;
  CppMirBodyEmissionMetrics *metrics_ = nullptr;
  std::span<const LoweredBody> debugBodies_;
  // Exact immutable-program cache. Discover preparation targets once rather
  // than scanning every constructor body again for each emitted constructor.
  mutable std::optional<std::vector<HirConstructorInstanceId>> baseGateTargets_;
};

} // namespace lang
