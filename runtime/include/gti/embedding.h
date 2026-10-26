#ifndef GTI_EMBEDDING_H
#define GTI_EMBEDDING_H

#include "gti/runtime_failure.h"
#include <stdint.h>

#define GTI_EMBED_ABI_VERSION_V1 UINT32_C(1)
#define GTI_EMBED_IDENTITY_SIZE_V1 UINT32_C(32)

typedef uint32_t gti_embed_status_v1;
enum {
  GTI_EMBED_STATUS_OK_V1 = 0,
  GTI_EMBED_STATUS_DEFINED_FAILURE_V1 = 1,
  GTI_EMBED_STATUS_INVALID_ARGUMENT_V1 = 2,
  GTI_EMBED_STATUS_ABI_MISMATCH_V1 = 3,
  GTI_EMBED_STATUS_CONTEXT_BUSY_V1 = 4,
  GTI_EMBED_STATUS_WRONG_THREAD_V1 = 5,
  GTI_EMBED_STATUS_CONTEXT_POISONED_V1 = 6,
  GTI_EMBED_STATUS_CONTEXT_INACTIVE_V1 = 7,
};

typedef struct gti_context_v1 gti_context_v1;
typedef struct gti_export_descriptor_v1 {
  gti_c_string_view symbol;
  uint8_t signature_identity[GTI_EMBED_IDENTITY_SIZE_V1];
} gti_export_descriptor_v1;
typedef struct gti_module_descriptor_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  uint8_t module_identity[GTI_EMBED_IDENTITY_SIZE_V1];
  const gti_failure_artifact_descriptor_v1 *failure_artifact;
  const gti_export_descriptor_v1 *exports;
  uint32_t export_count;
  uint32_t reserved;
} gti_module_descriptor_v1;

#ifdef __cplusplus
extern "C" {
#endif
const gti_module_descriptor_v1 *gti_get_module_descriptor_v1(void);
/* Generated per image. Output pointers must be valid, aligned host storage.
 * out_context/out_failure are required; only success publishes a context.
 * The native infallible profile retains the optional observer but cannot
 * invoke it without a failure. One live context belongs to its creating
 * thread. Destroy it before unloading; descriptor bytes and code pointers
 * must never be used after unloading. */
gti_embed_status_v1 gti_context_create_v1(uint32_t requested_abi,
                                          gti_failure_observer_v1 observer,
                                          void *observer_context,
                                          gti_context_v1 **out_context,
                                          gti_failure_record_v1 *out_failure);
gti_embed_status_v1 gti_context_destroy_v1(gti_context_v1 *context,
                                           gti_failure_record_v1 *out_failure);
#ifdef __cplusplus
}
#endif
#endif
