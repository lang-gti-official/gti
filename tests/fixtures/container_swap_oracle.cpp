#include <cstddef>
#include <cstdint>
#include <new>

namespace {
std::uint64_t allocations = 0;
}

extern "C" void swap_observe_allocations() { allocations = 0; }
extern "C" std::uint64_t swap_allocation_count() { return allocations; }

// Observe the real no-throw seams used by GTI node and storage allocation.
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
