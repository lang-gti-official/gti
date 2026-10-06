#include "gti/memory.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

extern "C" void *gti_rt_memory_allocate_v1(std::uint64_t bytes,
                                           std::uint64_t alignment,
                                           std::int32_t *status) {
  // A null out-parameter is a caller contract violation, but don't touch it.
  if (status == nullptr)
    return nullptr;
  *status = 0;
  if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
    *status = 1;
    return nullptr;
  }
  if (bytes > (std::numeric_limits<std::size_t>::max)() ||
      alignment > (std::numeric_limits<std::size_t>::max)()) {
    *status = 2;
    return nullptr;
  }
  if (bytes == 0)
    return nullptr;
  const auto effective = static_cast<std::size_t>(
      alignment < alignof(std::max_align_t) ? alignof(std::max_align_t)
                                            : alignment);
  void *address = ::operator new(static_cast<std::size_t>(bytes),
                                 std::align_val_t(effective), std::nothrow);
  if (!address)
    *status = 3;
  return address;
}

extern "C" void gti_rt_memory_release_v1(void *address,
                                         std::uint64_t alignment) {
  if (!address)
    return;
  const auto effective = static_cast<std::size_t>(
      alignment < alignof(std::max_align_t) ? alignof(std::max_align_t)
                                            : alignment);
  ::operator delete(address, std::align_val_t(effective));
}
