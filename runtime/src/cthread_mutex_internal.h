#pragma once

#include "cthread_internal.h"

namespace gti::runtime::detail {

// Native prerequisite of the manual GTI mutex wrappers. Handles are never
// reused. A successful init publishes one explicit destruction obligation;
// the caller must exclude concurrent destruction and release every lock.
CThreadStatus initCMutex(std::uint64_t *output, std::int32_t flags) noexcept;
CThreadStatus destroyCMutex(std::uint64_t handle) noexcept;
CThreadStatus lockCMutex(std::uint64_t handle) noexcept;
CThreadStatus tryLockCMutex(std::uint64_t handle) noexcept;
CThreadStatus timedLockCMutex(std::uint64_t handle, std::int64_t seconds,
                              std::int64_t nanoseconds) noexcept;
CThreadStatus unlockCMutex(std::uint64_t handle) noexcept;
void failNextCMutexAllocationForTest() noexcept;
void failNextCMutexWaitForTest() noexcept;
std::uint64_t waitingCMutexCallsForTest(std::uint64_t handle) noexcept;

} // namespace gti::runtime::detail
