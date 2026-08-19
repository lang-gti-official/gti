#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fills `out` with exactly `count` bytes drawn from the host entropy source.
// Returns 0 on success and a nonzero value when the host could not supply
// entropy, so the caller can raise a defined failure rather than proceed with
// unspecified bytes. A `count` of zero succeeds without touching `out`.
int32_t gti_rt_random_bytes(uint8_t *out, uint64_t count);

#ifdef __cplusplus
}
#endif
