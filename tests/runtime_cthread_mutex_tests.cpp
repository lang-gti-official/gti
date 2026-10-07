#include "cthread_mutex_internal.h"
#include "gti/cthread.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>

namespace {
using namespace gti::runtime::detail;
using Status = CThreadStatus;
std::atomic<int> failures{0};
void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void validation() {
  std::uint64_t handle = 91;
  for (int flags : {-1, 4, 8, std::numeric_limits<int>::max()}) {
    expect(initCMutex(&handle, flags) == Status::Error && handle == 91,
           "invalid flags preserve output");
  }
  expect(initCMutex(nullptr, 0) == Status::Error, "null init rejected");
  failNextCMutexAllocationForTest();
  expect(initCMutex(&handle, 0) == Status::NoMemory && handle == 91,
         "allocation failure preserves output and creates no obligation");
  expect(lockCMutex(0) == Status::Error && tryLockCMutex(0) == Status::Error &&
             unlockCMutex(0) == Status::Error &&
             destroyCMutex(0) == Status::Error,
         "zero is never a live mutex");
  expect(initCMutex(&handle, 0) == Status::Success && handle != 0,
         "plain mutex initializes");
  expect(unlockCMutex(handle) == Status::Error, "unlocked release rejected");
  expect(timedLockCMutex(handle, 0, 0) == Status::Error,
         "plain mutex does not support timed acquisition");
  expect(lockCMutex(handle) == Status::Success, "plain acquisition");
  expect(lockCMutex(handle) == Status::Error &&
             tryLockCMutex(handle) == Status::Busy,
         "plain self acquisition cannot create recursive ownership");
  expect(destroyCMutex(handle) == Status::Busy,
         "locked mutex cannot be retired");
  std::thread other([&] {
    expect(unlockCMutex(handle) == Status::Error,
           "wrong-thread release rejected");
    expect(tryLockCMutex(handle) == Status::Busy,
           "contended try never acquires");
  });
  other.join();
  expect(unlockCMutex(handle) == Status::Success &&
             destroyCMutex(handle) == Status::Success,
         "owner can release and destroy");
  const auto retired = handle;
  expect(initCMutex(&handle, 0) == Status::Success && handle != retired &&
             lockCMutex(retired) == Status::Error &&
             destroyCMutex(retired) == Status::Error &&
             destroyCMutex(handle) == Status::Success,
         "retired identities never acquire a later lifetime");
}

void recursionAndDeadlines() {
  for (int flags : {1, 2, 3}) {
    std::uint64_t handle = 0;
    expect(initCMutex(&handle, flags) == Status::Success, "mode initializes");
    if ((flags & 2) != 0) {
      for (const auto seconds : {std::numeric_limits<std::int64_t>::min(),
                                 std::numeric_limits<std::int64_t>::max()}) {
        expect(timedLockCMutex(handle, seconds, 0) == Status::Error,
               "unrepresentable deadline cannot acquire even an idle mutex");
      }
      expect(timedLockCMutex(handle, 0, -1) == Status::Error &&
                 timedLockCMutex(handle, 0, 1000000000) == Status::Error,
             "non-normalized deadlines rejected");
      expect(timedLockCMutex(handle, -1, 0) == Status::Success,
             "available timed mutex can acquire despite a past deadline");
    } else {
      expect(lockCMutex(handle) == Status::Success, "recursive acquisition");
    }
    if ((flags & 1) != 0) {
      expect(tryLockCMutex(handle) == Status::Success &&
                 lockCMutex(handle) == Status::Success,
             "recursive acquisitions increment depth");
      expect(unlockCMutex(handle) == Status::Success &&
                 unlockCMutex(handle) == Status::Success,
             "partial recursive release preserves one acquisition");
    }
    std::thread contender([&] {
      expect(tryLockCMutex(handle) == Status::Busy,
             "partial release is not publication");
      if ((flags & 2) != 0) {
        expect(timedLockCMutex(handle, 0, 0) == Status::TimedOut &&
                   timedLockCMutex(handle, -9223372037LL, 145224192) ==
                       Status::TimedOut,
               "past and minimum deadlines expire without acquiring");
        expect(timedLockCMutex(handle, -9223372037LL, 145224191) ==
                       Status::Error &&
                   timedLockCMutex(handle, 9223372036LL, 854775808) ==
                       Status::Error,
               "one nanosecond beyond either range limit is rejected");
        expect(unlockCMutex(handle) == Status::Error,
               "timeout grants no ownership");
      }
    });
    contender.join();
    expect(unlockCMutex(handle) == Status::Success, "final release");
    expect(tryLockCMutex(handle) == Status::Success &&
               unlockCMutex(handle) == Status::Success &&
               destroyCMutex(handle) == Status::Success,
           "released mutex remains reusable and destructible");
  }
}

void blockedAcquisition() {
  std::uint64_t handle = 0;
  expect(initCMutex(&handle, 2) == Status::Success, "timed mutex initializes");
  for (bool timed : {false, true}) {
    expect(lockCMutex(handle) == Status::Success, "hold before worker starts");
    int published = 0;
    std::thread waiter([&] {
      const auto result = timed
                              ? timedLockCMutex(handle, 9223372036LL, 854775807)
                              : lockCMutex(handle);
      expect(result == Status::Success, "blocked acquire resumes on release");
      if (result == Status::Success) {
        expect(published == 73, "release publishes ordinary data to waiter");
        expect(unlockCMutex(handle) == Status::Success, "waiter releases");
      }
    });
    // A source-private state observation, not a delay, establishes that the
    // worker is blocked in the actual runtime before we publish and release.
    while (waitingCMutexCallsForTest(handle) == 0) {
      std::this_thread::yield();
    }
    expect(destroyCMutex(handle) == Status::Busy,
           "an in-flight waiter prevents destruction");
    published = 73;
    expect(unlockCMutex(handle) == Status::Success,
           "wake the blocked acquirer");
    waiter.join();
  }
  expect(lockCMutex(handle) == Status::Success, "hold through timeout");
  std::thread timeout([&] {
    const auto until =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()) +
        std::chrono::milliseconds(1);
    expect(timedLockCMutex(handle, until.count() / 1000000000,
                           until.count() % 1000000000) == Status::TimedOut,
           "a future deadline expires without acquiring a held mutex");
  });
  timeout.join();
  expect(unlockCMutex(handle) == Status::Success &&
             destroyCMutex(handle) == Status::Success,
         "timeout preserves the original owner's obligation");
}

void failedWakeHandoff() {
  for (bool timed : {false, true}) {
    std::uint64_t handle = 0;
    expect(initCMutex(&handle, 2) == Status::Success &&
               lockCMutex(handle) == Status::Success,
           "hold mutex before both failure-handoff waiters enter");
    std::atomic<int> acquired{0};
    std::atomic<int> rejected{0};
    auto attempt = [&] {
      const auto status = timed
                              ? timedLockCMutex(handle, 9223372036LL, 854775807)
                              : lockCMutex(handle);
      if (status == Status::Success) {
        ++acquired;
        expect(unlockCMutex(handle) == Status::Success,
               "surviving waiter releases");
      } else {
        expect(status == Status::Error && unlockCMutex(handle) == Status::Error,
               "failed waiter receives no ownership");
        ++rejected;
      }
    };
    std::thread first(attempt);
    std::thread second(attempt);
    while (waitingCMutexCallsForTest(handle) != 2) {
      std::this_thread::yield();
    }
    failNextCMutexWaitForTest();
    expect(unlockCMutex(handle) == Status::Success, "wake the failing waiter");
    first.join();
    second.join();
    expect(acquired == 1 && rejected == 1 &&
               destroyCMutex(handle) == Status::Success,
           "a failed woken waiter hands the free mutex on instead of stranding "
           "another waiter");
  }
}

void publication() {
  std::uint64_t handle = 0;
  expect(initCMutex(&handle, 0) == Status::Success,
         "publication mutex initializes");
  int value = 0;
  std::array<std::thread, 4> workers;
  for (auto &worker : workers) {
    worker = std::thread([&] {
      for (int i = 0; i < 1000; ++i) {
        const auto locked = lockCMutex(handle);
        expect(locked == Status::Success, "contending acquire succeeds");
        if (locked != Status::Success) {
          return;
        }
        ++value;
        expect(unlockCMutex(handle) == Status::Success,
               "contending release succeeds");
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
  expect(value == 4000 && destroyCMutex(handle) == Status::Success,
         "all non-atomic writes are serialized and published");
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "destroy-locked") {
    std::set_terminate([] { std::_Exit(86); });
    std::uint64_t handle = 0;
    if (gti_rt_mtx_init_v1(&handle, 0) != 0 ||
        gti_rt_mtx_lock_v1(handle) != 0) {
      return 1;
    }
    gti_rt_mtx_destroy_v1(handle);
    return 2;
  }
  validation();
  recursionAndDeadlines();
  blockedAcquisition();
  failedWakeHandoff();
  publication();
  return failures == 0 ? 0 : 1;
}
