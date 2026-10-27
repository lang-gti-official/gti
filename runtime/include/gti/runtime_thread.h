#pragma once

#include "gti/runtime_failure.h"

#include <stdint.h>

#define GTI_THREAD_ABI_VERSION_V1 UINT32_C(1)
#define GTI_THREAD_ABI_VERSION_V2 UINT32_C(2)

typedef struct gti_thread_state_v1 gti_thread_state_v1;
typedef void (*gti_thread_task_entry_v1)(void *context);
typedef void (*gti_thread_task_destroy_v1)(void *context);
/* Completion consumes the task exactly once. A null result is permitted only
 * after publishing a worker failure, with no live result left to abandon.
 * A non-null result on failure is abandoned by the runtime before TLS drain. */
typedef void *(*gti_thread_task_complete_v1)(void *context);
typedef void (*gti_thread_task_abandon_v1)(void *context);
typedef void (*gti_thread_result_take_v1)(void *context, void *destination);
typedef void (*gti_thread_result_abandon_v1)(void *context);

typedef uint32_t gti_thread_spawn_status_v1;
enum {
  GTI_THREAD_SPAWN_OK_V1 = 0,
  GTI_THREAD_SPAWN_RESOURCE_UNAVAILABLE_V1 = 1,
  GTI_THREAD_SPAWN_OPERATION_NOT_PERMITTED_V1 = 2,
};

typedef uint32_t gti_thread_join_status_v1;
enum {
  GTI_THREAD_JOIN_OK_V1 = 0,
  GTI_THREAD_JOIN_NOT_JOINABLE_V1 = 1,
  GTI_THREAD_JOIN_FAILED_V1 = 2,
  GTI_THREAD_JOIN_WORKER_FAILURE_V1 = 3,
};

typedef uint32_t gti_thread_worker_failure_kind_v1;
enum {
  GTI_THREAD_WORKER_FAILURE_NONE_V1 = 0,
  GTI_THREAD_WORKER_FAILURE_ORDINARY_V1 = 1,
  GTI_THREAD_WORKER_FAILURE_CLEANUP_V1 = 2,
};

typedef struct gti_thread_spawn_result_v1 {
  uint32_t abi_version;
  gti_thread_spawn_status_v1 status;
  gti_thread_state_v1 *state;
} gti_thread_spawn_result_v1;

typedef struct gti_thread_join_result_v1 {
  uint32_t abi_version;
  gti_thread_join_status_v1 status;
  gti_thread_worker_failure_kind_v1 failure_kind;
  uint32_t reserved;
  gti_failure_record_v1 failure;
  gti_failure_emergency_v1 cleanup_failure;
  const gti_failure_artifact_descriptor_v1 *primary_artifact;
  const gti_failure_artifact_descriptor_v1 *secondary_artifact;
} gti_thread_join_result_v1;

typedef struct gti_thread_value_join_result_v1 {
  gti_thread_join_result_v1 join;
  void *result_context;
  gti_thread_result_take_v1 take_result;
  gti_thread_result_abandon_v1 abandon_result;
} gti_thread_value_join_result_v1;

/* Spawn, task callbacks, and the opaque state retain their v1 ABI. These join
 * results preserve v2 failure messages across the worker containment boundary.
 */
typedef struct gti_thread_join_result_v2 {
  uint32_t abi_version;
  gti_thread_join_status_v1 status;
  gti_thread_worker_failure_kind_v1 failure_kind;
  uint32_t reserved;
  gti_failure_record_v2 failure;
  gti_failure_emergency_v2 cleanup_failure;
  const gti_failure_artifact_descriptor_v1 *primary_artifact;
  const gti_failure_artifact_descriptor_v1 *secondary_artifact;
} gti_thread_join_result_v2;

typedef struct gti_thread_value_join_result_v2 {
  gti_thread_join_result_v2 join;
  void *result_context;
  gti_thread_result_take_v1 take_result;
  gti_thread_result_abandon_v1 abandon_result;
} gti_thread_value_join_result_v2;

#define GTI_THREAD_FAILURE_VIEW_ABI_VERSION_V1 UINT32_C(1)
#define GTI_THREAD_FAILURE_SINK_ABI_VERSION_V1 UINT32_C(1)

/* A borrowed failure view is valid only for publication. A sink must copy the
 * owned records before returning; artifact descriptors retain their existing
 * generated-program pinning obligation. No exception may cross publication.
 * Exactly one of ordinary/cleanup is non-null, according to failure_kind. */
typedef struct gti_thread_failure_view_v1 {
  uint32_t abi_version;
  gti_thread_worker_failure_kind_v1 failure_kind;
  const gti_failure_record_v2 *ordinary;
  const gti_failure_emergency_v2 *cleanup;
  const gti_failure_artifact_descriptor_v1 *primary_artifact;
  const gti_failure_artifact_descriptor_v1 *secondary_artifact;
} gti_thread_failure_view_v1;

typedef void (*gti_thread_failure_publish_v1)(
    void *context, const gti_thread_failure_view_v1 *failure);

/* Caller-owned, address-stable and thread-confined between enter/leave. Start
 * with ABI version 1 and all other fields zero. Nested scopes restore the exact
 * previous sink. Publication callbacks cannot re-enter or alter this stack.
 * The descriptor itself allocates nothing and owns no failure record. */
typedef struct gti_thread_failure_sink_v1 {
  uint32_t abi_version;
  uint32_t reserved;
  struct gti_thread_failure_sink_v1 *previous;
  gti_thread_failure_publish_v1 publish;
  void *context;
  const void *owner;
} gti_thread_failure_sink_v1;

/* A synchronous scope borrows its task contexts. It does not destroy or retain
 * them. The caller supplies distinct scratch slots; no task-box or handle-array
 * allocation is needed in addition to the native workers' own state. */
typedef struct gti_thread_scoped_task_v1 {
  gti_thread_task_entry_v1 entry;
  void *context;
} gti_thread_scoped_task_v1;

typedef struct gti_thread_scope_result_v1 {
  uint32_t abi_version;
  gti_thread_spawn_status_v1 spawn_status;
  uint64_t started;
  /* UINT64_MAX when no worker failed; otherwise the first failing input. */
  uint64_t failed_worker;
  gti_thread_join_result_v2 worker;
} gti_thread_scope_result_v1;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define GTI_THREAD_SINK_NOEXCEPT noexcept
#else
#define GTI_THREAD_SINK_NOEXCEPT
#endif

/* Generated adapters catch their private GTI transport before calling publish.
 * The runtime installs a worker sink around entry, task/result cleanup and TLS
 * drain. A generated caller installs its own sink around synchronous launch
 * rollback or post-join take/abandon. A nested caller sink takes precedence over
 * the worker sink; publishing does not perform a control transfer. Missing,
 * malformed, cross-thread or reentrant use terminates. */
void gti_rt_thread_failure_sink_enter_v1(
    gti_thread_failure_sink_v1 *sink, gti_thread_failure_publish_v1 publish,
    void *context) GTI_THREAD_SINK_NOEXCEPT;
void gti_rt_thread_failure_sink_leave_v1(gti_thread_failure_sink_v1 *sink)
    GTI_THREAD_SINK_NOEXCEPT;
void gti_rt_thread_failure_publish_v1(const gti_thread_failure_view_v1 *failure)
    GTI_THREAD_SINK_NOEXCEPT;

/* These queries expose only the runtime-owned cleanup phase of the current
 * worker sink. Outside that exact phase they return NONE/0 and do not modify
 * output storage. A nested caller sink does not inherit worker publication.
 * Successful copy preserves the original record and pinned artifact exactly. */
gti_thread_worker_failure_kind_v1 gti_rt_thread_cleanup_failure_kind_v1(void)
    GTI_THREAD_SINK_NOEXCEPT;
uint32_t gti_rt_thread_cleanup_primary_copy_v2(
    gti_failure_record_v2 *record,
    const gti_failure_artifact_descriptor_v1 **artifact) GTI_THREAD_SINK_NOEXCEPT;

/* Only terminal automatic-join failure may transfer a coherent joinable state
 * here. This clears the owned handle, but neither joins nor detaches the worker.
 * A quarantined worker can still access process storage: retirement readiness
 * becomes permanently false, including after that worker finishes. Hosted
 * terminal failure must leave native roots live; a host must reject unload or
 * context shutdown while readiness is false. Readiness is not a substitute for
 * the host's ordinary quiescence obligation for foreign/manual callers. */
void gti_rt_thread_quarantine_v1(gti_thread_state_v1 **state)
    GTI_THREAD_SINK_NOEXCEPT;
uint32_t gti_rt_thread_program_storage_retirement_ready_v1(void)
    GTI_THREAD_SINK_NOEXCEPT;

#undef GTI_THREAD_SINK_NOEXCEPT

/*
 * On entry, task_context is owned by this call. Success transfers that owner
 * to the worker. Failure invokes destroy exactly once before returning. The
 * worker invokes entry once and then invokes destroy exactly once.
 */
gti_thread_spawn_result_v1
gti_rt_thread_spawn_v1(gti_thread_task_entry_v1 entry, void *task_context,
                       gti_thread_task_destroy_v1 destroy);

/*
 * The result-bearing form gives complete_task ownership of task_context after
 * successful entry; it performs worker-side task cleanup and returns a
 * non-null, independently owned result context. Spawn or worker failure gives
 * abandon_task ownership instead. Successful join transfers the result
 * context plus take/abandon callbacks to the joining caller.
 */
gti_thread_spawn_result_v1
gti_rt_thread_spawn_value_v1(gti_thread_task_entry_v1 entry, void *task_context,
                             gti_thread_task_complete_v1 complete_task,
                             gti_thread_task_abandon_v1 abandon_task,
                             gti_thread_result_take_v1 take_result,
                             gti_thread_result_abandon_v1 abandon_result);

/*
 * A successful or worker-failure join consumes *state and writes null. A
 * worker failure returns either the exact ordinary record or the exact cleanup
 * emergency envelope together with the immutable artifact descriptors pinned
 * by the generated program. A recoverable native join failure preserves the
 * state so its obligation remains owned by the caller.
 */
gti_thread_join_result_v1 gti_rt_thread_join_v1(gti_thread_state_v1 **state);

/*
 * A successful value join consumes *state and transfers exactly one result
 * context to the caller. The caller must invoke either take_result or
 * abandon_result exactly once. Every non-success outcome returns null result
 * fields; recoverable native join failure preserves *state.
 */
gti_thread_value_join_result_v1
gti_rt_thread_join_value_v1(gti_thread_state_v1 **state);

/* Prefer v2 for new generated clients. A legacy v1 join preserves old failure
 * records, but terminates with the exact v2 report if a worker produces a
 * panic that cannot be represented by its v1 result. It never drops messages.
 */
gti_thread_join_result_v2 gti_rt_thread_join_v2(gti_thread_state_v1 **state);
gti_thread_value_join_result_v2
gti_rt_thread_join_value_v2(gti_thread_state_v1 **state);

/*
 * Starts tasks in input order, stopping at the first creation error, and joins
 * every started worker before returning any outcome. All contexts, their
 * reachable borrows and artifact descriptors must remain live through this
 * call. The caller proves disjoint/exclusive or synchronized access; this raw
 * ABI is not itself a source-language borrow proof.
 *
 * `scratch` addresses count writable pointer slots, disjoint from tasks and
 * task-accessible state. Incoming scratch contents are ignored. Every slot is
 * null on return; no join obligation escapes. Null arrays are allowed for zero
 * tasks. This call neither destroys contexts nor reports a failure observer.
 * The first worker failure in input order is retained byte-for-byte, even if
 * creation of a later task failed. Generated clients propagate that failure
 * before returning the creation error. Other workers are still joined.
 *
 * A native join failure cannot release a borrowed context or transfer its
 * obligation to quarantine. Such a runtime integrity failure terminates
 * without unwinding the borrowing caller. It never returns a live worker.
 */
gti_thread_scope_result_v1
gti_rt_thread_scope_v1(const gti_thread_scoped_task_v1 *tasks,
                       gti_thread_state_v1 **scratch, uint64_t count);

#ifdef __cplusplus
}
#endif
