#include "gti/runtime_execution.h"
#include "gti/runtime_thread.hpp"

extern "C" void gti_rt_thread_locals_begin_v1(void) {
  gti_runtime_detail::begin_thread_local_storage_lifetime();
}

extern "C" void gti_rt_thread_locals_drain_v2(
    const gti_failure_record_v2 *primary,
    const gti_failure_artifact_descriptor_v1 *primary_artifact) {
  gti_runtime_detail::drain_thread_local_cleanup(primary, primary_artifact);
}

extern "C" uint32_t gti_rt_thread_locals_drain_checked_v3(
    gti_failure_record_v2 *failure,
    const gti_failure_artifact_descriptor_v1 **artifact) {
  return gti_runtime_detail::drain_thread_local_cleanup_checked(failure,
                                                                artifact);
}

extern "C" uint32_t gti_rt_thread_locals_attached_v1(void) {
  return gti_runtime_detail::thread_local_storage_lifetime_active();
}

extern "C" uint32_t gti_rt_thread_locals_closing_v1(void) {
  return gti_runtime_detail::thread_local_cleanup_active();
}

extern "C" void
gti_rt_thread_locals_register_v1(gti_thread_local_cleanup_entry_v1 *entry) {
  gti_runtime_detail::register_thread_local_cleanup(entry);
}
