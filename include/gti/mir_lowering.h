#pragma once

#include "gti/mir.h"
#include "gti/mir_effects.h"

namespace lang {

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
      MirProgramInitializationPlan *loweredProgramInitialization = nullptr,
      bool compilerTrusted = false,
      const MirClassInstance *constructorOwner = nullptr);

  [[nodiscard]] static bool
  lowerHostedStartup(const HirProgram &source,
                     const FailureMetadata &failureMetadata,
                     MirProgram &program);
};

} // namespace lang
