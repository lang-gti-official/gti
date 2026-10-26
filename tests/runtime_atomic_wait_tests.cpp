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

namespace {
void require(bool condition) {
  if (!condition)
    std::abort();
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
    gti_rt_atomic_notify_v1(&lane, sizeof(T), order % 2);
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
  gti_rt_atomic_notify_v1(&lane, 4, 0);
  if (aba)
    __atomic_store_n(&lane, 0U, __ATOMIC_RELEASE);
  resume(state);
  if (aba) {
    await(state, 1);
    __atomic_store_n(&lane, 2U, __ATOMIC_RELEASE);
    gti_rt_atomic_notify_v1(&lane, 4, 1);
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
  gti_rt_atomic_notify_v1(&lanes[0], 8, 0);
  for (unsigned index = 0; index != states.size(); ++index)
    await(states[index], index == 0 ? 3 : 2);
  __atomic_store_n(&lanes[0], UINT64_MAX, __ATOMIC_RELEASE);
  gti_rt_atomic_notify_v1(&lanes[0], 8, 1);
  workers[0].join();
  workers[1].join();
  await(states[2], 3);
  __atomic_store_n(&lanes[256], UINT64_MAX, __ATOMIC_RELEASE);
  gti_rt_atomic_notify_v1(&lanes[256], 8, 0);
  workers[2].join();
  gti_rt_atomic_wait_v1(&lanes[0], 0, 8, 2);
}
} // namespace

// Only the separately compiled service test object substitutes these pthread
// calls. Production has no hook, callback, waiter allocation or polling loop.
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

int main(int argc, char **argv) {
  alignas(8) std::uint64_t lane = 1;
  if (argc > 1) {
    fault = argv[1];
    if (std::strcmp(fault, "null") == 0)
      gti_rt_atomic_wait_v1(nullptr, 0, 8, 0);
    else if (std::strcmp(fault, "width") == 0)
      gti_rt_atomic_wait_v1(&lane, 0, 3, 0);
    else if (std::strcmp(fault, "alignment") == 0)
      gti_rt_atomic_wait_v1(reinterpret_cast<char *>(&lane) + 1, 0, 8, 0);
    else if (std::strcmp(fault, "order") == 0)
      gti_rt_atomic_wait_v1(&lane, 0, 8, 3);
    else if (std::strcmp(fault, "bits") == 0)
      gti_rt_atomic_wait_v1(&lane, 256, 1, 0);
    else if (std::strcmp(fault, "notify") == 0)
      gti_rt_atomic_notify_v1(&lane, 8, 2);
    else if (std::strcmp(fault, "late") == 0) {
      std::thread worker([&] {
        struct AtExit {
          std::uint64_t *lane;
          ~AtExit() { gti_rt_atomic_notify_v1(lane, 8, 0); }
        };
        thread_local AtExit exit{&lane};
        (void)exit;
      });
      worker.join();
      require(initializations == 256);
      return 0;
    } else
      gti_rt_atomic_wait_v1(&lane, 1, 8, 0);
    return 1;
  }
  gti_rt_atomic_wait_v1(&lane, 0, 8, 0);
  require(initializations == 0);
  // Concurrent first use must publish exactly one initialized table.
  std::array<std::thread, 8> starters;
  for (auto &worker : starters)
    worker = std::thread([&] { gti_rt_atomic_notify_v1(&lane, 8, 0); });
  for (auto &worker : starters)
    worker.join();
  require(initializations == 256);
  scalar<std::uint8_t>();
  scalar<std::uint16_t>();
  scalar<std::uint32_t>();
  scalar<std::uint64_t>();
  beforeEnrollment(false);
  beforeEnrollment(true);
  collisions();
  std::cout << "runtime wait: lanes/orders, publication, enrollment, ABA, "
               "spurious wakes, collisions, one/all and once passed\n";
}
