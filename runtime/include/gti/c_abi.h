#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gti_c_string_view {
  const char *data;
  uint64_t length;
} gti_c_string_view;

#ifdef __cplusplus
}
#endif
