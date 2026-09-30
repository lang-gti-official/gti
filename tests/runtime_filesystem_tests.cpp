#include "gti/filesystem.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "runtime filesystem test failed: " << message << '\n';
    std::exit(1);
  }
}
gti_c_string_view view(std::string_view input) {
  return {input.data(), static_cast<uint64_t>(input.size())};
}
} // namespace

int main() {
  uint32_t type = 123;
  uint64_t count = 456;
  int64_t handle = 789;
  expect(gti_rt_fs_status_v1({nullptr, 1}, 1, &type) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             type == 123,
         "null input fails without publishing status");
  expect(gti_rt_fs_status_v1(view(""), 1, &type) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             type == 123,
         "empty input is rejected");
  expect(gti_rt_fs_status_v1(view("."), 2, &type) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             type == 123,
         "follow selector is checked");
  expect(gti_rt_fs_status_v1(view("."), 1, nullptr) ==
             GTI_FS_INVALID_ARGUMENT_V1,
         "status requires an output slot");
  expect(gti_rt_fs_status_v1(view(std::string_view(".\0ignored", 9)), 1,
                             &type) == GTI_FS_INVALID_ARGUMENT_V1,
         "embedded zero is never silently truncated");
  expect(gti_rt_fs_measure_v1(view("."), 9, &count) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             count == 456,
         "measurement operation is checked before output");
  expect(gti_rt_fs_mutate_v1(0, view("."), view(""), 1, &count) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             count == 456,
         "irrelevant copy flags cannot authorize mutation");
  expect(gti_rt_fs_mutate_v1(5, view("."), view("."), 3, &count) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             count == 456,
         "copy flags are bounded");
  expect(gti_rt_fs_mutate_v1(10, view("."), view(""), 0, &count) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             count == 456,
         "unknown mutation operation cannot dispatch");
  expect(gti_rt_fs_path_v1(6, view("."), &handle) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             handle == 789,
         "invalid resolution leaves ownership slot unchanged");
  expect(gti_rt_fs_directory_open_v1(view(""), &handle) ==
                 GTI_FS_INVALID_ARGUMENT_V1 &&
             handle == 789,
         "failed directory open leaves ownership slot unchanged");
  expect(gti_rt_fs_directory_next_v1(0, &type) == GTI_FS_INVALID_ARGUMENT_V1 &&
             type == 123,
         "null iterator does not publish an entry");

  expect(gti_rt_fs_path_v1(0, view(""), &handle) == GTI_FS_OK_V1 && handle != 0,
         "working directory produces one owned snapshot");
  const uint64_t length = gti_rt_fs_result_size_v1(handle);
  std::string observed;
  uint8_t bytes[3]{};
  for (uint64_t offset = 0; offset < length;) {
    const auto read =
        gti_rt_fs_result_read_v1(handle, offset, bytes, sizeof(bytes));
    expect(read > 0 && read <= sizeof(bytes),
           "chunked read makes bounded progress");
    observed.append(reinterpret_cast<const char *>(bytes),
                    static_cast<size_t>(read));
    offset += read;
  }
#if defined(_WIN32)
  const auto native = std::filesystem::current_path().u8string();
  expect(observed == std::string(reinterpret_cast<const char *>(native.data()),
                                 native.size()),
         "Windows snapshot uses UTF-8");
#else
  expect(observed == std::filesystem::current_path().native(),
         "snapshot preserves host path bytes");
#endif
  expect(gti_rt_fs_result_read_v1(handle, length, bytes, sizeof(bytes)) == 0,
         "read at exact end is empty");
  expect(gti_rt_fs_result_read_v1(handle, length + 1, bytes, sizeof(bytes)) ==
             0,
         "read beyond end is empty");
  expect(gti_rt_fs_result_read_v1(handle, 0, nullptr, 3) == 0,
         "null byte destination cannot be written");
  expect(gti_rt_fs_directory_next_v1(handle, &type) ==
             GTI_FS_INVALID_ARGUMENT_V1,
         "text ownership cannot impersonate a directory stream");
  gti_rt_fs_result_destroy_v1(handle);
  gti_rt_fs_result_destroy_v1(0);
  return 0;
}
