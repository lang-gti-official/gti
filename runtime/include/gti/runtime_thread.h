#pragma once

#include "gti/runtime_failure.h"

#include <stdint.h>

#define GTI_THREAD_ABI_VERSION_V1 UINT32_C(1)
#define GTI_THREAD_ABI_VERSION_V2 UINT32_C(2)

typedef struct gti_thread_state_v1 gti_thread_state_v1;
typedef void (*gti_thread_task_entry_v1)(void *context);
typedef void (*gti_thread_task_destroy_v1)(void *context);
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

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif
