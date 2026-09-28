#pragma once

#include "gti/chrono.h"

#include <cstdint>

namespace gti::runtime::detail {

// Joins a floor-based seconds count and a normalized [0, 1e9) nanosecond
// remainder without executing overflowing signed arithmetic. This seam keeps
// the public ABI scalar while making its exact range boundary deterministic in
// unit tests.
gti_chrono_status_v1
composeChronoNanoseconds(std::int64_t seconds,
                         std::int64_t subsecondNanoseconds,
                         std::int64_t *outputNanoseconds) noexcept;

} // namespace gti::runtime::detail
