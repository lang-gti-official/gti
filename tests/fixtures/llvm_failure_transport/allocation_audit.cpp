#include <atomic>
#include <cstddef>
#include <cstdint>

// GNU ld --wrap counts compiler-emitted exception-storage requests only.
// This is NOT a complete malloc/RSS measurement or an emergency-pool audit.
static std::atomic<std::uint64_t> allocations;
#if defined(__APPLE__)
extern "C" void *__cxa_allocate_exception(std::size_t) noexcept;
static void *count_exception(std::size_t size) noexcept {
  allocations.fetch_add(1, std::memory_order_relaxed);
  return __cxa_allocate_exception(size);
}
// dyld's documented interpose tuple. Calls within this image keep the original
// binding. This test-only dylib is never loaded by production GTI artifacts.
__attribute__((used, section("__DATA,__interpose"))) static const struct {
  const void *replacement;
  const void *original;
} interpose = {reinterpret_cast<const void *>(&count_exception),
               reinterpret_cast<const void *>(&__cxa_allocate_exception)};
#else
extern "C" void *__real___cxa_allocate_exception(std::size_t) noexcept;
extern "C" void *__wrap___cxa_allocate_exception(std::size_t size) noexcept {
  allocations.fetch_add(1, std::memory_order_relaxed);
  return __real___cxa_allocate_exception(size);
}
#endif
extern "C" std::uint64_t transport_exception_requests() {
  return allocations.load(std::memory_order_relaxed);
}
