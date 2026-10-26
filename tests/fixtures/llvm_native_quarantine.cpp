#include "gti/runtime_thread.h"
#include "../../runtime/src/thread_internal.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace {
struct Task {
  void (*observe)(void *);
  void *context;
  std::atomic<std::uint64_t> calls{0};
};
Task *task;
}

extern "C" void gti_test_quarantined_worker_progress() {
  const auto before = task->calls.load(std::memory_order_acquire);
  while (task->calls.load(std::memory_order_acquire) == before)
    std::this_thread::yield();
}

extern "C" void gti_test_quarantine_worker(void (*observe)(void *), void *context) {
  assert(task == nullptr && gti_rt_thread_program_storage_retirement_ready_v1());
  task = new Task{observe, context};
  auto launched = gti_rt_thread_spawn_v1(
      [](void *raw) {
        auto &task = *static_cast<Task *>(raw);
        for (;;) {
          task.observe(task.context);
          task.calls.fetch_add(1, std::memory_order_release);
          std::this_thread::yield();
        }
      }, task, [](void *) { std::abort(); });
  assert(launched.status == GTI_THREAD_SPAWN_OK_V1 && launched.state);
  gti_test_quarantined_worker_progress();
  gti::runtime::detail::setThreadTestFault(gti::runtime::detail::ThreadTestFault::JoinFailed);
  const auto joined = gti_rt_thread_join_v2(&launched.state);
  assert(joined.status == GTI_THREAD_JOIN_FAILED_V1 && launched.state);
  gti_rt_thread_quarantine_v1(&launched.state);
  assert(!launched.state && !gti_rt_thread_program_storage_retirement_ready_v1());
  gti_test_quarantined_worker_progress();
}
