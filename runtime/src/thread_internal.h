#pragma once

#include <cstdint>

namespace gti::runtime::detail {

// Deterministic fault injection for the runtime boundary tests. This header is
// source-private and is never installed; production callers can reach only the
// stable C ABI in runtime_thread.h.
enum class ThreadTestFault {
  None,
  SpawnResourceUnavailable,
  SpawnOperationNotPermitted,
  JoinFailed,
};

void setThreadTestFault(ThreadTestFault fault) noexcept;

// Fail creation after exactly this many successful starts in the next group
// on the calling thread. No scheduler timing or worker callback is involved.
void setScopedThreadTestFailureAfter(std::uint64_t starts) noexcept;

} // namespace gti::runtime::detail
