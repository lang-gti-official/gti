#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// Counts the two no-throw allocation seams GTI owners use, tracks which of
// those blocks are still live, and can fail the next one. No real resource
// exhaustion is needed for these oracles.
namespace {
bool counting = false;
bool failNext = false;
std::uint64_t allocations = 0;
void *live[4096]{};
std::size_t liveCount = 0;

void *track(void *result) {
  if (counting && result != nullptr) {
    ++allocations;
    if (liveCount == 4096)
      std::abort(); // A broken fixture must not silently lose rows.
    live[liveCount++] = result;
  }
  return result;
}

void forget(void *pointer) {
  for (std::size_t index = 0; index != liveCount; ++index) {
    if (live[index] == pointer) {
      live[index] = live[--liveCount];
      return;
    }
  }
}
} // namespace

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  if (counting && failNext) {
    failNext = false;
    return nullptr;
  }
  return track(std::malloc(size == 0 ? 1 : size));
}

void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  if (counting && failNext) {
    failNext = false;
    return nullptr;
  }
  void *result = nullptr;
  const auto aligned = static_cast<std::size_t>(alignment);
  if (posix_memalign(&result,
                     aligned < sizeof(void *) ? sizeof(void *) : aligned,
                     size == 0 ? 1 : size) != 0)
    return nullptr;
  return track(result);
}

void operator delete(void *pointer) noexcept {
  forget(pointer);
  std::free(pointer);
}
void operator delete(void *pointer, std::size_t) noexcept {
  forget(pointer);
  std::free(pointer);
}
void operator delete(void *pointer, std::align_val_t) noexcept {
  forget(pointer);
  std::free(pointer);
}
void operator delete(void *pointer, std::size_t, std::align_val_t) noexcept {
  forget(pointer);
  std::free(pointer);
}

extern "C" void inline_polymorphic_count_begin() {
  counting = true;
  allocations = 0;
}
extern "C" std::uint64_t inline_polymorphic_allocations() {
  return allocations;
}
extern "C" std::uint64_t inline_polymorphic_live() { return liveCount; }
extern "C" void inline_polymorphic_fail_next() { failNext = true; }
