#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
std::uint64_t allocations = 0;
}

extern "C" void swap_observe_allocations() { allocations = 0; }
extern "C" std::uint64_t swap_allocation_count() { return allocations; }

// Nodes still use the no-throw native allocation boundary.
// Delegate allocation to the ordinary throwing overloads, including alignment.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  ++allocations;
  try {
    return ::operator new(size);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}

void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  ++allocations;
  try {
    return ::operator new(size, alignment);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}

// Source-owned vectors and strings allocate through the process provider.
// Supply its complete adapter object, keeping allocation/release matched and
// avoiding accidental linkage of a second production provider definition.
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
  ++allocations;
  return _aligned_malloc(static_cast<std::size_t>(bytes),
                         static_cast<std::size_t>(alignment));
}
extern "C" void gti_rt_aligned_free_v1(void *address) {
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
  ++allocations;
  void *address = nullptr;
  *status = posix_memalign(&address, static_cast<std::size_t>(alignment),
                           static_cast<std::size_t>(bytes));
  return address;
}
extern "C" void gti_rt_posix_free_v1(void *address) { std::free(address); }
#endif
