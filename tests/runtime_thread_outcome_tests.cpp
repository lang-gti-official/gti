#include "../runtime/src/thread_internal.h"
#include "gti/runtime_execution.h"
#include "gti/runtime_thread.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>

extern "C" int gti_test_thread_outcome_c(void);

namespace {
const gti_failure_artifact_descriptor_v1 firstArtifact{};
const gti_failure_artifact_descriptor_v1 secondArtifact{};

gti_failure_record_v2 record(uint32_t site) {
  gti_failure_record_v2 value{};
  value.abi_version = GTI_FAILURE_ABI_VERSION_V2;
  value.code = GTI_FAILURE_CODE_PANIC_V2;
  value.detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2;
  value.site_index = site;
  value.message_size = 4;
  std::memcpy(value.message, "keep", 4);
  return value;
}

void publish(uint32_t site,
             const gti_failure_artifact_descriptor_v1 *artifact) {
  auto value = record(site);
  const gti_thread_failure_view_v1 view{
      .abi_version = GTI_THREAD_FAILURE_VIEW_ABI_VERSION_V1,
      .failure_kind = GTI_THREAD_WORKER_FAILURE_ORDINARY_V1,
      .ordinary = &value,
      .cleanup = nullptr,
      .primary_artifact = artifact,
      .secondary_artifact = nullptr};
  gti_rt_thread_failure_publish_v1(&view);
  std::memset(&value, 0, sizeof(value));
}

struct Capture {
  unsigned calls = 0;
  gti_thread_worker_failure_kind_v1 kind = GTI_THREAD_WORKER_FAILURE_NONE_V1;
  gti_failure_record_v2 ordinary{};
  gti_failure_emergency_v2 cleanup{};
  const gti_failure_artifact_descriptor_v1 *primary = nullptr;
  const gti_failure_artifact_descriptor_v1 *secondary = nullptr;
};

void capture(void *raw, const gti_thread_failure_view_v1 *failure) {
  auto &output = *static_cast<Capture *>(raw);
  ++output.calls;
  output.kind = failure->failure_kind;
  output.primary = failure->primary_artifact;
  output.secondary = failure->secondary_artifact;
  if (failure->ordinary)
    output.ordinary = *failure->ordinary;
  if (failure->cleanup)
    output.cleanup = *failure->cleanup;
}

gti_thread_failure_sink_v1 sink() {
  gti_thread_failure_sink_v1 value{};
  value.abi_version = GTI_THREAD_FAILURE_SINK_ABI_VERSION_V1;
  return value;
}

void checkRecord(const gti_failure_record_v2 &value, uint32_t site) {
  assert(value.abi_version == GTI_FAILURE_ABI_VERSION_V2 &&
         value.site_index == site);
  assert(value.message_size == 4 && std::memcmp(value.message, "keep", 4) == 0);
}

struct Task {
  unsigned destroyed = 0;
  bool entryFails = false;
  bool cleanupFails = false;
  bool tlsFails = false;
  bool resultAbandonFails = false;
  bool noCompletedResult = false;
  bool takeFails = false;
  bool cleanupEnvelope = false;
};

void destroy(void *raw) {
  auto &task = *static_cast<Task *>(raw);
  ++task.destroyed;
  if (task.entryFails) {
    assert(gti_rt_thread_cleanup_failure_kind_v1() ==
           GTI_THREAD_WORKER_FAILURE_ORDINARY_V1);
    gti_failure_record_v2 primary{};
    const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
    assert(gti_rt_thread_cleanup_primary_copy_v2(&primary, &artifact) == 1);
    checkRecord(primary, 11);
    assert(artifact == &firstArtifact);
  }
  if (task.cleanupFails)
    publish(22, &secondArtifact);
  if (task.cleanupEnvelope) {
    gti_failure_emergency_v2 envelope{};
    envelope.abi_version = GTI_FAILURE_ABI_VERSION_V2;
    const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
    assert(gti_rt_thread_cleanup_primary_copy_v2(&envelope.primary, &artifact));
    envelope.secondary = record(22);
    const gti_thread_failure_view_v1 view{
        GTI_THREAD_FAILURE_VIEW_ABI_VERSION_V1,
        GTI_THREAD_WORKER_FAILURE_CLEANUP_V1,
        nullptr,
        &envelope,
        artifact,
        &secondArtifact};
    gti_rt_thread_failure_publish_v1(&view);
  }
}

bool tlsCleanup(void *, gti_failure_record_v2 *failure,
                const gti_failure_record_v2 *,
                const gti_failure_artifact_descriptor_v1 *) {
  *failure = record(33);
  return false;
}

void entry(void *raw) {
  auto &task = *static_cast<Task *>(raw);
  assert(gti_rt_thread_cleanup_failure_kind_v1() ==
         GTI_THREAD_WORKER_FAILURE_NONE_V1);
  if (task.tlsFails) {
    thread_local gti_thread_local_cleanup_entry_v1 registration{};
    registration.storage = &registration;
    registration.cleanup = tlsCleanup;
    registration.artifact = &firstArtifact;
    gti_rt_thread_locals_register_v1(&registration);
  }
  if (task.entryFails)
    publish(11, &firstArtifact);
}

struct Result {
  unsigned *retired;
  bool fail;
  int value;
  bool takeFails;
};
unsigned resultRetired = 0;

void *complete(void *raw) {
  auto &task = *static_cast<Task *>(raw);
  destroy(raw);
  if (task.noCompletedResult)
    return nullptr;
  return new Result{&resultRetired, task.resultAbandonFails, 42,
                    task.takeFails};
}

void take(void *raw, void *destination) {
  auto *result = static_cast<Result *>(raw);
  if (result->takeFails) {
    ++*result->retired;
    delete result;
    publish(55, &secondArtifact);
    return;
  }
  *static_cast<int *>(destination) = result->value;
  ++*result->retired;
  delete result;
}

void abandon(void *raw) {
  auto *result = static_cast<Result *>(raw);
  const bool fail = result->fail;
  ++*result->retired;
  delete result;
  if (fail)
    publish(44, &secondArtifact);
}

void callerScopes() {
  Capture outer, inner;
  auto a = sink(), b = sink();
  gti_rt_thread_failure_sink_enter_v1(&a, capture, &outer);
  publish(1, &firstArtifact);
  gti_rt_thread_failure_sink_enter_v1(&b, capture, &inner);
  publish(2, &secondArtifact);
  gti_rt_thread_failure_sink_leave_v1(&b);
  publish(3, &firstArtifact);
  gti_rt_thread_failure_sink_leave_v1(&a);
  assert(outer.calls == 2 && inner.calls == 1 && a.owner == nullptr &&
         b.owner == nullptr);
  checkRecord(outer.ordinary, 3);
  checkRecord(inner.ordinary, 2);
  auto unchanged = record(9), output = unchanged;
  const auto *artifact = &firstArtifact;
  assert(gti_rt_thread_cleanup_primary_copy_v2(&output, &artifact) == 0);
  assert(std::memcmp(&unchanged, &output, sizeof(output)) == 0 &&
         artifact == &firstArtifact);
}

void workers() {
  for (const int secondary : {0, 1, 2}) {
    Task task;
    task.entryFails = true;
    task.cleanupFails = secondary == 1;
    task.cleanupEnvelope = secondary == 2;
    auto launched = gti_rt_thread_spawn_v1(entry, &task, destroy);
    assert(launched.status == GTI_THREAD_SPAWN_OK_V1 && launched.state);
    const auto joined = gti_rt_thread_join_v2(&launched.state);
    assert(!launched.state && task.destroyed == 1 &&
           joined.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1);
    if (secondary) {
      assert(joined.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1);
      checkRecord(joined.cleanup_failure.primary, 11);
      checkRecord(joined.cleanup_failure.secondary, 22);
      assert(joined.secondary_artifact == &secondArtifact);
    } else {
      assert(joined.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1);
      checkRecord(joined.failure, 11);
    }
    assert(joined.primary_artifact == &firstArtifact);
  }
}

void launchRollback() {
  Capture failure;
  auto active = sink();
  gti_rt_thread_failure_sink_enter_v1(&active, capture, &failure);
  Task task;
  task.cleanupFails = true;
  gti::runtime::detail::setThreadTestFault(
      gti::runtime::detail::ThreadTestFault::SpawnResourceUnavailable);
  const auto launched = gti_rt_thread_spawn_v1(entry, &task, destroy);
  assert(launched.status == GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1 &&
         !launched.state);
  assert(task.destroyed == 1 && failure.calls == 1);
  checkRecord(failure.ordinary, 22);
  gti_rt_thread_failure_sink_leave_v1(&active);
}

void results() {
  {
    Task task;
    task.cleanupFails = true;
    task.noCompletedResult = true;
    auto launched = gti_rt_thread_spawn_value_v1(entry, &task, complete,
                                                 destroy, take, abandon);
    const auto joined = gti_rt_thread_join_value_v2(&launched.state);
    assert(!launched.state && task.destroyed == 1 &&
           joined.join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
           joined.join.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1);
    checkRecord(joined.join.failure, 22);
    assert(!joined.result_context && !joined.take_result &&
           !joined.abandon_result);
  }
  for (const bool discard : {false, true}) {
    Task task;
    task.resultAbandonFails = discard;
    auto launched = gti_rt_thread_spawn_value_v1(entry, &task, complete,
                                                 destroy, take, abandon);
    assert(launched.status == GTI_THREAD_SPAWN_OK_V1);
    auto joined = gti_rt_thread_join_value_v2(&launched.state);
    assert(!launched.state && joined.join.status == GTI_THREAD_JOIN_OK_V1 &&
           task.destroyed == 1);
    Capture failure;
    auto active = sink();
    gti_rt_thread_failure_sink_enter_v1(&active, capture, &failure);
    if (discard) {
      joined.abandon_result(joined.result_context);
      checkRecord(failure.ordinary, 44);
      assert(failure.primary == &secondArtifact);
    } else {
      int output = 0;
      joined.take_result(joined.result_context, &output);
      assert(output == 42 && failure.calls == 0);
    }
    gti_rt_thread_failure_sink_leave_v1(&active);
  }
  assert(resultRetired == 2);
  Task task;
  task.tlsFails = true;
  task.resultAbandonFails = true;
  auto launched = gti_rt_thread_spawn_value_v1(entry, &task, complete, destroy,
                                               take, abandon);
  const auto joined = gti_rt_thread_join_value_v2(&launched.state);
  assert(!launched.state &&
         joined.join.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1);
  assert(!joined.result_context && !joined.take_result &&
         !joined.abandon_result);
  checkRecord(joined.join.cleanup_failure.primary, 33);
  checkRecord(joined.join.cleanup_failure.secondary, 44);
  assert(resultRetired == 3 && task.destroyed == 1);
  {
    Task failedTake;
    failedTake.takeFails = true;
    auto started = gti_rt_thread_spawn_value_v1(entry, &failedTake, complete,
                                                destroy, take, abandon);
    auto result = gti_rt_thread_join_value_v2(&started.state);
    assert(result.join.status == GTI_THREAD_JOIN_OK_V1 && !started.state);
    Capture failure;
    auto active = sink();
    gti_rt_thread_failure_sink_enter_v1(&active, capture, &failure);
    int destination = -1;
    result.take_result(result.result_context, &destination);
    gti_rt_thread_failure_sink_leave_v1(&active);
    checkRecord(failure.ordinary, 55);
    assert(destination == -1 && resultRetired == 4 &&
           failedTake.destroyed == 1);
  }
}

void nestedCaller(void *) {
  launchRollback();
  assert(gti_rt_thread_cleanup_failure_kind_v1() ==
         GTI_THREAD_WORKER_FAILURE_NONE_V1);
}

void noop(void *) {}

void quarantine() {
  assert(gti_rt_thread_program_storage_retirement_ready_v1() == 1);
  struct TaskState {
    std::atomic<bool> started{false}, finish{false}, destroyed{false};
  };
  auto *task = new TaskState;
  auto launched = gti_rt_thread_spawn_v1(
      [](void *raw) {
        auto &task = *static_cast<TaskState *>(raw);
        task.started.store(true, std::memory_order_release);
        while (!task.finish.load(std::memory_order_acquire))
          std::this_thread::yield();
      },
      task,
      [](void *raw) {
        static_cast<TaskState *>(raw)->destroyed.store(
            true, std::memory_order_release);
      });
  assert(launched.status == GTI_THREAD_SPAWN_OK_V1 && launched.state);
  while (!task->started.load(std::memory_order_acquire))
    std::this_thread::yield();
  gti::runtime::detail::setThreadTestFault(
      gti::runtime::detail::ThreadTestFault::JoinFailed);
  const auto joined = gti_rt_thread_join_v2(&launched.state);
  assert(joined.status == GTI_THREAD_JOIN_FAILED_V1 && launched.state);
  assert(gti_rt_thread_program_storage_retirement_ready_v1() == 1);
  gti_rt_thread_quarantine_v1(&launched.state);
  assert(!launched.state &&
         gti_rt_thread_program_storage_retirement_ready_v1() == 0);
  task->finish.store(true, std::memory_order_release);
  while (!task->destroyed.load(std::memory_order_acquire))
    std::this_thread::yield();
  assert(gti_rt_thread_program_storage_retirement_ready_v1() == 0);
  // Terminal quarantine intentionally retains state, even after worker cleanup.
  std::_Exit(0);
}

void costs() {
  using Clock = std::chrono::steady_clock;
  constexpr unsigned iterations = 500000;
  std::array<double, 9> samples{};
  auto active = sink();
  Capture output;
  for (auto &sample : samples) {
    const auto start = Clock::now();
    for (unsigned index = 0; index != iterations; ++index) {
      gti_rt_thread_failure_sink_enter_v1(&active, capture, &output);
      gti_rt_thread_failure_sink_leave_v1(&active);
    }
    sample =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count() /
        iterations;
  }
  assert(output.calls == 0 && active.owner == nullptr);
  std::sort(samples.begin(), samples.end());
  std::cout << "caller_sink_enter_leave median_ns=" << samples[4]
            << " min_ns=" << samples.front() << " max_ns=" << samples.back()
            << " iterations=" << iterations << " samples=" << samples.size()
            << " descriptor_bytes=" << sizeof(active)
            << " sink_owned_record_bytes=0\n";
}

void invalid(std::string_view mode) {
  if (mode == "null-quarantine") {
    gti_thread_state_v1 *state = nullptr;
    gti_rt_thread_quarantine_v1(&state);
    return;
  }
  if (mode == "wrong-primary-envelope" || mode == "third-failure") {
    Task task;
    task.entryFails = true;
    const auto cleanup = mode == "third-failure"
        ? +[](void *) { publish(22, &secondArtifact); publish(33, &secondArtifact); }
        : +[](void *) {
            gti_failure_emergency_v2 envelope{};
            envelope.abi_version = GTI_FAILURE_ABI_VERSION_V2;
            const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
            assert(gti_rt_thread_cleanup_primary_copy_v2(&envelope.primary, &artifact));
            envelope.primary.site_index = 999;
            envelope.secondary = record(22);
            const gti_thread_failure_view_v1 view{
                GTI_THREAD_FAILURE_VIEW_ABI_VERSION_V1,
                GTI_THREAD_WORKER_FAILURE_CLEANUP_V1, nullptr, &envelope,
                artifact, &secondArtifact};
            gti_rt_thread_failure_publish_v1(&view);
          };
    auto started = gti_rt_thread_spawn_v1(entry, &task, cleanup);
    static_cast<void>(gti_rt_thread_join_v2(&started.state));
    return;
  }
  if (mode == "successful-null-completion") {
    Task task;
    task.noCompletedResult = true;
    auto launched = gti_rt_thread_spawn_value_v1(entry, &task, complete,
                                                 destroy, take, abandon);
    static_cast<void>(gti_rt_thread_join_value_v2(&launched.state));
    return;
  }
  if (mode == "no-sink") {
    publish(1, &firstArtifact);
    return;
  }
  Capture output;
  auto a = sink(), b = sink();
  gti_rt_thread_failure_sink_enter_v1(&a, capture, &output);
  if (mode == "wrong-thread") {
    std::thread worker([&] { gti_rt_thread_failure_sink_leave_v1(&a); });
    worker.join();
  } else if (mode == "out-of-order") {
    gti_rt_thread_failure_sink_enter_v1(&b, capture, &output);
    gti_rt_thread_failure_sink_leave_v1(&a);
  } else if (mode == "duplicate-enter") {
    gti_rt_thread_failure_sink_enter_v1(&a, capture, &output);
  } else if (mode == "bad-sink-version") {
    a.abi_version = 99;
    publish(1, &firstArtifact);
  } else if (mode == "mutating-sink") {
    gti_rt_thread_failure_sink_leave_v1(&a);
    gti_rt_thread_failure_sink_enter_v1(
        &a,
        [](void *raw, const gti_thread_failure_view_v1 *) {
          static_cast<gti_thread_failure_sink_v1 *>(raw)->context = nullptr;
        },
        &a);
    publish(1, &firstArtifact);
  } else if (mode == "throwing-sink" || mode == "reentrant") {
    gti_rt_thread_failure_sink_leave_v1(&a);
    bool throwing = mode == "throwing-sink";
    gti_rt_thread_failure_sink_enter_v1(
        &a,
        [](void *raw, const gti_thread_failure_view_v1 *) {
          if (*static_cast<bool *>(raw))
            throw 1;
          publish(2, &firstArtifact);
        },
        &throwing);
    publish(1, &firstArtifact);
  } else {
    auto value = record(1);
    value.message_size = GTI_FAILURE_MESSAGE_CAPACITY_V2 + 1;
    const gti_thread_failure_view_v1 view{
        GTI_THREAD_FAILURE_VIEW_ABI_VERSION_V1,
        GTI_THREAD_WORKER_FAILURE_ORDINARY_V1,
        &value,
        nullptr,
        &firstArtifact,
        nullptr};
    gti_rt_thread_failure_publish_v1(&view);
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string_view(argv[1]) == "quarantine") {
    quarantine();
    return 99;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--cost") {
    costs();
    return 0;
  }
  if (argc == 2) {
    invalid(argv[1]);
    return 99;
  }
  assert(gti_test_thread_outcome_c());
  callerScopes();
  workers();
  launchRollback();
  results();
  int context = 0;
  auto launched = gti_rt_thread_spawn_v1(nestedCaller, &context, noop);
  const auto joined = gti_rt_thread_join_v2(&launched.state);
  assert(joined.status == GTI_THREAD_JOIN_OK_V1 && !launched.state);
  assert(gti_rt_thread_program_storage_retirement_ready_v1() == 1);
}
