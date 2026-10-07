#pragma once

#include "gti/c_abi.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Versioned host filesystem transport. Inputs are counted native path bytes
// (UTF-8 on Windows), must be nonempty, and may not contain NUL. No input is
// retained. Output pointers must be valid and distinct; published only on
// success. Integer handles are private, uniquely owned, process-local pointers.
enum {
  GTI_FS_OK_V1 = 0,
  GTI_FS_INVALID_ARGUMENT_V1 = 1,
  GTI_FS_NOT_FOUND_V1 = 2,
  GTI_FS_PERMISSION_DENIED_V1 = 3,
  GTI_FS_ALREADY_EXISTS_V1 = 4,
  GTI_FS_NOT_DIRECTORY_V1 = 5,
  GTI_FS_IS_DIRECTORY_V1 = 6,
  GTI_FS_DIRECTORY_NOT_EMPTY_V1 = 7,
  GTI_FS_TOO_MANY_SYMLINKS_V1 = 8,
  GTI_FS_NO_SPACE_V1 = 9,
  GTI_FS_READ_ONLY_V1 = 10,
  GTI_FS_CROSS_DEVICE_V1 = 11,
  GTI_FS_NAME_TOO_LONG_V1 = 12,
  GTI_FS_OUT_OF_MEMORY_V1 = 13,
  GTI_FS_IO_ERROR_V1 = 14,
  GTI_FS_UNSUPPORTED_V1 = 15,
};

// File type: 0 not_found, 1 regular, 2 directory, 3 symlink, 4 block,
// 5 character, 6 fifo, 7 socket, 8 unknown. follow is exactly 0 or 1.
uint32_t gti_rt_fs_status_v1(gti_c_string_view path, uint32_t follow,
                             uint32_t *type);
// Measure: 0 file_size, 1 is_empty.
uint32_t gti_rt_fs_measure_v1(gti_c_string_view path, uint32_t operation,
                              uint64_t *value);
uint32_t gti_rt_fs_equivalent_v1(gti_c_string_view left,
                                 gti_c_string_view right, uint32_t *value);
// Mutation: 0 create_directory, 1 create_directories, 2 remove, 3 remove_all,
// 4 rename, 5 copy_file, 6 create_symlink, 7 create_directory_symlink,
// 8 current_path setter, 9 create_hard_link.
// copy option: 0 fail if existing, 1 skip existing, 2 overwrite existing.
// Only two-path operations inspect second; only copy_file accepts options.
uint32_t gti_rt_fs_mutate_v1(uint32_t operation, gti_c_string_view first,
                             gti_c_string_view second, uint32_t option,
                             uint64_t *value);
// Owned text: 0 current_path (ignores input), 1 absolute, 2 canonical,
// 3 weakly_canonical, 4 temp_directory_path (ignores input), 5 read_symlink.
uint32_t gti_rt_fs_path_v1(uint32_t operation, gti_c_string_view input,
                           int64_t *result);
uint32_t gti_rt_fs_directory_open_v1(gti_c_string_view path, int64_t *result);
// Begins before the first entry. Each successful next publishes 1 plus a
// current entry, or 0 at EOF. An increment error ends traversal. No recursion.
uint32_t gti_rt_fs_directory_next_v1(int64_t result, uint32_t *available);
uint64_t gti_rt_fs_result_size_v1(int64_t result);
// Reads a stable current text snapshot without advancing the iterator.
uint64_t gti_rt_fs_result_read_v1(int64_t result, uint64_t offset,
                                  uint8_t *output, uint64_t capacity);
void gti_rt_fs_result_destroy_v1(int64_t result);

#ifdef __cplusplus
}
#endif
