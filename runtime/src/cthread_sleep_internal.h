#pragma once

#include <cstdint>

namespace gti::runtime::detail {

// Source-private host seam for deterministic interruption/error tests. The host
// receives validated input and private output slots; only a validated -1 result
// may be published to caller memory. This header is not installed.
using CThreadSleepHost = std::int32_t (*)(std::int64_t, std::int64_t,
                                          std::int64_t *,
                                          std::int64_t *) noexcept;
std::int32_t sleepCThread(std::int64_t seconds, std::int64_t nanoseconds,
                          std::int64_t *remainingSeconds,
                          std::int64_t *remainingNanoseconds,
                          CThreadSleepHost host) noexcept;

} // namespace gti::runtime::detail
