#pragma once

#include "llvm_atomic_wait_lane_profile.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

// TEST ONLY. This shim observes a live, completed GTI integer lane using the
// reviewed compiler extension. It creates no C++ object at that address. A
// verified typed capability/layout and a loan spanning the entire wait must
// supply identity, width and lifetime. No pointer may survive that loan.
// Compile the shim with -fno-strict-aliasing, as the oracle/codegen checks do;
// that flag does not begin or prove an object's lifetime.
namespace gti_test_wait_lane {
inline constexpr std::uint32_t relaxed = 0, acquire = 1, sequential = 2;

static_assert(CHAR_BIT == 8 && sizeof(void *) == 8 && sizeof(std::size_t) == 8);

template <typename T>
inline constexpr bool enabled =
    std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::uint16_t> ||
    std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::uint64_t>;

template <typename T> constexpr bool exactLane() {
  static_assert(enabled<T>, "GTI_TEST_WAIT_LANE_UNSIGNED_DOMAIN");
  static_assert(std::numeric_limits<T>::digits == sizeof(T) * CHAR_BIT);
  static_assert(alignof(T) == sizeof(T));
  static_assert(__atomic_always_lock_free(sizeof(T), nullptr),
                "GTI_TEST_WAIT_LANE_REQUIRES_NATIVE_ATOMIC");
  return true;
}
static_assert(exactLane<std::uint8_t>() && exactLane<std::uint16_t>() &&
              exactLane<std::uint32_t>() && exactLane<std::uint64_t>());

inline void require(bool condition) noexcept {
  if (!condition)
    std::abort();
}

inline void validateAddress(const void *address, std::uint32_t width) noexcept {
  require(address != nullptr &&
          (width == 1 || width == 2 || width == 4 || width == 8) &&
          reinterpret_cast<std::uintptr_t>(address) % width == 0);
}

template <typename T>
T load(const void *address, std::uint32_t order) noexcept {
  static_assert(exactLane<T>());
  validateAddress(address, sizeof(T));
  // The only accesses to the lane are same-width unsigned atomic builtins.
  // No ordinary dereference, memcpy, std::atomic/atomic_ref, alias metadata or
  // lock fallback establishes or substitutes for this native extension ABI.
  const auto *lane = static_cast<const T *>(address);
  switch (order) {
  case relaxed:
    return __atomic_load_n(lane, __ATOMIC_RELAXED);
  case acquire:
    return __atomic_load_n(lane, __ATOMIC_ACQUIRE);
  case sequential:
    return __atomic_load_n(lane, __ATOMIC_SEQ_CST);
  default:
    std::abort();
  }
}
// Bool shares the uint8 lane. The verified GTI producer must encode its old
// i1 value as canonical 0/1, and zero-extend other old lane bits. This
// width-only ABI cannot rediscover bool identity or validate arbitrary foreign
// storage.
} // namespace gti_test_wait_lane
