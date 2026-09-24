#pragma once

#include "cthread_internal.h"

namespace gti::runtime::detail {

CThreadStatus initCCondition(std::uint64_t *output) noexcept;
CThreadStatus destroyCCondition(std::uint64_t handle) noexcept;
CThreadStatus notifyCCondition(std::uint64_t handle, bool all) noexcept;
CThreadStatus waitCCondition(std::uint64_t condition,
                             std::uint64_t mutex) noexcept;
CThreadStatus timedWaitCCondition(std::uint64_t condition, std::uint64_t mutex,
                                  std::int64_t seconds,
                                  std::int64_t nanos) noexcept;
void failNextCConditionAllocationForTest() noexcept;
void failNextCConditionWaitForTest() noexcept;
std::uint64_t waitingCConditionCallsForTest(std::uint64_t handle) noexcept;

} // namespace gti::runtime::detail
