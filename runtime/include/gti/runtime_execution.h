#pragma once

#include "gti/runtime_failure.h"

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
 * Typed LLVM TLS registration is a separate, later ABI capability. */
void gti_rt_thread_locals_begin_v1(void);
void gti_rt_thread_locals_drain_v2(
    const gti_failure_record_v2 *primary,
    const gti_failure_artifact_descriptor_v1 *primary_artifact);

#ifdef __cplusplus
}
#endif
