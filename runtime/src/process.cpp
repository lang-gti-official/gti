#include "gti/runtime.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
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

extern "C" gti_process_error_v1
gti_rt_process_exec_v1(gti_c_string_view arguments, int32_t *nativeCode) {
  if (nativeCode == nullptr) {
    return GTI_PROCESS_INVALID_ARGUMENT_V1;
  }
  *nativeCode = 0;
  if (arguments.data == nullptr || arguments.length == 0 ||
      arguments.length > SIZE_MAX || arguments.data[0] == '\0' ||
      arguments.data[arguments.length - 1] != '\0') {
    return GTI_PROCESS_INVALID_ARGUMENT_V1;
  }
#if defined(_WIN32)
  return GTI_PROCESS_UNSUPPORTED_V1;
#else
  try {
    // POSIX execv does not modify argument bytes. Borrow the source-owned
    // buffer and allocate only the pointer table, never a second byte copy.
    std::size_t count = 0;
    for (std::size_t i = 0; i < arguments.length; ++i) {
      if (arguments.data[i] == '\0') {
        ++count;
      }
    }
    std::vector<char *> argv;
    argv.reserve(count + 1);
    argv.push_back(const_cast<char *>(arguments.data));
    for (std::size_t i = 0; i + 1 < arguments.length; ++i) {
      if (arguments.data[i] == '\0') {
        argv.push_back(const_cast<char *>(arguments.data + i + 1));
      }
    }
    argv.push_back(nullptr);
    ::execv(argv.front(), argv.data());
    // Snapshot before any cleanup can change the thread-local errno.
    *nativeCode = errno;
  } catch (const std::bad_alloc &) {
    *nativeCode = ENOMEM;
  } catch (const std::length_error &) {
    *nativeCode = E2BIG;
  }
  return GTI_PROCESS_EXECUTE_FAILED_V1;
#endif
}

namespace {
struct CapturedProcess {
  std::string streams[2];
  std::int32_t kind = -1;
  std::int32_t value = -1;
  std::int32_t stopped = 0;
};
CapturedProcess *captured(std::int64_t value) {
  return reinterpret_cast<CapturedProcess *>(static_cast<std::intptr_t>(value));
}
#if !defined(_WIN32)
struct CapturePipe {
  int ends[2]{-1, -1};
  ~CapturePipe() {
    closeEnd(0);
    closeEnd(1);
  }
  void closeEnd(int end) {
    if (ends[end] >= 0) {
      (void)close(ends[end]);
      ends[end] = -1;
    }
  }
  bool open() {
    if (pipe(ends))
      return false;
    for (auto &fd : ends) {
      // Keep child dup2 independent of whether the caller closed stdio.
      if (fd < 3) {
        int replacement = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (replacement < 0)
          return false;
        (void)close(fd);
        fd = replacement;
      }
      if (!setCloseOnExec(fd))
        return false;
    }
    return true;
  }
};
struct CaptureChild {
  pid_t pid = -1;
  bool owned = false;
  void stop() {
    if (owned) {
      (void)kill(-pid, SIGKILL);
      (void)kill(pid, SIGKILL);
    }
  }
  ~CaptureChild() {
    if (owned) {
      stop();
      while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
};
#endif
} // namespace

extern "C" gti_process_error_v1
gti_rt_process_output_v1(gti_c_string_view arguments,
                         gti_c_string_view workingDirectory,
                         std::uint64_t timeoutMs, std::uint64_t outputLimit,
                         std::int64_t *output, std::int32_t *nativeCode) {
  if (!output || !nativeCode)
    return GTI_PROCESS_INVALID_ARGUMENT_V1;
  *output = 0;
  *nativeCode = 0;
  if (timeoutMs == 0 || timeoutMs > 86400000 || outputLimit == 0 ||
      outputLimit > 67108864 || workingDirectory.length > SIZE_MAX ||
      (workingDirectory.length &&
       (!workingDirectory.data ||
        std::memchr(workingDirectory.data, 0, workingDirectory.length))))
    return GTI_PROCESS_INVALID_ARGUMENT_V1;
  try {
    std::vector<std::string> storage;
    if (!decodeArguments(arguments, storage))
      return GTI_PROCESS_INVALID_ARGUMENT_V1;
#if defined(_WIN32)
    return GTI_PROCESS_UNSUPPORTED_V1;
#else
    std::string cwd;
    if (workingDirectory.length)
      cwd.assign(workingDirectory.data, workingDirectory.length);
    std::vector<char *> argv;
    for (auto &arg : storage)
      argv.push_back(arg.data());
    argv.push_back(nullptr);
    auto result = std::make_unique<CapturedProcess>();
    CapturePipe pipes[3]; // stdout, stderr and pre-exec error transport
    for (auto &p : pipes) {
      if (!p.open()) {
        *nativeCode = errno;
        return GTI_PROCESS_SPAWN_FAILED_V1;
      }
      int flags = fcntl(p.ends[0], F_GETFL);
      if (flags < 0 || fcntl(p.ends[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        *nativeCode = errno;
        return GTI_PROCESS_SPAWN_FAILED_V1;
      }
    }
    CaptureChild child;
    child.pid = fork();
    if (child.pid < 0) {
      *nativeCode = errno;
      return GTI_PROCESS_SPAWN_FAILED_V1;
    }
    if (child.pid == 0) {
      for (auto &p : pipes)
        (void)close(p.ends[0]);
      auto fail = [&](int code) {
        writeExecError(pipes[2].ends[1], code);
        _exit(127);
      };
      if (setpgid(0, 0) < 0)
        fail(errno);
      int input = open("/dev/null", O_RDONLY);
      if (input < 0)
        fail(errno);
      if (dup2(input, STDIN_FILENO) < 0 ||
          dup2(pipes[0].ends[1], STDOUT_FILENO) < 0 ||
          dup2(pipes[1].ends[1], STDERR_FILENO) < 0)
        fail(errno);
      if (input > 2)
        (void)close(input);
      (void)close(pipes[0].ends[1]);
      (void)close(pipes[1].ends[1]);
      if (!cwd.empty() && chdir(cwd.c_str()) < 0)
        fail(errno);
      execvp(argv.front(), argv.data());
      fail(errno);
    }
    child.owned = true;
    (void)setpgid(child.pid, child.pid);
    for (auto &p : pipes)
      p.closeEnd(1);
    const auto start = std::chrono::steady_clock::now();
    bool observed = false;
    int executeError = 0;
    std::size_t errorSize = 0;
    std::uint64_t total = 0;
    while (true) {
      siginfo_t info{};
      int observation = waitid(P_PID, static_cast<id_t>(child.pid), &info,
                               WEXITED | WNOHANG | WNOWAIT);
      if (observation < 0 && errno != EINTR) {
        *nativeCode = errno;
        // Another reaper/SIGCHLD auto-reaping violates exclusive child
        // ownership. Do not signal a PID that may already have been reused.
        if (errno == ECHILD)
          child.owned = false;
        return GTI_PROCESS_WAIT_FAILED_V1;
      }
      if (observation == 0 && info.si_pid == child.pid && !observed) {
        observed = true;
        child.stop(); // kill descendants while unreaped leader reserves PID
      }
      if (std::chrono::steady_clock::now() - start >=
          std::chrono::milliseconds(timeoutMs)) {
        result->stopped = 1;
        child.stop();
        break;
      }
      pollfd fds[3]{};
      bool open = false;
      for (int i = 0; i < 3; ++i) {
        fds[i] = {pipes[i].ends[0], POLLIN, 0};
        open |= fds[i].fd >= 0;
      }
      if (!open && observed)
        break;
      int ready = poll(fds, 3, 10);
      if (ready < 0) {
        if (errno == EINTR)
          continue;
        *nativeCode = errno;
        return GTI_PROCESS_WAIT_FAILED_V1;
      }
      for (int i = 0; i < 3; ++i) {
        if (fds[i].fd < 0 ||
            !(fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
          continue;
        // One bounded read per stream per pass prevents an endlessly writing
        // stdout from starving stderr or the monotonic deadline.
        char bytes[16384];
        const ssize_t count = read(fds[i].fd, bytes, sizeof(bytes));
        if (count == 0) {
          pipes[i].closeEnd(0);
          continue;
        }
        if (count < 0) {
          if (errno == EINTR || errno == EAGAIN)
            continue;
          *nativeCode = errno;
          return GTI_PROCESS_WAIT_FAILED_V1;
        }
        if (i == 2) {
          if (static_cast<std::size_t>(count) >
              sizeof(executeError) - errorSize)
            return GTI_PROCESS_WAIT_FAILED_V1;
          std::memcpy(reinterpret_cast<char *>(&executeError) + errorSize,
                      bytes, count);
          errorSize += count;
        } else {
          const auto retained =
              std::min<std::uint64_t>(count, outputLimit - total);
          result->streams[i].append(bytes, retained);
          total += retained;
          if (retained != static_cast<std::uint64_t>(count)) {
            result->stopped = 2;
            child.stop();
            break;
          }
        }
      }
      if (result->stopped)
        break;
    }
    int status = 0;
    pid_t reaped;
    do {
      reaped = waitpid(child.pid, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    if (reaped < 0) {
      *nativeCode = errno;
      if (errno == ECHILD)
        child.owned = false;
      return GTI_PROCESS_WAIT_FAILED_V1;
    }
    child.owned = false;
    if (errorSize) {
      if (errorSize != sizeof(executeError))
        return GTI_PROCESS_WAIT_FAILED_V1;
      *nativeCode = executeError;
      return GTI_PROCESS_EXECUTE_FAILED_V1;
    }
    if (WIFEXITED(status)) {
      result->kind = 0;
      result->value = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      result->kind = 1;
      result->value = WTERMSIG(status);
    } else
      return GTI_PROCESS_WAIT_FAILED_V1;
    *output = static_cast<std::int64_t>(
        reinterpret_cast<std::intptr_t>(result.release()));
    return GTI_PROCESS_OK_V1;
#endif
  } catch (const std::bad_alloc &) {
    *nativeCode = ENOMEM;
    return GTI_PROCESS_SPAWN_FAILED_V1;
  } catch (...) {
    return GTI_PROCESS_SPAWN_FAILED_V1;
  }
}
extern "C" std::int64_t gti_rt_process_output_info_v1(std::int64_t value,
                                                      std::uint32_t selector) {
  const auto *p = captured(value);
  if (!p)
    return -1;
  switch (selector) {
  case 0:
    return p->kind;
  case 1:
    return p->value;
  case 2:
    return p->streams[0].size();
  case 3:
    return p->streams[1].size();
  case 4:
    return p->stopped;
  default:
    return -1;
  }
}
extern "C" std::uint64_t gti_rt_process_output_read_v1(std::int64_t value,
                                                       std::uint32_t stream,
                                                       std::uint64_t offset,
                                                       std::uint8_t *output,
                                                       std::uint64_t capacity) {
  const auto *p = captured(value);
  if (!p || stream > 1 || !output || offset > p->streams[stream].size())
    return 0;
  const auto count =
      std::min<std::uint64_t>(capacity, p->streams[stream].size() - offset);
  std::memcpy(output, p->streams[stream].data() + offset, count);
  return count;
}
extern "C" void gti_rt_process_output_destroy_v1(std::int64_t value) {
  delete captured(value);
}
