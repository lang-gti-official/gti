#pragma once

#include "gti/mir_verifier.h"

namespace lang {
// Independent, executable proof for per-thread storage. No source/HIR policy
// is consulted when verifying initializer bodies, cleanup or first-use guards.
[[nodiscard]] MirVerificationResult
verifyMirThreadLocalStorage(const MirProgram &program);
} // namespace lang
