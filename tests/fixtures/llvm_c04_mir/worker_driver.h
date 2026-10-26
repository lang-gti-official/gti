#pragma once

#include <gti/runtime_thread.h>
#include <gti/runtime_thread.hpp>
#include "../../../runtime/src/thread_internal.h"

// The emitter checks these channels against the candidate's FunctionAbi. This
// descriptor and query envelope exist only in the opt-in comparison harness.
#if GTI_C04_STATUS
using C04TaskInvoke = bool (*)(Channel *, void *);
#else
using C04TaskInvoke = void (*)(void *);
#endif
struct C04Task {
  C04TaskInvoke invoke;
  void *context;
  bool (*cleanup)(void *, Channel *, bool);
};

inline void c04WorkerEntry(void *opaque) noexcept {
  const auto &task = *static_cast<const C04Task *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    Channel failure{};
    bool succeeded = false;
#if GTI_C04_STATUS
    succeeded = task.invoke(&failure, task.context);
#else
    try {
      task.invoke(task.context);
      succeeded = true;
    } catch (const c04_test::EvaluationFailure &caught) {
      if (!c04_test::unwinding || &caught != c04_test::activeCarrier)
        c04_test::foreignException();
      failure = c04_test::activeFailure;
      c04_test::unwinding = false;
    }
#endif
    // All GTI frames and the primary carrier are gone before the existing
    // C++ runtime handoff; that exception never crosses LLVM or C frames.
    if (!succeeded)
      gti_runtime_detail::handoff_managed_worker_failure(failure.record, failure.artifact);
  });
}

extern "C" int32_t __gti_c04_worker_scope(Channel *failure,
                                         C04Task *descriptors,
                                         std::uint64_t count) noexcept {
  if (!count || count > 8)
    std::abort();
  gti_thread_scoped_task_v1 tasks[8]{};
  gti_thread_state_v1 *scratch[8]{};
  for (std::uint64_t index = 0; index < count; ++index)
    tasks[index] = {c04WorkerEntry, &descriptors[index]};
  const auto result = gti_rt_thread_scope_v1(tasks, scratch, count);
  if (result.abi_version != GTI_THREAD_ABI_VERSION_V1 ||
      (result.spawn_status != GTI_THREAD_SPAWN_OK_V1 &&
       result.spawn_status != GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1 &&
       result.spawn_status != GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1) ||
      result.started > count ||
      (result.spawn_status == GTI_THREAD_SPAWN_OK_V1 && result.started != count) ||
      result.worker.abi_version != GTI_THREAD_ABI_VERSION_V2 || result.worker.reserved ||
      (result.worker.status != GTI_THREAD_JOIN_OK_V1 &&
       result.worker.status != GTI_THREAD_JOIN_WORKER_FAILURE_V1) ||
      (result.worker.status == GTI_THREAD_JOIN_WORKER_FAILURE_V1
           ? result.failed_worker >= result.started : result.failed_worker != UINT64_MAX))
    std::abort();
  for (std::uint64_t index = 0; index < count; ++index)
    if (scratch[index])
      std::abort();
  bool failed = result.worker.failure_kind == GTI_THREAD_WORKER_FAILURE_ORDINARY_V1;
  if (failed) {
    if (result.worker.status != GTI_THREAD_JOIN_WORKER_FAILURE_V1 ||
        gti_rt_failure_validate_v2(&result.worker.failure, result.worker.primary_artifact) !=
            GTI_FAILURE_REPORT_OK_V1)
      std::abort();
    *failure = {result.worker.failure, result.worker.primary_artifact};
  }
  if (result.worker.failure_kind == GTI_THREAD_WORKER_FAILURE_CLEANUP_V1)
    gti_rt_failure_terminate_cleanup_v2(&result.worker.cleanup_failure,
                                      result.worker.primary_artifact,
                                      result.worker.secondary_artifact);
  if (!failed && (result.worker.failure_kind != GTI_THREAD_WORKER_FAILURE_NONE_V1 ||
                  result.worker.status != GTI_THREAD_JOIN_OK_V1))
    std::abort();
  const gti_failure_record_v2 *token = failed
      ? gti_rt_failure_cleanup_enter_v1(&failure->record, failure->artifact) : nullptr;
  for (std::uint64_t index = count; index-- > 0;) {
    const auto &task = descriptors[index];
    if (!task.cleanup)
      continue;
    Channel secondary{};
    bool cleaned = false;
#if GTI_C04_STATUS
    cleaned = task.cleanup(task.context, &secondary, failed);
#else
    try {
      cleaned = task.cleanup(task.context, &secondary, failed);
    } catch (const c04_test::EvaluationFailure &caught) {
      if (!c04_test::unwinding || &caught != c04_test::activeCarrier)
        c04_test::foreignException();
      secondary = c04_test::activeFailure;
      c04_test::unwinding = false;
    }
#endif
    if (!cleaned) {
      // The primary carrier is gone before continuing caller-side cleanup.
      // Existing context authority preserves a worker/outer primary and
      // terminates immediately if this is the secondary failure.
      gti_rt_failure_cleanup_check_v1(&secondary.record, secondary.artifact);
      *failure = secondary;
      failed = true;
      token = gti_rt_failure_cleanup_enter_v1(&failure->record, failure->artifact);
    }
  }
  gti_rt_failure_cleanup_leave_v1(token);
  return failed ? -1 : static_cast<int32_t>(result.spawn_status);
}
