#include "gti/driver/process.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
namespace {
enum class Fault { None, SeekEnd, SeekStart, Tell, Read, TruncatedRead, Close };
Fault armed = Fault::None;
bool injected = false;

bool inject(Fault fault) {
  if (armed != fault)
    return false;
  armed = Fault::None;
  injected = true;
  errno = EIO;
  return true;
}

template <class Function> Function original(const char *name) {
  auto function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
  if (!function)
    std::abort();
  return function;
}

// Match libc's exception specifications on both Darwin and Linux.
constexpr bool seekNoexcept = noexcept(std::fseek(nullptr, 0, 0));
constexpr bool tellNoexcept = noexcept(std::ftell(nullptr));
constexpr bool readNoexcept = noexcept(std::fread(nullptr, 0, 0, nullptr));
constexpr bool closeNoexcept = noexcept(std::fclose(nullptr));
} // namespace

extern "C" int fseek(std::FILE *stream, long offset,
                     int origin) noexcept(seekNoexcept) {
  static auto real = original<decltype(&::fseek)>("fseek");
  if ((origin == SEEK_END && inject(Fault::SeekEnd)) ||
      (origin == SEEK_SET && inject(Fault::SeekStart)))
    return -1;
  return real(stream, offset, origin);
}
extern "C" long ftell(std::FILE *stream) noexcept(tellNoexcept) {
  static auto real = original<decltype(&::ftell)>("ftell");
  if (inject(Fault::Tell))
    return -1;
  return real(stream);
}
extern "C" std::size_t fread(void *destination, std::size_t size,
                             std::size_t count,
                             std::FILE *stream) noexcept(readNoexcept) {
  static auto real = original<decltype(&::fread)>("fread");
  if (armed == Fault::TruncatedRead && size == 1 && count > 3) {
    // A real short file reports EOF without ferror. It must not become a
    // complete capture after the parent already measured the larger extent.
    if (ftruncate(fileno(stream), 3) != 0)
      std::abort();
    inject(Fault::TruncatedRead);
    errno = 0;
    return real(destination, size, count, stream);
  }
  if (armed == Fault::Read && size == 1 && count > 3) {
    // Return a useful prefix, then force libc itself to set the FILE error
    // indicator. A projected ferror() symbol is insufficient when libc headers
    // inline that query (as optimized glibc builds may do).
    const auto partial = real(destination, size, 3, stream);
    if (partial != 3 || std::fseek(stream, 0, SEEK_END) != 0)
      std::abort();
    // Seeking to the end leaves no buffered input to satisfy the next read.
    // Replace the underlying descriptor with a valid write-only descriptor so
    // the next real fread reaches the kernel and fails with EBADF.
    const int writeOnly = open("/dev/null", O_WRONLY);
    if (writeOnly == -1 || dup2(writeOnly, fileno(stream)) == -1)
      std::abort();
    close(writeOnly);
    errno = 0;
    const auto rejected =
        real(static_cast<char *>(destination) + partial, 1, 1, stream);
    const int readError = errno;
    if (rejected != 0 || std::ferror(stream) == 0 || readError != EBADF)
      std::abort();
    inject(Fault::Read);
    errno = readError;
    return partial;
  }
  return real(destination, size, count, stream);
}
extern "C" int fclose(std::FILE *stream) noexcept(closeNoexcept) {
  static auto real = original<decltype(&::fclose)>("fclose");
  const bool fail = armed == Fault::Close;
  const auto result = real(stream);
  if (fail) {
    inject(Fault::Close);
    return EOF;
  }
  return result;
}
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  return 77;
#else
  if (argc == 4 && std::string(argv[1]) == "--child") {
    if (std::string(argv[2]) == "output")
      std::fputs("child-output\n", stdout);
    return std::atoi(argv[3]);
  }
  using namespace lang::driver;
  int failures = 0;
  auto check = [&](bool condition, const std::string &message) {
    if (!condition) {
      ++failures;
      std::cerr << message << '\n';
    }
  };
  const auto executable = std::filesystem::absolute(argv[0]).string();
  auto run = [&](Fault fault, int childStatus, bool empty = false,
                 bool captureSuccess = true) {
    auto child =
        startProcess({executable, "--child", empty ? "empty" : "output",
                      std::to_string(childStatus)},
                     {.captureSuccessfulOutput = captureSuccess,
                      .timeout = std::chrono::seconds(5),
                      .isolateProcessGroup = true});
    injected = false;
    armed = fault;
    auto result = child.wait();
    check(fault == Fault::None || injected,
          "requested capture fault was not injected");
    armed = Fault::None;
    return result;
  };
  for (int childStatus : {0, 23, 127}) {
    for (auto [fault, operation] :
         {std::pair{Fault::SeekEnd, "seek"},
          std::pair{Fault::SeekStart, "seek"}, std::pair{Fault::Tell, "size"},
          std::pair{Fault::Read, "read"},
          std::pair{Fault::TruncatedRead, "read"},
          std::pair{Fault::Close, "close"}}) {
      const auto result = run(fault, childStatus);
      const std::string label =
          std::string(operation) + "/exit=" + std::to_string(childStatus);
      check(!result.succeeded(), label + " must never silently pass");
      check(result.exitCode == (childStatus == 0 ? 74 : childStatus),
            label + " must preserve genuine nonzero child status");
      check(result.driverDiagnostic &&
                result.driverDiagnostic->find(operation) != std::string::npos,
            label + " must identify the failed capture operation");
      if (fault == Fault::Read || fault == Fault::TruncatedRead)
        check(result.output == "chi",
              label + " must preserve partial captured bytes");
      if (fault == Fault::Close)
        check(result.output == "child-output\n",
              label + " must preserve complete captured bytes");
    }
  }
  auto result = run(Fault::None, 0, true);
  check(result.succeeded() && result.output.empty(),
        "valid empty capture must pass");
  result = run(Fault::None, 0);
  check(result.succeeded() && result.output == "child-output\n",
        "ordinary capture must pass");
  result = run(Fault::None, 127);
  check(result.exitCode == 127 && !result.driverDiagnostic &&
            result.output == "child-output\n",
        "a genuine child exit127 must not become a launch/capture error");
  result = run(Fault::Close, 0, true);
  check(result.exitCode == 74 && result.driverDiagnostic,
        "closing an empty capture can still fail");
  result = run(Fault::Close, 0, false, false);
  check(result.exitCode == 74 && result.driverDiagnostic &&
            result.output.empty(),
        "close errors must fail even when successful output was not requested");
  return failures == 0 ? 0 : 1;
#endif
}
