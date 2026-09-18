#pragma once

#include "gti/c_abi.h"
#include "gti/math.h"
#include "gti/random.h"
#include "gti/runtime_failure.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// IO
typedef uint32_t gti_file_open_mode_v1;
enum {
  GTI_FILE_OPEN_READ_V1 = 0,
  GTI_FILE_OPEN_WRITE_V1 = 1,
  GTI_FILE_OPEN_APPEND_V1 = 2,
  GTI_FILE_OPEN_READ_WRITE_V1 = 3,
  GTI_FILE_OPEN_READ_WRITE_TRUNCATE_V1 = 4,
  GTI_FILE_OPEN_READ_APPEND_V1 = 5,
};

typedef uint32_t gti_file_seek_origin_v1;
enum {
  GTI_FILE_SEEK_BEGIN_V1 = 0,
  GTI_FILE_SEEK_CURRENT_V1 = 1,
  GTI_FILE_SEEK_END_V1 = 2,
};

int32_t gti_rt_write_stdout(gti_c_string_view value);
int32_t gti_rt_write_stdout_byte(uint8_t value);
int64_t gti_rt_write_stdout_bytes_v1(gti_c_string_view value);
int64_t gti_rt_write_stderr_bytes_v1(gti_c_string_view value);
int32_t gti_rt_flush_stdout_v1(void);
int32_t gti_rt_flush_stderr_v1(void);
int32_t gti_rt_read_stdin_byte(void);
int64_t gti_rt_read_stdin_bytes_v1(uint8_t *output, uint64_t capacity);
int64_t gti_rt_open_file_read(gti_c_string_view path);
int64_t gti_rt_open_file_v1(gti_c_string_view path, gti_file_open_mode_v1 mode);
int32_t gti_rt_read_file_byte(int64_t descriptor);
int64_t gti_rt_read_file_v1(int64_t descriptor, uint8_t *output,
                            uint64_t capacity);
int64_t gti_rt_write_file_v1(int64_t descriptor, gti_c_string_view value);
int64_t gti_rt_seek_file_v1(int64_t descriptor, int64_t offset,
                            gti_file_seek_origin_v1 origin);
int32_t gti_rt_close_file(int64_t descriptor);

#ifdef __cplusplus
}
#endif
