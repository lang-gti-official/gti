#include "process_allocation_test_provider.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <mutex>

namespace {
struct Region {
  void *address = nullptr;
  std::size_t bytes = 0;
  std::size_t alignment = 0;
  int identity = 0;
};
std::mutex mutex;
std::array<Region, 256> regions{};
std::array<int, 256> events{};
int eventCount = 0;
int attempts = 0;
int acquired = 0;
int released = 0;
bool observing = false;
bool failNext = false;

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "%s (attempts=%d acquired=%d released=%d)\n", message,
               attempts, acquired, released);
  std::abort();
}

void event(int value) {
  if (eventCount == static_cast<int>(events.size()))
    std::abort();
  events[eventCount++] = value;
}

void requireEmpty() {
  for (const auto &region : regions)
    if (region.address && region.identity)
      fail("GTI owner allocation remained live");
}
} // namespace

namespace gti_test_process_provider {
void *acquire(std::size_t bytes, std::size_t alignment) {
  std::lock_guard lock(mutex);
  const int identity = observing ? ++attempts : 0;
  if (observing && failNext) {
    failNext = false;
    return nullptr;
  }
  void *address = systemAllocate(bytes, alignment);
  if (!address || !bytes || !alignment ||
      reinterpret_cast<std::uintptr_t>(address) % alignment)
    std::abort();
  for (auto &region : regions) {
    if (!region.address) {
      region = {address, bytes, alignment, identity};
      if (observing)
        ++acquired;
      return address;
    }
  }
  std::abort();
}

void release(void *address) {
  std::lock_guard lock(mutex);
  for (auto &region : regions) {
    if (region.address && region.address == address) {
      if (region.identity) {
        ++released;
        event(-region.identity);
      }
      region = {};
      systemRelease(address);
      return;
    }
  }
  fail("unmatched GTI process release"); // Wrong-family, interior or duplicate.
}
} // namespace gti_test_process_provider

extern "C" void owner_audit_begin() {
  std::lock_guard lock(mutex);
  requireEmpty();
  observing = true;
  failNext = false;
  attempts = acquired = released = eventCount = 0;
}

extern "C" void owner_audit_fail_next() {
  std::lock_guard lock(mutex);
  failNext = true;
}

extern "C" void owner_audit_event(std::int32_t value) {
  std::lock_guard lock(mutex);
  event(value);
}

extern "C" std::int32_t owner_audit_event_count(std::int32_t value) {
  std::lock_guard lock(mutex);
  int count = 0;
  for (int index = 0; index < eventCount; ++index)
    count += events[index] == value;
  return count;
}

extern "C" std::int32_t owner_audit_event_at(std::int32_t index) {
  std::lock_guard lock(mutex);
  if (index < 0 || index >= eventCount)
    std::abort();
  return events[index];
}

// Besides checking the selected route, this ordinary foreign call exposes the
// real object address. Legal allocation elision cannot erase the tested owner.
extern "C" void owner_audit_object(const void *address, std::uint64_t bytes,
                                   std::uint64_t alignment) {
  std::lock_guard lock(mutex);
  for (const auto &region : regions) {
    if (region.address != address)
      continue;
#ifdef _WIN32
    const auto expected = alignment;
#else
    const auto expected =
        alignment == 1 ||
                (bytes >= sizeof(void *) && alignment <= alignof(void *))
            ? 1
            : std::max<std::uint64_t>(alignment, sizeof(void *));
#endif
    if (!region.identity || region.bytes != bytes ||
        region.alignment != expected || !alignment ||
        reinterpret_cast<std::uintptr_t>(address) % alignment)
      fail("GTI owner layout or provider route drift");
    return;
  }
  fail("unobserved GTI owner allocation");
}

extern "C" void owner_audit_finish(std::int32_t expectedAttempts,
                                   std::int32_t expectedAcquired) {
  std::lock_guard lock(mutex);
  requireEmpty();
  if (attempts != expectedAttempts || acquired != expectedAcquired ||
      released != acquired || failNext) {
    std::fprintf(stderr,
                 "provider counts: attempts=%d acquired=%d released=%d\n",
                 attempts, acquired, released);
    std::abort();
  }
  observing = false;
}

extern "C" void owner_audit_cleanup_complete(std::int32_t expectedAttempts,
                                             std::int32_t expectedAcquired) {
  owner_audit_finish(expectedAttempts, expectedAcquired);
  std::puts("owner cleanup complete");
}
