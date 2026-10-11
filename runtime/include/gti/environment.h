#pragma once

#include "gti/c_abi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  GTI_ENVIRONMENT_OK_V1 = 0,
  GTI_ENVIRONMENT_INVALID_NAME_V1 = 1,
  GTI_ENVIRONMENT_UNSUPPORTED_V1 = 2,
  GTI_ENVIRONMENT_OUT_OF_MEMORY_V1 = 3,
};

// Counted native bytes; a nonnull name must cover length readable bytes and the
// output pointer must be writable. Names are nonempty with neither NUL nor '='.
// Input is never retained. A successful missing lookup publishes handle zero;
// a present value (including an empty one) publishes a unique immutable copy.
// On failure *snapshot is zero. Caller must exclude concurrent native mutation
// of the process environment while get runs. Concurrent read-only calls work.
uint32_t gti_rt_environment_get_v1(gti_c_string_view name, int64_t *snapshot);

// Handles are private process-local pointers, not public GTI values. A nonzero
// handle must be live and returned by get. Its bytes remain valid until
// destroy, not until the next environment query. size/data do not allocate or
// mutate.
uint64_t gti_rt_environment_snapshot_size_v1(int64_t snapshot);
const void *gti_rt_environment_snapshot_data_v1(int64_t snapshot);
// Null is accepted. Each nonzero handle must be destroyed exactly once.
void gti_rt_environment_snapshot_destroy_v1(int64_t snapshot);

#ifdef __cplusplus
}
#endif
