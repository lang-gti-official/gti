// Test-only runtime fault hooks for managed-thread join quarantine controls.
#include "thread_internal.h"
#include "gti/runtime_thread.h"

#include <cstdint>
#include <cstdlib>

extern "C" void gti_test_inject_thread_join_failure() noexcept {
  gti::runtime::detail::setThreadTestFault(
      gti::runtime::detail::ThreadTestFault::JoinFailed);
}

extern "C" void gti_test_expect_native_storage_pinned(std::int32_t value) noexcept {
  if (value != 7 || gti_rt_thread_program_storage_retirement_ready_v1())
    std::abort();
}
