#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

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
  // A Class row's semantic lifecycle proves the prelude-declaration shape
  // compiles: a usable default constructor and move assignment let the
  // value declare value-initialized and receive its construction by
  // assignment (the 0.215 boundary rule, carried here because the MIR
  // program records no special-member status).
  bool boundaryConstructible = false;
  // The 0.286 copy proof: a usable copy constructor and copy assignment
  // let a Load copy the place into the declared local.
  bool copyable = false;

  friend bool operator==(const CppMirTypeRepresentation &,
                         const CppMirTypeRepresentation &) = default;
};

struct CppMirBodyNameRepresentation {
  MirBodyAddress address;
  std::string spelling;

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

  friend bool operator==(const CppMirSymbolRepresentation &,
                         const CppMirSymbolRepresentation &) = default;
};

struct CppMirPayloadVariantRepresentation {
  std::size_t index = 0;
  std::string spelling;
  std::vector<SemanticType> fieldTypes;

  friend bool operator==(const CppMirPayloadVariantRepresentation &,
                         const CppMirPayloadVariantRepresentation &) = default;
};

struct CppMirEnumRepresentation {
  EnumId owner = 0;
  std::string spelling;
  SemanticType underlyingType = SemanticType::Unknown;
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

enum class CppMirBodyEmissionReadiness {
  Ready,
  MissingRepresentation,
  MissingMirAuthority,
  Incoherent,
};

enum class CppMirBodyEmissionIssueKind {
  InvalidMirProgram,
  InvalidBodyAddress,
  InvalidRepresentationEnum,
  InvalidRepresentationRow,
  DuplicateTypeRepresentation,
  DuplicateBodyRepresentation,
  DuplicateSymbolRepresentation,
  DuplicateEnumRepresentation,
  DuplicateContainedConstructorRepresentation,
  DuplicateCapabilityRepresentation,
  InvalidBodyKind,
  InvalidInstructionKind,
  InvalidOperation,
  InvalidOperandKind,
  InvalidPlaceRootKind,
  InvalidProjectionKind,
  InvalidTerminatorKind,
  MissingTypeRepresentation,
  MissingBodyRepresentation,
  MissingSymbolRepresentation,
  MissingEnumRepresentation,
  MissingCapabilityRepresentation,
  MissingOrderedCompoundMir,
  MissingCheckedFailureControlFlow,
  MissingAggregateRollbackMir,
  MissingCallInputScheduleMir,
  MissingConstructionScheduleMir,
  MissingPartialConstructionRollbackMir,
  MissingFailureCleanupMir,
  MissingProgramInitializationMir,
  MissingHostedStartupMir,
  UnsupportedTextVocabulary,
  Count,
};

struct CppMirBodyEmissionIssue {
  CppMirBodyEmissionIssueKind kind =
      CppMirBodyEmissionIssueKind::InvalidMirProgram;
  MirBodyAddress body;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::string detail;
};

struct CppMirBodyEmissionAnalysis {
  MirBodyAddress body;
  CppMirBodyEmissionReadiness readiness =
      CppMirBodyEmissionReadiness::Incoherent;
  std::vector<CppMirBodyEmissionIssue> issues;

  [[nodiscard]] bool ready() const {
    return readiness == CppMirBodyEmissionReadiness::Ready;
  }
};

struct CppMirProgramEmissionAnalysis {
  CppMirBodyEmissionReadiness readiness =
      CppMirBodyEmissionReadiness::Incoherent;
  std::vector<CppMirBodyEmissionAnalysis> bodies;
  // Program/map validation issues only. Body-local representation and text
  // issues remain attached to their exact entry in `bodies`, so one declined
  // specialized surface cannot suppress admission of every unrelated body.
  std::vector<CppMirBodyEmissionIssue> issues;

  [[nodiscard]] bool ready() const {
    return readiness == CppMirBodyEmissionReadiness::Ready;
  }
};

// One general text emission attempt. `text` is complete exactly when the
// fail-closed analysis is Ready and every construct in the body is inside the
// text step's ported vocabulary; there is never partial text.
struct CppMirBodyEmissionText {
  CppMirBodyEmissionAnalysis analysis;
  std::string text;

  [[nodiscard]] bool emitted() const {
    return analysis.ready() && !text.empty();
  }
};

// One field-initializer stage of a passive initializer body: the field's
// storage symbol and the spelled literal value, or an empty spelling for the
// bare default (no in-class initializer text).
struct CppMirFieldInitializerSpelling {
  SymbolId field = 0;
  std::string spelling;
};

// The extracted schedule of a FieldInitializers, StaticFieldInitializers, or
// Module body: a straight-line schedule whose values can be spelled from MIR
// literals, exact copied representation rows, and admitted construction
// stages before their per-field Initialize operations. `supported` is false
// for every shape the schedule cannot reproduce exactly.
struct CppMirInitializerScheduleText {
  CppMirBodyEmissionAnalysis analysis;
  bool supported = false;
  std::vector<CppMirFieldInitializerSpelling> fields;
};

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

// True when the concrete class has a compiler-proven, observation-free empty
// representation that the C++ backend may use while a failure-form
// constructor publishes its MIR field schedule. This is representation-only:
// it does not make a default constructor available to GTI source.
[[nodiscard]] bool
cppMirFailureConstructorEmptyStateEligible(const MirProgram &program,
                                           HirClassInstanceId instance);

// An explicit base initializer whose arguments are exact copy-loads of this
// constructor's scalar parameters. The native C++ initializer list may spell
// those parameters directly; all other base-expression shapes remain on the
// compatibility path until MIR carries a general initializer expression
// schedule.
struct CppMirBaseParameterInitializerBinding {
  std::size_t initializer = 0;
  SemanticType baseType = SemanticType::Unknown;
  HirClassInstanceId base = 0;
  HirConstructorInstanceId constructor = 0;
  std::vector<std::size_t> parameters;
};
[[nodiscard]]
std::optional<std::vector<CppMirBaseParameterInitializerBinding>>
cppMirBaseParameterInitializerBindings(
    const MirProgram &program, const MirConstructorInstance &constructor);

// A scalar field initializer whose argument is one exact copy-load of a
// constructor parameter. The native member-initializer list may spell the
// parameter directly and erase only the named Load/Initialize pair; every
// other expression shape remains in the MIR body schedule.
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

// The owned-parameter initializer schedule: each `field(std::move(parameter))`
// metadata row is tied bijectively to the exact parameter place, ownership
// Move, optional explicit This.field publication, and lexical parameter Drop.
// A backend may publish the field in its native member-initializer list and
// erase only the records named by this proof. A malformed or partially
// claimed schedule returns nullopt so constructor admission fails closed.
struct CppMirOwnedParameterFieldBinding {
  std::size_t initializer = 0;
  SymbolId field = 0;
  std::size_t parameter = 0;
  MirPlaceId sourcePlace = 0;
  MirValueId movedValue = 0;
  MirInstructionId moveInstruction = 0;
  MirInstructionId initializeInstruction = 0;
  MirInstructionId dropInstruction = 0;
  MirDropObligationId parameterDrop = 0;
};
[[nodiscard]]
std::optional<std::vector<CppMirOwnedParameterFieldBinding>>
cppMirOwnedParameterFieldBindings(const MirProgram &program,
                                  const MirConstructorInstance &constructor);

class CppMirBodyEmitter {
public:
  CppMirBodyEmitter(const MirProgram &program,
                    const CppMirBodyEmissionMap &representations)
      : program_(program), representations_(representations) {}

  [[nodiscard]] CppMirBodyEmissionAnalysis
  analyze(MirBodyAddress address) const;
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
  [[nodiscard]] bool supportsBodyTextImpl(MirBodyAddress address,
                                          bool failureForm) const;

  const MirProgram &program_;
  const CppMirBodyEmissionMap &representations_;
};

} // namespace lang
