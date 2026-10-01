#pragma once

#include "gti/c_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Non-retained counted input. Formats: 0 general, 1 fixed, 2 scientific,
// 3 hexadecimal (without 0x). Status: 0 success, 1 invalid, 2 out of range.
// Output changes only on success; position is an absolute byte offset and is
// published after a well-formed request even for a conversion failure.
uint32_t gti_rt_parse_f32_v1(gti_c_string_view input, uint64_t offset,
                             uint32_t format, float *output,
                             uint64_t *position);
uint32_t gti_rt_parse_f64_v1(gti_c_string_view input, uint64_t offset,
                             uint32_t format, double *output,
                             uint64_t *position);

// Locale-independent binary floating-point formatting. Formats use the same
// 0 general, 1 fixed, 2 scientific, 3 hexadecimal mapping. A precision of -1
// selects the shortest representation for the requested format. Status: 0
// success, 1 invalid request, 2 output buffer too small.
uint32_t gti_rt_format_f32_v1(float value, uint32_t format, int32_t precision,
                              uint32_t flags, uint32_t fill, uint32_t alignment,
                              uint32_t sign, uint32_t zero_pad, uint64_t width,
                              uint8_t *output, uint64_t capacity,
                              uint64_t *size);
uint32_t gti_rt_format_f64_v1(double value, uint32_t format, int32_t precision,
                              uint32_t flags, uint32_t fill, uint32_t alignment,
                              uint32_t sign, uint32_t zero_pad, uint64_t width,
                              uint8_t *output, uint64_t capacity,
                              uint64_t *size);

#ifdef __cplusplus
}
#endif
