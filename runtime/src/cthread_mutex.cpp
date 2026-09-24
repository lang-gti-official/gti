#include "cthread_condition_internal.h"
#include "cthread_mutex_internal.h"
#include "gti/cthread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace gti::runtime::detail {
namespace {

constexpr std::int32_t recursiveFlag = 1;
constexpr std::int32_t timedFlag = 2;
using Deadline = std::chrono::time_point<std::chrono::system_clock,
                                         std::chrono::nanoseconds>;
std::atomic<bool> failAllocation{false};
std::atomic<bool> failWait{false};
std::atomic<bool> failConditionAllocation{false};
std::atomic<bool> failConditionWait{false};

struct ConditionWaiter {
  std::condition_variable wake;
  ConditionWaiter *next = nullptr;
  bool notified = false;
};

struct Condition {
  std::mutex gate;
  ConditionWaiter *waiters = nullptr;
};

struct ConditionRegistry {
  std::mutex gate;
  std::uint64_t next = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<Condition>> values;
};

ConditionRegistry &conditionRegistry() {
  static ConditionRegistry value;
  return value;
}

std::shared_ptr<Condition> findCondition(std::uint64_t handle) {
  auto &state = conditionRegistry();
  const std::lock_guard lock(state.gate);
  const auto found = state.values.find(handle);
  return found == state.values.end() ? nullptr : found->second;
}

struct Mutex {
  explicit Mutex(std::int32_t requested) : flags(requested) {}
  std::mutex gate;
  std::condition_variable available;
  std::thread::id owner;
  std::uint64_t depth = 0;
  std::uint64_t waiting = 0;
  const std::int32_t flags;
};

struct Registry {
  std::mutex gate;
  std::uint64_t next = 1;
  std::unordered_map<std::uint64_t, std::shared_ptr<Mutex>> values;
};

Registry &registry() {
  static Registry value;
  return value;
}

std::shared_ptr<Mutex> find(std::uint64_t handle) {
  auto &state = registry();
  const std::lock_guard lock(state.gate);
  const auto found = state.values.find(handle);
  return found == state.values.end() ? nullptr : found->second;
}

bool deadline(std::int64_t seconds, std::int64_t nanos, Deadline &result) {
  constexpr std::int64_t scale = 1000000000;
  constexpr auto low = std::numeric_limits<std::int64_t>::min();
  constexpr auto high = std::numeric_limits<std::int64_t>::max();
  constexpr auto lowSeconds = low / scale - 1;
  constexpr auto lowNanos = scale + low % scale;
  if (nanos < 0 || nanos >= scale || seconds < lowSeconds ||
      seconds > high / scale || (seconds == lowSeconds && nanos < lowNanos) ||
      (seconds == high / scale && nanos > high % scale)) {
    return false;
  }
  const auto ticks = seconds == lowSeconds ? low + (nanos - lowNanos)
                                           : seconds * scale + nanos;
  result = Deadline(std::chrono::nanoseconds(ticks));
  return true;
}

enum class Wait { Block, Try, Timed };

CThreadStatus conditionWait(std::uint64_t condition, std::uint64_t mutex,
                            bool timed, Deadline until = {}) noexcept {
  bool released = false;
  try {
    const auto cv = findCondition(condition);
    const auto value = find(mutex);
    if (!cv || !value) {
      return CThreadStatus::Error;
    }
    // Construct everything fallible before releasing caller ownership. Registry
    // references keep both objects live through reacquisition, not merely wake.
    ConditionWaiter waiter;
    std::unique_lock conditionLock(cv->gate);
    {
      const std::lock_guard mutexLock(value->gate);
      if (value->depth != 1 || value->owner != std::this_thread::get_id()) {
        return CThreadStatus::Error;
      }
      waiter.next = cv->waiters;
      cv->waiters = &waiter;
      value->depth = 0;
      value->owner = {};
      released = true;
      value->available.notify_one();
    }
    // Notification uses the same condition gate: it cannot pass between
    // enrollment and the atomic release-and-wait below. Signals aren't stored
    // for future waiters, and a selected waiter cannot steal another's wake.
    CThreadStatus result = CThreadStatus::Success;
    try {
      while (!waiter.notified) {
        if (failConditionWait.exchange(false, std::memory_order_relaxed)) {
          throw std::system_error(
              std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        if (timed) {
          if (until <= std::chrono::system_clock::now()) {
            result = CThreadStatus::TimedOut;
            break;
          }
          waiter.wake.wait_until(conditionLock, until);
        } else {
          waiter.wake.wait(conditionLock);
        }
      }
    } catch (...) {
      result = CThreadStatus::Error;
    }
    auto **link = &cv->waiters;
    while (*link != &waiter) {
      link = &(*link)->next;
    }
    *link = waiter.next;
    conditionLock.unlock();
    // A valid wait never returns without the caller's one acquisition. An
    // unrecoverable host reacquisition error must not pretend it owns the lock.
    if (lockCMutex(mutex) != CThreadStatus::Success) {
      std::terminate();
    }
    released = false;
    return result;
  } catch (...) {
    if (released) {
      std::terminate();
    }
    return CThreadStatus::Error;
  }
}

CThreadStatus acquire(std::uint64_t handle, Wait mode,
                      Deadline until = {}) noexcept {
  try {
    const auto value = find(handle);
    if (!value) {
      return CThreadStatus::Error;
    }
    std::unique_lock lock(value->gate);
    if (mode == Wait::Timed && (value->flags & timedFlag) == 0) {
      return CThreadStatus::Error;
    }
    const auto current = std::this_thread::get_id();
    if (value->depth != 0 && value->owner == current) {
      if ((value->flags & recursiveFlag) == 0) {
        return mode == Wait::Try ? CThreadStatus::Busy : CThreadStatus::Error;
      }
      if (value->depth == std::numeric_limits<std::uint64_t>::max()) {
        return CThreadStatus::Error;
      }
      ++value->depth;
      return CThreadStatus::Success;
    }
    // The gate orders user writes across final unlock and successful acquire.
    // A recursive acquisition/release never makes the mutex available early.
    if (mode == Wait::Try && value->depth != 0) {
      return CThreadStatus::Busy;
    }
    while (value->depth != 0) {
      if (mode == Wait::Timed) {
        // Check past deadlines before the host computes a duration difference,
        // including the minimum representable signed nanosecond timestamp.
        if (until <= std::chrono::system_clock::now()) {
          return CThreadStatus::TimedOut;
        }
        ++value->waiting;
        try {
          value->available.wait_until(lock, until);
          if (value->depth == 0 &&
              failWait.exchange(false, std::memory_order_relaxed)) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
          }
        } catch (...) {
          --value->waiting;
          // A failed waiter must not consume the only wakeup for a free mutex.
          if (value->depth == 0) {
            value->available.notify_one();
          }
          throw;
        }
        --value->waiting;
      } else {
        ++value->waiting;
        try {
          value->available.wait(lock);
          if (value->depth == 0 &&
              failWait.exchange(false, std::memory_order_relaxed)) {
            throw std::system_error(std::make_error_code(
                std::errc::resource_unavailable_try_again));
          }
        } catch (...) {
          --value->waiting;
          if (value->depth == 0) {
            value->available.notify_one();
          }
          throw;
        }
        --value->waiting;
      }
    }
    value->owner = current;
    value->depth = 1;
    return CThreadStatus::Success;
  } catch (...) {
    return CThreadStatus::Error;
  }
}

} // namespace

CThreadStatus initCMutex(std::uint64_t *output, std::int32_t flags) noexcept {
  if (output == nullptr || flags < 0 || flags > (recursiveFlag | timedFlag)) {
    return CThreadStatus::Error;
  }
  try {
    if (failAllocation.exchange(false, std::memory_order_relaxed)) {
      throw std::bad_alloc{};
    }
    auto value = std::make_shared<Mutex>(flags);
    auto &state = registry();
    const std::lock_guard lock(state.gate);
    if (state.next == 0) {
      return CThreadStatus::Error;
    }
    const auto handle = state.next;
    state.values.emplace(handle, std::move(value));
    state.next =
        handle == std::numeric_limits<std::uint64_t>::max() ? 0 : handle + 1;
    *output = handle;
    return CThreadStatus::Success;
  } catch (const std::bad_alloc &) {
    return CThreadStatus::NoMemory;
  } catch (...) {
    return CThreadStatus::Error;
  }
}

CThreadStatus destroyCMutex(std::uint64_t handle) noexcept {
  try {
    auto &state = registry();
    const std::lock_guard registryLock(state.gate);
    const auto found = state.values.find(handle);
    if (found == state.values.end()) {
      return CThreadStatus::Error;
    }
    // Registry exclusion prevents a new user. Existing calls retain a shared
    // reference, including while blocked; no native state is destroyed in use.
    if (found->second.use_count() != 1) {
      return CThreadStatus::Busy;
    }
    {
      const std::lock_guard lock(found->second->gate);
      if (found->second->depth != 0) {
        return CThreadStatus::Busy;
      }
    }
    state.values.erase(found);
    return CThreadStatus::Success;
  } catch (...) {
    return CThreadStatus::Error;
  }
}

CThreadStatus lockCMutex(std::uint64_t handle) noexcept {
  return acquire(handle, Wait::Block);
}
void failNextCMutexAllocationForTest() noexcept {
  failAllocation.store(true, std::memory_order_relaxed);
}
void failNextCMutexWaitForTest() noexcept {
  failWait.store(true, std::memory_order_relaxed);
}
std::uint64_t waitingCMutexCallsForTest(std::uint64_t handle) noexcept {
  const auto value = find(handle);
  if (!value) {
    return 0;
  }
  const std::lock_guard lock(value->gate);
  return value->waiting;
}
CThreadStatus tryLockCMutex(std::uint64_t handle) noexcept {
  return acquire(handle, Wait::Try);
}
CThreadStatus timedLockCMutex(std::uint64_t handle, std::int64_t seconds,
                              std::int64_t nanoseconds) noexcept {
  Deadline until;
  if (!deadline(seconds, nanoseconds, until)) {
    return CThreadStatus::Error;
  }
  return acquire(handle, Wait::Timed, until);
}
CThreadStatus unlockCMutex(std::uint64_t handle) noexcept {
  try {
    const auto value = find(handle);
    if (!value) {
      return CThreadStatus::Error;
    }
    const std::lock_guard lock(value->gate);
    if (value->depth == 0 || value->owner != std::this_thread::get_id()) {
      return CThreadStatus::Error;
    }
    if (--value->depth == 0) {
      value->owner = {};
      value->available.notify_one();
    }
    return CThreadStatus::Success;
  } catch (...) {
    return CThreadStatus::Error;
  }
}

CThreadStatus initCCondition(std::uint64_t *output) noexcept {
  if (output == nullptr) {
    return CThreadStatus::Error;
  }
  try {
    if (failConditionAllocation.exchange(false, std::memory_order_relaxed)) {
      throw std::bad_alloc{};
    }
    auto value = std::make_shared<Condition>();
    auto &state = conditionRegistry();
    const std::lock_guard lock(state.gate);
    if (state.next == 0) {
      return CThreadStatus::Error;
    }
    const auto handle = state.next;
    state.values.emplace(handle, std::move(value));
    state.next =
        handle == std::numeric_limits<std::uint64_t>::max() ? 0 : handle + 1;
    *output = handle;
    return CThreadStatus::Success;
  } catch (const std::bad_alloc &) {
    return CThreadStatus::NoMemory;
  } catch (...) {
    return CThreadStatus::Error;
  }
}

CThreadStatus destroyCCondition(std::uint64_t handle) noexcept {
  try {
    auto &state = conditionRegistry();
    const std::lock_guard lock(state.gate);
    const auto found = state.values.find(handle);
    if (found == state.values.end()) {
      return CThreadStatus::Error;
    }
    if (found->second.use_count() != 1) {
      return CThreadStatus::Busy;
    }
    state.values.erase(found);
    return CThreadStatus::Success;
  } catch (...) {
    return CThreadStatus::Error;
  }
}

CThreadStatus notifyCCondition(std::uint64_t handle, bool all) noexcept {
  try {
    const auto cv = findCondition(handle);
    if (!cv) {
      return CThreadStatus::Error;
    }
    const std::lock_guard lock(cv->gate);
    for (auto *waiter = cv->waiters; waiter != nullptr; waiter = waiter->next) {
      if (!waiter->notified) {
        waiter->notified = true;
        waiter->wake.notify_one();
        if (!all) {
          break;
        }
      }
    }
    return CThreadStatus::Success;
  } catch (...) {
    return CThreadStatus::Error;
  }
}

CThreadStatus waitCCondition(std::uint64_t condition,
                             std::uint64_t mutex) noexcept {
  return conditionWait(condition, mutex, false);
}
CThreadStatus timedWaitCCondition(std::uint64_t condition, std::uint64_t mutex,
                                  std::int64_t seconds,
                                  std::int64_t nanos) noexcept {
  Deadline until;
  if (!deadline(seconds, nanos, until)) {
    return CThreadStatus::Error;
  }
  return conditionWait(condition, mutex, true, until);
}
void failNextCConditionAllocationForTest() noexcept {
  failConditionAllocation.store(true, std::memory_order_relaxed);
}
void failNextCConditionWaitForTest() noexcept {
  failConditionWait.store(true, std::memory_order_relaxed);
}
std::uint64_t waitingCConditionCallsForTest(std::uint64_t handle) noexcept {
  const auto cv = findCondition(handle);
  if (!cv) {
    return 0;
  }
  const std::lock_guard lock(cv->gate);
  std::uint64_t count = 0;
  for (auto *waiter = cv->waiters; waiter != nullptr; waiter = waiter->next) {
    ++count;
  }
  return count;
}

} // namespace gti::runtime::detail

extern "C" int32_t gti_rt_mtx_init_v1(uint64_t *output, int32_t flags) {
  return static_cast<int32_t>(gti::runtime::detail::initCMutex(output, flags));
}
extern "C" void gti_rt_mtx_destroy_v1(uint64_t handle) {
  if (gti::runtime::detail::destroyCMutex(handle) !=
      gti::runtime::detail::CThreadStatus::Success) {
    std::terminate();
  }
}
extern "C" int32_t gti_rt_mtx_lock_v1(uint64_t handle) {
  return static_cast<int32_t>(gti::runtime::detail::lockCMutex(handle));
}
extern "C" int32_t gti_rt_mtx_trylock_v1(uint64_t handle) {
  return static_cast<int32_t>(gti::runtime::detail::tryLockCMutex(handle));
}
extern "C" int32_t gti_rt_mtx_timedlock_v1(uint64_t handle, int64_t seconds,
                                           int64_t nanoseconds) {
  return static_cast<int32_t>(
      gti::runtime::detail::timedLockCMutex(handle, seconds, nanoseconds));
}
extern "C" int32_t gti_rt_mtx_unlock_v1(uint64_t handle) {
  return static_cast<int32_t>(gti::runtime::detail::unlockCMutex(handle));
}

extern "C" int32_t gti_rt_cnd_init_v1(uint64_t *output) {
  return static_cast<int32_t>(gti::runtime::detail::initCCondition(output));
}
extern "C" void gti_rt_cnd_destroy_v1(uint64_t handle) {
  if (gti::runtime::detail::destroyCCondition(handle) !=
      gti::runtime::detail::CThreadStatus::Success) {
    std::terminate();
  }
}
extern "C" int32_t gti_rt_cnd_signal_v1(uint64_t handle) {
  return static_cast<int32_t>(
      gti::runtime::detail::notifyCCondition(handle, false));
}
extern "C" int32_t gti_rt_cnd_broadcast_v1(uint64_t handle) {
  return static_cast<int32_t>(
      gti::runtime::detail::notifyCCondition(handle, true));
}
extern "C" int32_t gti_rt_cnd_wait_v1(uint64_t condition, uint64_t mutex) {
  return static_cast<int32_t>(
      gti::runtime::detail::waitCCondition(condition, mutex));
}
extern "C" int32_t gti_rt_cnd_timedwait_v1(uint64_t condition, uint64_t mutex,
                                           int64_t seconds, int64_t nanos) {
  return static_cast<int32_t>(gti::runtime::detail::timedWaitCCondition(
      condition, mutex, seconds, nanos));
}
