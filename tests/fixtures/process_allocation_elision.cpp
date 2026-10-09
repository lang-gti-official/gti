#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {
bool enabled = false;
std::uint64_t attempts = 0;
std::uint64_t releases = 0;
void *observed_allocation = nullptr;
}

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
void operator delete(void *value, std::size_t) noexcept { ::operator delete(value); }
