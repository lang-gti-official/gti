#pragma once

#include "gti/mir.h"

#include <string>
#include <string_view>
#include <vector>

namespace lang {

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
  std::vector<CppMirBodyEmissionIssue> issues;

  [[nodiscard]] bool ready() const {
    return readiness == CppMirBodyEmissionReadiness::Ready;
  }
};

struct CppMirBodyEmissionText {
  CppMirBodyEmissionAnalysis analysis;
  std::string text;

  [[nodiscard]] bool emitted() const {
    return analysis.ready() && !text.empty();
  }
};

struct CppMirFieldInitializerSpelling {
  SymbolId field = 0;
  std::string spelling;

  friend bool operator==(const CppMirFieldInitializerSpelling &,
                         const CppMirFieldInitializerSpelling &) = default;
};

struct CppMirInitializerScheduleText {
  CppMirBodyEmissionAnalysis analysis;
  bool supported = false;
  std::vector<CppMirFieldInitializerSpelling> fields;
};

enum class CppMirHostedStartupEmissionKind {
  None,
  NoArgumentsFailure,
  NoArgumentsFailureFree,
  OwnedArguments,
};

class CppMirBodyEmitter;
class CppMirBodyTextWriter;

// Pointer-free, immutable admission result for one MIR body. Shape
// recognition and representation lookup happen while this value is built.
// Writers receive only the approved templates/schedules below and cannot
// independently admit a MIR operation.
class CppMirBodyEmissionPlan {
public:
  [[nodiscard]] MirBodyAddress body() const { return analysis_.body; }
  [[nodiscard]] const CppMirBodyEmissionAnalysis &analysis() const {
    return analysis_;
  }
  [[nodiscard]] bool ready() const { return analysis_.ready(); }
  [[nodiscard]] bool supportsPlainText() const {
    return ready() && !plainTextTemplate_.empty();
  }
  [[nodiscard]] bool supportsFailureText() const {
    return ready() && !failureTextTemplate_.empty();
  }
  [[nodiscard]] bool boundaryDeclaration() const {
    return ready() && boundaryDeclaration_;
  }
  [[nodiscard]] bool supportsInitializerSchedule() const {
    return ready() && initializerSchedule_;
  }
  [[nodiscard]] const std::vector<CppMirFieldInitializerSpelling> &
  initializerFields() const {
    return initializerFields_;
  }
  [[nodiscard]] CppMirHostedStartupEmissionKind hostedStartupKind() const {
    return ready() ? hostedStartupKind_ : CppMirHostedStartupEmissionKind::None;
  }
  [[nodiscard]] bool admitted() const {
    return supportsPlainText() || supportsFailureText() ||
           boundaryDeclaration() || supportsInitializerSchedule() ||
           hostedStartupKind() != CppMirHostedStartupEmissionKind::None;
  }

private:
  CppMirBodyEmissionAnalysis analysis_;
  std::string plainTextTemplate_;
  std::string failureTextTemplate_;
  bool boundaryDeclaration_ = false;
  bool initializerSchedule_ = false;
  std::vector<CppMirFieldInitializerSpelling> initializerFields_;
  CppMirHostedStartupEmissionKind hostedStartupKind_ =
      CppMirHostedStartupEmissionKind::None;

  friend class CppMirBodyEmitter;
  friend class CppMirBodyTextWriter;
};

class CppMirProgramEmissionPlan {
public:
  [[nodiscard]] bool ready() const {
    return readiness_ == CppMirBodyEmissionReadiness::Ready;
  }
  [[nodiscard]] CppMirBodyEmissionReadiness readiness() const {
    return readiness_;
  }
  [[nodiscard]] const std::vector<CppMirBodyEmissionPlan> &bodies() const {
    return bodies_;
  }
  [[nodiscard]] const std::vector<CppMirBodyEmissionIssue> &issues() const {
    return issues_;
  }
  [[nodiscard]] const CppMirBodyEmissionPlan *
  find(MirBodyAddress address) const;
  [[nodiscard]] CppMirProgramEmissionAnalysis analysis() const;

private:
  CppMirBodyEmissionReadiness readiness_ =
      CppMirBodyEmissionReadiness::Incoherent;
  std::vector<CppMirBodyEmissionPlan> bodies_;
  std::vector<CppMirBodyEmissionIssue> issues_;

  friend class CppMirBodyEmitter;
};

inline constexpr std::string_view cppMirBodyEmissionFamilyPlaceholder =
    "__gti_emission_family__";

} // namespace lang
