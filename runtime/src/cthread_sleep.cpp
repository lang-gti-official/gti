#include "cthread_sleep_internal.h"
#include "gti/cthread.h"

#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <time.h>
#endif

namespace {

constexpr std::int64_t nanosPerSecond = 1000000000;

bool validDuration(std::int64_t seconds, std::int64_t nanoseconds) noexcept {
  return seconds >= 0 && nanoseconds >= 0 && nanoseconds < nanosPerSecond;
}

std::int32_t nativeSleep(std::int64_t seconds, std::int64_t nanoseconds,
                         std::int64_t *remainingSeconds,
                         std::int64_t *remainingNanoseconds) noexcept {
#if defined(_WIN32)
  // Round up to the host's 100ns unit without overflowing a relative due time.
  constexpr std::int64_t ticksPerSecond = 10000000;
  const auto fractionalTicks = (nanoseconds + 99) / 100;
  if (seconds > (std::numeric_limits<std::int64_t>::max() - fractionalTicks) /
                    ticksPerSecond) {
    return -2;
  }
  const ULONGLONG start = GetTickCount64();
  const HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
  if (timer == nullptr) {
    return -2;
  }
  LARGE_INTEGER due{};
  due.QuadPart = -(seconds * ticksPerSecond + fractionalTicks);
  if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    CloseHandle(timer);
    return -2;
  }
  const DWORD status = WaitForSingleObjectEx(timer, INFINITE, TRUE);
  const ULONGLONG end = GetTickCount64();
  if (!CloseHandle(timer)) {
    return -2;
  }
  if (status == WAIT_OBJECT_0) {
    return 0;
  }
  if (status != WAIT_IO_COMPLETION) {
    return -2;
  }
  const auto elapsed = end - start;
  // Remaining time follows the host tick resolution (including suspended time),
  // not an invented nanosecond-precision wall-clock guarantee.
  const auto elapsedSeconds = elapsed / 1000;
  const auto elapsedNanos = static_cast<std::int64_t>(elapsed % 1000) * 1000000;
  if (elapsedSeconds > static_cast<std::uint64_t>(seconds) ||
      (elapsedSeconds == static_cast<std::uint64_t>(seconds) &&
       elapsedNanos >= nanoseconds)) {
    *remainingSeconds = 0;
    *remainingNanoseconds = 0;
  } else {
    *remainingSeconds = seconds - static_cast<std::int64_t>(elapsedSeconds);
    *remainingNanoseconds = nanoseconds - elapsedNanos;
    if (*remainingNanoseconds < 0) {
      --*remainingSeconds;
      *remainingNanoseconds += nanosPerSecond;
    }
  }
  return -1;
#else
  if (!std::in_range<time_t>(seconds)) {
    return -2;
  }
  const timespec requested{static_cast<time_t>(seconds),
                           static_cast<long>(nanoseconds)};
  timespec remaining{};
  if (nanosleep(&requested, &remaining) == 0) {
    return 0;
  }
  if (errno != EINTR) {
    return -2;
  }
  // Linux measures the remainder against the timer's slack-extended expiry,
  // so an interruption inside that slack reports more than was requested.
  // Clamp it to the request, as the Windows path clamps its tick remainder.
  if (remaining.tv_sec > requested.tv_sec ||
      (remaining.tv_sec == requested.tv_sec &&
       remaining.tv_nsec > requested.tv_nsec)) {
    remaining = requested;
  }
  *remainingSeconds = static_cast<std::int64_t>(remaining.tv_sec);
  *remainingNanoseconds = static_cast<std::int64_t>(remaining.tv_nsec);
  return -1;
#endif
}

} // namespace

namespace gti::runtime::detail {

std::int32_t sleepCThread(std::int64_t seconds, std::int64_t nanoseconds,
                          std::int64_t *remainingSeconds,
                          std::int64_t *remainingNanoseconds,
                          CThreadSleepHost host) noexcept {
  if (!validDuration(seconds, nanoseconds) || host == nullptr ||
      (remainingSeconds == nullptr) != (remainingNanoseconds == nullptr) ||
      (remainingSeconds != nullptr &&
       remainingSeconds == remainingNanoseconds)) {
    return -2;
  }
  if (seconds == 0 && nanoseconds == 0) {
    return 0;
  }
  std::int64_t secondsLeft = 0;
  std::int64_t nanosLeft = 0;
  const auto status = host(seconds, nanoseconds, &secondsLeft, &nanosLeft);
  if (status == 0) {
    return 0;
  }
  if (status != -1 || !validDuration(secondsLeft, nanosLeft) ||
      secondsLeft > seconds ||
      (secondsLeft == seconds && nanosLeft > nanoseconds)) {
    return -2;
  }
  if (remainingSeconds != nullptr) {
    *remainingSeconds = secondsLeft;
    *remainingNanoseconds = nanosLeft;
  }
  return -1;
}

} // namespace gti::runtime::detail

extern "C" int32_t gti_rt_cthread_sleep_v1(int64_t seconds, int64_t nanoseconds,
                                           int64_t *remaining_seconds,
                                           int64_t *remaining_nanoseconds) {
  return gti::runtime::detail::sleepCThread(seconds, nanoseconds,
                                            remaining_seconds,
                                            remaining_nanoseconds, nativeSleep);
}
