#include "thread_internal.h"

extern "C" void gti_test_fail_scoped_creation(std::uint64_t starts) noexcept {
  gti::runtime::detail::setScopedThreadTestFailureAfter(starts);
}
