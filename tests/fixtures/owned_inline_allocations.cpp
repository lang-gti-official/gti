#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
std::uint64_t allocations = 0;
std::uint64_t live = 0;
} // namespace

extern "C" std::uint64_t inline_probe_allocations() { return allocations; }
extern "C" std::uint64_t inline_probe_live() { return live; }
extern "C" void inline_probe_drop(std::int32_t value) {
  std::printf("drop %d\n", value);
  std::fflush(stdout);
}

extern "C" std::uint64_t gti_rt_native_size_max_v1() {
  return (std::numeric_limits<std::size_t>::max)();
}
extern "C" std::int32_t gti_rt_address_is_aligned_v1(const void *address,
                                                     std::uint64_t alignment) {
  return address && alignment && (alignment & (alignment - 1)) == 0 &&
         reinterpret_cast<std::uintptr_t>(address) % alignment == 0;
}
extern "C" std::uint64_t gti_rt_posix_pointer_size_v1() {
  return sizeof(void *);
}
extern "C" std::int32_t gti_rt_posix_enomem_v1() { return ENOMEM; }
extern "C" std::int32_t gti_rt_posix_einval_v1() { return EINVAL; }
extern "C" void *gti_rt_posix_memalign_v1(std::uint64_t bytes,
                                          std::uint64_t alignment,
                                          std::int32_t *status) {
  void *address = nullptr;
  *status = posix_memalign(&address, static_cast<std::size_t>(alignment),
                           static_cast<std::size_t>(bytes));
  if (address) {
    ++allocations;
    ++live;
  }
  return address;
}
extern "C" void gti_rt_posix_free_v1(void *address) {
  if (address)
    --live;
  std::free(address);
}

// Generated owners allocate through the runtime owner pair rather than the
// process provider; count both so an inline callback that allocates either
// kind of storage is observed. Matches the runtime's nothrow new selection.
extern "C" void *gti_rt_owner_allocate_v1(std::uint64_t bytes,
                                          std::uint64_t alignment) {
  void *address =
      alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__
          ? ::operator new(
                static_cast<std::size_t>(bytes),
                std::align_val_t(static_cast<std::size_t>(alignment)),
                std::nothrow)
          : ::operator new(static_cast<std::size_t>(bytes), std::nothrow);
  if (address) {
    ++allocations;
    ++live;
  }
  return address;
}
extern "C" void gti_rt_owner_release_v1(void *address,
                                        std::uint64_t alignment) {
  if (!address)
    return;
  --live;
  if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
    ::operator delete(address,
                      std::align_val_t(static_cast<std::size_t>(alignment)));
  else
    ::operator delete(address);
}
