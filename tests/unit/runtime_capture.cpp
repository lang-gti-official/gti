// Bounded process capture preserves bytes/status and cleans up child groups;
// private temporary-directory creation is atomic, unique and mode 0700.
#include "gti/filesystem.h"
#include "gti/runtime.h"
#include "unit_test.h"
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
namespace {
int failures = 0;
void expect(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}
gti_c_string_view view(const std::string &value) {
  return {value.data(), value.size()};
}
std::string args(std::initializer_list<std::string> values) {
  std::string result;
  for (const auto &value : values) {
    result += value;
    result += '\0';
  }
  return result;
}
struct Output {
  int64_t handle = 0;
  int32_t native = 0;
  uint32_t error = 0;
  Output(const std::string &command, uint64_t timeout = 2000,
         uint64_t limit = 1048576, const std::string &cwd = "") {
    error = gti_rt_process_output_v1(view(command), view(cwd), timeout, limit,
                                     &handle, &native);
  }
  ~Output() { gti_rt_process_output_destroy_v1(handle); }
  int64_t info(uint32_t field) {
    return gti_rt_process_output_info_v1(handle, field);
  }
  std::string text(uint32_t stream) {
    std::string result(static_cast<size_t>(info(stream + 2)), '\0');
    auto n = gti_rt_process_output_read_v1(
        handle, stream, 0, reinterpret_cast<uint8_t *>(result.data()),
        result.size());
    expect(n == result.size(), "capture read must return exact byte count");
    return result;
  }
};
std::string pathText(int64_t handle) {
  std::string result(gti_rt_fs_result_size_v1(handle), '\0');
  gti_rt_fs_result_read_v1(
      handle, 0, reinterpret_cast<uint8_t *>(result.data()), result.size());
  gti_rt_fs_result_destroy_v1(handle);
  return result;
}
} // namespace
GTI_UNIT_TEST(runtime_capture_and_temporary_directory) {
#if defined(_WIN32)
  Output unavailable(args({"cmd", "/c", "exit 0"}));
  expect(unavailable.error == GTI_PROCESS_UNSUPPORTED_V1,
         "Windows must fail explicitly");
#else
  Output exact(args(
      {"/bin/sh", "-c",
       "printf '%s|%s|%s' \"$1\" \"$2\" \"$3\"; printf 'error' >&2; exit 23",
       "fixture", "two words", "", "$(false)"}));
  expect(exact.error == 0 && exact.info(0) == 0 && exact.info(1) == 23 &&
             exact.info(4) == 0,
         "ordinary nonzero exit must remain a captured result");
  expect(exact.text(0) == "two words||$(false)" && exact.text(1) == "error",
         "argv and streams must remain distinct");
  Output binary(args({"/bin/sh", "-c", "printf 'a\\000b'"}));
  expect(binary.error == 0 && binary.text(0) == std::string("a\0b", 3),
         "capture must preserve embedded NUL");
  Output both(args({"/bin/sh", "-c",
                    "i=0; while [ $i -lt 10000 ]; do printf xxxxxxxxxx; printf "
                    "yyyyyyyyyy >&2; i=$((i+1)); done"}));
  expect(both.error == 0 && both.text(0) == std::string(100000, 'x') &&
             both.text(1) == std::string(100000, 'y'),
         "both pipes must drain beyond pipe capacity without deadlock");
  Output bounded(
      args({"/bin/sh", "-c", "while :; do printf 'a long output'; done"}), 2000,
      111);
  expect(bounded.error == 0 && bounded.info(4) == 2 &&
             bounded.text(0).size() <= 111,
         "output overflow must terminate capture and retain bounded bytes");
  Output timeout(args({"/bin/sh", "-c", "sleep 30"}), 30);
  expect(timeout.error == 0 && timeout.info(4) == 1 && timeout.info(0) == 1,
         "deadline must kill and reap the child, distinct from signal status");
  Output signal(args({"/bin/sh", "-c", "kill -TERM $$"}));
  expect(signal.error == 0 && signal.info(0) == 1 &&
             signal.info(1) == SIGTERM && signal.info(4) == 0,
         "natural signal must remain distinct from a deadline");
  Output missing(args({"/gti-no-such-executable-328742"}));
  Output status127(args({"/bin/sh", "-c", "exit 127"}));
  expect(missing.error == GTI_PROCESS_EXECUTE_FAILED_V1 &&
             status127.error == 0 && status127.info(1) == 127,
         "exec failure must not masquerade as a child exit 127");
  Output invalid("unterminated");
  Output invalidLimit(args({"/bin/echo"}), 0);
  Output invalidDirectory(args({"/bin/echo"}), 1000, 10,
                          std::string("bad\0dir", 7));
  expect(invalid.error == GTI_PROCESS_INVALID_ARGUMENT_V1 &&
             invalidLimit.error == GTI_PROCESS_INVALID_ARGUMENT_V1 &&
             invalidDirectory.error == GTI_PROCESS_INVALID_ARGUMENT_V1,
         "invalid transport/options must fail before spawning");
  std::string parent = std::filesystem::temp_directory_path().string();
  int64_t first = 0, second = 0;
  expect(gti_rt_fs_temp_directory_v1(view(parent), &first) == 0 &&
             gti_rt_fs_temp_directory_v1(view(parent), &second) == 0,
         "private temporary directories must be created");
  std::string a = pathText(first), b = pathText(second);
  expect(a != b && std::filesystem::is_directory(a) &&
             std::filesystem::is_directory(b),
         "directory names must be unique");
  struct stat metadata{};
  expect(stat(a.c_str(), &metadata) == 0 && (metadata.st_mode & 0777) == 0700,
         "temporary directories must be private");
  auto original = std::filesystem::current_path();
  Output cwd(args({"/bin/pwd"}), 1000, 4096, a);
  expect(cwd.error == 0 && std::filesystem::canonical(
                               cwd.text(0).substr(0, cwd.text(0).size() - 1)) ==
                               std::filesystem::canonical(a),
         "working directory applies only to the child");
  expect(std::filesystem::current_path() == original,
         "parent current directory must remain unchanged");
  Output descendants(args({"/bin/sh", "-c", "(sleep 1; : > escaped) & exit 0"}),
                     2000, 4096, a);
  expect(descendants.error == 0 && descendants.info(1) == 0 &&
             descendants.info(4) == 0,
         "successful leader completion must clean up descendants");
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  expect(!std::filesystem::exists(std::filesystem::path(a) / "escaped"),
         "background descendant escaped cleanup");
  std::filesystem::remove_all(a);
  std::filesystem::remove_all(b);
  int64_t unused = 0;
  expect(gti_rt_fs_temp_directory_v1(view(std::string("bad\0path", 8)),
                                     &unused) == GTI_FS_INVALID_ARGUMENT_V1,
         "temporary-directory input must reject embedded NUL");
#endif
  return failures == 0 ? 0 : 1;
}
