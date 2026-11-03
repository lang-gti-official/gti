#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sys/resource.h>

// The native-backend counterpart of unique_scalar_allocation_fault.cpp. Native
// owners allocate through the runtime's POSIX provider instead of operator
// new, so this companion fails that provider. It replaces the whole runtime
// memory object, so the archive member is never linked beside these
// definitions. It is POSIX-only, like the provider it replaces.
namespace {
bool failScalarAllocation = false;
// Abort deliberately does not flush streams. Make cleanup markers observable
// before any program I/O, so an empty capture proves cleanup did not execute.
struct ConfigureFaultProcess {
  ConfigureFaultProcess() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const rlimit limit{0, 0};
    setrlimit(RLIMIT_CORE, &limit);
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

extern "C" std::uint64_t gti_rt_native_size_max_v1() {
  return (std::numeric_limits<std::size_t>::max)();
}
extern "C" std::int32_t gti_rt_address_is_aligned_v1(const void *address,
                                                     std::uint64_t alignment) {
  if (!address || alignment == 0 || (alignment & (alignment - 1)) != 0)
    return 0;
  return reinterpret_cast<std::uintptr_t>(address) % alignment == 0;
}
extern "C" std::uint64_t gti_rt_posix_pointer_size_v1() {
  return sizeof(void *);
}
extern "C" std::int32_t gti_rt_posix_enomem_v1() { return ENOMEM; }
extern "C" std::int32_t gti_rt_posix_einval_v1() { return EINVAL; }
extern "C" void *gti_rt_posix_memalign_v1(std::uint64_t bytes,
                                          std::uint64_t alignment,
                                          std::int32_t *status) {
  if (failScalarAllocation) {
    *status = ENOMEM;
    return nullptr;
  }
  void *address = nullptr;
  *status = posix_memalign(&address, static_cast<std::size_t>(alignment),
                           static_cast<std::size_t>(bytes));
  return address;
}
extern "C" void gti_rt_posix_free_v1(void *address) { std::free(address); }
