// Test-only fit/lifetime oracle. No generated GTI body uses this interface.
// Payload transfer/drop policy is intentionally absent: it belongs to MIR.
#include "fixtures/llvm_mutex_slot_oracle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <type_traits>

namespace {
using Slot = gti_test_mutex_slot_v1;
using Native = std::mutex;
static_assert(sizeof(Slot) == 64 && alignof(Slot) == 8);
static_assert(sizeof(Native) <= sizeof(Slot));
static_assert(alignof(Native) <= alignof(Slot));
static_assert(std::is_nothrow_default_constructible_v<Native>);
static_assert(std::is_nothrow_destructible_v<Native>);
static_assert(std::is_trivially_destructible_v<Slot>);

void require(bool condition) {
  if (!condition)
    std::abort();
}

// Only after successful placement construction and before explicit destruction.
// The live enclosing Slot/unsigned-char array provides the object's storage;
// no active-union-member assumption or std::mutex byte-layout access is used.
Native *native(Slot *slot) {
  return std::launder(reinterpret_cast<Native *>(slot->bytes));
}
} // namespace

extern "C" void gti_test_mutex_slot_initialize_v1(Slot *slot) {
  std::construct_at(reinterpret_cast<Native *>(slot->bytes));
}
extern "C" void gti_test_mutex_slot_destroy_v1(Slot *slot) {
  std::destroy_at(native(slot));
}

// Fixed-width status matches the existing lock failure policy only. Unexpected
// init/destroy/unlock failure policy is NOT selected by this test proposal;
// those operations are exercised solely with their valid lifetime/owner proof.
extern "C" std::int32_t gti_test_mutex_slot_lock_v1(Slot *slot) {
  try {
    native(slot)->lock();
    return 0;
  } catch (const std::system_error &) {
    return 1;
  } catch (...) {
    return 1;
  }
}
extern "C" void gti_test_mutex_slot_unlock_v1(Slot *slot) {
  native(slot)->unlock();
}

int main() {
  require(gti_test_mutex_c_size() == sizeof(Slot));
  require(gti_test_mutex_c_alignment() == alignof(Slot));
  require(gti_test_mutex_c_stride() == sizeof(Slot));
  require(gti_test_mutex_c_embedding_offset() ==
          offsetof(gti_test_mutex_embedding_v1, slot));
  require(gti_test_mutex_c_embedding_size() ==
          sizeof(gti_test_mutex_embedding_v1));
  auto *slots = gti_test_mutex_c_slots();
  require(reinterpret_cast<std::uintptr_t>(&slots[0]) % alignof(Slot) == 0);
  require(reinterpret_cast<std::uintptr_t>(&slots[1]) % alignof(Slot) == 0);

  // Construct independent objects, even with the first lock already held.
  // A future GTI owner move must use this fresh-lock contract, never memcpy.
  gti_test_mutex_slot_initialize_v1(&slots[0]);
  require(gti_test_mutex_slot_lock_v1(&slots[0]) == 0);
  gti_test_mutex_slot_initialize_v1(&slots[1]);
  require(gti_test_mutex_c_roundtrip(&slots[1]) == 0);
  gti_test_mutex_slot_unlock_v1(&slots[0]);

  int protectedValue = 0;
  std::array<std::thread, 2> workers;
  for (auto &worker : workers)
    worker = std::thread([&] {
      for (unsigned index = 0; index < 10000; ++index) {
        require(gti_test_mutex_slot_lock_v1(&slots[1]) == 0);
        ++protectedValue;
        gti_test_mutex_slot_unlock_v1(&slots[1]);
      }
    });
  for (auto &worker : workers)
    worker.join();
  require(protectedValue == 20000);
  gti_test_mutex_slot_destroy_v1(&slots[1]);
  gti_test_mutex_slot_destroy_v1(&slots[0]);

  // C byte-array carrier remains live. Reuse starts a NEW native lifetime; no
  // stale pointer or copied native state is retained across the destruction.
  gti_test_mutex_slot_initialize_v1(&slots[0]);
  require(gti_test_mutex_c_roundtrip(&slots[0]) == 0);
  gti_test_mutex_slot_destroy_v1(&slots[0]);
  std::cout
      << "mutex slot proposal: C17 size64/align8/stride/embedding; native "
         "fit, fresh locks, contention, explicit destruction/reuse passed\n";
}
