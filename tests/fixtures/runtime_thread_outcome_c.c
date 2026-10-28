#include "gti/runtime_thread.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(gti_thread_failure_view_v1) == 40,
               "failure view C layout");
_Static_assert(sizeof(gti_thread_failure_sink_v1) == 40,
               "failure sink C layout");
_Static_assert(offsetof(gti_thread_failure_sink_v1, owner) == 32,
               "sink owner C offset");
_Static_assert(_Generic(&gti_rt_thread_quarantine_v1,
                   void (*)(gti_thread_state_v1 **): 1,
                   default: 0),
               "quarantine C signature");
_Static_assert(_Generic(&gti_rt_thread_program_storage_retirement_ready_v1,
                   uint32_t (*)(void): 1,
                   default: 0),
               "retirement C signature");
_Static_assert(_Generic(&gti_rt_thread_cleanup_failure_kind_v1,
                   gti_thread_worker_failure_kind_v1 (*)(void): 1,
                   default: 0),
               "cleanup kind C signature");
_Static_assert(_Generic(&gti_rt_thread_cleanup_primary_copy_v2,
                   uint32_t (*)(gti_failure_record_v2 *,
                                const gti_failure_artifact_descriptor_v1 **): 1,
                   default: 0),
               "primary copy C signature");

static void copy_record(void *context,
                        const gti_thread_failure_view_v1 *failure) {
  memcpy(context, failure->ordinary, sizeof(gti_failure_record_v2));
}

int gti_test_thread_outcome_c(void) {
  gti_thread_failure_sink_v1 sink = {
      .abi_version = GTI_THREAD_FAILURE_SINK_ABI_VERSION_V1};
  gti_failure_record_v2 source = {.abi_version = GTI_FAILURE_ABI_VERSION_V2,
                                  .code = GTI_FAILURE_CODE_PANIC_V2,
                                  .detail =
                                      GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2,
                                  .message_size = 3,
                                  .message = {'C', '1', '7'}};
  gti_failure_record_v2 copy = {0};
  const gti_thread_failure_view_v1 view = {
      .abi_version = GTI_THREAD_FAILURE_VIEW_ABI_VERSION_V1,
      .failure_kind = GTI_THREAD_WORKER_FAILURE_ORDINARY_V1,
      .ordinary = &source};
  void (*enter)(gti_thread_failure_sink_v1 *, gti_thread_failure_publish_v1,
                void *) = gti_rt_thread_failure_sink_enter_v1;
  enter(&sink, copy_record, &copy);
  gti_rt_thread_failure_publish_v1(&view);
  gti_rt_thread_failure_sink_leave_v1(&sink);
  return memcmp(&source, &copy, sizeof(copy)) == 0 && sink.owner == NULL;
}
