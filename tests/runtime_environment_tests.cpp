#include "gti/environment.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Deterministically exercise both name-copy and snapshot allocation failure.
// The flag is enabled only during one lookup, after all readers have joined.
std::atomic<bool> failAllocation{false};
std::atomic<unsigned> allocationCalls{0};
void *operator new(std::size_t size) {
  allocationCalls.fetch_add(1);
  if (failAllocation.load())
    throw std::bad_alloc{};
  if (void *pointer = std::malloc(size == 0 ? 1 : size))
    return pointer;
  throw std::bad_alloc{};
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *pointer) noexcept { std::free(pointer); }
void operator delete(void *pointer, const std::nothrow_t &) noexcept {
  std::free(pointer);
}
void operator delete(void *pointer, std::size_t) noexcept {
  std::free(pointer);
}

namespace {
int failures = 0;
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "runtime environment test failure: " << message << '\n';
    ++failures;
  }
}
gti_c_string_view view(std::string_view value) {
  return {value.data(), static_cast<uint64_t>(value.size())};
}
std::string_view text(int64_t value) {
  return {static_cast<const char *>(gti_rt_environment_snapshot_data_v1(value)),
          static_cast<size_t>(gti_rt_environment_snapshot_size_v1(value))};
}
} // namespace

int main() {
  int64_t result = 42;
  expect(gti_rt_environment_get_v1(view(""), &result) ==
                 GTI_ENVIRONMENT_INVALID_NAME_V1 &&
             result == 0,
         "empty name must fail and clear output");
  result = 42;
  expect(gti_rt_environment_get_v1(view("A=B"), &result) ==
                 GTI_ENVIRONMENT_INVALID_NAME_V1 &&
             result == 0,
         "equals must not be interpreted as assignment");
  expect(gti_rt_environment_get_v1(view(std::string_view{"A\0B", 3}),
                                   &result) == GTI_ENVIRONMENT_INVALID_NAME_V1,
         "embedded NUL must not become a prefix lookup");
  expect(gti_rt_environment_get_v1({nullptr, UINT64_MAX}, &result) ==
             GTI_ENVIRONMENT_INVALID_NAME_V1,
         "null input must fail before inspecting bytes");
  expect(gti_rt_environment_get_v1(view("PATH"), nullptr) ==
             GTI_ENVIRONMENT_INVALID_NAME_V1,
         "null output must fail");
  expect(gti_rt_environment_snapshot_size_v1(0) == 0 &&
             gti_rt_environment_snapshot_data_v1(0) == nullptr,
         "null snapshot observation must be defined");
  gti_rt_environment_snapshot_destroy_v1(0);

#if defined(_WIN32)
  expect(gti_rt_environment_get_v1(view("PATH"), &result) ==
                 GTI_ENVIRONMENT_UNSUPPORTED_V1 &&
             result == 0,
         "Windows must not silently use the narrow CRT environment");
#else
  constexpr auto name = "GTI_ENV_NATIVE";
  expect(unsetenv(name) == 0, "remove test variable");
  const auto beforeMissing = allocationCalls.load();
  expect(gti_rt_environment_get_v1(view(name), &result) ==
                 GTI_ENVIRONMENT_OK_V1 &&
             result == 0,
         "missing is successful and has no snapshot allocation");
  expect(allocationCalls.load() == beforeMissing,
         "short missing lookup must not allocate native storage");
  expect(setenv(name, "", 1) == 0, "set empty test value");
  const auto beforeEmpty = allocationCalls.load();
  expect(gti_rt_environment_get_v1(view(name), &result) ==
                 GTI_ENVIRONMENT_OK_V1 &&
             result != 0 && text(result).empty(),
         "present empty is distinct from missing");
  expect(allocationCalls.load() == beforeEmpty + 1,
         "metadata and empty bytes must use one native allocation");
  gti_rt_environment_snapshot_destroy_v1(result);

  const std::string value{"native bytes \xff\xc3\xa9"};
  expect(setenv(name, value.c_str(), 1) == 0, "set byte-preserving value");
  const auto beforePresent = allocationCalls.load();
  expect(gti_rt_environment_get_v1(view(name), &result) ==
                 GTI_ENVIRONMENT_OK_V1 &&
             result != 0 && text(result) == value,
         "snapshot must preserve non-UTF-8 native bytes");
  expect(allocationCalls.load() == beforePresent + 1,
         "metadata and nonempty bytes must use one native allocation");
  int64_t second = 0;
  expect(gti_rt_environment_get_v1(view(name), &second) ==
                 GTI_ENVIRONMENT_OK_V1 &&
             second != 0 && second != result,
         "lookups must own independent snapshots");
  expect(setenv(name, "changed", 1) == 0 && unsetenv(name) == 0,
         "serial native mutations must succeed");
  expect(
      text(result) == value && text(second) == value,
      "later queries, replacement and removal must not invalidate snapshots");
  gti_rt_environment_snapshot_destroy_v1(result);
  gti_rt_environment_snapshot_destroy_v1(second);

  const std::string large(16384, 'x');
  expect(setenv(name, large.c_str(), 1) == 0, "set large test value");
  std::atomic<bool> readersSucceeded{true};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      for (int iteration = 0; iteration < 100; ++iteration) {
        int64_t owned = 0;
        if (gti_rt_environment_get_v1(view(name), &owned) !=
                GTI_ENVIRONMENT_OK_V1 ||
            owned == 0 || text(owned) != large)
          readersSucceeded.store(false);
        gti_rt_environment_snapshot_destroy_v1(owned);
      }
    });
  }
  for (auto &reader : readers)
    reader.join();
  expect(readersSucceeded.load(), "read-only concurrent queries must work");

  // name fits the host string's inline capacity, so the next allocation is
  // the single native snapshot. The long name exercises throwing string new.
  result = 42;
  failAllocation.store(true);
  const auto snapshotFailure = gti_rt_environment_get_v1(view(name), &result);
  failAllocation.store(false);
  expect(snapshotFailure == GTI_ENVIRONMENT_OUT_OF_MEMORY_V1 && result == 0,
         "snapshot exhaustion must return an error without publication");
  const std::string longName(128, 'N');
  result = 42;
  failAllocation.store(true);
  const auto nameFailure = gti_rt_environment_get_v1(view(longName), &result);
  failAllocation.store(false);
  expect(nameFailure == GTI_ENVIRONMENT_OUT_OF_MEMORY_V1 && result == 0,
         "name allocation exceptions must not cross the C ABI");
  expect(unsetenv(name) == 0, "remove native test variable");
#endif
  return failures == 0 ? 0 : 1;
}
