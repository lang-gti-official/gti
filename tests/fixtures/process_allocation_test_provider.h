#pragma once
// Complete injectable GTI process provider for native allocation oracles.
// Each companion supplies acquire/release; C++ new/delete remain independent.
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#ifdef _WIN32
#include <malloc.h>
#endif
namespace gti_test_process_provider {
void *acquire(std::size_t bytes, std::size_t alignment);
void release(void *address);
// The injected provider deliberately emulates growth by moving. Preserve its
// allocation/fault/release observations while exercising byte preservation.
struct Region {
  void *address = nullptr;
  std::size_t bytes = 0;
};
inline Region regions[8192]{};
inline std::mutex regionsMutex;
inline void remember(void *address, std::size_t bytes) {
  if (!address)
    return;
  const std::lock_guard<std::mutex> lock(regionsMutex);
  for (auto &region : regions) {
    if (!region.address) {
      region = {address, bytes};
      return;
    }
  }
  std::abort();
}
inline std::size_t forget(void *address) {
  if (!address)
    return 0;
  const std::lock_guard<std::mutex> lock(regionsMutex);
  for (auto &region : regions) {
    if (region.address == address) {
      auto bytes = region.bytes;
      region = {};
      return bytes;
    }
  }
  std::abort();
}
inline void *systemAllocate(std::size_t bytes, std::size_t alignment) {
#ifdef _WIN32
  return _aligned_malloc(bytes, alignment < sizeof(void *) ? sizeof(void *)
                                                           : alignment);
#else
  if (alignment <= alignof(std::max_align_t))
    return std::malloc(bytes);
  void *result = nullptr;
  return posix_memalign(&result, alignment, bytes) == 0 ? result : nullptr;
#endif
}
inline void systemRelease(void *address) {
#ifdef _WIN32
  _aligned_free(address);
#else
  std::free(address);
#endif
}
} // namespace gti_test_process_provider
extern "C" std::uint64_t gti_rt_native_size_max_v1() {
  return (std::numeric_limits<std::size_t>::max)();
}
extern "C" std::int32_t gti_rt_address_is_aligned_v1(const void *p,
                                                     std::uint64_t a) {
  return p && a && !(a & (a - 1)) &&
         reinterpret_cast<std::uintptr_t>(p) % a == 0;
}
#ifdef _WIN32
extern "C" void *gti_rt_aligned_malloc_v1(std::uint64_t n, std::uint64_t a) {
  return gti_test_process_provider::acquire(n, a);
}
extern "C" void gti_rt_aligned_free_v1(void *p) {
  gti_test_process_provider::release(p);
}
#else
extern "C" std::uint64_t gti_rt_posix_pointer_size_v1() {
  return sizeof(void *);
}
extern "C" std::int32_t gti_rt_posix_enomem_v1() { return ENOMEM; }
extern "C" std::int32_t gti_rt_posix_einval_v1() { return EINVAL; }
extern "C" void *gti_rt_malloc_v1(std::uint64_t n) {
  void *result = gti_test_process_provider::acquire(n, 1);
  gti_test_process_provider::remember(result, n);
  return result;
}
extern "C" void *gti_rt_posix_memalign_v1(std::uint64_t n, std::uint64_t a,
                                          std::int32_t *status) {
  void *result = gti_test_process_provider::acquire(n, a);
  gti_test_process_provider::remember(result, n);
  *status = result ? 0 : ENOMEM;
  return result;
}
extern "C" void *gti_rt_realloc_v1(void *p, std::uint64_t n) {
  void *result = gti_rt_malloc_v1(n);
  if (!result)
    return nullptr;
  auto oldBytes = gti_test_process_provider::forget(p);
  std::memcpy(result, p, oldBytes < n ? oldBytes : n);
  gti_test_process_provider::release(p);
  return result;
}
extern "C" void gti_rt_posix_free_v1(void *p) {
  gti_test_process_provider::forget(p);
  gti_test_process_provider::release(p);
}
#endif
