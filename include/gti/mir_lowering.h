#pragma once

#include "gti/mir.h"
#include "gti/mir_effects.h"

namespace lang {

// Transient construction diagnostics, not part of serialized executable MIR.
struct MirLoweringError {
  MirBodyAddress body;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::optional<SourceSpan> source;
  std::string message;
};

struct MirLoweringResult {
  MirProgram program;
  std::vector<MirLoweringError> errors;

  [[nodiscard]] bool valid() const { return program.valid(); }
};

// Counters for shared normal-exit cleanup, thread-local like the switches:
// bodies in which an exit joined another exit's cleanup, and how often the
// body or program fallback lowered again with each exit destroying its own
// owners. Lowering a program runs several rounds, so bodies count per round.
struct MirCleanupSharingStatistics {
  std::size_t exitSharingBodies = 0;
  std::size_t exitSharingBodyFallbacks = 0;
  std::size_t exitSharingProgramFallbacks = 0;
  // The body and verification error of the latest body fallback.
  std::string lastExitSharingBodyFallback;
};

class MirLowerer {
public:
  [[nodiscard]] MirLoweringResult
  lower(const HirProgram &source, const FailureMetadata &failureMetadata) const;

  // Function and lambda bodies share identical normal-exit cleanup suffixes
  // unless this is cleared, which lowers every exit's own cleanup chain. The
  // setting is thread-local; tests and measurements use it to compare both
  // forms of one program.
  static void setShareExitCleanup(bool enabled);
  [[nodiscard]] static bool shareExitCleanup();
  [[nodiscard]] static const MirCleanupSharingStatistics &
  cleanupSharingStatistics();
  static void resetCleanupSharingStatistics();

private:
  // Function and lambda bodies share identical failure-cleanup suffixes
  // unless this is cleared, which lowers one cleanup chain per failure edge.
  static thread_local bool shareFailureCleanup_;
  static thread_local bool shareExitCleanup_;
  static thread_local MirCleanupSharingStatistics cleanupSharingStatistics_;

  [[nodiscard]] MirLoweringResult
  lowerVerified(const HirProgram &source,
                const FailureMetadata &failureMetadata) const;

  [[nodiscard]] static MirLoweringResult
  lowerProgram(const HirProgram &source, const FailureMetadata &failureMetadata,
               const MirDefinedFailureEffects &definedFailureEffects,
               bool includeHostedStartup);

  [[nodiscard]] static MirBody lowerBody(
      const HirProgram &program, const FailureMetadata &failureMetadata,
      const HirBody &body, MirBodyKind kind, SemanticType returnType,
      const std::vector<HirValueId> &prologueValues,
      const MirDefinedFailureEffects &definedFailureEffects, bool &valid,
      std::vector<MirLoweringError> &errors, std::size_t owner,
      bool implicitZeroReturn = false,
      const std::vector<HirConstructorInitializer> *initializers = nullptr,
      const HirFunctionInstance *function = nullptr,
      const HirConstructorInstance *constructor = nullptr,
      const HirLambda *lambda = nullptr,
      const HirProgramInitializationPlan *programInitialization = nullptr,
      MirProgramInitializationPlan *loweredProgramInitialization = nullptr,
      bool compilerTrusted = false,
      const MirClassInstance *constructorOwner = nullptr,
      const HirDestructorInstance *destructor = nullptr);

  [[nodiscard]] static bool
  lowerHostedStartup(const HirProgram &source,
                     const FailureMetadata &failureMetadata,
                     MirProgram &program);
};

} // namespace lang
