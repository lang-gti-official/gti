#include <cstddef>
#include <cstdint>
#include <new>

namespace {
bool failScalarAllocation = false;
}

extern "C" void gti_test_fail_scalar_allocation(std::int32_t enabled) {
  failScalarAllocation = enabled != 0;
}

using Callback = std::int32_t (*)(std::int32_t);
extern "C" std::int32_t gti_test_call_scalar_callback(Callback function,
                                                      std::int32_t value) {
  return function == nullptr ? -1 : function(value);
}

// Replace the standard no-throw allocation seam, not generated GTI code or
// the failure helper. No real resource exhaustion is needed for this oracle.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  if (failScalarAllocation) {
    return nullptr;
  }
  try {
    return ::operator new(size);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}
