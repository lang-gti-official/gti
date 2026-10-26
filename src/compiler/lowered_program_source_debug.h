#pragma once

#include "gti/lowered_program.h"

#include <optional>

namespace lang::lowered_detail {

// Recomputed from the exact owned MIR; no query result is a reusable proof.
[[nodiscard]] std::optional<std::vector<LoweredSourceRange>>
deriveSourceRanges(const MirProgram &program, MirBodyAddress address);
[[nodiscard]] std::vector<LoweredSourceValueRange>
deriveSourceValueRanges(const MirBody &body);

} // namespace lang::lowered_detail
