#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <string>
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

// Fail-closed generic body-emission front gate. This class deliberately has no
// Program, AST, HirBody, SemanticModel, or OptimizationResult input. A future
// text-emission step may run only after this analysis is Ready; current known
// gaps are therefore explicit rather than delegated to the compatibility
// emitter.
class CppMirBodyEmitter {
public:
  CppMirBodyEmitter(const MirProgram &program,
                    const CppMirBodyEmissionMap &representations)
      : program_(program), representations_(representations) {}

  [[nodiscard]] CppMirBodyEmissionAnalysis
  analyze(MirBodyAddress address) const;
  [[nodiscard]] CppMirProgramEmissionAnalysis analyzeProgram() const;

private:
  const MirProgram &program_;
  const CppMirBodyEmissionMap &representations_;
};

} // namespace lang
