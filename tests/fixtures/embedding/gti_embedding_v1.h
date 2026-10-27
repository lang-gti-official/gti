#ifndef GTI_TEST_EMBEDDING_V1_H
#define GTI_TEST_EMBEDDING_V1_H

#include "gti/embedding.h"

#include <stdint.h>

/* Representative module-specific types and generated wrapper. */
typedef struct game_engine_v1 game_engine_v1;

typedef struct game_frame_v1 {
  int32_t frame;
  double delta_seconds;
} game_frame_v1;

#ifdef __cplusplus
extern "C" {
#endif

const gti_module_descriptor_v1 *gti_get_module_descriptor_v1(void);

gti_embed_status_v1 gti_context_create_v1(uint32_t requested_abi,
                                          gti_failure_observer_v1 observer,
                                          void *observer_context,
                                          gti_context_v1 **out_context,
                                          gti_failure_record_v1 *out_failure);

gti_embed_status_v1 gti_context_destroy_v1(gti_context_v1 *context,
                                           gti_failure_record_v1 *out_failure);

gti_embed_status_v1 game_update_v1(gti_context_v1 *context,
                                   game_engine_v1 *engine, game_frame_v1 frame,
                                   int32_t *out_result,
                                   gti_failure_record_v1 *out_failure);

#ifdef __cplusplus
}
#endif

#endif
