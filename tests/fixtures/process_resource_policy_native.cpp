#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
int mode = 0;
int calls = 0;
int releases = 0;
} // namespace

extern "C" void provider_test_mode(std::int32_t selected) {
  mode = selected;
  calls = 0;
  releases = 0;
}
extern "C" std::int32_t provider_test_calls() { return calls; }
extern "C" std::int32_t provider_test_releases() { return releases; }
extern "C" std::uint64_t gti_rt_native_size_max_v1() {
  return mode == 1 ? 4095 : (std::numeric_limits<std::size_t>::max)();
}
// Keep the observation seam in this injected adapter set too: pulling the
// production memory object from the runtime archive would also pull its
// allocation definitions and conflict with this fixture's injected provider.
extern "C" std::int32_t gti_rt_address_is_aligned_v1(const void *address,
                                                     std::uint64_t alignment) {
  if (!address || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      alignment > (std::numeric_limits<std::uintptr_t>::max)())
    return 0;
  return reinterpret_cast<std::uintptr_t>(address) % alignment == 0;
}
#if defined(_WIN32)
extern "C" void *gti_rt_aligned_malloc_v1(std::uint64_t bytes,
                                          std::uint64_t alignment) {
  ++calls;
  if (mode >= 2)
    return nullptr;
  if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
    std::abort();
  return _aligned_malloc(static_cast<std::size_t>(bytes),
                         static_cast<std::size_t>(alignment));
}
extern "C" void gti_rt_aligned_free_v1(void *address) {
  ++releases;
  _aligned_free(address);
}
#else
extern "C" std::uint64_t gti_rt_posix_pointer_size_v1() {
  return sizeof(void *);
}
extern "C" std::int32_t gti_rt_posix_enomem_v1() { return ENOMEM; }
extern "C" std::int32_t gti_rt_posix_einval_v1() { return EINVAL; }
extern "C" void *gti_rt_posix_memalign_v1(std::uint64_t bytes,
                                          std::uint64_t alignment,
                                          std::int32_t *status) {
  ++calls;
  if (mode >= 2) {
    *status = mode == 2 ? ENOMEM : mode == 3 ? EINVAL : mode == 4 ? -123 : 0;
    return nullptr;
  }
  if (bytes == 0 || alignment < sizeof(void *) ||
      alignment % sizeof(void *) != 0 || (alignment & (alignment - 1)) != 0) {
    std::abort(); // Source policy must normalize before invoking this adapter.
  }
  void *address = nullptr;
  *status = posix_memalign(&address, static_cast<std::size_t>(alignment),
                           static_cast<std::size_t>(bytes));
  return address;
}
extern "C" void gti_rt_posix_free_v1(void *address) {
  ++releases;
  std::free(address);
}
#endif
extern "C" std::int32_t provider_test_region(void *address, std::uint64_t bytes,
                                             std::uint64_t alignment) {
  if (!address || reinterpret_cast<std::uintptr_t>(address) % alignment != 0)
    return 1;
  auto *data = static_cast<unsigned char *>(address);
  for (std::uint64_t index = 0; index < bytes; ++index)
    data[index] = static_cast<unsigned char>(index);
  for (std::uint64_t index = 0; index < bytes; ++index)
    if (data[index] != static_cast<unsigned char>(index))
      return 2;
  return 0;
}
