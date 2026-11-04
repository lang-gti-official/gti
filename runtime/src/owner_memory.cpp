#include "gti/memory.h"

#include <cstddef>
#include <cstdint>
#include <new>

// Storage for compiler-generated owner representations: unique-owner pointees,
// shared-owner control blocks and runtime task boxes. It uses the same
// replaceable global allocation functions as the generated C++ support code
// (nothrow new, aligned only for over-aligned requests, and the matching
// delete), so a program that replaces them observes owners identically on
// every backend. It is distinct from the source-owned process provider in
// memory.cpp, and lives in its own object so a program that injects those
// provider adapters links without pulling the default adapter object in.
namespace {
bool overAligned(std::uint64_t alignment) {
  return alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
}
} // namespace

extern "C" void *gti_rt_owner_allocate_v1(std::uint64_t bytes,
                                          std::uint64_t alignment) {
  if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
    return nullptr;
  if (overAligned(alignment))
    return ::operator new(static_cast<std::size_t>(bytes),
                          std::align_val_t(static_cast<std::size_t>(alignment)),
                          std::nothrow);
  return ::operator new(static_cast<std::size_t>(bytes), std::nothrow);
}

extern "C" void gti_rt_owner_release_v1(void *address,
                                        std::uint64_t alignment) {
  if (address == nullptr)
    return;
  if (overAligned(alignment))
    ::operator delete(address,
                      std::align_val_t(static_cast<std::size_t>(alignment)));
  else
    ::operator delete(address);
}
