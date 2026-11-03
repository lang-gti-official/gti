#include "gti/memory.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif

// Storage for compiler-generated owner representations: unique-owner pointees,
// shared-owner control blocks and runtime task boxes. This family is distinct
// from the source-owned process provider in memory.cpp, whose adapter symbols
// an application may inject; keeping these entries in their own object file
// lets such an injected provider link without pulling those adapters in.
extern "C" void *gti_rt_owner_allocate_v1(std::uint64_t bytes,
                                          std::uint64_t alignment) {
  if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
    return nullptr;
#if defined(_WIN32)
  return _aligned_malloc(static_cast<std::size_t>(bytes),
                         static_cast<std::size_t>(alignment));
#else
  if (alignment < sizeof(void *))
    alignment = sizeof(void *);
  void *address = nullptr;
  if (posix_memalign(&address, static_cast<std::size_t>(alignment),
                     static_cast<std::size_t>(bytes)) != 0)
    return nullptr;
  return address;
#endif
}

extern "C" void gti_rt_owner_release_v1(void *address) {
#if defined(_WIN32)
  _aligned_free(address);
#else
  std::free(address);
#endif
}
