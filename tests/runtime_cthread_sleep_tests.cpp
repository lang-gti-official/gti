#include "cthread_sleep_internal.h"
#include "gti/cthread.h"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <signal.h>
#include <thread>
#endif

namespace {
int failures = 0;
int calls = 0;
std::int32_t hostStatus = -1;
std::int64_t hostSeconds = 2;
std::int64_t hostNanos = 250000000;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::int32_t host(std::int64_t seconds, std::int64_t nanos,
                  std::int64_t *remainingSeconds,
                  std::int64_t *remainingNanos) noexcept {
  ++calls;
  expect(seconds == 8 && nanos == 900000000,
         "the host receives exact validated scalars");
  *remainingSeconds = hostSeconds;
  *remainingNanos = hostNanos;
  return hostStatus;
}

void testPublication() {
  using gti::runtime::detail::sleepCThread;
  std::int64_t seconds = 7;
  std::int64_t nanos = 9;
  expect(sleepCThread(8, 900000000, &seconds, &nanos, host) == -1 &&
             seconds == 2 && nanos == 250000000 && calls == 1,
         "interruption publishes both remaining components exactly once");
  expect(sleepCThread(8, 900000000, nullptr, nullptr, host) == -1 && calls == 2,
         "null outputs discard the remaining duration without changing status");
  for (const auto status : {0, -2, 1, -7}) {
    seconds = 7;
    nanos = 9;
    hostStatus = status;
    expect(sleepCThread(8, 900000000, &seconds, &nanos, host) ==
                   (status == 0 ? 0 : -2) &&
               seconds == 7 && nanos == 9,
           "elapsed/error/invalid host statuses cannot publish outputs");
  }
  hostStatus = -1;
  for (const auto invalid :
       {-1LL, 1000000000LL, std::numeric_limits<long long>::max()}) {
    hostNanos = invalid;
    expect(sleepCThread(8, 900000000, &seconds, &nanos, host) == -2 &&
               seconds == 7 && nanos == 9,
           "malformed host nanoseconds cannot escape");
  }
  hostNanos = 250000000;
  for (const auto invalid :
       {-1LL, 9LL, std::numeric_limits<long long>::max()}) {
    hostSeconds = invalid;
    expect(sleepCThread(8, 900000000, &seconds, &nanos, host) == -2 &&
               seconds == 7 && nanos == 9,
           "negative or excessive host remaining seconds cannot escape");
  }
  hostSeconds = 8;
  hostNanos = 900000001;
  expect(sleepCThread(8, 900000000, &seconds, &nanos, host) == -2,
         "remaining time cannot exceed the request's fractional component");
  hostSeconds = 0;
  hostNanos = 0;
  expect(sleepCThread(8, 900000000, &seconds, &nanos, host) == -1 &&
             seconds == 0 && nanos == 0,
         "interruption racing expiry may have zero remaining time");
}

void testValidation() {
  using gti::runtime::detail::sleepCThread;
  calls = 0;
  std::int64_t seconds = 7;
  std::int64_t nanos = 9;
  expect(sleepCThread(0, 0, &seconds, &nanos, host) == 0 && calls == 0,
         "zero duration succeeds without waiting");
  expect(sleepCThread(-1, 0, &seconds, &nanos, host) == -2 &&
             sleepCThread(0, -1, &seconds, &nanos, host) == -2 &&
             sleepCThread(0, 1000000000, &seconds, &nanos, host) == -2,
         "invalid durations never enter the host");
  expect(sleepCThread(8, 900000000, nullptr, &nanos, host) == -2 &&
             sleepCThread(8, 900000000, &seconds, nullptr, host) == -2 &&
             sleepCThread(8, 900000000, &seconds, &seconds, host) == -2 &&
             sleepCThread(8, 900000000, &seconds, &nanos, nullptr) == -2,
         "invalid output pairs and a missing host fail closed");
  expect(seconds == 7 && nanos == 9 && calls == 0,
         "all validation failures preserve caller state");
}

void testNative() {
  std::int64_t seconds = 7;
  std::int64_t nanos = 9;
  expect(gti_rt_cthread_sleep_v1(0, 0, &seconds, &nanos) == 0 &&
             gti_rt_cthread_sleep_v1(0, 1, nullptr, nullptr) == 0,
         "real native zero and positive duration sleep complete");
  expect(gti_rt_cthread_sleep_v1(-1, 0, &seconds, &nanos) == -2 &&
             gti_rt_cthread_sleep_v1(0, -1, &seconds, &nanos) == -2 &&
             gti_rt_cthread_sleep_v1(0, 1000000000, &seconds, &nanos) == -2 &&
             gti_rt_cthread_sleep_v1(0, 0, &seconds, nullptr) == -2,
         "the public C ABI validates the same duration and pointer contract");
  expect(seconds == 7 && nanos == 9,
         "native non-interruption preserves output");
}

#if defined(_WIN32)
void CALLBACK interruptSleep(ULONG_PTR) {}
#else
void interruptSleep(int) {}
#endif

void testNativeInterruption() {
  std::int64_t seconds = 7;
  std::int64_t nanos = 9;
#if defined(_WIN32)
  const auto queued = QueueUserAPC(interruptSleep, GetCurrentThread(), 0);
  expect(queued != 0, "native interruption APC is queued");
  if (queued == 0) {
    return;
  }
  const auto status = gti_rt_cthread_sleep_v1(5, 0, &seconds, &nanos);
#else
  struct sigaction action{};
  struct sigaction previous{};
  action.sa_handler = interruptSleep;
  sigemptyset(&action.sa_mask);
  const int installed = sigaction(SIGUSR1, &action, &previous);
  expect(installed == 0, "native interruption handler is installed");
  if (installed != 0) {
    return;
  }
  sigset_t signal;
  sigset_t previousMask;
  sigemptyset(&signal);
  sigaddset(&signal, SIGUSR1);
  const int unblocked = pthread_sigmask(SIG_UNBLOCK, &signal, &previousMask);
  expect(unblocked == 0, "the test thread can receive its targeted signal");
  if (unblocked != 0) {
    sigaction(SIGUSR1, &previous, nullptr);
    return;
  }
  const pthread_t target = pthread_self();
  std::atomic<bool> done{false};
  std::thread sender([&] {
    // Repeat until completion to close the race between entering nanosleep and
    // the first signal. The interval limits test load; it is not a timing
    // oracle.
    while (!done.load(std::memory_order_acquire)) {
      expect(pthread_kill(target, SIGUSR1) == 0,
             "targeted interruption is sent");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  const auto status = gti_rt_cthread_sleep_v1(5, 0, &seconds, &nanos);
  done.store(true, std::memory_order_release);
  sender.join();
  // No more sends can occur. Drain a late pending signal while our handler is
  // still installed, before restoring a possibly terminating disposition.
  const int blocked = pthread_sigmask(SIG_BLOCK, &signal, nullptr);
  expect(blocked == 0, "interruption is blocked during handler restoration");
  if (blocked == 0) {
    sigset_t pending;
    const int inspected = sigpending(&pending);
    expect(inspected == 0, "pending interruption can be inspected");
    if (inspected == 0 && sigismember(&pending, SIGUSR1) == 1) {
      int received = 0;
      expect(sigwait(&signal, &received) == 0 && received == SIGUSR1,
             "late targeted interruption is drained");
    }
  }
  sigaction(SIGUSR1, &previous, nullptr);
  pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
#endif
  expect(status == -1 && seconds >= 0 && seconds <= 5 && nanos >= 0 &&
             nanos < 1000000000 && (seconds < 5 || nanos == 0),
         "real host interruption returns normalized remaining time");
}
} // namespace

int main() {
  testPublication();
  testValidation();
  testNative();
  testNativeInterruption();
  return failures == 0 ? 0 : 1;
}
