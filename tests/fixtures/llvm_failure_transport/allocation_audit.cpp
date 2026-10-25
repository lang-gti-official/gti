#include <cstddef>
#include <cstdint>

// GNU ld --wrap counts compiler-emitted exception-storage requests only.
// This is NOT a complete malloc/RSS measurement or an emergency-pool audit.
static std::uint64_t allocations;
extern "C" void *__real___cxa_allocate_exception(std::size_t) noexcept;
extern "C" void *__wrap___cxa_allocate_exception(std::size_t size) noexcept {
  ++allocations;
  return __real___cxa_allocate_exception(size);
}
extern "C" std::uint64_t transport_exception_requests() { return allocations; }
