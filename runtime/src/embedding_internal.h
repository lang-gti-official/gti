#pragma once

#include "gti/embedding.h"

// Image-local lifecycle mechanisms, not a GTI failure transport. Only generated
// infallible wrappers use these until the shared fallible boundary is selected.
extern "C" {
gti_embed_status_v1 gti_rt_embed_create_begin_v1(
    uint32_t abi, gti_failure_observer_v1 observer, void *observer_context,
    gti_context_v1 **out_context, gti_failure_record_v1 *out_failure) noexcept;
void gti_rt_embed_create_end_v1(gti_context_v1 **out_context) noexcept;
gti_embed_status_v1
gti_rt_embed_enter_v1(gti_context_v1 *context,
                      gti_failure_record_v1 *out_failure) noexcept;
void gti_rt_embed_leave_v1(gti_context_v1 *context) noexcept;
gti_embed_status_v1
gti_rt_embed_destroy_v1(gti_context_v1 *context,
                        gti_failure_record_v1 *out_failure) noexcept;
}
