#include "gti/runtime_thread.hpp"

#include "thread_internal.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

struct gti_thread_state_v1 {
  std::thread worker;
  std::mutex failureMutex;
  gti_thread_worker_failure_kind_v1 failureKind =
      GTI_THREAD_WORKER_FAILURE_NONE_V1;
  gti_failure_record_v1 failure{};
  gti_failure_emergency_v1 cleanupFailure{};
  const gti_failure_artifact_descriptor_v1 *primaryArtifact = nullptr;
  const gti_failure_artifact_descriptor_v1 *secondaryArtifact = nullptr;
  bool valueBearing = false;
  void *resultContext = nullptr;
  gti_thread_result_take_v1 takeResult = nullptr;
  gti_thread_result_abandon_v1 abandonResult = nullptr;
  gti_thread_state_v1 *quarantineNext = nullptr;
};

namespace {

thread_local gti_thread_state_v1 *activeWorker = nullptr;
thread_local bool activeTaskObjectCleanup = false;
std::atomic<gti::runtime::detail::ThreadTestFault> testFault{
    gti::runtime::detail::ThreadTestFault::None};
std::mutex quarantineMutex;
gti_thread_state_v1 *quarantinedStates = nullptr;

[[nodiscard]] bool
consumeTestFault(gti::runtime::detail::ThreadTestFault expected) noexcept {
  return testFault.compare_exchange_strong(
      expected, gti::runtime::detail::ThreadTestFault::None,
      std::memory_order_acq_rel, std::memory_order_acquire);
}

class ActiveWorkerScope final {
public:
  explicit ActiveWorkerScope(gti_thread_state_v1 &state) noexcept
      : previous(activeWorker) {
    activeWorker = &state;
  }

  ActiveWorkerScope(const ActiveWorkerScope &) = delete;
  ActiveWorkerScope &operator=(const ActiveWorkerScope &) = delete;

  ~ActiveWorkerScope() { activeWorker = previous; }

private:
  gti_thread_state_v1 *previous;
};

class ActiveTaskObjectCleanupScope final {
public:
  ActiveTaskObjectCleanupScope() noexcept {
    if (activeTaskObjectCleanup) {
      std::terminate();
    }
    activeTaskObjectCleanup = true;
  }

  ActiveTaskObjectCleanupScope(const ActiveTaskObjectCleanupScope &) = delete;
  ActiveTaskObjectCleanupScope &
  operator=(const ActiveTaskObjectCleanupScope &) = delete;

  ~ActiveTaskObjectCleanupScope() { activeTaskObjectCleanup = false; }
};

void recordWorkerFailure(
    gti_thread_state_v1 &state, const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) noexcept {
  if (state.failureKind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1) {
    // A cleanup emergency is already the bounded two-record envelope. A
    // further failure has no representable language result.
    std::terminate();
  }
  if (state.failureKind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1) {
    state.cleanupFailure = {
        .abi_version = GTI_FAILURE_ABI_VERSION_V1,
        .reserved = 0,
        .primary = state.failure,
        .secondary = record,
    };
    state.failure = {};
    state.secondaryArtifact = artifact;
    state.failureKind = GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;
    return;
  }
  state.failure = record;
  state.primaryArtifact = artifact;
  state.failureKind = GTI_THREAD_WORKER_FAILURE_ORDINARY_V1;
}

[[nodiscard]] gti_thread_spawn_status_v1
spawnStatus(const std::system_error &error) noexcept {
  const int value = error.code().value();
  return value == EPERM ? GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1
                        : GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
}

[[nodiscard]] gti_thread_join_result_v1 emptyJoinResult() noexcept {
  return {.abi_version = GTI_THREAD_ABI_VERSION_V1,
          .status = GTI_THREAD_JOIN_OK_V1,
          .failure_kind = GTI_THREAD_WORKER_FAILURE_NONE_V1,
          .reserved = 0,
          .failure = {},
          .cleanup_failure = {},
          .primary_artifact = nullptr,
          .secondary_artifact = nullptr};
}

[[nodiscard]] bool workerFailed(gti_thread_state_v1 &state) noexcept {
  const std::lock_guard lock(state.failureMutex);
  return state.failureKind != GTI_THREAD_WORKER_FAILURE_NONE_V1;
}

void releaseTaskContext(void *context,
                        gti_thread_task_abandon_v1 abandon) noexcept {
  if (context == nullptr || abandon == nullptr) {
    return;
  }
  try {
    abandon(context);
  } catch (...) {
    std::terminate();
  }
}

} // namespace

namespace gti_runtime_detail {

bool managed_worker_active() noexcept { return activeWorker != nullptr; }

[[noreturn]] void handoff_managed_worker_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr) {
    std::terminate();
  }
  {
    const std::lock_guard lock(state->failureMutex);
    recordWorkerFailure(*state, record, artifact);
  }
  throw worker_failure_handoff{};
}

[[noreturn]] void handoff_managed_worker_cleanup_failure(
    const gti_failure_emergency_v1 &failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact) {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr) {
    std::terminate();
  }
  {
    const std::lock_guard lock(state->failureMutex);
    if (state->failureKind != GTI_THREAD_WORKER_FAILURE_NONE_V1) {
      std::terminate();
    }
    state->cleanupFailure = failure;
    state->primaryArtifact = primaryArtifact;
    state->secondaryArtifact = secondaryArtifact;
    state->failureKind = GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;
  }
  throw worker_failure_handoff{};
}

bool record_managed_task_cleanup_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) noexcept {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr || !activeTaskObjectCleanup) {
    return false;
  }
  const std::lock_guard lock(state->failureMutex);
  // Callable/capture destruction is the final task-entry cleanup phase. If
  // the body already failed, this exact secondary record completes GTI-R0014;
  // otherwise it becomes the worker's ordinary failure outcome.
  recordWorkerFailure(*state, record, artifact);
  return true;
}

bool record_managed_task_cleanup_emergency(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact) noexcept {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr || !activeTaskObjectCleanup) {
    return false;
  }
  if (failure == nullptr) {
    std::terminate();
  }
  const std::lock_guard lock(state->failureMutex);
  if (state->failureKind != GTI_THREAD_WORKER_FAILURE_NONE_V1) {
    // The supplied envelope already contains two failures. Any prior worker
    // failure would require an unrepresentable third record.
    std::terminate();
  }
  state->cleanupFailure = *failure;
  state->primaryArtifact = primaryArtifact;
  state->secondaryArtifact = secondaryArtifact;
  state->failureKind = GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;
  return true;
}

void quarantine_managed_thread_state(
    gti_thread_state_v1 **stateAddress) noexcept {
  if (stateAddress == nullptr || *stateAddress == nullptr ||
      !(*stateAddress)->worker.joinable()) {
    std::terminate();
  }
  gti_thread_state_v1 *state = *stateAddress;
  {
    const std::lock_guard lock(quarantineMutex);
    state->quarantineNext = quarantinedStates;
    quarantinedStates = state;
  }
  *stateAddress = nullptr;
}

} // namespace gti_runtime_detail

namespace gti::runtime::detail {

void setThreadTestFault(ThreadTestFault fault) noexcept {
  testFault.store(fault, std::memory_order_release);
}

} // namespace gti::runtime::detail

extern "C" gti_thread_spawn_result_v1
gti_rt_thread_spawn_v1(gti_thread_task_entry_v1 entry, void *task_context,
                       gti_thread_task_destroy_v1 destroy) {
  gti_thread_spawn_result_v1 result{.abi_version = GTI_THREAD_ABI_VERSION_V1,
                                    .status = GTI_THREAD_SPAWN_OK_V1,
                                    .state = nullptr};
  if (entry == nullptr || destroy == nullptr || task_context == nullptr) {
    if (destroy != nullptr && task_context != nullptr) {
      destroy(task_context);
    }
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }
  if (consumeTestFault(
          gti::runtime::detail::ThreadTestFault::SpawnResourceUnavailable)) {
    destroy(task_context);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }
  if (consumeTestFault(
          gti::runtime::detail::ThreadTestFault::SpawnOperationNotPermitted)) {
    destroy(task_context);
    result.status = GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1;
    return result;
  }

  gti_thread_state_v1 *state = new (std::nothrow) gti_thread_state_v1;
  if (state == nullptr) {
    destroy(task_context);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }

  try {
    state->worker = std::thread([state, entry, task_context, destroy] {
      ActiveWorkerScope scope(*state);
      try {
        entry(task_context);
      } catch (const gti_runtime_detail::worker_failure_handoff &) {
        // Generated adapters catch this. Retain a defensive runtime boundary
        // so a malformed adapter still cannot leak an exception to std::thread.
      } catch (...) {
        std::terminate();
      }
      try {
        ActiveTaskObjectCleanupScope cleanup;
        destroy(task_context);
      } catch (...) {
        // No native unwind may cross the task cleanup/runtime boundary.
        std::terminate();
      }
    });
  } catch (const std::system_error &error) {
    destroy(task_context);
    result.status = spawnStatus(error);
    delete state;
    return result;
  } catch (...) {
    destroy(task_context);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    delete state;
    return result;
  }

  result.state = state;
  return result;
}

extern "C" gti_thread_spawn_result_v1
gti_rt_thread_spawn_value_v1(gti_thread_task_entry_v1 entry, void *task_context,
                             gti_thread_task_complete_v1 complete_task,
                             gti_thread_task_abandon_v1 abandon_task,
                             gti_thread_result_take_v1 take_result,
                             gti_thread_result_abandon_v1 abandon_result) {
  gti_thread_spawn_result_v1 result{.abi_version = GTI_THREAD_ABI_VERSION_V1,
                                    .status = GTI_THREAD_SPAWN_OK_V1,
                                    .state = nullptr};
  if (entry == nullptr || complete_task == nullptr || abandon_task == nullptr ||
      take_result == nullptr || abandon_result == nullptr ||
      task_context == nullptr) {
    releaseTaskContext(task_context, abandon_task);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }
  if (consumeTestFault(
          gti::runtime::detail::ThreadTestFault::SpawnResourceUnavailable)) {
    releaseTaskContext(task_context, abandon_task);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }
  if (consumeTestFault(
          gti::runtime::detail::ThreadTestFault::SpawnOperationNotPermitted)) {
    releaseTaskContext(task_context, abandon_task);
    result.status = GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1;
    return result;
  }

  gti_thread_state_v1 *state = new (std::nothrow) gti_thread_state_v1;
  if (state == nullptr) {
    releaseTaskContext(task_context, abandon_task);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    return result;
  }
  state->valueBearing = true;

  try {
    state->worker = std::thread([state, entry, task_context, complete_task,
                                 abandon_task, take_result, abandon_result] {
      ActiveWorkerScope scope(*state);
      try {
        entry(task_context);
      } catch (const gti_runtime_detail::worker_failure_handoff &) {
        // Generated adapters normally contain this handoff themselves.
      } catch (...) {
        std::terminate();
      }
      try {
        ActiveTaskObjectCleanupScope cleanup;
        if (workerFailed(*state)) {
          abandon_task(task_context);
          return;
        }
        void *result_context = complete_task(task_context);
        if (result_context == nullptr) {
          std::terminate();
        }
        if (workerFailed(*state)) {
          abandon_result(result_context);
          return;
        }
        state->resultContext = result_context;
        state->takeResult = take_result;
        state->abandonResult = abandon_result;
      } catch (...) {
        // No native unwind may cross task completion or abandonment.
        std::terminate();
      }
    });
  } catch (const std::system_error &error) {
    releaseTaskContext(task_context, abandon_task);
    result.status = spawnStatus(error);
    delete state;
    return result;
  } catch (...) {
    releaseTaskContext(task_context, abandon_task);
    result.status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
    delete state;
    return result;
  }

  result.state = state;
  return result;
}

extern "C" gti_thread_join_result_v1
gti_rt_thread_join_v1(gti_thread_state_v1 **stateAddress) {
  gti_thread_join_result_v1 result{.abi_version = GTI_THREAD_ABI_VERSION_V1,
                                   .status = GTI_THREAD_JOIN_OK_V1,
                                   .failure_kind =
                                       GTI_THREAD_WORKER_FAILURE_NONE_V1,
                                   .reserved = 0,
                                   .failure = {},
                                   .cleanup_failure = {},
                                   .primary_artifact = nullptr,
                                   .secondary_artifact = nullptr};
  if (stateAddress == nullptr || *stateAddress == nullptr) {
    result.status = GTI_THREAD_JOIN_NOT_JOINABLE_V1;
    return result;
  }

  gti_thread_state_v1 *state = *stateAddress;
  if (state->valueBearing) {
    std::terminate();
  }
  if (!state->worker.joinable()) {
    // A non-null opaque state is the runtime's ownership proof for exactly
    // one native join obligation.  Losing joinability without consuming and
    // clearing that state leaves no coherent result the language can recover
    // from.
    std::terminate();
  }
  if (consumeTestFault(gti::runtime::detail::ThreadTestFault::JoinFailed)) {
    result.status = GTI_THREAD_JOIN_FAILED_V1;
    return result;
  }
  try {
    state->worker.join();
  } catch (...) {
    if (!state->worker.joinable()) {
      // Native failure changed the ownership state despite not completing the
      // GTI join contract. There is no coherent recoverable handle to return.
      std::terminate();
    }
    result.status = GTI_THREAD_JOIN_FAILED_V1;
    return result;
  }

  {
    const std::lock_guard lock(state->failureMutex);
    if (state->failureKind != GTI_THREAD_WORKER_FAILURE_NONE_V1) {
      result.status = GTI_THREAD_JOIN_WORKER_FAILURE_V1;
      result.failure_kind = state->failureKind;
      result.failure = state->failure;
      result.cleanup_failure = state->cleanupFailure;
      result.primary_artifact = state->primaryArtifact;
      result.secondary_artifact = state->secondaryArtifact;
    }
  }
  *stateAddress = nullptr;
  delete state;
  return result;
}

extern "C" gti_thread_value_join_result_v1
gti_rt_thread_join_value_v1(gti_thread_state_v1 **stateAddress) {
  gti_thread_value_join_result_v1 result{
      .join = emptyJoinResult(),
      .result_context = nullptr,
      .take_result = nullptr,
      .abandon_result = nullptr,
  };
  if (stateAddress == nullptr || *stateAddress == nullptr) {
    result.join.status = GTI_THREAD_JOIN_NOT_JOINABLE_V1;
    return result;
  }

  gti_thread_state_v1 *state = *stateAddress;
  if (!state->valueBearing || !state->worker.joinable()) {
    std::terminate();
  }
  if (consumeTestFault(gti::runtime::detail::ThreadTestFault::JoinFailed)) {
    result.join.status = GTI_THREAD_JOIN_FAILED_V1;
    return result;
  }
  try {
    state->worker.join();
  } catch (...) {
    if (!state->worker.joinable()) {
      std::terminate();
    }
    result.join.status = GTI_THREAD_JOIN_FAILED_V1;
    return result;
  }

  {
    const std::lock_guard lock(state->failureMutex);
    if (state->failureKind != GTI_THREAD_WORKER_FAILURE_NONE_V1) {
      result.join.status = GTI_THREAD_JOIN_WORKER_FAILURE_V1;
      result.join.failure_kind = state->failureKind;
      result.join.failure = state->failure;
      result.join.cleanup_failure = state->cleanupFailure;
      result.join.primary_artifact = state->primaryArtifact;
      result.join.secondary_artifact = state->secondaryArtifact;
    }
  }
  const bool hasResult = state->resultContext != nullptr &&
                         state->takeResult != nullptr &&
                         state->abandonResult != nullptr;
  if ((result.join.status == GTI_THREAD_JOIN_OK_V1) != hasResult) {
    std::terminate();
  }
  if (hasResult) {
    result.result_context = state->resultContext;
    result.take_result = state->takeResult;
    result.abandon_result = state->abandonResult;
    state->resultContext = nullptr;
    state->takeResult = nullptr;
    state->abandonResult = nullptr;
  }
  *stateAddress = nullptr;
  delete state;
  return result;
}
