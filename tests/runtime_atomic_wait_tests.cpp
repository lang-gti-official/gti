#include "gti/runtime_atomic_wait.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <thread>
#include <utility>

#ifdef GTI_WAIT_LLVM_NOTIFY
#define GTI_DECLARE_NOTIFY(Bits)                                               \
  extern "C" void gti_test_wait_u##Bits##_one(const void *);                   \
  extern "C" void gti_test_wait_u##Bits##_all(const void *);
GTI_DECLARE_NOTIFY(8)
GTI_DECLARE_NOTIFY(16)
GTI_DECLARE_NOTIFY(32)
GTI_DECLARE_NOTIFY(64)
#undef GTI_DECLARE_NOTIFY
#endif

namespace {
void require(bool condition) {
  if (!condition)
    std::abort();
}

void notify(const void *lane, std::uint32_t width, std::uint32_t all) {
#ifdef GTI_WAIT_LLVM_NOTIFY
  require(all <= 1);
  switch (width) {
  case 1:
    return all ? gti_test_wait_u8_all(lane) : gti_test_wait_u8_one(lane);
  case 2:
    return all ? gti_test_wait_u16_all(lane) : gti_test_wait_u16_one(lane);
  case 4:
    return all ? gti_test_wait_u32_all(lane) : gti_test_wait_u32_one(lane);
  case 8:
    return all ? gti_test_wait_u64_all(lane) : gti_test_wait_u64_one(lane);
  default:
    std::abort();
  }
#else
  gti_rt_atomic_notify_v1(lane, width, all);
#endif
}

struct Probe {
  std::atomic<unsigned> waits{0};
  std::atomic<bool> beforeLock{false};
  std::atomic<bool> resume{false};
  bool pause = false;
  bool spurious = false;
};
thread_local Probe *probe = nullptr;
std::atomic<unsigned> initializations{0};
const char *fault = "";
thread_local bool pauseBroadcast = false;
std::atomic<bool> broadcastEntered{false}, releaseBroadcast{false};

void await(Probe &state, unsigned count) {
  auto seen = state.waits.load();
  while (seen < count) {
    state.waits.wait(seen);
    seen = state.waits.load();
  }
}
void resume(Probe &state) {
  state.resume.store(true);
  state.resume.notify_all();
}

template <typename T> void scalar() {
  for (std::uint32_t order = 0; order != 3; ++order) {
    T lane = static_cast<T>(UINT64_C(1) << (sizeof(T) * 8 - 1));
    const T old = lane;
    const T next = static_cast<T>(old | 1);
    int published = 0;
    Probe state;
    std::thread worker([&] {
      probe = &state;
      gti_rt_atomic_wait_v1(&lane, old, sizeof(T), order);
      require(__atomic_load_n(&lane, __ATOMIC_ACQUIRE) == next);
      if (order != 0)
        require(published == 42);
    });
    await(state, 1);
    published = 42;
    __atomic_store_n(&lane, next, __ATOMIC_RELEASE);
    notify(&lane, sizeof(T), order % 2);
    worker.join();
  }
}

void beforeEnrollment(bool aba) {
  std::uint32_t lane = 0;
  Probe state;
  state.pause = true;
  std::thread worker([&] {
    probe = &state;
    gti_rt_atomic_wait_v1(&lane, 0, 4, 1);
  });
  state.beforeLock.wait(false);
  __atomic_store_n(&lane, 1U, __ATOMIC_RELEASE);
  notify(&lane, 4, 0);
  if (aba)
    __atomic_store_n(&lane, 0U, __ATOMIC_RELEASE);
  resume(state);
  if (aba) {
    await(state, 1);
    __atomic_store_n(&lane, 2U, __ATOMIC_RELEASE);
    notify(&lane, 4, 1);
  }
  worker.join();
  require(aba || state.waits.load() == 0);
}

void collisions() {
  // Same width and address difference 2048 select the same runtime bucket.
  alignas(8) std::array<std::uint64_t, 257> lanes{};
  std::array<Probe, 3> states;
  states[0].spurious = true;
  std::array<std::thread, 3> workers;
  for (unsigned index = 0; index != workers.size(); ++index)
    workers[index] = std::thread([&, index] {
      probe = &states[index];
      gti_rt_atomic_wait_v1(&lanes[index == 2 ? 256 : 0], 0, 8, 2);
    });
  for (unsigned index = 0; index != states.size(); ++index)
    await(states[index], index == 0 ? 2 : 1);
  notify(&lanes[0], 8, 0);
  for (unsigned index = 0; index != states.size(); ++index)
    await(states[index], index == 0 ? 3 : 2);
  __atomic_store_n(&lanes[0], UINT64_MAX, __ATOMIC_RELEASE);
  notify(&lanes[0], 8, 1);
  workers[0].join();
  workers[1].join();
  await(states[2], 3);
  __atomic_store_n(&lanes[256], UINT64_MAX, __ATOMIC_RELEASE);
  notify(&lanes[256], 8, 0);
  workers[2].join();
  gti_rt_atomic_wait_v1(&lanes[0], 0, 8, 2);
}

void parkedCpuBudget() {
  std::uint32_t lane = 0;
  Probe state;
  double cpu = 0;
  std::thread worker([&] {
    probe = &state;
    timespec start{}, finish{};
    require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start) == 0);
    gti_rt_atomic_wait_v1(&lane, 0, 4, 1);
    require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &finish) == 0);
    cpu = static_cast<double>(finish.tv_sec - start.tv_sec) +
          static_cast<double>(finish.tv_nsec - start.tv_nsec) / 1e9;
  });
  await(state, 1);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  __atomic_store_n(&lane, 1U, __ATOMIC_RELEASE);
  notify(&lane, 4, 0);
  worker.join();
  require(cpu < 0.025);
  std::cout << "parked_wait_thread_cpu_seconds=" << cpu
            << " minimum_park_seconds=0.1\n";
}

void coalescedNotifications() {
  alignas(8) std::array<std::uint64_t, 257> lanes{};
  std::array<Probe, 2> states;
  std::array<std::thread, 2> waiters;
  for (unsigned index = 0; index != 2; ++index)
    waiters[index] = std::thread([&, index] {
      probe = &states[index];
      gti_rt_atomic_wait_v1(&lanes[index * 256], 0, 8, 1);
      require(__atomic_load_n(&lanes[index * 256], __ATOMIC_ACQUIRE) == 1);
    });
  for (auto &state : states)
    await(state, 1);
  std::thread first([&] {
    pauseBroadcast = true;
    __atomic_store_n(&lanes[0], 1U, __ATOMIC_RELEASE);
    notify(&lanes[0], 8, 0);
  });
  broadcastEntered.wait(false);
  std::atomic<bool> published{false}, notified{false};
  std::thread second([&] {
    __atomic_store_n(&lanes[256], 1U, __ATOMIC_RELEASE);
    published.store(true);
    published.notify_all();
    notify(&lanes[256], 8, 0);
    notified.store(true);
    notified.notify_all();
  });
  published.wait(false);
#if defined(__x86_64__)
  // The first notifier still holds the bucket mutex after broadcasting. The
  // second publication must coalesce, and survive the first notifier's rearm.
  notified.wait(false);
#endif
  releaseBroadcast.store(true);
  releaseBroadcast.notify_all();
  first.join();
  second.join();
  for (auto &waiter : waiters)
    waiter.join();
}
} // namespace

// Only the separately compiled service test object substitutes these pthread
// calls. Production has no hook, callback or waiter allocation. Every unchanged
// lane exhausts its bounded pre-parking probe and reaches this real sleep.
extern "C" int gti_test_wait_init(pthread_mutex_t *mutex,
                                  const pthread_mutexattr_t *attributes) {
  const auto count = initializations.fetch_add(1);
  if (std::strcmp(fault, "init") == 0 && count == 3)
    return 12;
  return pthread_mutex_init(mutex, attributes);
}
extern "C" int gti_test_wait_lock(pthread_mutex_t *mutex) {
  if (std::strcmp(fault, "lock") == 0)
    return 22;
  if (probe && probe->pause) {
    probe->pause = false;
    probe->beforeLock.store(true);
    probe->beforeLock.notify_all();
    probe->resume.wait(false);
  }
  return pthread_mutex_lock(mutex);
}
extern "C" int gti_test_wait_sleep(pthread_cond_t *condition,
                                   pthread_mutex_t *mutex) {
  if (std::strcmp(fault, "wait") == 0)
    return 22;
  if (probe) {
    probe->waits.fetch_add(1);
    probe->waits.notify_all();
    if (std::exchange(probe->spurious, false))
      return 0;
  }
  return pthread_cond_wait(condition, mutex);
}

extern "C" int gti_test_wait_wake(pthread_cond_t *condition) {
  const int status = pthread_cond_broadcast(condition);
  if (pauseBroadcast) {
    broadcastEntered.store(true);
    broadcastEntered.notify_all();
    releaseBroadcast.wait(false);
  }
  return status;
}

int main(int argc, char **argv) {
  alignas(8) std::uint64_t lane = 1;
  if (argc > 1) {
    fault = argv[1];
    if (std::strcmp(fault, "null") == 0)
      gti_rt_atomic_wait_v1(nullptr, 0, 8, 0);
    else if (std::strcmp(fault, "width") == 0)
      gti_rt_atomic_wait_v1(&lane, 0, 3, 0);
    else if (std::strcmp(fault, "width-zero") == 0)
      gti_rt_atomic_wait_v1(&lane, 0, 0, 0);
    else if (std::strcmp(fault, "width-wide") == 0)
      gti_rt_atomic_wait_v1(&lane, 0, 16, 0);
    else if (std::strcmp(fault, "alignment-2") == 0)
      gti_rt_atomic_wait_v1(reinterpret_cast<char *>(&lane) + 1, 0, 2, 0);
    else if (std::strcmp(fault, "alignment-4") == 0)
      gti_rt_atomic_wait_v1(reinterpret_cast<char *>(&lane) + 1, 0, 4, 0);
    else if (std::strcmp(fault, "alignment") == 0)
      gti_rt_atomic_wait_v1(reinterpret_cast<char *>(&lane) + 1, 0, 8, 0);
    else if (std::strcmp(fault, "order") == 0)
      gti_rt_atomic_wait_v1(&lane, 0, 8, 3);
    else if (std::strcmp(fault, "bits") == 0)
      gti_rt_atomic_wait_v1(&lane, 256, 1, 0);
    else if (std::strcmp(fault, "notify") == 0)
      gti_rt_atomic_notify_v1(&lane, 8, 2);
    else if (std::strcmp(fault, "registered-null") == 0)
      gti_rt_atomic_notify_registered_v1(nullptr, 8, 0);
    else if (std::strcmp(fault, "registered-width") == 0)
      gti_rt_atomic_notify_registered_v1(&lane, 3, 0);
    else if (std::strcmp(fault, "registered-all") == 0)
      gti_rt_atomic_notify_registered_v1(&lane, 8, 2);
    else if (std::strcmp(fault, "late") == 0) {
      std::thread worker([&] {
        struct AtExit {
          std::uint64_t *lane;
          ~AtExit() { notify(lane, 8, 0); }
        };
        thread_local AtExit exit{&lane};
        (void)exit;
      });
      worker.join();
      require(initializations == 0);
      return 0;
    } else
      gti_rt_atomic_wait_v1(&lane, 1, 8, 0);
    return 1;
  }
  gti_rt_atomic_wait_v1(&lane, 0, 8, 0);
  require(initializations == 0);
  // Empty notifications need no native parking domain, including late use.
  std::array<std::thread, 8> starters;
  for (auto &worker : starters)
    worker = std::thread([&] { notify(&lane, 8, 0); });
  for (auto &worker : starters)
    worker.join();
  require(initializations == 0);
  std::array<Probe, 8> enrollments;
  for (unsigned index = 0; index != starters.size(); ++index)
    starters[index] = std::thread([&, index] {
      probe = &enrollments[index];
      gti_rt_atomic_wait_v1(&lane, 1, 8, 1);
    });
  for (auto &state : enrollments)
    await(state, 1);
  require(initializations == 256);
  __atomic_store_n(&lane, UINT64_C(2), __ATOMIC_RELEASE);
  notify(&lane, 8, 1);
  for (auto &worker : starters)
    worker.join();
  scalar<std::uint8_t>();
  scalar<std::uint16_t>();
  scalar<std::uint32_t>();
  scalar<std::uint64_t>();
  require(initializations == 256);
  beforeEnrollment(false);
  beforeEnrollment(true);
  collisions();
  coalescedNotifications();
  parkedCpuBudget();
  std::cout << "runtime wait: lanes/orders, publication, enrollment, ABA, "
               "spurious wakes, collisions, one/all and once passed\n";
}
