#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

using gti_c_string_view = struct gti_c_string_view {
  const char *data;
  uint64_t length;
};

#ifdef __cplusplus
}
#endif
