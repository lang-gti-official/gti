#pragma once

#include "gti/mir_verifier.h"

#include <functional>
#include <span>
#include <vector>

namespace lang {

// Private verifier input, independently derived from executable MIR. These are
// same-thread ambient accesses, not accesses through a caller's staged
// reference operands, and not a raw-pointer validity or native retention
// contract.
struct MirPersistentStorageAccess {
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  SymbolId symbol = 0;
  std::vector<MirPlaceProjection> projections;
  AccessMode access = AccessMode::ReadOnly;

  friend bool operator==(const MirPersistentStorageAccess &,
                         const MirPersistentStorageAccess &) = default;
};

using MirPersistentStorageEffectQuery =
    std::function<std::vector<MirPersistentStorageAccess>(
        MirBlockId, MirInstructionId, std::span<const SymbolId> ready)>;

// Reuses the body verifier's canonical-place and exact CFG loan-flow engine.
// A collector observes checked direct accesses; hidden accesses are checked at
// their invocation's existing loan state. Neither argument grants authority.
[[nodiscard]] MirVerificationResult verifyMirPersistentStorageLoanFlow(
    const MirBody &body, std::size_t owner,
    std::span<const MirPersistentStorageAccess> hidden,
    std::vector<MirPersistentStorageAccess> *direct = nullptr,
    const MirPersistentStorageEffectQuery *query = nullptr);

[[nodiscard]] MirVerificationResult
verifyMirPersistentStorageEffects(const MirProgram &program);

} // namespace lang
