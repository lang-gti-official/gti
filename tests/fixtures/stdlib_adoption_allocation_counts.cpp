#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
bool enabled = false;
std::uint64_t allocations = 0;
std::uint64_t bytes = 0;
std::uint64_t fail_at = 0;
bool audit_cleanup = false;
void *live[4096]{};
std::size_t live_count = 0;
} // namespace

void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  if (enabled) {
    ++allocations;
    bytes += size;
    if (fail_at != 0 && allocations == fail_at) {
      fail_at = 0; // Cleanup can itself print through allocating library code.
      return nullptr;
    }
  }
  void *result = nullptr;
  const auto aligned = static_cast<std::size_t>(alignment);
#if defined(_WIN32)
  result = _aligned_malloc(size,
                           aligned < sizeof(void *) ? sizeof(void *) : aligned);
#else
  if (posix_memalign(&result,
                     aligned < sizeof(void *) ? sizeof(void *) : aligned,
                     size) != 0)
    return nullptr;
#endif
  if (audit_cleanup && result != nullptr) {
    if (live_count == 4096)
      std::abort(); // A broken fixture must not silently lose allocation rows.
    live[live_count++] = result;
  }
  return result;
}

void operator delete(void *pointer, std::align_val_t) noexcept {
  for (std::size_t index = 0; index != live_count; ++index) {
    if (live[index] == pointer) {
      live[index] = live[--live_count];
      break;
    }
  }
#if defined(_WIN32)
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}

extern "C" void adoption_count_begin() {
  allocations = 0;
  bytes = 0;
  enabled = true;
}
extern "C" void adoption_count_end() { enabled = false; }
extern "C" std::uint64_t adoption_allocations() { return allocations; }
extern "C" std::uint64_t adoption_bytes() { return bytes; }
extern "C" void adoption_fail_at(std::uint64_t allocation) {
  adoption_count_begin();
  fail_at = allocation;
  audit_cleanup = true;
}
// Called by the outermost GTI guard after URL and temporary cleanup. GTI's
// terminal failure deliberately does not run unmanaged C++ static destructors.
extern "C" void adoption_check_cleanup() {
  std::printf("live=%zu\n", live_count);
  audit_cleanup = false;
}
