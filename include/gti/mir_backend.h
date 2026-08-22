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
class MirBackend final : public Backend {
public:
  [[nodiscard]] std::string_view name() const override { return "mir"; }

  [[nodiscard]] BackendArtifact
  generate(const LoweredProgram &program) override;
};

} // namespace lang
