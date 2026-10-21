#pragma once

#include "gti/mir_verifier.h"

namespace lang::mir_detail {
[[nodiscard]] MirVerificationResult
verifyOwnerlessTextContracts(const MirProgram &program);
// Shape query used only after the complete program's text-storage invariants
// have been independently verified. This does not establish shell lifetime.
[[nodiscard]] bool isOwnerlessTextFieldProjection(const MirProgram &program,
                                                  const MirBody &body,
                                                  const MirPlace &place);
} // namespace lang::mir_detail
