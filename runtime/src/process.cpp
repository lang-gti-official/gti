#include "gti/runtime.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

bool decodeArguments(gti_c_string_view arguments,
                     std::vector<std::string> &storage) {
  if (arguments.data == nullptr || arguments.length == 0 ||
      arguments.length > SIZE_MAX ||
      arguments.data[arguments.length - 1] != '\0') {
    return false;
  }

  storage.clear();
  std::size_t start = 0;
  const std::size_t length = static_cast<std::size_t>(arguments.length);
  while (start < length) {
    std::size_t end = start;
    while (end < length && arguments.data[end] != '\0') {
      ++end;
    }
    if (end == length) {
      return false;
    }
    storage.emplace_back(arguments.data + start, end - start);
    start = end + 1;
  }
  return !storage.empty() && !storage.front().empty();
}

#if !defined(_WIN32)

bool setCloseOnExec(int descriptor) {
  const int current = fcntl(descriptor, F_GETFD);
  return current != -1 &&
         fcntl(descriptor, F_SETFD, current | FD_CLOEXEC) != -1;
}

void writeExecError(int descriptor, int value) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(&value);
  std::size_t offset = 0;
  while (offset < sizeof(value)) {
    const ssize_t written =
        write(descriptor, bytes + offset, sizeof(value) - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written == -1 && errno == EINTR) {
      continue;
    }
    return;
  }
}

bool readExecError(int descriptor, int &value, bool &hasValue,
                   int &nativeCode) {
  auto *bytes = reinterpret_cast<unsigned char *>(&value);
  std::size_t offset = 0;
  hasValue = false;
  while (offset < sizeof(value)) {
    const ssize_t received =
        read(descriptor, bytes + offset, sizeof(value) - offset);
    if (received > 0) {
      hasValue = true;
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received == 0) {
      return !hasValue || offset == sizeof(value);
    }
    if (errno == EINTR) {
      continue;
    }
    nativeCode = errno;
    return false;
  }
  return true;
}

#endif

} // namespace

extern "C" gti_process_error_v1 gti_rt_process_run_v1(
    gti_c_string_view arguments, std::int32_t *terminationKind,
    std::int32_t *terminationValue, std::int32_t *nativeCode) {
  if (terminationKind == nullptr || terminationValue == nullptr ||
      nativeCode == nullptr) {
    return GTI_PROCESS_INVALID_ARGUMENT_V1;
  }
  *terminationKind = -1;
  *terminationValue = -1;
  *nativeCode = 0;

  try {
    std::vector<std::string> storage;
    if (!decodeArguments(arguments, storage)) {
      return GTI_PROCESS_INVALID_ARGUMENT_V1;
    }

#if defined(_WIN32)
    return GTI_PROCESS_UNSUPPORTED_V1;
#else
    std::vector<char *> argv;
    argv.reserve(storage.size() + 1);
    for (std::string &argument : storage) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    int errorPipe[2]{-1, -1};
    if (pipe(errorPipe) == -1) {
      *nativeCode = errno;
      return GTI_PROCESS_SPAWN_FAILED_V1;
    }
    if (!setCloseOnExec(errorPipe[1])) {
      *nativeCode = errno;
      (void)close(errorPipe[0]);
      (void)close(errorPipe[1]);
      return GTI_PROCESS_SPAWN_FAILED_V1;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t child = fork();
    if (child == -1) {
      *nativeCode = errno;
      (void)close(errorPipe[0]);
      (void)close(errorPipe[1]);
      return GTI_PROCESS_SPAWN_FAILED_V1;
    }
    if (child == 0) {
      (void)close(errorPipe[0]);
      execvp(argv.front(), argv.data());
      const int executeError = errno;
      writeExecError(errorPipe[1], executeError);
      (void)close(errorPipe[1]);
      _exit(127);
    }

    (void)close(errorPipe[1]);
    int executeError = 0;
    bool hasExecuteError = false;
    int pipeError = 0;
    const bool readSucceeded =
        readExecError(errorPipe[0], executeError, hasExecuteError, pipeError);
    (void)close(errorPipe[0]);

    int status = 0;
    while (waitpid(child, &status, 0) == -1) {
      if (errno == EINTR) {
        continue;
      }
      *nativeCode = errno;
      return GTI_PROCESS_WAIT_FAILED_V1;
    }
    if (!readSucceeded) {
      *nativeCode = pipeError;
      return GTI_PROCESS_WAIT_FAILED_V1;
    }
    if (hasExecuteError) {
      *nativeCode = executeError;
      return GTI_PROCESS_EXECUTE_FAILED_V1;
    }
    if (WIFEXITED(status)) {
      *terminationKind = GTI_PROCESS_TERMINATION_EXITED_V1;
      *terminationValue = WEXITSTATUS(status);
      return GTI_PROCESS_OK_V1;
    }
    if (WIFSIGNALED(status)) {
      *terminationKind = GTI_PROCESS_TERMINATION_SIGNALED_V1;
      *terminationValue = WTERMSIG(status);
      return GTI_PROCESS_OK_V1;
    }
    return GTI_PROCESS_WAIT_FAILED_V1;
#endif
  } catch (const std::bad_alloc &) {
    // C ABI entry points must not allow allocation failures to escape.
    *nativeCode = ENOMEM;
    return GTI_PROCESS_SPAWN_FAILED_V1;
  } catch (...) {
    // No C++ exception may cross the runtime's C ABI.
    return GTI_PROCESS_SPAWN_FAILED_V1;
  }
}

extern "C" std::int32_t gti_rt_run_process_v1(gti_c_string_view arguments) {
  std::int32_t terminationKind = -1;
  std::int32_t terminationValue = -1;
  std::int32_t nativeCode = 0;
  const gti_process_error_v1 result = gti_rt_process_run_v1(
      arguments, &terminationKind, &terminationValue, &nativeCode);
  (void)nativeCode;
  if (result != GTI_PROCESS_OK_V1) {
    return 127;
  }
  if (terminationKind == GTI_PROCESS_TERMINATION_EXITED_V1) {
    return terminationValue;
  }
  if (terminationKind == GTI_PROCESS_TERMINATION_SIGNALED_V1) {
    return 128 + terminationValue;
  }
  return 127;
}
