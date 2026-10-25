#pragma once

#include "gti/runtime_failure.h"
#include <stdbool.h>
#include <stddef.h>

/* Caller-owned, zero-initialized, address-stable until thread shutdown.
 * Runtime registration is intrusive and performs no allocation. */
#ifdef __cplusplus
#define GTI_TLS_DEFAULT(value) = value
#else
#define GTI_TLS_DEFAULT(value)
#endif
typedef struct gti_thread_local_cleanup_entry_v1 {
  struct gti_thread_local_cleanup_entry_v1 *previous GTI_TLS_DEFAULT(NULL);
  void *storage GTI_TLS_DEFAULT(NULL);
  bool (*cleanup)(void *, gti_failure_record_v2 *,
                  const gti_failure_record_v2 *,
                  const gti_failure_artifact_descriptor_v1 *) GTI_TLS_DEFAULT(NULL);
  const gti_failure_artifact_descriptor_v1 *artifact GTI_TLS_DEFAULT(NULL);
  bool registered GTI_TLS_DEFAULT(false);
} gti_thread_local_cleanup_entry_v1;
#undef GTI_TLS_DEFAULT

#ifdef __cplusplus
extern "C" {
#endif

/* Generated hosted entry establishes an attached lifetime before any dynamic
 * TLS initialization, even when the program currently owns no TLS objects.
 * Misordered/repeated begin is a runtime contract violation. No exception may
 * cross either C boundary. Drain is idempotent after completion and executes
 * registered cleanup in reverse successful-initialization order. With a primary
 * record, a secondary failure uses the existing cleanup-emergency protocol.
 * All records/descriptors are borrowed for the call; no ownership is retained.
 * Registration never transfers storage ownership to the runtime. */
void gti_rt_thread_locals_begin_v1(void);
void gti_rt_thread_locals_drain_v2(
    const gti_failure_record_v2 *primary,
    const gti_failure_artifact_descriptor_v1 *primary_artifact);
uint32_t gti_rt_thread_locals_attached_v1(void);
uint32_t gti_rt_thread_locals_closing_v1(void);
void gti_rt_thread_locals_register_v1(gti_thread_local_cleanup_entry_v1 *entry);

#ifdef __cplusplus
}
#endif
