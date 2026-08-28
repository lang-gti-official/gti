#include "gti/runtime.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "runtime I/O test failure: " << message << '\n';
  ++failures;
}

gti_c_string_view view(std::string_view value) {
  return {.data = value.data(),
          .length = static_cast<std::uint64_t>(value.size())};
}

} // namespace

int main() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("gti-runtime-io-" + std::to_string(nonce) + ".bin");
  const std::string pathText = path.string();
  std::error_code removeError;
  std::filesystem::remove(path, removeError);

  const std::int64_t descriptor =
      gti_rt_open_file_v1(view(pathText), GTI_FILE_OPEN_READ_WRITE_TRUNCATE_V1);
  expect(descriptor >= 0, "read/write truncate should create a file");
  if (descriptor >= 0) {
    const std::array<char, 3> bytes{'A', '\0', 'Z'};
    expect(gti_rt_write_file_v1(
               descriptor, {.data = bytes.data(), .length = bytes.size()}) == 3,
           "counted writes must preserve embedded zero bytes");
    expect(gti_rt_seek_file_v1(descriptor, 0, GTI_FILE_SEEK_CURRENT_V1) == 3,
           "tell-through-seek should report the byte offset");
    expect(gti_rt_seek_file_v1(descriptor, 0, GTI_FILE_SEEK_BEGIN_V1) == 0,
           "seeking to the beginning should succeed");

    std::array<std::uint8_t, 8> output{};
    expect(gti_rt_read_file_v1(descriptor, output.data(), output.size()) == 3,
           "a chunk read should return the available byte count");
    expect(output[0] == 65 && output[1] == 0 && output[2] == 90,
           "chunk reads must preserve exact byte values");
    expect(gti_rt_read_file_v1(descriptor, output.data(), output.size()) == 0,
           "end of file should be an empty successful chunk");
    expect(gti_rt_read_file_v1(descriptor, nullptr, 0) == 0,
           "zero-capacity reads should not require a buffer");
    expect(gti_rt_write_file_v1(descriptor, {.data = nullptr, .length = 0}) ==
               0,
           "empty writes should not require a buffer");
    expect(gti_rt_close_file(descriptor) == 0,
           "closing an open descriptor should succeed");
    expect(gti_rt_close_file(descriptor) != 0,
           "closing the same descriptor twice should fail");
  }

  const std::int64_t appendDescriptor =
      gti_rt_open_file_v1(view(pathText), GTI_FILE_OPEN_APPEND_V1);
  expect(appendDescriptor >= 0, "append should open an existing file");
  if (appendDescriptor >= 0) {
    expect(gti_rt_write_file_v1(appendDescriptor, view("!")) == 1,
           "append writes should complete at the file end");
    expect(gti_rt_close_file(appendDescriptor) == 0,
           "the append descriptor should close normally");
  }

  const std::int64_t readDescriptor =
      gti_rt_open_file_v1(view(pathText), GTI_FILE_OPEN_READ_V1);
  expect(readDescriptor >= 0, "the completed file should reopen for reading");
  if (readDescriptor >= 0) {
    std::array<std::uint8_t, 4> output{};
    expect(gti_rt_read_file_v1(readDescriptor, output.data(), output.size()) ==
               4,
           "the reopened file should contain all written bytes");
    expect(output[0] == 65 && output[1] == 0 && output[2] == 90 &&
               output[3] == 33,
           "append must retain the original contents");
    expect(gti_rt_close_file(readDescriptor) == 0,
           "the read descriptor should close normally");
  }

  const std::string embeddedNull{"bad\0path", 8};
  expect(gti_rt_open_file_v1(view(embeddedNull), GTI_FILE_OPEN_READ_V1) < 0,
         "paths containing an embedded zero must be rejected");
  expect(gti_rt_open_file_v1(view(pathText), 99) < 0,
         "unknown open modes must be rejected");
  expect(gti_rt_seek_file_v1(-1, 0, GTI_FILE_SEEK_BEGIN_V1) < 0,
         "invalid descriptors must not be seekable");
  expect(gti_rt_seek_file_v1(0, 0, 99) < 0,
         "unknown seek origins must be rejected before host access");

#if !defined(_WIN32)
  int pipeDescriptors[2]{-1, -1};
  const int pipeStatus = ::pipe(pipeDescriptors);
  expect(pipeStatus == 0,
         "the broken-pipe regression fixture should create a pipe");
  if (pipeStatus == 0) {
    (void)::close(pipeDescriptors[0]);
    expect(gti_rt_write_file_v1(pipeDescriptors[1], view("x")) < 0,
           "a closed pipe must report write failure without SIGPIPE escape");
    (void)::close(pipeDescriptors[1]);
  }

  const int bufferingStatus = std::setvbuf(stdout, nullptr, _IONBF, 0);
  expect(bufferingStatus == 0,
         "stdout should become unbuffered for the broken-pipe probe");
  int stdoutPipe[2]{-1, -1};
  const int savedStdout = ::dup(STDOUT_FILENO);
  const int stdoutPipeStatus = savedStdout >= 0 ? ::pipe(stdoutPipe) : -1;
  expect(savedStdout >= 0 && stdoutPipeStatus == 0,
         "the stdout broken-pipe fixture should preserve stdout");
  if (bufferingStatus == 0 && savedStdout >= 0 && stdoutPipeStatus == 0) {
    (void)::close(stdoutPipe[0]);
    const bool installed = ::dup2(stdoutPipe[1], STDOUT_FILENO) >= 0;
    expect(installed, "the stdout probe should install its pipe");
    (void)::close(stdoutPipe[1]);
    if (installed) {
      const std::int64_t stdoutStatus = gti_rt_write_stdout_bytes_v1(view("x"));
      expect(::dup2(savedStdout, STDOUT_FILENO) >= 0,
             "the stdout probe should restore stdout");
      std::clearerr(stdout);
      expect(stdoutStatus < 0,
             "stdout should report a closed pipe without SIGPIPE escape");
    }
    (void)::close(savedStdout);
  } else {
    if (stdoutPipe[0] >= 0) {
      (void)::close(stdoutPipe[0]);
    }
    if (stdoutPipe[1] >= 0) {
      (void)::close(stdoutPipe[1]);
    }
    if (savedStdout >= 0) {
      (void)::close(savedStdout);
    }
  }
#endif

  std::filesystem::remove(path, removeError);
  if (removeError) {
    std::cerr << "runtime I/O test cleanup failure: " << removeError.message()
              << '\n';
    ++failures;
  }
  return failures == 0 ? 0 : 1;
}
