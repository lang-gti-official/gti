#pragma once

#include "gti/c_abi.h"
#include "gti/charconv.h"
#include "gti/chrono.h"
#include "gti/cthread.h"
#include "gti/filesystem.h"
#include "gti/http.h"
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

// Host process execution used by GTI-authored package build programs. The
// counted payload is one or more NUL-terminated argv elements, including a
// non-empty executable at index zero. The child inherits the caller's working
// directory, environment, and standard streams. Windows support is deferred
// with the rest of package build-script execution.
int32_t gti_rt_run_process_v1(gti_c_string_view arguments);

// Public <std/process> transport. The result is one GTI_PROCESS_* error. On
// success, termination_kind is GTI_PROCESS_TERMINATION_EXITED with an ordinary
// exit code or GTI_PROCESS_TERMINATION_SIGNALED with a signal number. The
// native code is zero on success and carries errno-style detail on a host
// failure; it is intentionally not part of the public GTI error vocabulary.
typedef uint32_t gti_process_error_v1;
enum {
  GTI_PROCESS_OK_V1 = 0,
  GTI_PROCESS_INVALID_ARGUMENT_V1 = 1,
  GTI_PROCESS_UNSUPPORTED_V1 = 2,
  GTI_PROCESS_SPAWN_FAILED_V1 = 3,
  GTI_PROCESS_EXECUTE_FAILED_V1 = 4,
  GTI_PROCESS_WAIT_FAILED_V1 = 5,
};

typedef uint32_t gti_process_termination_kind_v1;
enum {
  GTI_PROCESS_TERMINATION_EXITED_V1 = 0,
  GTI_PROCESS_TERMINATION_SIGNALED_V1 = 1,
};

gti_process_error_v1 gti_rt_process_run_v1(gti_c_string_view arguments,
                                           int32_t *termination_kind,
                                           int32_t *termination_value,
                                           int32_t *native_code);

#ifdef __cplusplus
}
#endif
