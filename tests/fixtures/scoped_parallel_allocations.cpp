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

// Match the global delete replacement for ordinary host-library allocations
// too, without counting them. Otherwise libstdc++'s worker launch allocation
// is made by native operator new and incorrectly released through free under
// ASan; that would be an instrumentation mismatch, not a GTI runtime defect.
void *operator new(std::size_t size) {
  if (void *address = std::malloc(size == 0 ? 1 : size))
    return address;
  throw std::bad_alloc();
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
