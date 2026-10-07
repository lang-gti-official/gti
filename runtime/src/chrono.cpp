#include "gti/chrono.h"

#include "chrono_internal.h"

#include <chrono>
#include <cstdint>
#include <limits>

namespace gti::runtime::detail {

gti_chrono_status_v1
composeChronoNanoseconds(const std::int64_t seconds,
                         const std::int64_t subsecondNanoseconds,
                         std::int64_t *const outputNanoseconds) noexcept {
  constexpr std::int64_t nanosecondsPerSecond = 1'000'000'000;
  if (outputNanoseconds == nullptr || subsecondNanoseconds < 0 ||
      subsecondNanoseconds >= nanosecondsPerSecond) {
    return GTI_CHRONO_OUT_OF_RANGE_V1;
  }

  constexpr std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t maximumSeconds = maximum / nanosecondsPerSecond;
  constexpr std::int64_t minimumTruncatedSeconds =
      minimum / nanosecondsPerSecond;

  std::int64_t result = 0;
  if (seconds >= 0) {
    if (seconds > maximumSeconds) {
      return GTI_CHRONO_OUT_OF_RANGE_V1;
    }
    const std::int64_t whole = seconds * nanosecondsPerSecond;
    if (subsecondNanoseconds > maximum - whole) {
      return GTI_CHRONO_OUT_OF_RANGE_V1;
    }
    result = whole + subsecondNanoseconds;
  } else if (seconds >= minimumTruncatedSeconds) {
    // Multiplication is representable at and above the truncated quotient.
    result = seconds * nanosecondsPerSecond + subsecondNanoseconds;
  } else {
    // A floor-based negative timestamp can use one additional second when its
    // positive remainder brings the result back into the int64_t range.
    if (seconds != minimumTruncatedSeconds - 1) {
      return GTI_CHRONO_OUT_OF_RANGE_V1;
    }
    constexpr std::int64_t minimumRemainder =
        nanosecondsPerSecond + minimum % nanosecondsPerSecond;
    if (subsecondNanoseconds < minimumRemainder) {
      return GTI_CHRONO_OUT_OF_RANGE_V1;
    }
    result = minimum + (subsecondNanoseconds - minimumRemainder);
  }

  *outputNanoseconds = result;
  return GTI_CHRONO_OK_V1;
}

namespace {

template <typename Clock>
gti_chrono_status_v1 readClock(std::int64_t *const outputNanoseconds) noexcept {
  if (outputNanoseconds == nullptr) {
    return GTI_CHRONO_OUT_OF_RANGE_V1;
  }

  const auto elapsed = Clock::now().time_since_epoch();
  const auto wholeSeconds = std::chrono::floor<std::chrono::seconds>(elapsed);
  const auto remainder = std::chrono::duration_cast<std::chrono::nanoseconds>(
      elapsed - wholeSeconds);

  // The two standard clocks used below have an integral representation no
  // wider than int64_t on supported GTI hosts. Keep this a compile-time host
  // requirement rather than silently narrowing a novel standard library ABI.
  static_assert(
      std::numeric_limits<decltype(wholeSeconds.count())>::is_integer);
  static_assert(std::numeric_limits<decltype(wholeSeconds.count())>::digits <=
                std::numeric_limits<std::int64_t>::digits);
  return composeChronoNanoseconds(
      static_cast<std::int64_t>(wholeSeconds.count()),
      static_cast<std::int64_t>(remainder.count()), outputNanoseconds);
}

} // namespace

} // namespace gti::runtime::detail

extern "C" gti_chrono_status_v1
gti_rt_chrono_steady_now_v1(std::int64_t *const outputNanoseconds) {
  return gti::runtime::detail::readClock<std::chrono::steady_clock>(
      outputNanoseconds);
}

extern "C" gti_chrono_status_v1
gti_rt_chrono_system_now_v1(std::int64_t *const outputNanoseconds) {
  return gti::runtime::detail::readClock<std::chrono::system_clock>(
      outputNanoseconds);
}
