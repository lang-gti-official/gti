#include "cthread_condition_internal.h"
#include "cthread_mutex_internal.h"
#include "gti/cthread.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
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
  std::uint64_t condition = 91;
  expect(initCCondition(nullptr) == Status::Error, "null output");
  failNextCConditionAllocationForTest();
  expect(initCCondition(&condition) == Status::NoMemory && condition == 91,
         "OOM preserves output");
  expect(notifyCCondition(0, false) == Status::Error &&
             destroyCCondition(0) == Status::Error,
         "invalid condition");
  expect(initCCondition(&condition) == Status::Success, "condition init");
  std::uint64_t mutex = 0;
  expect(initCMutex(&mutex, 1) == Status::Success, "recursive mutex init");
  expect(waitCCondition(condition, mutex) == Status::Error,
         "unlocked wait rejected");
  expect(lockCMutex(mutex) == Status::Success, "lock once");
  std::thread other([&] {
    expect(waitCCondition(condition, mutex) == Status::Error,
           "wrong-thread wait rejected");
  });
  other.join();
  expect(lockCMutex(mutex) == Status::Success, "recursive lock");
  expect(waitCCondition(condition, mutex) == Status::Error,
         "recursive depth two rejected");
  expect(unlockCMutex(mutex) == Status::Success, "depth preserved");
  expect(waitCCondition(0, mutex) == Status::Error,
         "invalid condition preserves mutex");
  for (const auto nanos : {-1LL, 1000000000LL}) {
    expect(timedWaitCCondition(condition, mutex, 0, nanos) == Status::Error,
           "invalid nanoseconds preserve lock");
  }
  expect(timedWaitCCondition(condition, mutex, -9223372037LL, 145224191) ==
                 Status::Error &&
             timedWaitCCondition(condition, mutex, 9223372036LL, 854775808) ==
                 Status::Error,
         "out-of-range deadlines rejected");
  expect(notifyCCondition(condition, false) == Status::Success &&
             notifyCCondition(condition, true) == Status::Success,
         "empty notification succeeds");
  expect(timedWaitCCondition(condition, mutex, -9223372037LL, 145224192) ==
             Status::TimedOut,
         "past deadline times out; empty notification is not retained");
  failNextCConditionWaitForTest();
  expect(waitCCondition(condition, mutex) == Status::Error,
         "host wait failure reported");
  expect(unlockCMutex(mutex) == Status::Success &&
             unlockCMutex(mutex) == Status::Error,
         "error/timeout reacquire exactly once");
  const auto old = condition;
  expect(destroyCCondition(condition) == Status::Success &&
             initCCondition(&condition) == Status::Success &&
             condition != old &&
             notifyCCondition(old, false) == Status::Error &&
             destroyCCondition(condition) == Status::Success &&
             destroyCMutex(mutex) == Status::Success,
         "stale identities not reused");
}

void publication(bool timed, bool broadcast) {
  std::uint64_t condition = 0, mutex = 0;
  expect(initCCondition(&condition) == Status::Success &&
             initCMutex(&mutex, 0) == Status::Success,
         "publication init");
  int data = 0;
  std::atomic<int> finished{0};
  std::array<std::thread, 4> workers;
  for (auto &worker : workers) {
    worker = std::thread([&] {
      expect(lockCMutex(mutex) == Status::Success, "worker locks");
      while (data == 0) {
        const auto result = timed ? timedWaitCCondition(condition, mutex,
                                                        9223372036LL, 854775807)
                                  : waitCCondition(condition, mutex);
        expect(result == Status::Success, "notified wait succeeds");
      }
      expect(data == 73, "ordinary writes published through reacquisition");
      expect(unlockCMutex(mutex) == Status::Success,
             "worker owns mutex on return");
      ++finished;
    });
  }
  while (waitingCConditionCallsForTest(condition) != workers.size()) {
    std::this_thread::yield();
  }
  expect(lockCMutex(mutex) == Status::Success, "every waiter released mutex");
  expect(destroyCCondition(condition) == Status::Busy &&
             destroyCMutex(mutex) == Status::Busy,
         "waiters retain both lifetimes");
  data = 73;
  if (broadcast) {
    expect(notifyCCondition(condition, true) == Status::Success,
           "broadcast selects every waiter");
  } else {
    for (std::size_t count = 1; count <= workers.size(); ++count) {
      expect(notifyCCondition(condition, false) == Status::Success,
             "signal selects one");
      while (waitingCMutexCallsForTest(mutex) != count) {
        std::this_thread::yield();
      }
      expect(waitingCConditionCallsForTest(condition) == workers.size() - count,
             "one signal cannot release two waiters");
    }
  }
  while (waitingCMutexCallsForTest(mutex) != workers.size()) {
    std::this_thread::yield();
  }
  expect(finished == 0 && destroyCCondition(condition) == Status::Busy,
         "notified waiters retain condition and cannot return before "
         "reacquisition");
  expect(unlockCMutex(mutex) == Status::Success, "publisher releases");
  for (auto &worker : workers) {
    worker.join();
  }
  expect(finished == 4 && destroyCCondition(condition) == Status::Success &&
             destroyCMutex(mutex) == Status::Success,
         "all waiters complete and lifetimes close");
}

void terminal(bool reacquireFailure) {
  std::set_terminate([] { std::_Exit(86); });
  std::uint64_t condition = 0, mutex = 0;
  if (gti_rt_cnd_init_v1(&condition) != 0 ||
      gti_rt_mtx_init_v1(&mutex, 0) != 0) {
    std::_Exit(2);
  }
  std::thread worker([&] {
    gti_rt_mtx_lock_v1(mutex);
    gti_rt_cnd_wait_v1(condition, mutex);
    std::_Exit(3);
  });
  while (waitingCConditionCallsForTest(condition) != 1) {
    std::this_thread::yield();
  }
  if (reacquireFailure) {
    if (gti_rt_mtx_lock_v1(mutex) != 0 ||
        gti_rt_cnd_signal_v1(condition) != 0) {
      std::_Exit(5);
    }
    while (waitingCMutexCallsForTest(mutex) != 1) {
      std::this_thread::yield();
    }
    failNextCMutexWaitForTest();
    if (gti_rt_mtx_unlock_v1(mutex) != 0) {
      std::_Exit(6);
    }
    worker.join();
    std::_Exit(7);
  }
  gti_rt_cnd_destroy_v1(condition);
  std::_Exit(4);
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "destroy-waiting") {
    terminal(false);
  }
  if (argc == 2 && std::string_view(argv[1]) == "reacquire-failure") {
    terminal(true);
  }
  validation();
  for (bool timed : {false, true}) {
    for (bool broadcast : {false, true}) {
      publication(timed, broadcast);
    }
  }
  return failures == 0 ? 0 : 1;
}
