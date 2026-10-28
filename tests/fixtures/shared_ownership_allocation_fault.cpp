#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#ifndef _WIN32
#include <cerrno>
#include <dlfcn.h>
#include <sys/resource.h>
#endif

namespace {
bool failScalarAllocation = false;
// Abort deliberately does not flush streams. Make cleanup markers observable
// before any program I/O, so an empty capture proves cleanup did not execute.
struct ConfigureFaultProcess {
  ConfigureFaultProcess() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifndef _WIN32
    const rlimit limit{0, 0};
    setrlimit(RLIMIT_CORE, &limit);
#endif
  }
} configureFaultProcess;
} // namespace

extern "C" void gti_test_fail_scalar_allocation(std::int32_t enabled) {
  failScalarAllocation = enabled != 0;
}

// The fault tests require a real allocation, not an optimizable owner. This
// external observation makes its address escape without modifying the value.
extern "C" std::int32_t
gti_test_observe_scalar_allocation(const std::int32_t *value) {
  return *value;
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

#ifndef _WIN32
// Source owners allocate through the production process provider. Interpose
// only its libc entry; successful calls still use the real allocator.
extern "C" int posix_memalign(void **address, std::size_t alignment,
                              std::size_t bytes)
#if defined(__linux__)
    noexcept
#endif
{
  if (failScalarAllocation)
    return ENOMEM;
  using Allocate = int (*)(void **, std::size_t, std::size_t);
  static auto real =
      reinterpret_cast<Allocate>(dlsym(RTLD_NEXT, "posix_memalign"));
  return real ? real(address, alignment, bytes) : ENOMEM;
}
#endif
