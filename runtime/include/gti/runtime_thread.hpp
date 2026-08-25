#pragma once

#include "gti/runtime_thread.h"

#include <exception>
#include <utility>

namespace gti_runtime_detail {

// This exception never crosses the generated task adapter. It is only the
// in-process control transfer used after verified GTI cleanup has completed
// and the exact failure record has been copied into worker-owned state.
class worker_failure_handoff final {};

[[nodiscard]] bool managed_worker_active() noexcept;

[[noreturn]] void handoff_managed_worker_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact);

template <typename Invocation>
void run_managed_task_adapter(Invocation &&invocation) noexcept {
  try {
    std::forward<Invocation>(invocation)();
  } catch (const worker_failure_handoff &) {
    // The exact record was already handed to the worker state. Returning from
    // the adapter lets the runtime destroy the task object exactly once.
  } catch (...) {
    // No foreign C++ unwind may cross the managed task boundary.
    std::terminate();
  }
}

} // namespace gti_runtime_detail
