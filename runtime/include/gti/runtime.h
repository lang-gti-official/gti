#pragma once

#include "gti/c_abi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int32_t gti_rt_write_stdout(gti_c_string_view value);
int32_t gti_rt_read_stdin_byte(void);
int64_t gti_rt_open_file_read(gti_c_string_view path);
int32_t gti_rt_read_file_byte(int64_t descriptor);
int32_t gti_rt_close_file(int64_t descriptor);

#ifdef __cplusplus
}
#endif
