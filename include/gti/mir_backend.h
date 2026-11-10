#pragma once

#include "gti/backend.h"

#include <string_view>

namespace lang {

// Emits the deterministic MIR serialization of the checked program: the same
// verified optimized snapshot every executable backend consumes, in the
// versioned `mir-v*` text format owned by `MirPrinter`. The artifact is an
// inspection surface for the backend-authority migration, not an executable
// representation; its shape is the canonical serialization contract in
// `docs/architecture/mir.md`.
struct MirBackendOptions {
  // Serialize the RFC 0004 erasure-gate view instead: semantic loan numbers
  // zeroed, EndBorrow removed and instruction IDs renumbered
  // (`mir_loan_normalization.h`). An inspection view only; the verified
  // program itself is unchanged.
  bool normalizeLoanBookkeeping = false;
};

class MirBackend final : public Backend {
public:
  explicit MirBackend(MirBackendOptions options = {}) : options(options) {}

  [[nodiscard]] std::string_view name() const override { return "mir"; }

  [[nodiscard]] BackendArtifact generate(LoweredProgram program) override;

private:
  MirBackendOptions options;
};

} // namespace lang
