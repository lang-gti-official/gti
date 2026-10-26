#pragma once

#include "gti/lowered_program.h"

#include <optional>

namespace lang::lowered_detail {

// Recomputed from the exact owned MIR; no query result is a reusable proof.
[[nodiscard]] std::optional<std::vector<LoweredSourceRange>>
deriveSourceRanges(const MirProgram &program, MirBodyAddress address);
[[nodiscard]] std::vector<LoweredSourceValueRange>
deriveSourceValueRanges(const MirBody &body);
// Intersect exact existing field loads with freshly derived readable ranges.
[[nodiscard]] std::vector<LoweredSourceFragmentRange>
deriveSourceFragments(const MirProgram &program, MirBodyAddress address,
                      std::span<const LoweredSourceRange> readable);
[[nodiscard]] std::vector<LoweredSourceParameterFragment>
deriveSourceParameterFragments(const MirProgram &program,
                               MirBodyAddress address,
                               std::span<const LoweredSourceRange> readable);

} // namespace lang::lowered_detail
