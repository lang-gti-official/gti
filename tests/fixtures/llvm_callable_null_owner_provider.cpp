#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

#if defined(_WIN32)
#include <malloc.h>
#endif

extern "C" std::uint64_t gti_rt_native_size_max_v1() {
  return (std::numeric_limits<std::size_t>::max)();
}

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
  (void)bytes;
  (void)alignment;
  std::abort();
}

extern "C" void gti_rt_aligned_free_v1(void *address) {
  (void)address;
  std::abort();
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
  (void)bytes;
  (void)alignment;
  (void)status;
  std::abort();
}

extern "C" void gti_rt_posix_free_v1(void *address) {
  (void)address;
  std::abort();
}
#endif
