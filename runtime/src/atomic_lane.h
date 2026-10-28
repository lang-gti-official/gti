#pragma once

// Reviewed raw-lane compiler extension profiles. The translation unit must
// also use -fno-strict-aliasing; these are GTI storage bytes, not std::atomic.
#if defined(__APPLE__) && defined(__MACH__) && defined(__aarch64__) &&         \
    defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
#if !defined(__clang__) ||                                                     \
    !((defined(__apple_build_version__) && __clang_major__ == 21) ||           \
      (!defined(__apple_build_version__) && __clang_major__ == 23))
#error GTI_ATOMIC_LANE_UNREVIEWED_COMPILER
#endif
#elif defined(__linux__) && defined(__x86_64__)
#if defined(__clang__)
#if __clang_major__ != 21 || defined(__apple_build_version__)
#error GTI_ATOMIC_LANE_UNREVIEWED_COMPILER
#endif
#elif !defined(__GNUC__) || __GNUC__ != 14 || defined(__INTEL_COMPILER) ||     \
    defined(__INTEL_LLVM_COMPILER)
#error GTI_ATOMIC_LANE_UNREVIEWED_COMPILER
#endif
#else
#error GTI_ATOMIC_LANE_UNREVIEWED_TARGET
#endif
#if __CHAR_BIT__ != 8 || __SIZEOF_POINTER__ != 8 || __SIZEOF_SIZE_T__ != 8 ||  \
    __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error GTI_ATOMIC_LANE_UNREVIEWED_ABI
#endif

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <type_traits>

namespace gti::runtime::atomic_lane {
inline void require(bool condition) noexcept {
  if (!condition)
    std::abort();
}

inline void validate(const void *lane, std::uint32_t width) noexcept {
  require(lane && (width == 1 || width == 2 || width == 4 || width == 8) &&
          (reinterpret_cast<std::uintptr_t>(lane) & (width - 1)) == 0);
}

template <typename T>
T load(const void *address, std::uint32_t order) noexcept {
  static_assert(
      std::is_same_v<T, std::uint8_t> || std::is_same_v<T, std::uint16_t> ||
      std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::uint64_t>);
  static_assert(std::numeric_limits<T>::digits == sizeof(T) * 8);
  static_assert(alignof(T) == sizeof(T));
  static_assert(__atomic_always_lock_free(sizeof(T), nullptr));
  const auto *lane = static_cast<const T *>(address);
  switch (order) {
  case 0:
    return __atomic_load_n(lane, __ATOMIC_RELAXED);
  case 1:
    return __atomic_load_n(lane, __ATOMIC_ACQUIRE);
  case 2:
    return __atomic_load_n(lane, __ATOMIC_SEQ_CST);
  default:
    std::abort();
  }
}
} // namespace gti::runtime::atomic_lane
