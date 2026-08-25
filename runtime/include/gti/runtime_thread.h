#pragma once

#include "gti/runtime_failure.h"

#include <stdint.h>

#define GTI_THREAD_ABI_VERSION_V1 UINT32_C(1)

typedef struct gti_thread_state_v1 gti_thread_state_v1;
typedef void (*gti_thread_task_entry_v1)(void *context);
typedef void (*gti_thread_task_destroy_v1)(void *context);

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
 * A successful or worker-failure join consumes *state and writes null. A
 * worker failure returns either the exact ordinary record or the exact cleanup
 * emergency envelope together with the immutable artifact descriptors pinned
 * by the generated program. A recoverable native join failure preserves the
 * state so its obligation remains owned by the caller.
 */
gti_thread_join_result_v1 gti_rt_thread_join_v1(gti_thread_state_v1 **state);

#ifdef __cplusplus
}
#endif
