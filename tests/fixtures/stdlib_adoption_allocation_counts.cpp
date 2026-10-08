#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
enum class Path { LegacyAligned, Process };
struct Region {
  void *address = nullptr;
  Path path = Path::Process;
  bool audited = false;
};
Region regions[4096]{};
std::size_t live_count = 0;

bool failed_attempt(std::size_t size) {
  if (!enabled)
    return false;
  ++allocations;
  bytes += size;
  if (fail_at == 0 || allocations != fail_at)
    return false;
  fail_at = 0; // Cleanup can itself print through allocating library code.
  return true;
}

void remember(void *address, Path path) {
  if (!address)
    return;
  for (const auto &region : regions)
    if (region.address == address)
      std::abort();
  for (auto &region : regions) {
    if (!region.address) {
      // Startup argv allocations precede the observation window. Track their
      // exact release too, without including them in scenario counts/leaks.
      region = {address, path, audit_cleanup};
      live_count += audit_cleanup ? 1 : 0;
      return;
    }
  }
  std::abort(); // Never silently lose a live allocation's identity.
}

void retire(void *address, Path path) {
  if (!address)
    return;
  for (auto &region : regions) {
    if (region.address == address) {
      if (region.path != path)
        std::abort();
      if (region.audited) {
        if (live_count == 0)
          std::abort();
        --live_count;
      }
      region = {};
      return;
    }
  }
  std::abort(); // A wrong-provider, untracked or duplicate release is a bug.
}
} // namespace

void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  if (failed_attempt(size))
    return nullptr;
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
  remember(result, Path::LegacyAligned);
  return result;
}

void operator delete(void *pointer, std::align_val_t) noexcept {
  retire(pointer, Path::LegacyAligned);
#if defined(_WIN32)
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}

// Inject the real source process-provider boundary, rather than observing an
// obsolete container representation's allocation implementation. These exact
// adapters avoid pulling the production adapter object from the runtime
// archive.
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
extern "C" void *gti_rt_aligned_malloc_v1(std::uint64_t size,
                                          std::uint64_t alignment) {
  if (failed_attempt(static_cast<std::size_t>(size)))
    return nullptr;
  void *address = _aligned_malloc(static_cast<std::size_t>(size),
                                  static_cast<std::size_t>(alignment));
  remember(address, Path::Process);
  return address;
}
extern "C" void gti_rt_aligned_free_v1(void *address) {
  retire(address, Path::Process);
  _aligned_free(address);
}
#else
extern "C" std::uint64_t gti_rt_posix_pointer_size_v1() {
  return sizeof(void *);
}
extern "C" std::int32_t gti_rt_posix_enomem_v1() { return ENOMEM; }
extern "C" std::int32_t gti_rt_posix_einval_v1() { return EINVAL; }
extern "C" void *gti_rt_posix_memalign_v1(std::uint64_t size,
                                          std::uint64_t alignment,
                                          std::int32_t *status) {
  if (failed_attempt(static_cast<std::size_t>(size))) {
    *status = ENOMEM;
    return nullptr;
  }
  void *address = nullptr;
  *status = posix_memalign(&address, static_cast<std::size_t>(alignment),
                           static_cast<std::size_t>(size));
  remember(address, Path::Process);
  return address;
}
extern "C" void gti_rt_posix_free_v1(void *address) {
  retire(address, Path::Process);
  std::free(address);
}
#endif

extern "C" void adoption_count_begin() {
  allocations = 0;
  bytes = 0;
  enabled = true;
}
extern "C" void adoption_count_end() { enabled = false; }
extern "C" std::uint64_t adoption_allocations() { return allocations; }
extern "C" std::uint64_t adoption_bytes() { return bytes; }
extern "C" void adoption_fail_at(std::uint64_t allocation) {
  if (live_count != 0)
    std::abort(); // Reset must never erase leaks from an earlier failure
                  // window.
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
