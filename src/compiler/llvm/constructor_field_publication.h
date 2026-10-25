#pragma once

#include "gti/mir.h"

namespace lang::native_llvm {

// A constructor record may leave one field initializer unstaged in its body.
// The shared MIR query identifies that source record; this plan additionally
// proves that the record has one exact SSA producer and one exact destination
// field. Admission decides whether the producer's concrete storage transport
// is implemented, while body emission consumes the same exact plan.
struct UnstagedConstructorFieldPublicationPlan final {
  const MirConstructorInitializer *initializer = nullptr;
  MirValueId value = 0;
  SemanticType type = SemanticType::Unknown;
  bool exact = false;

  [[nodiscard]] bool matched() const { return initializer != nullptr; }
};

[[nodiscard]] UnstagedConstructorFieldPublicationPlan
unstagedConstructorFieldPublication(const MirProgram &program,
                                    const MirBody &body,
                                    HirConstructorInstanceId constructor,
                                    const MirInstruction &instruction);

} // namespace lang::native_llvm
