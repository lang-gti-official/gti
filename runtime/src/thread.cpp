#include "gti/runtime_thread.hpp"

#include "cthread_internal.h"
#include "failure_internal.h"
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
  gti_failure_record_v2 failure{};
  gti_failure_emergency_v2 cleanupFailure{};
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
thread_local gti_thread_state_v1 *emergencyHandoffWorker = nullptr;
thread_local uint64_t scopedTestFailureAfter = UINT64_MAX;
enum class ThreadLocalCleanupPhase { Open, Draining, Complete };
thread_local ThreadLocalCleanupPhase threadLocalCleanupPhase =
    ThreadLocalCleanupPhase::Open;
thread_local gti_runtime_detail::thread_local_cleanup_entry
    *threadLocalCleanup = nullptr;
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
    gti::runtime::detail::enterCThreadManagedScope();
  }

  ActiveWorkerScope(const ActiveWorkerScope &) = delete;
  ActiveWorkerScope &operator=(const ActiveWorkerScope &) = delete;

  ~ActiveWorkerScope() {
    if (emergencyHandoffWorker != nullptr) {
      std::terminate();
    }
    gti::runtime::detail::leaveCThreadManagedScope();
    activeWorker = previous;
  }

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

[[nodiscard]] bool acceptsCleanupEnvelope(
    const gti_thread_state_v1 &state, const gti_failure_emergency_v2 &failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact) noexcept {
  if (state.failureKind == GTI_THREAD_WORKER_FAILURE_NONE_V1) {
    return true;
  }
  // The incoming context copies this already-owned primary. Completing its
  // two-record envelope is not a third failure; any other primary still is.
  return activeTaskObjectCleanup &&
         state.failureKind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1 &&
         state.primaryArtifact == primaryArtifact &&
         state.failure.abi_version == failure.primary.abi_version &&
         state.failure.code == failure.primary.code &&
         state.failure.detail == failure.primary.detail &&
         state.failure.site_index == failure.primary.site_index &&
         state.failure.reserved == failure.primary.reserved &&
         std::memcmp(state.failure.artifact_identity,
                     failure.primary.artifact_identity,
                     sizeof(state.failure.artifact_identity)) == 0 &&
         state.failure.message_size == failure.primary.message_size &&
         state.failure.message_flags == failure.primary.message_flags &&
         std::memcmp(state.failure.message, failure.primary.message,
                     sizeof(state.failure.message)) == 0;
}

void recordWorkerFailure(
    gti_thread_state_v1 &state, const gti_failure_record_v2 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) noexcept {
  if (state.failureKind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1) {
    // A cleanup emergency is already the bounded two-record envelope. A
    // further failure has no representable language result.
    std::terminate();
  }
  if (state.failureKind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1) {
    state.cleanupFailure = {
        .abi_version = GTI_FAILURE_ABI_VERSION_V2,
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

[[nodiscard]] gti_thread_join_result_v2 emptyJoinResult() noexcept {
  return {.abi_version = GTI_THREAD_ABI_VERSION_V2,
          .status = GTI_THREAD_JOIN_OK_V1,
          .failure_kind = GTI_THREAD_WORKER_FAILURE_NONE_V1,
          .reserved = 0,
          .failure = {},
          .cleanup_failure = {},
          .primary_artifact = nullptr,
          .secondary_artifact = nullptr};
}

gti_failure_record_v2
promoteFailure(const gti_failure_record_v1 &record) noexcept {
  gti_failure_record_v2 result{};
  // Copy only the frozen v1 fields, never reinterpret an ABI object. Preserve
  // malformed records as malformed instead of legitimizing an unknown version.
  result.abi_version = record.abi_version == GTI_FAILURE_ABI_VERSION_V1 &&
                               record.code != GTI_FAILURE_CODE_PANIC_V2
                           ? GTI_FAILURE_ABI_VERSION_V2
                           : 0;
  result.code = record.code;
  result.detail = record.detail;
  result.site_index = record.site_index;
  result.reserved = record.reserved;
  std::memcpy(result.artifact_identity, record.artifact_identity, 32);
  return result;
}

gti_failure_emergency_v2
promoteFailure(const gti_failure_emergency_v1 &failure) noexcept {
  return {.abi_version = failure.abi_version == GTI_FAILURE_ABI_VERSION_V1
                             ? GTI_FAILURE_ABI_VERSION_V2
                             : 0,
          .reserved = failure.reserved,
          .primary = promoteFailure(failure.primary),
          .secondary = promoteFailure(failure.secondary)};
}

bool fitsLegacy(const gti_failure_record_v2 &record) noexcept {
  if (record.code == GTI_FAILURE_CODE_PANIC_V2 || record.message_size != 0 ||
      record.message_flags != 0) {
    return false;
  }
  for (const auto byte : record.message) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

gti_failure_record_v1
legacyFailure(const gti_failure_record_v2 &record) noexcept {
  gti_failure_record_v1 result{.abi_version = record.abi_version ==
                                                      GTI_FAILURE_ABI_VERSION_V2
                                                  ? GTI_FAILURE_ABI_VERSION_V1
                                                  : 0,
                               .code = record.code,
                               .detail = record.detail,
                               .site_index = record.site_index,
                               .reserved = record.reserved,
                               .artifact_identity = {}};
  std::memcpy(result.artifact_identity, record.artifact_identity, 32);
  return result;
}

gti_thread_join_result_v1 legacyJoin(const gti_thread_join_result_v2 &result) {
  if (result.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1 &&
      !fitsLegacy(result.failure)) {
    gti::runtime::detail::terminateLegacyFailure(result.failure,
                                                 result.primary_artifact);
  }
  if (result.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 &&
      (!fitsLegacy(result.cleanup_failure.primary) ||
       !fitsLegacy(result.cleanup_failure.secondary))) {
    gti::runtime::detail::terminateLegacyFailure(result.cleanup_failure,
                                                 result.primary_artifact,
                                                 result.secondary_artifact);
  }
  gti_thread_join_result_v1 legacy{.abi_version = GTI_THREAD_ABI_VERSION_V1,
                                   .status = result.status,
                                   .failure_kind = result.failure_kind,
                                   .reserved = result.reserved,
                                   .failure = {},
                                   .cleanup_failure = {},
                                   .primary_artifact = result.primary_artifact,
                                   .secondary_artifact =
                                       result.secondary_artifact};
  if (result.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1) {
    legacy.failure = legacyFailure(result.failure);
  } else if (result.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1) {
    legacy.cleanup_failure = {
        .abi_version =
            result.cleanup_failure.abi_version == GTI_FAILURE_ABI_VERSION_V2
                ? GTI_FAILURE_ABI_VERSION_V1
                : 0,
        .reserved = result.cleanup_failure.reserved,
        .primary = legacyFailure(result.cleanup_failure.primary),
        .secondary = legacyFailure(result.cleanup_failure.secondary)};
  }
  return legacy;
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

class ThreadLocalExitScope final {
public:
  explicit ThreadLocalExitScope(gti_thread_state_v1 &owner) noexcept
      : state(owner) {}
  ~ThreadLocalExitScope() {
    gti_runtime_detail::drain_thread_local_cleanup();
    if (!workerFailed(state) || state.resultContext == nullptr)
      return;
    // A result is not published by join if thread-exit cleanup failed. Retire
    // its pending ownership on the producing thread, once, through the same
    // failure-aware callback used for task-result abandonment.
    void *context = std::exchange(state.resultContext, nullptr);
    auto abandon = std::exchange(state.abandonResult, nullptr);
    state.takeResult = nullptr;
    if (abandon == nullptr)
      std::terminate();
    try {
      ActiveTaskObjectCleanupScope cleanup;
      abandon(context);
    } catch (const gti_runtime_detail::worker_failure_handoff &) {
      gti_runtime_detail::clear_managed_failure_handoff();
    } catch (...) {
      std::terminate();
    }
  }
  ThreadLocalExitScope(const ThreadLocalExitScope &) = delete;
  ThreadLocalExitScope &operator=(const ThreadLocalExitScope &) = delete;

private:
  gti_thread_state_v1 &state;
};

} // namespace

namespace gti_runtime_detail {

void register_thread_local_cleanup(thread_local_cleanup_entry *entry) noexcept {
  if (threadLocalCleanupPhase != ThreadLocalCleanupPhase::Open ||
      entry == nullptr || entry->registered || entry->storage == nullptr ||
      entry->cleanup == nullptr)
    std::terminate();
  entry->previous = threadLocalCleanup;
  entry->registered = true;
  threadLocalCleanup = entry;
}

bool thread_local_cleanup_active() noexcept {
  return threadLocalCleanupPhase != ThreadLocalCleanupPhase::Open;
}

void drain_thread_local_cleanup_legacy(
    const gti_failure_record_v1 *primary,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact) noexcept {
  if (primary == nullptr) {
    drain_thread_local_cleanup(nullptr, primaryArtifact);
    return;
  }
  const auto promoted = promoteFailure(*primary);
  drain_thread_local_cleanup(&promoted, primaryArtifact);
}

void drain_thread_local_cleanup(
    const gti_failure_record_v2 *primary,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact) noexcept {
  if (threadLocalCleanupPhase == ThreadLocalCleanupPhase::Complete)
    return;
  if (threadLocalCleanupPhase == ThreadLocalCleanupPhase::Draining)
    std::terminate();
  threadLocalCleanupPhase = ThreadLocalCleanupPhase::Draining;
  gti_failure_record_v2 pending{};
  bool hasPrimary = primary != nullptr;
  if (hasPrimary)
    pending = *primary;
  bool emergency = false;
  if (activeWorker != nullptr) {
    const std::lock_guard lock(activeWorker->failureMutex);
    emergency =
        activeWorker->failureKind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;
    if (activeWorker->failureKind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1) {
      pending = activeWorker->failure;
      primaryArtifact = activeWorker->primaryArtifact;
      hasPrimary = true;
    }
  }
  while (threadLocalCleanup != nullptr && !emergency) {
    auto *entry = threadLocalCleanup;
    threadLocalCleanup = entry->previous;
    entry->previous = nullptr;
    entry->registered = false;
    gti_failure_record_v2 failure{};
    bool succeeded = false;
    try {
      if (activeWorker != nullptr) {
        ActiveTaskObjectCleanupScope cleanup;
        succeeded = entry->cleanup(entry->storage, &failure,
                                   hasPrimary ? &pending : nullptr,
                                   hasPrimary ? primaryArtifact : nullptr);
      } else {
        succeeded = entry->cleanup(entry->storage, &failure,
                                   hasPrimary ? &pending : nullptr,
                                   hasPrimary ? primaryArtifact : nullptr);
      }
    } catch (const worker_failure_handoff &) {
      clear_managed_failure_handoff();
      if (activeWorker == nullptr)
        std::terminate();
      const std::lock_guard lock(activeWorker->failureMutex);
      emergency =
          activeWorker->failureKind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;
      if (!emergency) {
        pending = activeWorker->failure;
        primaryArtifact = activeWorker->primaryArtifact;
        hasPrimary = true;
      }
      continue;
    } catch (...) {
      std::terminate();
    }
    if (activeWorker != nullptr) {
      const std::lock_guard lock(activeWorker->failureMutex);
      if (activeWorker->failureKind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1) {
        emergency = true;
        continue;
      }
      if (!hasPrimary &&
          activeWorker->failureKind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1) {
        pending = activeWorker->failure;
        primaryArtifact = activeWorker->primaryArtifact;
        hasPrimary = true;
        // The callback used the failure-aware native destruction boundary;
        // the ordinary record has already been installed exactly once.
        continue;
      }
    }
    if (succeeded)
      continue;
    if (activeWorker != nullptr) {
      const std::lock_guard lock(activeWorker->failureMutex);
      recordWorkerFailure(*activeWorker, failure, entry->artifact);
      emergency =
          activeWorker->failureKind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;
    } else if (hasPrimary) {
      const gti_failure_emergency_v2 combined{.abi_version =
                                                  GTI_FAILURE_ABI_VERSION_V2,
                                              .reserved = 0,
                                              .primary = pending,
                                              .secondary = failure};
      threadLocalCleanup = nullptr;
      threadLocalCleanupPhase = ThreadLocalCleanupPhase::Complete;
      gti_rt_failure_terminate_cleanup_v2(&combined, primaryArtifact,
                                          entry->artifact);
    }
    if (!hasPrimary) {
      pending = failure;
      primaryArtifact = entry->artifact;
      hasPrimary = true;
    }
  }
  // A two-record cleanup emergency abandons remaining lifetimes. Generated
  // TLS slots own byte storage and must not execute implicit native cleanup.
  threadLocalCleanup = nullptr;
  threadLocalCleanupPhase = ThreadLocalCleanupPhase::Complete;
  if (hasPrimary && primary == nullptr && activeWorker == nullptr)
    gti_rt_failure_terminate_v2(&pending, primaryArtifact, nullptr, nullptr);
}

bool managed_worker_active() noexcept {
  return activeWorker != nullptr &&
         !gti::runtime::detail::terminalCThreadCallbackActive();
}

bool managed_cleanup_emergency_handoff_active() noexcept {
  return emergencyHandoffWorker != nullptr &&
         emergencyHandoffWorker == activeWorker;
}

void clear_managed_failure_handoff() noexcept {
  if (emergencyHandoffWorker != nullptr &&
      emergencyHandoffWorker != activeWorker) {
    std::terminate();
  }
  emergencyHandoffWorker = nullptr;
}

[[noreturn]] void handoff_managed_worker_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) {
  handoff_managed_worker_failure(promoteFailure(record), artifact);
}

[[noreturn]] void handoff_managed_worker_failure(
    const gti_failure_record_v2 &record,
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
  handoff_managed_worker_cleanup_failure(promoteFailure(failure),
                                         primaryArtifact, secondaryArtifact);
}

[[noreturn]] void handoff_managed_worker_cleanup_failure(
    const gti_failure_emergency_v2 &failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact) {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr) {
    std::terminate();
  }
  {
    const std::lock_guard lock(state->failureMutex);
    if (!acceptsCleanupEnvelope(*state, failure, primaryArtifact)) {
      std::terminate();
    }
    state->cleanupFailure = failure;
    // Cleanup kind owns only the emergency envelope. Retire the ordinary
    // payload when its exact primary is promoted into that envelope.
    state->failure = {};
    state->primaryArtifact = primaryArtifact;
    state->secondaryArtifact = secondaryArtifact;
    state->failureKind = GTI_THREAD_WORKER_FAILURE_CLEANUP_V1;
  }
  if (emergencyHandoffWorker != nullptr) {
    std::terminate();
  }
  // Only this explicit two-record terminal handoff authorizes abandoning
  // representation cleanup. Ordinary failure propagation and a merely stored
  // pending failure never enable it. Adapter catches clear it before task
  // completion/destruction begins.
  emergencyHandoffWorker = state;
  throw worker_failure_handoff{};
}

gti_thread_worker_failure_kind_v1 managed_task_cleanup_failure_kind() noexcept {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr || !activeTaskObjectCleanup) {
    return GTI_THREAD_WORKER_FAILURE_NONE_V1;
  }
  const std::lock_guard lock(state->failureMutex);
  return state->failureKind;
}

bool record_managed_task_cleanup_failure(
    const gti_failure_record_v1 &record,
    const gti_failure_artifact_descriptor_v1 *artifact) noexcept {
  return record_managed_task_cleanup_failure(promoteFailure(record), artifact);
}

bool copy_managed_task_cleanup_primary_v2(
    gti_failure_record_v2 &record,
    const gti_failure_artifact_descriptor_v1 *&artifact) noexcept {
  gti_thread_state_v1 *state = activeWorker;
  if (state == nullptr || !activeTaskObjectCleanup) {
    return false;
  }
  const std::lock_guard lock(state->failureMutex);
  if (state->failureKind != GTI_THREAD_WORKER_FAILURE_ORDINARY_V1) {
    return false;
  }
  record = state->failure;
  artifact = state->primaryArtifact;
  return true;
}

bool record_managed_task_cleanup_failure(
    const gti_failure_record_v2 &record,
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
  if (failure == nullptr) {
    return record_managed_task_cleanup_emergency(
        static_cast<const gti_failure_emergency_v2 *>(nullptr), primaryArtifact,
        secondaryArtifact);
  }
  const auto promoted = promoteFailure(*failure);
  return record_managed_task_cleanup_emergency(&promoted, primaryArtifact,
                                               secondaryArtifact);
}

bool record_managed_task_cleanup_emergency(
    const gti_failure_emergency_v2 *failure,
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
  if (!acceptsCleanupEnvelope(*state, *failure, primaryArtifact)) {
    // A differing pending primary would require a third record.
    std::terminate();
  }
  state->cleanupFailure = *failure;
  state->failure = {};
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

void setScopedThreadTestFailureAfter(std::uint64_t starts) noexcept {
  scopedTestFailureAfter = starts;
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
      ThreadLocalExitScope threadLocals(*state);
      try {
        entry(task_context);
      } catch (const gti_runtime_detail::worker_failure_handoff &) {
        gti_runtime_detail::clear_managed_failure_handoff();
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
      ThreadLocalExitScope threadLocals(*state);
      try {
        entry(task_context);
      } catch (const gti_runtime_detail::worker_failure_handoff &) {
        gti_runtime_detail::clear_managed_failure_handoff();
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

extern "C" gti_thread_join_result_v2
gti_rt_thread_join_v2(gti_thread_state_v1 **stateAddress) {
  gti_thread_join_result_v2 result{.abi_version = GTI_THREAD_ABI_VERSION_V2,
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

extern "C" gti_thread_value_join_result_v2
gti_rt_thread_join_value_v2(gti_thread_state_v1 **stateAddress) {
  gti_thread_value_join_result_v2 result{
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

extern "C" gti_thread_join_result_v1
gti_rt_thread_join_v1(gti_thread_state_v1 **stateAddress) {
  return legacyJoin(gti_rt_thread_join_v2(stateAddress));
}

extern "C" gti_thread_scope_result_v1
gti_rt_thread_scope_v1(const gti_thread_scoped_task_v1 *tasks,
                       gti_thread_state_v1 **scratch, uint64_t count) {
  gti_thread_scope_result_v1 result{
      .abi_version = GTI_THREAD_ABI_VERSION_V1,
      .spawn_status = GTI_THREAD_SPAWN_OK_V1,
      .started = 0,
      .failed_worker = UINT64_MAX,
      .worker = emptyJoinResult(),
  };
  if (count == 0)
    return result;
  if (scratch == nullptr) {
    result.spawn_status = GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1;
    return result;
  }
  for (uint64_t index = 0; index < count; ++index)
    scratch[index] = nullptr;
  if (tasks == nullptr) {
    result.spawn_status = GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1;
    return result;
  }

  // These contexts remain in caller-owned storage. The managed entry adapter
  // still contains worker failures, but cleanup of the borrowed context is
  // deliberately not part of worker completion or creation failure.
  const auto leaveBorrowedContext = [](void *) {};
  const auto failAfter = std::exchange(scopedTestFailureAfter, UINT64_MAX);
  for (uint64_t index = 0; index < count; ++index) {
    if (index == failAfter) {
      result.spawn_status = GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1;
      break;
    }
    const auto spawned = gti_rt_thread_spawn_v1(
        tasks[index].entry, tasks[index].context, leaveBorrowedContext);
    if (spawned.status != GTI_THREAD_SPAWN_OK_V1) {
      result.spawn_status = spawned.status;
      break;
    }
    scratch[index] = spawned.state;
    ++result.started;
  }

  for (uint64_t index = 0; index < result.started; ++index) {
    const auto joined = gti_rt_thread_join_v2(&scratch[index]);
    if (scratch[index] != nullptr ||
        (joined.status != GTI_THREAD_JOIN_OK_V1 &&
         joined.status != GTI_THREAD_JOIN_WORKER_FAILURE_V1)) {
      // Returning or unwinding here would let caller cleanup invalidate a
      // context that a live worker may still access. Unlike an owned thread,
      // a borrowed worker cannot be placed in terminal ownership quarantine.
      std::terminate();
    }
    if (joined.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1 &&
        result.failed_worker == UINT64_MAX) {
      result.failed_worker = index;
      result.worker = joined;
    }
  }
  return result;
}

extern "C" gti_thread_value_join_result_v1
gti_rt_thread_join_value_v1(gti_thread_state_v1 **stateAddress) {
  const auto result = gti_rt_thread_join_value_v2(stateAddress);
  return {.join = legacyJoin(result.join),
          .result_context = result.result_context,
          .take_result = result.take_result,
          .abandon_result = result.abandon_result};
}
