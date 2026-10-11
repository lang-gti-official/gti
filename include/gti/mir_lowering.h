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

class MirLowerer {
public:
  [[nodiscard]] MirLoweringResult
  lower(const HirProgram &source, const FailureMetadata &failureMetadata) const;

private:
  // Function and lambda bodies share identical failure-cleanup suffixes
  // unless this is cleared, which lowers one cleanup chain per failure edge.
  static thread_local bool shareFailureCleanup_;

  [[nodiscard]] MirLoweringResult
  lowerVerified(const HirProgram &source,
                const FailureMetadata &failureMetadata) const;

  [[nodiscard]] static MirLoweringResult
  lowerProgram(const HirProgram &source, const FailureMetadata &failureMetadata,
               const MirDefinedFailureEffects &definedFailureEffects,
               bool includeProgramInitialization);

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
