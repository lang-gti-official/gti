#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int gti_rt_write_stdout(const char *data, size_t length);
int32_t gti_rt_read_stdin_byte(void);
int64_t gti_rt_open_file_read(const char *path, size_t length);
int32_t gti_rt_read_file_byte(int64_t descriptor);
int32_t gti_rt_close_file(int64_t descriptor);

#ifdef __cplusplus
}
#endif
