#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable scalar status vocabulary for the private <std/chrono> host boundary.
typedef int32_t gti_chrono_status_v1;
enum {
  GTI_CHRONO_OK_V1 = 0,
  GTI_CHRONO_UNAVAILABLE_V1 = 1,
  GTI_CHRONO_OUT_OF_RANGE_V1 = 2,
};

// Reads a monotonic clock whose epoch is intentionally unspecified. The value
// is suitable only for comparison with another reading from this process.
// On success, `output_nanoseconds` receives one signed nanosecond count. On
// failure, the output is unchanged.
gti_chrono_status_v1 gti_rt_chrono_steady_now_v1(int64_t *output_nanoseconds);

// Reads wall-clock time as signed nanoseconds since the Unix epoch
// (1970-01-01T00:00:00Z), excluding leap seconds. The signed 64-bit unit gives
// an exact representable range from 1677-09-21 through 2262-04-11. On failure,
// the output is unchanged.
gti_chrono_status_v1 gti_rt_chrono_system_now_v1(int64_t *output_nanoseconds);

#ifdef __cplusplus
}
#endif
