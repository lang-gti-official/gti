#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

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

// Generated owners allocate through the runtime owner pair. Define the whole
// pair, with the runtime's exact nothrow new/delete selection, so the runtime's
// own owner object is never linked beside this recording release.
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
  record(address);
  if (overAligned(alignment))
    ::operator delete(address,
                      std::align_val_t(static_cast<std::size_t>(alignment)));
  else
    ::operator delete(address);
}
