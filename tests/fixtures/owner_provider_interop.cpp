#include "process_allocation_test_provider.h"
#include <new>
namespace {
bool enabled = false;
int failed = 0, gtiAllocations = 0, gtiReleases = 0;
int cppAllocations = 0, cppReleases = 0, destructors = 0;
void *gtiAddress = nullptr, *cppAddress = nullptr;
struct alignas(64) Foreign {
  int value = 73;
  ~Foreign() { ++destructors; }
};
} // namespace
namespace gti_test_process_provider {
void *acquire(std::size_t bytes, std::size_t alignment) {
  void *address = systemAllocate(bytes, alignment);
  if (enabled) {
    ++gtiAllocations;
    gtiAddress = address;
  }
  return address;
}
void release(void *address) {
  if (enabled && address) {
    if (address != gtiAddress || address == cppAddress)
      failed = 1;
    ++gtiReleases;
    gtiAddress = nullptr;
  }
  systemRelease(address);
}
} // namespace gti_test_process_provider
void *operator new(std::size_t n) {
  if (void *address = std::malloc(n ? n : 1))
    return address;
  throw std::bad_alloc{};
}
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
  void *address = std::malloc(n);
  if (enabled) {
    ++cppAllocations;
    cppAddress = address;
  }
  return address;
}
void *operator new(std::size_t n, std::align_val_t a,
                   const std::nothrow_t &) noexcept {
  void *address =
      gti_test_process_provider::systemAllocate(n, static_cast<std::size_t>(a));
  if (enabled) {
    ++cppAllocations;
    cppAddress = address;
  }
  return address;
}
void operator delete(void *address) noexcept {
  if (enabled && address == cppAddress) {
    ++cppReleases;
    cppAddress = nullptr;
  }
  std::free(address);
}
void operator delete(void *address, std::size_t) noexcept {
  ::operator delete(address);
}
void operator delete(void *address, std::align_val_t) noexcept {
  if (enabled && address) {
    if (address != cppAddress || address == gtiAddress)
      failed = 2;
    ++cppReleases;
    cppAddress = nullptr;
  }
  gti_test_process_provider::systemRelease(address);
}
void operator delete(void *address, std::size_t, std::align_val_t a) noexcept {
  ::operator delete(address, a);
}
extern "C" void owner_probe_begin() { enabled = true; }
extern "C" void owner_probe_observe(const std::uint64_t *value) {
  if (!value || *value != 42 || gtiAllocations != 1 || cppAllocations != 0)
    failed = 3;
}
extern "C" void *owner_probe_cpp_create() { return new (std::nothrow) Foreign; }
extern "C" std::int32_t owner_probe_cpp_value(void *value) {
  return value ? static_cast<Foreign *>(value)->value : -1;
}
extern "C" void owner_probe_cpp_destroy(void *value) {
  delete static_cast<Foreign *>(value);
}
extern "C" std::int32_t owner_probe_finish() {
  enabled = false;
  if (failed)
    return failed;
  return gtiAllocations == 1 && gtiReleases == 1 && cppAllocations == 1 &&
                 cppReleases == 1 && destructors == 1 && !gtiAddress &&
                 !cppAddress
             ? 0
             : 4;
}
