#include "gti/runtime_thread.h"
#include "gti/runtime_thread.hpp"

#include "thread_internal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <type_traits>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct TaskContext {
  std::atomic<int> *entries = nullptr;
  std::atomic<int> *destructions = nullptr;
};

void runTask(void *opaque) {
  auto &context = *static_cast<TaskContext *>(opaque);
  context.entries->fetch_add(1, std::memory_order_relaxed);
}

void destroyTask(void *opaque) {
  auto *context = static_cast<TaskContext *>(opaque);
  context->destructions->fetch_add(1, std::memory_order_relaxed);
  delete context;
}

TaskContext *newTask(std::atomic<int> &entries,
                     std::atomic<int> &destructions) {
  return new TaskContext{.entries = &entries, .destructions = &destructions};
}

void testSpawnJoinAndConsumption() {
  std::atomic<int> entries{0};
  std::atomic<int> destructions{0};
  gti_thread_spawn_result_v1 spawn = gti_rt_thread_spawn_v1(
      runTask, newTask(entries, destructions), destroyTask);
  expect(spawn.abi_version == GTI_THREAD_ABI_VERSION_V1 &&
             spawn.status == GTI_THREAD_SPAWN_OK_V1 && spawn.state != nullptr,
         "spawn should publish one opaque joinable state");

  gti_thread_join_result_v1 join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.abi_version == GTI_THREAD_ABI_VERSION_V1 &&
             join.status == GTI_THREAD_JOIN_OK_V1 &&
             join.failure_kind == GTI_THREAD_WORKER_FAILURE_NONE_V1 &&
             spawn.state == nullptr,
         "join should consume and clear the opaque state");
  expect(entries.load(std::memory_order_relaxed) == 1 &&
             destructions.load(std::memory_order_relaxed) == 1,
         "a successful task and its context should each run exactly once");

  join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.status == GTI_THREAD_JOIN_NOT_JOINABLE_V1,
         "joining a consumed state should report not_joinable");
}

struct PublicationContext {
  int *published = nullptr;
};

void publishTask(void *opaque) {
  *static_cast<PublicationContext *>(opaque)->published = 42;
}

void destroyPublicationTask(void *opaque) {
  delete static_cast<PublicationContext *>(opaque);
}

void testJoinPublishesWorkerWrites() {
  int published = 0;
  gti_thread_spawn_result_v1 spawn = gti_rt_thread_spawn_v1(
      publishTask, new PublicationContext{.published = &published},
      destroyPublicationTask);
  expect(spawn.status == GTI_THREAD_SPAWN_OK_V1,
         "publication setup should spawn successfully");
  const gti_thread_join_result_v1 join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.status == GTI_THREAD_JOIN_OK_V1 && published == 42,
         "successful join should publish ordinary worker writes");
}

struct ValueCounters {
  std::atomic<int> entries{0};
  std::atomic<int> taskCompletions{0};
  std::atomic<int> taskAbandons{0};
  std::atomic<int> resultTakes{0};
  std::atomic<int> resultAbandons{0};
};

struct ValueTaskContext {
  ValueCounters *counters = nullptr;
  int value = 0;
};

struct ValueResultContext {
  ValueCounters *counters = nullptr;
  int value = 0;
};

void runValueTask(void *opaque) {
  auto &context = *static_cast<ValueTaskContext *>(opaque);
  context.counters->entries.fetch_add(1, std::memory_order_relaxed);
  context.value *= 2;
}

void *completeValueTask(void *opaque) {
  auto *task = static_cast<ValueTaskContext *>(opaque);
  task->counters->taskCompletions.fetch_add(1, std::memory_order_relaxed);
  auto *result =
      new ValueResultContext{.counters = task->counters, .value = task->value};
  delete task;
  return result;
}

void abandonValueTask(void *opaque) {
  auto *task = static_cast<ValueTaskContext *>(opaque);
  task->counters->taskAbandons.fetch_add(1, std::memory_order_relaxed);
  delete task;
}

void takeValueResult(void *opaque, void *destination) {
  auto *result = static_cast<ValueResultContext *>(opaque);
  *static_cast<int *>(destination) = result->value;
  result->counters->resultTakes.fetch_add(1, std::memory_order_relaxed);
  delete result;
}

void abandonValueResult(void *opaque) {
  auto *result = static_cast<ValueResultContext *>(opaque);
  result->counters->resultAbandons.fetch_add(1, std::memory_order_relaxed);
  delete result;
}

gti_thread_spawn_result_v1 spawnValueTask(ValueCounters &counters, int value) {
  return gti_rt_thread_spawn_value_v1(
      runValueTask, new ValueTaskContext{.counters = &counters, .value = value},
      completeValueTask, abandonValueTask, takeValueResult, abandonValueResult);
}

void testValueJoinTakeAndAbandon() {
  ValueCounters counters;
  gti_thread_spawn_result_v1 spawn = spawnValueTask(counters, 21);
  expect(spawn.status == GTI_THREAD_SPAWN_OK_V1 && spawn.state != nullptr,
         "value spawn should publish one opaque joinable state");
  gti_thread_value_join_result_v1 join =
      gti_rt_thread_join_value_v1(&spawn.state);
  expect(join.join.status == GTI_THREAD_JOIN_OK_V1 && spawn.state == nullptr &&
             join.result_context != nullptr && join.take_result != nullptr &&
             join.abandon_result != nullptr,
         "value join should transfer one exact result owner");
  int value = 0;
  join.take_result(join.result_context, &value);
  expect(value == 42 && counters.entries.load() == 1 &&
             counters.taskCompletions.load() == 1 &&
             counters.taskAbandons.load() == 0 &&
             counters.resultTakes.load() == 1 &&
             counters.resultAbandons.load() == 0,
         "taking a value result should consume task and result owners once");

  spawn = spawnValueTask(counters, 7);
  join = gti_rt_thread_join_value_v1(&spawn.state);
  expect(join.join.status == GTI_THREAD_JOIN_OK_V1 &&
             join.abandon_result != nullptr,
         "a second value result should remain explicitly abandonable");
  join.abandon_result(join.result_context);
  expect(counters.entries.load() == 2 && counters.taskCompletions.load() == 2 &&
             counters.resultTakes.load() == 1 &&
             counters.resultAbandons.load() == 1,
         "abandoning an unobserved result should consume it exactly once");
}

void testValueSpawnAndJoinFailuresPreserveOwnership() {
  using gti::runtime::detail::setThreadTestFault;
  using gti::runtime::detail::ThreadTestFault;

  ValueCounters counters;
  setThreadTestFault(ThreadTestFault::SpawnResourceUnavailable);
  gti_thread_spawn_result_v1 spawn = spawnValueTask(counters, 1);
  expect(spawn.status == GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1 &&
             spawn.state == nullptr && counters.entries.load() == 0 &&
             counters.taskCompletions.load() == 0 &&
             counters.taskAbandons.load() == 1,
         "value spawn failure should abandon transferred task state once");

  spawn = spawnValueTask(counters, 5);
  setThreadTestFault(ThreadTestFault::JoinFailed);
  gti_thread_value_join_result_v1 join =
      gti_rt_thread_join_value_v1(&spawn.state);
  expect(join.join.status == GTI_THREAD_JOIN_FAILED_V1 &&
             spawn.state != nullptr && join.result_context == nullptr &&
             join.take_result == nullptr && join.abandon_result == nullptr,
         "recoverable value join failure should preserve all runtime state");
  join = gti_rt_thread_join_value_v1(&spawn.state);
  expect(join.join.status == GTI_THREAD_JOIN_OK_V1 && spawn.state == nullptr &&
             join.result_context != nullptr,
         "retrying value join should transfer the original result owner");
  join.abandon_result(join.result_context);
  expect(counters.entries.load() == 1 && counters.taskCompletions.load() == 1 &&
             counters.taskAbandons.load() == 1 &&
             counters.resultAbandons.load() == 1,
         "value join retry should preserve exact cleanup counts");
}

void testCreationFailureDestroysExactlyOnce() {
  using gti::runtime::detail::setThreadTestFault;
  using gti::runtime::detail::ThreadTestFault;

  for (const auto &[fault, expected] : {
           std::pair{ThreadTestFault::SpawnResourceUnavailable,
                     GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1},
           std::pair{ThreadTestFault::SpawnOperationNotPermitted,
                     GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1},
       }) {
    std::atomic<int> entries{0};
    std::atomic<int> destructions{0};
    setThreadTestFault(fault);
    const gti_thread_spawn_result_v1 spawn = gti_rt_thread_spawn_v1(
        runTask, newTask(entries, destructions), destroyTask);
    expect(
        spawn.status == expected && spawn.state == nullptr,
        "an injected native creation failure should return its exact status");
    expect(
        entries.load(std::memory_order_relaxed) == 0 &&
            destructions.load(std::memory_order_relaxed) == 1,
        "creation failure should destroy transferred task state exactly once");
  }
}

void testRecoverableJoinFailurePreservesOwnership() {
  using gti::runtime::detail::setThreadTestFault;
  using gti::runtime::detail::ThreadTestFault;

  std::atomic<int> entries{0};
  std::atomic<int> destructions{0};
  gti_thread_spawn_result_v1 spawn = gti_rt_thread_spawn_v1(
      runTask, newTask(entries, destructions), destroyTask);
  expect(spawn.status == GTI_THREAD_SPAWN_OK_V1,
         "join-failure setup should spawn successfully");

  setThreadTestFault(ThreadTestFault::JoinFailed);
  gti_thread_join_result_v1 join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.status == GTI_THREAD_JOIN_FAILED_V1 && spawn.state != nullptr,
         "a recoverable join failure should preserve the join obligation");
  join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.status == GTI_THREAD_JOIN_OK_V1 && spawn.state == nullptr &&
             entries.load(std::memory_order_relaxed) == 1 &&
             destructions.load(std::memory_order_relaxed) == 1,
         "retrying a recoverable join should consume the original state once");
}

struct FailureContext {
  std::atomic<int> *observers = nullptr;
  std::atomic<int> *destructions = nullptr;
  gti_failure_record_v1 record{};
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
};

void observeFailure(const gti_failure_record_v1 *, void *opaque) {
  static_cast<std::atomic<int> *>(opaque)->fetch_add(1,
                                                     std::memory_order_relaxed);
}

void runFailingTask(void *opaque) {
  auto &context = *static_cast<FailureContext *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v1(&context.record, context.artifact,
                                observeFailure, context.observers);
  });
}

void destroyFailingTask(void *opaque) {
  auto *context = static_cast<FailureContext *>(opaque);
  context->destructions->fetch_add(1, std::memory_order_relaxed);
  delete context;
}

void testWorkerFailureHandoff() {
  std::atomic<int> observers{0};
  std::atomic<int> destructions{0};
  const gti_failure_record_v1 original{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
      .detail = GTI_FAILURE_DETAIL_ADDITION_V1,
      .site_index = 0,
      .reserved = 0,
      .artifact_identity = {},
  };
  const gti_failure_artifact_descriptor_v1 artifact{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .reserved = 0,
      .artifact_identity = {},
      .sites = nullptr,
      .site_count = 0,
      .sites_reserved = 0,
      .canonical_descriptor = nullptr,
      .canonical_descriptor_size = 0,
  };
  auto *context = new FailureContext{.observers = &observers,
                                     .destructions = &destructions,
                                     .record = original,
                                     .artifact = &artifact};
  gti_thread_spawn_result_v1 spawn =
      gti_rt_thread_spawn_v1(runFailingTask, context, destroyFailingTask);
  expect(spawn.status == GTI_THREAD_SPAWN_OK_V1,
         "worker-failure setup should spawn successfully");
  const gti_thread_join_result_v1 join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
             join.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1 &&
             std::memcmp(&join.failure, &original, sizeof(original)) == 0 &&
             join.primary_artifact == &artifact &&
             join.secondary_artifact == nullptr,
         "join should return the worker's exact fixed failure record");
  expect(observers.load(std::memory_order_relaxed) == 0,
         "a worker must not invoke the hosted failure observer");
  expect(destructions.load(std::memory_order_relaxed) == 1,
         "worker failure should still destroy task-owned state exactly once");
}

struct CleanupFailureContext {
  std::atomic<int> *destructions = nullptr;
  gti_failure_emergency_v1 failure{};
  const gti_failure_artifact_descriptor_v1 *primaryArtifact = nullptr;
  const gti_failure_artifact_descriptor_v1 *secondaryArtifact = nullptr;
};

void runCleanupFailingTask(void *opaque) {
  auto &context = *static_cast<CleanupFailureContext *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_cleanup_v1(
        &context.failure, context.primaryArtifact, context.secondaryArtifact);
  });
}

void destroyCleanupFailingTask(void *opaque) {
  auto *context = static_cast<CleanupFailureContext *>(opaque);
  context->destructions->fetch_add(1, std::memory_order_relaxed);
  delete context;
}

void testWorkerCleanupFailureHandoff() {
  std::atomic<int> destructions{0};
  const gti_failure_record_v1 primary{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .code = GTI_FAILURE_CODE_MODULO_BY_ZERO_V1,
      .detail = GTI_FAILURE_DETAIL_INTEGER_MODULO_V1,
      .site_index = 1,
      .reserved = 0,
      .artifact_identity = {1},
  };
  const gti_failure_record_v1 secondary{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .code = GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1,
      .detail = GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1,
      .site_index = 2,
      .reserved = 0,
      .artifact_identity = {2},
  };
  const gti_failure_emergency_v1 original{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .reserved = 0,
      .primary = primary,
      .secondary = secondary,
  };
  const gti_failure_artifact_descriptor_v1 primaryArtifact{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
  };
  const gti_failure_artifact_descriptor_v1 secondaryArtifact{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
  };
  auto *context = new CleanupFailureContext{
      .destructions = &destructions,
      .failure = original,
      .primaryArtifact = &primaryArtifact,
      .secondaryArtifact = &secondaryArtifact,
  };
  gti_thread_spawn_result_v1 spawn = gti_rt_thread_spawn_v1(
      runCleanupFailingTask, context, destroyCleanupFailingTask);
  expect(spawn.status == GTI_THREAD_SPAWN_OK_V1,
         "cleanup-failure setup should spawn successfully");
  const gti_thread_join_result_v1 join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
             join.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 &&
             std::memcmp(&join.cleanup_failure, &original, sizeof(original)) ==
                 0 &&
             join.primary_artifact == &primaryArtifact &&
             join.secondary_artifact == &secondaryArtifact,
         "join should return the worker's exact cleanup emergency envelope");
  expect(destructions.load(std::memory_order_relaxed) == 1,
         "cleanup emergency handoff should destroy task state exactly once");
}

struct TaskCleanupFailureContext {
  std::atomic<int> *observers = nullptr;
  std::atomic<int> *destructions = nullptr;
  gti_failure_record_v1 primary{};
  gti_failure_record_v1 secondary{};
  const gti_failure_artifact_descriptor_v1 *primaryArtifact = nullptr;
  const gti_failure_artifact_descriptor_v1 *secondaryArtifact = nullptr;
};

void runTaskThenFail(void *opaque) {
  auto &context = *static_cast<TaskCleanupFailureContext *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v1(&context.primary, context.primaryArtifact,
                                observeFailure, context.observers);
  });
}

void destroyTaskWithFailure(void *opaque) {
  auto *context = static_cast<TaskCleanupFailureContext *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v1(&context->secondary, context->secondaryArtifact,
                                observeFailure, context->observers);
  });
  context->destructions->fetch_add(1, std::memory_order_relaxed);
  delete context;
}

void testTaskObjectCleanupFailureHandoff() {
  std::atomic<int> observers{0};
  std::atomic<int> destructions{0};
  const gti_failure_record_v1 primary{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .code = GTI_FAILURE_CODE_MODULO_BY_ZERO_V1,
      .detail = GTI_FAILURE_DETAIL_INTEGER_MODULO_V1,
      .site_index = 1,
      .reserved = 0,
      .artifact_identity = {1},
  };
  const gti_failure_record_v1 secondary{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .code = GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1,
      .detail = GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1,
      .site_index = 2,
      .reserved = 0,
      .artifact_identity = {2},
  };
  const gti_failure_artifact_descriptor_v1 primaryArtifact{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
  };
  const gti_failure_artifact_descriptor_v1 secondaryArtifact{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
  };
  auto *context = new TaskCleanupFailureContext{
      .observers = &observers,
      .destructions = &destructions,
      .primary = primary,
      .secondary = secondary,
      .primaryArtifact = &primaryArtifact,
      .secondaryArtifact = &secondaryArtifact,
  };
  gti_thread_spawn_result_v1 spawn =
      gti_rt_thread_spawn_v1(runTaskThenFail, context, destroyTaskWithFailure);
  expect(spawn.status == GTI_THREAD_SPAWN_OK_V1,
         "task-cleanup failure setup should spawn successfully");
  const gti_thread_join_result_v1 join = gti_rt_thread_join_v1(&spawn.state);
  expect(join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
             join.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 &&
             std::memcmp(&join.cleanup_failure.primary, &primary,
                         sizeof(primary)) == 0 &&
             std::memcmp(&join.cleanup_failure.secondary, &secondary,
                         sizeof(secondary)) == 0 &&
             join.primary_artifact == &primaryArtifact &&
             join.secondary_artifact == &secondaryArtifact,
         "task-object cleanup should upgrade the worker result to the exact "
         "cleanup emergency envelope");
  expect(observers.load(std::memory_order_relaxed) == 0,
         "task cleanup must not invoke the hosted failure observer");
  expect(destructions.load(std::memory_order_relaxed) == 1,
         "task cleanup failure should still release its context exactly once");
}

void testRepeatedSpawnJoin() {
  std::atomic<int> entries{0};
  std::atomic<int> destructions{0};
  constexpr int repetitions = 256;
  for (int index = 0; index < repetitions; ++index) {
    gti_thread_spawn_result_v1 spawn = gti_rt_thread_spawn_v1(
        runTask, newTask(entries, destructions), destroyTask);
    if (spawn.status != GTI_THREAD_SPAWN_OK_V1) {
      expect(false, "stress spawn should succeed");
      break;
    }
    if (gti_rt_thread_join_v1(&spawn.state).status != GTI_THREAD_JOIN_OK_V1) {
      expect(false, "stress join should succeed");
      break;
    }
  }
  expect(entries.load(std::memory_order_relaxed) == repetitions &&
             destructions.load(std::memory_order_relaxed) == repetitions,
         "stress creation should preserve exact entry and cleanup counts");
}

struct GatedTaskContext {
  std::atomic<bool> *release = nullptr;
  std::atomic<int> *entries = nullptr;
  std::atomic<int> *destructions = nullptr;
};

void runGatedTask(void *opaque) {
  auto &context = *static_cast<GatedTaskContext *>(opaque);
  context.release->wait(false, std::memory_order_acquire);
  context.entries->fetch_add(1, std::memory_order_relaxed);
}

void destroyGatedTask(void *opaque) {
  auto *context = static_cast<GatedTaskContext *>(opaque);
  context->destructions->fetch_add(1, std::memory_order_relaxed);
  delete context;
}

void testSimultaneousWorkers() {
  constexpr std::size_t workerCount = 32;
  std::atomic<bool> release{false};
  std::atomic<int> entries{0};
  std::atomic<int> destructions{0};
  std::array<gti_thread_state_v1 *, workerCount> states{};
  std::size_t spawned = 0;

  for (; spawned < workerCount; ++spawned) {
    const gti_thread_spawn_result_v1 result = gti_rt_thread_spawn_v1(
        runGatedTask,
        new GatedTaskContext{.release = &release,
                             .entries = &entries,
                             .destructions = &destructions},
        destroyGatedTask);
    if (result.status != GTI_THREAD_SPAWN_OK_V1) {
      expect(false, "simultaneous worker creation should succeed");
      break;
    }
    states[spawned] = result.state;
  }

  release.store(true, std::memory_order_release);
  release.notify_all();
  for (std::size_t index = 0; index < spawned; ++index) {
    const gti_thread_join_result_v1 join =
        gti_rt_thread_join_v1(&states[index]);
    expect(join.status == GTI_THREAD_JOIN_OK_V1 && states[index] == nullptr,
           "every simultaneous worker should join and consume its state");
  }
  expect(spawned == workerCount &&
             entries.load(std::memory_order_relaxed) ==
                 static_cast<int>(workerCount) &&
             destructions.load(std::memory_order_relaxed) ==
                 static_cast<int>(workerCount),
         "simultaneous workers should each enter and destroy exactly once");
}

} // namespace

static_assert(std::is_standard_layout_v<gti_thread_spawn_result_v1> &&
              std::is_trivially_copyable_v<gti_thread_spawn_result_v1>);
static_assert(std::is_standard_layout_v<gti_thread_join_result_v1> &&
              std::is_trivially_copyable_v<gti_thread_join_result_v1>);
static_assert(std::is_standard_layout_v<gti_thread_value_join_result_v1> &&
              std::is_trivially_copyable_v<gti_thread_value_join_result_v1>);
static_assert(offsetof(gti_thread_spawn_result_v1, state) == 8);
static_assert(offsetof(gti_thread_join_result_v1, failure_kind) == 8);
static_assert(offsetof(gti_thread_join_result_v1, failure) == 16);
static_assert(offsetof(gti_thread_value_join_result_v1, result_context) ==
              sizeof(gti_thread_join_result_v1));

int main() {
  testSpawnJoinAndConsumption();
  testJoinPublishesWorkerWrites();
  testValueJoinTakeAndAbandon();
  testValueSpawnAndJoinFailuresPreserveOwnership();
  testCreationFailureDestroysExactlyOnce();
  testRecoverableJoinFailurePreservesOwnership();
  testWorkerFailureHandoff();
  testWorkerCleanupFailureHandoff();
  testTaskObjectCleanupFailureHandoff();
  testRepeatedSpawnJoin();
  testSimultaneousWorkers();
  return failures == 0 ? 0 : 1;
}
