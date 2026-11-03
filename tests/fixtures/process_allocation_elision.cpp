#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
bool enabled = false;
std::uint64_t attempts = 0;
std::uint64_t releases = 0;
void *observed_allocation = nullptr;
} // namespace

extern "C" void allocation_probe_begin() {
  attempts = releases = 0;
  observed_allocation = nullptr;
  enabled = true;
}
extern "C" void allocation_probe_end() { enabled = false; }
extern "C" std::int32_t allocation_probe_input() { return 37; }
extern "C" std::int32_t allocation_probe_observe(const std::int32_t *value) {
  return *value;
}
extern "C" std::uint64_t allocation_probe_attempts() { return attempts; }
extern "C" std::uint64_t allocation_probe_releases() { return releases; }
extern "C" void allocation_probe_report() {
  std::printf("%llu %llu\n", static_cast<unsigned long long>(attempts),
              static_cast<unsigned long long>(releases));
}

void *operator new(std::size_t bytes) {
  if (void *result = std::malloc(bytes == 0 ? 1 : bytes))
    return result;
  throw std::bad_alloc{};
}

void *operator new(std::size_t bytes, const std::nothrow_t &) noexcept {
  void *result = std::malloc(bytes == 0 ? 1 : bytes);
  if (enabled) {
    ++attempts;
    observed_allocation = result;
  }
  return result;
}

void operator delete(void *value) noexcept {
  if (enabled && value == observed_allocation) {
    ++releases;
    observed_allocation = nullptr;
  }
  std::free(value);
}
void operator delete(void *value, std::size_t) noexcept {
  ::operator delete(value);
}

#ifndef _WIN32
// Native-backend owners allocate through the runtime's POSIX provider instead
// of operator new. Replace that whole runtime object, so the archive member is
// never linked beside these definitions, and count through the same probe.
extern "C" std::uint64_t gti_rt_native_size_max_v1() {
  return (std::numeric_limits<std::size_t>::max)();
}
extern "C" std::int32_t gti_rt_address_is_aligned_v1(const void *address,
                                                     std::uint64_t alignment) {
  if (!address || alignment == 0 || (alignment & (alignment - 1)) != 0)
    return 0;
  return reinterpret_cast<std::uintptr_t>(address) % alignment == 0;
}
extern "C" std::uint64_t gti_rt_posix_pointer_size_v1() {
  return sizeof(void *);
}
extern "C" std::int32_t gti_rt_posix_enomem_v1() { return ENOMEM; }
extern "C" std::int32_t gti_rt_posix_einval_v1() { return EINVAL; }
extern "C" void *gti_rt_posix_memalign_v1(std::uint64_t bytes,
                                          std::uint64_t alignment,
                                          std::int32_t *status) {
  void *address = nullptr;
  *status = posix_memalign(&address, static_cast<std::size_t>(alignment),
                           static_cast<std::size_t>(bytes));
  if (enabled && address) {
    ++attempts;
    observed_allocation = address;
  }
  return address;
}
extern "C" void gti_rt_posix_free_v1(void *address) {
  if (enabled && address == observed_allocation) {
    ++releases;
    observed_allocation = nullptr;
  }
  std::free(address);
}
#endif
