#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>
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
  PackFold,
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
  [[nodiscard]] const std::vector<CppMirEmissionCapabilityRepresentation> &
  capabilities() const {
    return capabilities_;
  }

private:
  std::vector<CppMirTypeRepresentation> types_;
  std::vector<CppMirBodyNameRepresentation> bodies_;
  std::vector<CppMirSymbolRepresentation> symbols_;
  std::vector<CppMirEnumRepresentation> enums_;
  std::vector<CppMirEmissionCapabilityRepresentation> capabilities_;
};

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
  MissingPackExpansionMir,
  MissingOrderedCompoundMir,
  MissingCheckedFailureControlFlow,
  MissingAggregateRollbackMir,
  MissingCallInputScheduleMir,
  MissingConstructionScheduleMir,
  MissingPartialConstructionRollbackMir,
  MissingFailureCleanupMir,
  MissingProgramInitializationMir,
  MissingHostedStartupMir,
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

// The extracted schedule of a passive FieldInitializers,
// StaticFieldInitializers, or Module body: one straight-line block ending in
// Exit whose only work is literal materialization and per-field Initialize
// stages. `supported` is false for any other shape (checked detectors,
// storage reads, cross-field references), which stays with the
// compatibility route.
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

// Single authority for the owned-arguments hosted-startup marshaling
// schedule: count validation and conversion, argument-vector construction,
// the per-argument view/string/append loop with its failure-cleanup
// envelope, and the entry call, each under immediate-exit-70 containment.
// The emitted argc/argv main is then that body's complete emission.
[[nodiscard]] bool
cppMirHostedStartupOwnedArgumentsSchedule(const MirProgram &program);

// Single naming authority for the transformed failure sibling (ADR 017),
// shared by the compatibility signature/wrapper emission and the verified
// caller spelling. A plain member name carries the __gti_mir_failure
// suffix directly. A structural operator bridge spells a real C++
// operator name, which cannot carry a suffix, so its sibling derives a
// mangled token name; an operator outside the token map returns empty and
// the body keeps the compatibility route.
[[nodiscard]] std::string
cppMirFailureSiblingSpelling(std::string_view memberSpelling);

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

class CppMirBodyEmitter {
public:
  CppMirBodyEmitter(const MirProgram &program,
                    const CppMirBodyEmissionMap &representations)
      : program_(program), representations_(representations) {}

  [[nodiscard]] CppMirBodyEmissionAnalysis
  analyze(MirBodyAddress address) const;
  [[nodiscard]] CppMirProgramEmissionAnalysis analyzeProgram() const;

  // General per-instance body text for the scalar vocabulary. `familyLabel`
  // is the production verified-MIR marker label and `indentation` is the
  // caller's two-space indentation depth at the body's opening brace.
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

private:
  [[nodiscard]] bool supportsBodyTextImpl(MirBodyAddress address,
                                          bool failureForm) const;

  const MirProgram &program_;
  const CppMirBodyEmissionMap &representations_;
};

} // namespace lang
