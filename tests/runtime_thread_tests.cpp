#include "gti/runtime_thread.h"
#include "gti/runtime_thread.hpp"

#include "thread_internal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>
#include <type_traits>

namespace {

int failures = 0;

bool sameFailureRecord(const gti_failure_record_v2 &left,
                       const gti_failure_record_v2 &right) {
  return left.abi_version == right.abi_version && left.code == right.code &&
         left.detail == right.detail && left.site_index == right.site_index &&
         left.reserved == right.reserved &&
         std::memcmp(left.artifact_identity, right.artifact_identity,
                     sizeof(left.artifact_identity)) == 0 &&
         left.message_size == right.message_size &&
         left.message_flags == right.message_flags &&
         std::memcmp(left.message, right.message, sizeof(left.message)) == 0;
}

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

struct HandoffStateProbe {
  bool emergency;
  ~HandoffStateProbe() {
    expect(gti_runtime_detail::managed_cleanup_emergency_handoff_active() ==
               emergency,
           "only explicit cleanup-emergency handoff permits abandonment");
  }
};

void runFailingTask(void *opaque) {
  auto &context = *static_cast<FailureContext *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    HandoffStateProbe ordinary{false};
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
  // A foreign native exception cannot enable representation abandonment.
  try {
    HandoffStateProbe foreign{false};
    throw 1;
  } catch (int) {
  }
  gti_runtime_detail::run_managed_task_adapter([&] {
    HandoffStateProbe emergency{true};
    gti_rt_failure_terminate_cleanup_v1(
        &context.failure, context.primaryArtifact, context.secondaryArtifact);
  });
  expect(!gti_runtime_detail::managed_cleanup_emergency_handoff_active(),
         "generated adapter catch clears emergency abandonment");
}

void runCleanupFailingTaskWithoutAdapter(void *opaque) {
  auto &context = *static_cast<CleanupFailureContext *>(opaque);
  HandoffStateProbe emergency{true};
  gti_rt_failure_terminate_cleanup_v1(&context.failure, context.primaryArtifact,
                                      context.secondaryArtifact);
}

void destroyCleanupFailingTask(void *opaque) {
  expect(!gti_runtime_detail::managed_cleanup_emergency_handoff_active(),
         "task cleanup is outside the emergency handoff window");
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
  spawn = gti_rt_thread_spawn_v1(
      runCleanupFailingTaskWithoutAdapter,
      new CleanupFailureContext{&destructions, original, &primaryArtifact,
                                &secondaryArtifact},
      destroyCleanupFailingTask);
  expect(spawn.status == GTI_THREAD_SPAWN_OK_V1,
         "defensive runtime handoff setup spawns successfully");
  const auto defensiveJoin = gti_rt_thread_join_v1(&spawn.state);
  expect(defensiveJoin.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 &&
             destructions.load(std::memory_order_relaxed) == 2,
         "defensive runtime catch also clears abandonment before task cleanup");
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
  gti_failure_record_v2 unavailable{};
  const gti_failure_artifact_descriptor_v1 *unavailableArtifact = nullptr;
  expect(!gti_runtime_detail::copy_managed_task_cleanup_primary_v2(
             unavailable, unavailableArtifact),
         "worker execution cannot borrow a pending task-cleanup failure");
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v1(&context.primary, context.primaryArtifact,
                                observeFailure, context.observers);
  });
}

void destroyTaskWithFailure(void *opaque) {
  auto *context = static_cast<TaskCleanupFailureContext *>(opaque);
  gti_failure_record_v2 copied{};
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
  expect(gti_runtime_detail::copy_managed_task_cleanup_primary_v2(copied,
                                                                  artifact) &&
             copied.abi_version == GTI_FAILURE_ABI_VERSION_V2 &&
             copied.code == context->primary.code &&
             copied.detail == context->primary.detail &&
             copied.site_index == context->primary.site_index &&
             std::memcmp(copied.artifact_identity,
                         context->primary.artifact_identity,
                         sizeof(copied.artifact_identity)) == 0 &&
             artifact == context->primaryArtifact,
         "cleanup-frame copy retains promoted primary and exact descriptor");
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

struct PanicContext {
  std::atomic<int> *destructions;
  std::atomic<int> *observers;
  gti_failure_record_v2 primary;
  gti_failure_record_v2 secondary;
  bool failCleanup;
};

void observePanic(const gti_failure_record_v2 *, void *opaque) {
  static_cast<std::atomic<int> *>(opaque)->fetch_add(1,
                                                     std::memory_order_relaxed);
}

void runPanicTask(void *opaque) {
  auto &context = *static_cast<PanicContext *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v2(&context.primary, nullptr, observePanic,
                                context.observers);
  });
}

void destroyPanicTask(void *opaque) {
  auto *context = static_cast<PanicContext *>(opaque);
  context->destructions->fetch_add(1, std::memory_order_relaxed);
  gti_failure_record_v2 copied{};
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
  expect(gti_runtime_detail::copy_managed_task_cleanup_primary_v2(copied,
                                                                  artifact) &&
             sameFailureRecord(copied, context->primary) && artifact == nullptr,
         "task cleanup copies the exact pending primary and its descriptor");
  copied.code = GTI_FAILURE_CODE_NONE_V1;
  expect(gti_runtime_detail::copy_managed_task_cleanup_primary_v2(copied,
                                                                  artifact) &&
             sameFailureRecord(copied, context->primary),
         "mutating a cleanup-frame copy cannot modify worker-owned failure");
  if (context->failCleanup) {
    expect(gti_runtime_detail::record_managed_task_cleanup_failure(
               context->secondary, nullptr),
           "panic during task cleanup uses the worker-owned failure channel");
    expect(!gti_runtime_detail::managed_cleanup_emergency_handoff_active(),
           "stored task-cleanup emergency is not an active private handoff");
  }
  // Wiping task-owned source records proves the join does not retain pointers.
  std::memset(&context->primary, 0, sizeof(context->primary));
  std::memset(&context->secondary, 0, sizeof(context->secondary));
  delete context;
}

void testPanicHandoff() {
  gti_failure_record_v2 outside{};
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
  expect(!gti_runtime_detail::copy_managed_task_cleanup_primary_v2(outside,
                                                                   artifact),
         "pending primary is unavailable outside managed task cleanup");
  std::atomic<int> destructions{0};
  std::atomic<int> observers{0};
  gti_failure_record_v2 primary{.abi_version = GTI_FAILURE_ABI_VERSION_V2,
                                .code = GTI_FAILURE_CODE_PANIC_V2,
                                .detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2};
  (void)gti_rt_failure_capture_panic_v2(
      &primary, {.data = "worker failed", .length = 13});
  auto secondary = primary;
  (void)gti_rt_failure_capture_panic_v2(
      &secondary, {.data = "cleanup failed", .length = 14});
  for (bool failCleanup : {false, true}) {
    auto spawn = gti_rt_thread_spawn_v1(
        runPanicTask,
        new PanicContext{&destructions, &observers, primary, secondary,
                         failCleanup},
        destroyPanicTask);
    expect(spawn.status == GTI_THREAD_SPAWN_OK_V1,
           "panic handoff setup spawns a worker");
    const auto join = gti_rt_thread_join_v2(&spawn.state);
    expect(join.abi_version == GTI_THREAD_ABI_VERSION_V2 &&
               join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
               spawn.state == nullptr,
           "v2 join consumes the failed worker state exactly once");
    if (failCleanup) {
      expect(join.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 &&
                 join.cleanup_failure.abi_version ==
                     GTI_FAILURE_ABI_VERSION_V2 &&
                 std::memcmp(&join.cleanup_failure.primary, &primary,
                             sizeof(primary)) == 0 &&
                 std::memcmp(&join.cleanup_failure.secondary, &secondary,
                             sizeof(secondary)) == 0,
             "worker cleanup preserves both exact panic messages by value");
    } else {
      expect(join.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1 &&
                 std::memcmp(&join.failure, &primary, sizeof(primary)) == 0,
             "ordinary worker handoff preserves the complete panic message by "
             "value");
    }
  }
  expect(
      observers.load() == 0 && destructions.load() == 2,
      "worker panics do not invoke hosted observers and clean task state once");

  auto valueSpawn = gti_rt_thread_spawn_value_v1(
      runPanicTask,
      new PanicContext{&destructions, &observers, primary, secondary, false},
      +[](void *) -> void * { std::terminate(); }, destroyPanicTask,
      +[](void *, void *) { std::terminate(); },
      +[](void *) { std::terminate(); });
  const auto valueJoin = gti_rt_thread_join_value_v2(&valueSpawn.state);
  expect(
      valueJoin.join.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
          valueJoin.join.failure_kind ==
              GTI_THREAD_WORKER_FAILURE_ORDINARY_V1 &&
          std::memcmp(&valueJoin.join.failure, &primary, sizeof(primary)) ==
              0 &&
          valueJoin.result_context == nullptr &&
          valueJoin.take_result == nullptr &&
          valueJoin.abandon_result == nullptr && valueSpawn.state == nullptr &&
          destructions.load() == 3 && observers.load() == 0,
      "value-bearing worker panic abandons the task and publishes no result");

  const gti_failure_record_v1 legacy{.abi_version = GTI_FAILURE_ABI_VERSION_V1,
                                     .code =
                                         GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
                                     .detail = GTI_FAILURE_DETAIL_ADDITION_V1};
  auto spawn = gti_rt_thread_spawn_v1(
      runFailingTask,
      new FailureContext{&observers, &destructions, legacy, nullptr},
      destroyFailingTask);
  const auto joined = gti_rt_thread_join_v2(&spawn.state);
  expect(joined.failure.abi_version == GTI_FAILURE_ABI_VERSION_V2 &&
             joined.failure.code == legacy.code &&
             joined.failure.detail == legacy.detail &&
             joined.failure.message_size == 0 &&
             joined.failure.message_flags == 0 &&
             gti_rt_failure_validate_v2(&joined.failure, nullptr) ==
                 GTI_FAILURE_REPORT_OK_V1,
         "v2 join promotes legacy failures without inventing a message or "
         "changing origin");
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
static_assert(std::is_standard_layout_v<gti_thread_join_result_v2> &&
              std::is_trivially_copyable_v<gti_thread_join_result_v2>);
static_assert(sizeof(gti_thread_join_result_v2) == 1744);
static_assert(offsetof(gti_thread_join_result_v2, failure) == 16);
static_assert(offsetof(gti_thread_value_join_result_v2, result_context) ==
              sizeof(gti_thread_join_result_v2));
static_assert(std::is_standard_layout_v<gti_thread_value_join_result_v1> &&
              std::is_trivially_copyable_v<gti_thread_value_join_result_v1>);
static_assert(offsetof(gti_thread_spawn_result_v1, state) == 8);
static_assert(offsetof(gti_thread_join_result_v1, failure_kind) == 8);
static_assert(offsetof(gti_thread_join_result_v1, failure) == 16);
static_assert(offsetof(gti_thread_value_join_result_v1, result_context) ==
              sizeof(gti_thread_join_result_v1));

struct EnvelopeProbe {
  gti_failure_record_v2 primary{};
  gti_failure_record_v2 secondary{};
  gti_failure_artifact_descriptor_v1 primaryArtifact{};
  gti_failure_artifact_descriptor_v1 secondaryArtifact{};
  std::string_view mutation;
};

void runEnvelopeProbe(void *opaque) {
  auto &probe = *static_cast<EnvelopeProbe *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v2(&probe.primary, &probe.primaryArtifact, nullptr,
                                nullptr);
  });
  if (probe.mutation == "body") {
    const gti_failure_emergency_v2 emergency{GTI_FAILURE_ABI_VERSION_V2, 0,
                                             probe.primary, probe.secondary};
    gti_runtime_detail::run_managed_task_adapter([&] {
      gti_rt_failure_terminate_cleanup_v2(&emergency, &probe.primaryArtifact,
                                          &probe.secondaryArtifact);
    });
  }
}

void destroyEnvelopeProbe(void *opaque) {
  auto &probe = *static_cast<EnvelopeProbe *>(opaque);
  gti_failure_record_v2 primary{};
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
  if (!gti_runtime_detail::copy_managed_task_cleanup_primary_v2(primary,
                                                                artifact)) {
    expect(false, "envelope probe needs a pending primary in task cleanup");
    return;
  }
  if (probe.mutation == "code")
    primary.code = GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1;
  else if (probe.mutation == "site")
    ++primary.site_index;
  else if (probe.mutation == "message")
    primary.message[0] ^= 1;
  else if (probe.mutation == "descriptor")
    artifact = &probe.secondaryArtifact;
  else if (probe.mutation == "third")
    expect(gti_runtime_detail::record_managed_task_cleanup_failure(
               probe.secondary, &probe.secondaryArtifact),
           "third-record probe first completes the pending emergency");
  const gti_failure_emergency_v2 emergency{GTI_FAILURE_ABI_VERSION_V2, 0,
                                           primary, probe.secondary};
  if (probe.mutation == "stored") {
    expect(gti_runtime_detail::record_managed_task_cleanup_emergency(
               &emergency, artifact, &probe.secondaryArtifact),
           "task cleanup can store the exact emergency without a handoff");
    return;
  }
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_cleanup_v2(&emergency, artifact,
                                        &probe.secondaryArtifact);
  });
}

int main(int argc, char **argv) {
  if (argc == 2) {
    EnvelopeProbe probe;
    probe.mutation = argv[1];
    probe.primary.abi_version = GTI_FAILURE_ABI_VERSION_V2;
    probe.primary.code = GTI_FAILURE_CODE_PANIC_V2;
    probe.primary.detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2;
    (void)gti_rt_failure_capture_panic_v2(
        &probe.primary, {.data = "first primary", .length = 13});
    probe.secondary = probe.primary;
    (void)gti_rt_failure_capture_panic_v2(
        &probe.secondary, {.data = "second failure", .length = 14});
    const auto spawned =
        gti_rt_thread_spawn_v1(runEnvelopeProbe, &probe, destroyEnvelopeProbe);
    auto state = spawned.state;
    if (spawned.status != GTI_THREAD_SPAWN_OK_V1)
      return 1;
    const auto joined = gti_rt_thread_join_v2(&state);
    expect(
        joined.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 &&
            sameFailureRecord(joined.failure, gti_failure_record_v2{}) &&
            sameFailureRecord(joined.cleanup_failure.primary, probe.primary) &&
            sameFailureRecord(joined.cleanup_failure.secondary,
                              probe.secondary) &&
            joined.primary_artifact == &probe.primaryArtifact &&
            joined.secondary_artifact == &probe.secondaryArtifact,
        "exact emergency promotion clears the ordinary payload and preserves "
        "both records and descriptors");
    return failures == 0 ? 0 : 1;
  }
  testSpawnJoinAndConsumption();
  testJoinPublishesWorkerWrites();
  testValueJoinTakeAndAbandon();
  testValueSpawnAndJoinFailuresPreserveOwnership();
  testCreationFailureDestroysExactlyOnce();
  testRecoverableJoinFailurePreservesOwnership();
  testWorkerFailureHandoff();
  testWorkerCleanupFailureHandoff();
  testTaskObjectCleanupFailureHandoff();
  testPanicHandoff();
  testRepeatedSpawnJoin();
  testSimultaneousWorkers();
  return failures == 0 ? 0 : 1;
}
