#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif

extern "C" void gti_test_cleanup_event_v1(std::uint32_t kind,
                                          std::uint64_t address);

namespace {
constexpr std::size_t capacity = 8;
std::array<std::uintptr_t, capacity> addresses{};
std::uint64_t releases = 0;
std::uint32_t overflow = 0;

void record(void *address) {
  const auto identity =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address));
  gti_test_cleanup_event_v1(2, identity);
  const std::uint64_t index = releases++;
  if (index < addresses.size())
    addresses[static_cast<std::size_t>(index)] = identity;
  else
    overflow = 1;
}
} // namespace

extern "C" std::uint64_t gti_test_cleanup_release_count_v1() {
  return releases;
}

extern "C" std::uint64_t
gti_test_cleanup_release_address_v1(std::uint64_t index) {
  if (index >= releases || index >= addresses.size()) {
    overflow = 1;
    return 0;
  }
  return static_cast<std::uint64_t>(addresses[static_cast<std::size_t>(index)]);
}

extern "C" std::uint32_t gti_test_cleanup_release_overflow_v1() {
  return overflow;
}

#if defined(_WIN32)
extern "C" void gti_rt_aligned_free_v1(void *address) {
  record(address);
  _aligned_free(address);
}
#else
extern "C" void gti_rt_posix_free_v1(void *address) {
  record(address);
  std::free(address);
}
#endif
