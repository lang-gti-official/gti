#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// Count the exact no-throw C++ allocation seam used by managed task boxes and
// runtime worker state. Host thread-library and libc allocations are excluded;
// this is a transport-allocation oracle, not total process heap accounting.
namespace {
std::atomic<std::uint64_t> allocations{0};
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  allocations.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void *address) noexcept { std::free(address); }
void operator delete(void *address, std::size_t) noexcept {
  std::free(address);
}

extern "C" void gti_scoped_alloc_reset() {
  allocations.store(0, std::memory_order_relaxed);
}
extern "C" std::uint64_t gti_scoped_allocations() {
  return allocations.load(std::memory_order_relaxed);
}
