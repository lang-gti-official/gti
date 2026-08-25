#pragma once

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

} // namespace gti::runtime::detail
