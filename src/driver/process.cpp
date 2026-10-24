#include "gti/driver/process.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace lang::driver {
namespace {

std::vector<char *>
processArguments(const std::vector<std::string> &arguments) {
  std::vector<char *> result;
  result.reserve(arguments.size() + 1);
  for (const std::string &argument : arguments) {
    result.push_back(const_cast<char *>(argument.c_str()));
  }
  result.push_back(nullptr);
  return result;
}

void captureError(ProcessResult &result, std::string_view operation,
                  int error) {
  if (!result.driverDiagnostic) {
    result.driverDiagnostic =
        "gti: failed to " + std::string(operation) +
        " captured process output: " + std::strerror(error == 0 ? EIO : error);
  }
  // Preserve the actual failure of an already-failing child, including a real
  // exit 127. Capture failure must never turn an otherwise successful child
  // into a passing test with missing output.
  if (result.exitCode == 0)
    result.exitCode = 74;
}

void readCapture(std::FILE *capture, ProcessResult &result) {
  errno = 0;
  if (std::fseek(capture, 0, SEEK_END) != 0) {
    captureError(result, "seek", errno);
    return;
  }
  errno = 0;
  const long capturedSize = std::ftell(capture);
  if (capturedSize < 0) {
    captureError(result, "determine size of", errno);
    return;
  }
  if (capturedSize == 0)
    return;
  errno = 0;
  if (std::fseek(capture, 0, SEEK_SET) != 0) {
    captureError(result, "seek", errno);
    return;
  }

  result.output.resize(static_cast<std::size_t>(capturedSize));
  std::size_t total = 0;
  while (total < result.output.size()) {
    errno = 0;
    const std::size_t size =
        std::fread(result.output.data() + total, sizeof(char),
                   result.output.size() - total, capture);
    const int readError = errno;
    total += size;
    if (std::ferror(capture) != 0) {
      captureError(result, "read", readError);
      break;
    }
    if (size == 0 || std::feof(capture) != 0) {
      // The seek/tell snapshot promised these bytes. A premature EOF or lack
      // of progress is a capture failure, not an empty successful read.
      if (total < result.output.size())
        captureError(result, "read", readError);
      break;
    }
  }
  result.output.resize(total);
}

ProcessResult finishCapturedProcess(std::FILE *capture, int status,
                                    bool captureSuccessfulOutput) {
  ProcessResult result{.exitCode = status};
  if (status != 0 || captureSuccessfulOutput)
    readCapture(capture, result);
  errno = 0;
  if (std::fclose(capture) != 0)
    captureError(result, "close", errno);
  return result;
}

#if !defined(_WIN32)
int observeChildExit(pid_t child, siginfo_t &info) {
  int result;
  do {
    // Darwin leaves info unchanged when WNOHANG finds no exited child.
    info = {};
    result = waitid(P_PID, static_cast<id_t>(child), &info,
                    WEXITED | WNOHANG | WNOWAIT);
  } while (result == -1 && errno == EINTR);
  return result;
}

bool applyChildOptions(const ProcessInvocationOptions &options) {
  if (options.workingDirectory &&
      chdir(options.workingDirectory->c_str()) == -1) {
    std::fprintf(
        stderr, "gti: failed to enter process working directory '%s': %s\n",
        options.workingDirectory->string().c_str(), std::strerror(errno));
    return false;
  }
  for (const auto &[name, value] : options.environment) {
    if (setenv(name.c_str(), value.c_str(), 1) == -1) {
      std::fprintf(stderr, "gti: failed to set process environment '%s': %s\n",
                   name.c_str(), std::strerror(errno));
      return false;
    }
  }
  return true;
}

ProcessResult waitForProcess(pid_t child, std::string_view description,
                             std::FILE *capture, bool captureSuccessfulOutput) {
  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno == EINTR) {
      continue;
    }
    const int waitError = errno;
    if (capture != nullptr) {
      std::fclose(capture);
    }
    return {.exitCode = 127,
            .driverDiagnostic = "gti: failed while waiting for " +
                                std::string(description) + ": " +
                                std::strerror(waitError)};
  }

  const int exitCode = WIFEXITED(status)     ? WEXITSTATUS(status)
                       : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                             : 127;
  if (capture != nullptr) {
    auto result =
        finishCapturedProcess(capture, exitCode, captureSuccessfulOutput);
    if (WIFSIGNALED(status))
      result.signal = WTERMSIG(status);
    return result;
  }
  ProcessResult result{.exitCode = exitCode};
  if (WIFSIGNALED(status))
    result.signal = WTERMSIG(status);
  return result;
}
#endif

ProcessResult invokeInherited(const std::vector<std::string> &arguments,
                              const ProcessInvocationOptions &options) {
  std::vector<char *> process = processArguments(arguments);
  std::cout.flush();
  std::cerr.flush();

#if defined(_WIN32)
  if (options.workingDirectory || !options.environment.empty()) {
    return {.exitCode = 127,
            .driverDiagnostic =
                "gti: working-directory and environment process overrides "
                "are not supported on Windows yet"};
  }
  const intptr_t status = _spawnvp(_P_WAIT, process.front(), process.data());
  if (status == -1) {
    return {.exitCode = 127,
            .driverDiagnostic = "gti: failed to execute '" + arguments.front() +
                                "' as " + options.description + ": " +
                                std::strerror(errno)};
  }
  return {.exitCode = static_cast<int>(status)};
#else
  const pid_t child = fork();
  if (child == -1) {
    return {.exitCode = 127,
            .driverDiagnostic = "gti: failed to start " + options.description +
                                ": " + std::strerror(errno)};
  }
  if (child == 0) {
    if (!applyChildOptions(options)) {
      _exit(127);
    }
    execvp(process.front(), process.data());
    const int executeError = errno;
    std::fprintf(stderr, "gti: failed to execute '%s': %s\n",
                 arguments.front().c_str(), std::strerror(executeError));
    std::fflush(stderr);
    _exit(127);
  }
  return waitForProcess(child, options.description, nullptr, false);
#endif
}

ProcessResult invokeCaptured(const std::vector<std::string> &arguments,
                             const ProcessInvocationOptions &options) {
  std::vector<char *> process = processArguments(arguments);
  std::FILE *capture = std::tmpfile();
  if (capture == nullptr) {
    return {.exitCode = 74,
            .driverDiagnostic = "gti: failed to create " + options.description +
                                " output capture: " + std::strerror(errno)};
  }
  std::cout.flush();
  std::cerr.flush();

#if defined(_WIN32)
  if (options.workingDirectory || !options.environment.empty()) {
    std::fclose(capture);
    return {.exitCode = 127,
            .driverDiagnostic =
                "gti: working-directory and environment process overrides "
                "are not supported on Windows yet"};
  }
  const int standardOutput = _fileno(stdout);
  const int standardError = _fileno(stderr);
  const int savedOutput = _dup(standardOutput);
  const int savedError = _dup(standardError);
  if (savedOutput == -1 || savedError == -1 ||
      _dup2(_fileno(capture), standardOutput) != 0 ||
      _dup2(_fileno(capture), standardError) != 0) {
    if (savedOutput != -1) {
      _dup2(savedOutput, standardOutput);
      _close(savedOutput);
    }
    if (savedError != -1) {
      _dup2(savedError, standardError);
      _close(savedError);
    }
    std::fclose(capture);
    return {.exitCode = 74,
            .driverDiagnostic =
                "gti: failed to redirect " + options.description + " output"};
  }

  const intptr_t status = _spawnvp(_P_WAIT, process.front(), process.data());
  const int spawnError = errno;
  std::fflush(stdout);
  std::fflush(stderr);
  _dup2(savedOutput, standardOutput);
  _dup2(savedError, standardError);
  _close(savedOutput);
  _close(savedError);
  if (status == -1) {
    ProcessResult result = finishCapturedProcess(capture, 127, true);
    result.driverDiagnostic = "gti: failed to execute '" + arguments.front() +
                              "': " + std::strerror(spawnError);
    return result;
  }
  return finishCapturedProcess(capture, static_cast<int>(status),
                               options.captureSuccessfulOutput);
#else
  const pid_t child = fork();
  if (child == -1) {
    const int forkError = errno;
    std::fclose(capture);
    return {.exitCode = 127,
            .driverDiagnostic = "gti: failed to start " + options.description +
                                ": " + std::strerror(forkError)};
  }
  if (child == 0) {
    const int descriptor = fileno(capture);
    if (descriptor == -1 || dup2(descriptor, STDOUT_FILENO) == -1 ||
        dup2(descriptor, STDERR_FILENO) == -1) {
      const int redirectError = errno;
      std::fprintf(stderr, "gti: failed to redirect %s output: %s\n",
                   options.description.c_str(), std::strerror(redirectError));
      std::fflush(stderr);
      _exit(127);
    }
    if (descriptor > STDERR_FILENO) {
      close(descriptor);
    }
    if (!applyChildOptions(options)) {
      _exit(127);
    }
    execvp(process.front(), process.data());
    const int executeError = errno;
    std::fprintf(stderr, "gti: failed to execute '%s': %s\n",
                 arguments.front().c_str(), std::strerror(executeError));
    std::fflush(stderr);
    _exit(127);
  }
  return waitForProcess(child, options.description, capture,
                        options.captureSuccessfulOutput);
#endif
}

} // namespace

ProcessResult invokeProcess(const std::vector<std::string> &arguments,
                            ProcessInvocationOptions options) {
  if (options.timeout || options.isolateProcessGroup) {
    return startProcess(arguments, std::move(options)).wait();
  }
  if (arguments.empty() || arguments.front().empty()) {
    return {.exitCode = 127,
            .driverDiagnostic =
                "gti: " + options.description + " command is empty"};
  }
  if (options.outputMode == ProcessOutputMode::Inherit) {
    return invokeInherited(arguments, options);
  }
  return invokeCaptured(arguments, options);
}

StartedProcess::StartedProcess(StartedProcess &&other) noexcept
    : child(other.child), capture(other.capture),
      captureSuccessfulOutput(other.captureSuccessfulOutput),
      description(std::move(other.description)),
      startFailure(std::move(other.startFailure)), deadline(other.deadline),
      isolatedProcessGroup(other.isolatedProcessGroup) {
  other.child = -1;
  other.capture = nullptr;
  other.startFailure.reset();
}

StartedProcess &StartedProcess::operator=(StartedProcess &&other) noexcept {
  if (this != &other) {
    if (running()) {
      static_cast<void>(wait());
    }
    child = other.child;
    capture = other.capture;
    captureSuccessfulOutput = other.captureSuccessfulOutput;
    description = std::move(other.description);
    startFailure = std::move(other.startFailure);
    deadline = other.deadline;
    isolatedProcessGroup = other.isolatedProcessGroup;
    other.child = -1;
    other.capture = nullptr;
    other.startFailure.reset();
  }
  return *this;
}

StartedProcess::~StartedProcess() {
  if (running()) {
    static_cast<void>(wait());
  }
}

bool StartedProcess::running() const { return child != -1; }

void StartedProcess::terminate() {
#if !defined(_WIN32)
  if (child != -1) {
    siginfo_t info{};
    if (observeChildExit(static_cast<pid_t>(child), info) == -1 &&
        errno == ECHILD)
      return;
    kill(isolatedProcessGroup ? -static_cast<pid_t>(child)
                              : static_cast<pid_t>(child),
         SIGKILL);
  }
#endif
}

ProcessResult StartedProcess::wait() {
  for (;;) {
    if (auto result = poll())
      return std::move(*result);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::optional<ProcessResult> StartedProcess::poll() {
  if (startFailure) {
    ProcessResult failure = std::move(*startFailure);
    startFailure.reset();
    return failure;
  }
  if (child == -1) {
    return ProcessResult{.exitCode = 127,
                         .driverDiagnostic = "gti: " + description +
                                             " has already been waited for"};
  }
#if defined(_WIN32)
  std::FILE *captureFile = static_cast<std::FILE *>(capture);
  capture = nullptr;
  int status = 0;
  const intptr_t waited = _cwait(&status, child, 0);
  child = -1;
  if (waited == -1) {
    if (captureFile != nullptr) {
      std::fclose(captureFile);
    }
    return ProcessResult{.exitCode = 127,
                         .driverDiagnostic = "gti: failed while waiting for " +
                                             description + ": " +
                                             std::strerror(errno)};
  }
  return finishCapturedProcess(captureFile, status, captureSuccessfulOutput);
#else
  const pid_t waitedChild = static_cast<pid_t>(child);
  int status = 0;
  siginfo_t info{};
  const int observation = observeChildExit(waitedChild, info);
  const int observationError = observation == -1 ? errno : 0;
  const bool pending = observation == 0 && info.si_pid == 0;
  const bool expired =
      pending && deadline && std::chrono::steady_clock::now() >= *deadline;
  if (pending && !expired)
    return std::nullopt;
  // Keep the child waitable until its group is gone: reaping first releases
  // its numeric identity, so a subsequent group signal could hit a reused PID.
  // ECHILD means another reaper already took ownership; do not signal that ID.
  pid_t observed = -1;
  int waitError = observationError;
  if (observationError != ECHILD) {
    if (isolatedProcessGroup || expired || observationError != 0)
      kill(isolatedProcessGroup ? -waitedChild : waitedChild, SIGKILL);
    do {
      observed = waitpid(waitedChild, &status, 0);
    } while (observed == -1 && errno == EINTR);
    if (observed == -1 && waitError == 0)
      waitError = errno;
  }
  child = -1;
  auto *captureFile = static_cast<std::FILE *>(capture);
  capture = nullptr;
  if (observed == -1 || observationError != 0) {
    if (captureFile)
      std::fclose(captureFile);
    return ProcessResult{.exitCode = 127,
                         .driverDiagnostic = "gti: failed while waiting for " +
                                             description + ": " +
                                             std::strerror(waitError)};
  }
  const int code = expired               ? 124
                   : WIFEXITED(status)   ? WEXITSTATUS(status)
                   : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                         : 127;
  auto result = captureFile ? finishCapturedProcess(captureFile, code,
                                                    captureSuccessfulOutput)
                            : ProcessResult{.exitCode = code};
  if (WIFSIGNALED(status))
    result.signal = WTERMSIG(status);
  result.timedOut = expired;
  return result;
#endif
}

StartedProcess startProcess(const std::vector<std::string> &arguments,
                            ProcessInvocationOptions options) {
  StartedProcess started;
  started.description = options.description;
  started.captureSuccessfulOutput = options.captureSuccessfulOutput;
  started.isolatedProcessGroup = options.isolateProcessGroup;
  const auto maximumTimeout =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::duration::max() / 2);
  if (options.timeout &&
      (options.timeout->count() < 0 || *options.timeout > maximumTimeout)) {
    started.startFailure = ProcessResult{
        .exitCode = 127, .driverDiagnostic = "gti: invalid process timeout"};
    return started;
  }
  if (options.timeout && options.timeout->count() > 0)
    started.deadline = std::chrono::steady_clock::now() + *options.timeout;
  if (arguments.empty() || arguments.front().empty()) {
    started.startFailure =
        ProcessResult{.exitCode = 127,
                      .driverDiagnostic =
                          "gti: " + options.description + " command is empty"};
    return started;
  }
#if defined(_WIN32)
  if (options.outputMode == ProcessOutputMode::Inherit ||
      options.workingDirectory || !options.environment.empty() ||
      options.timeout || options.isolateProcessGroup) {
    started.startFailure = ProcessResult{
        .exitCode = 127,
        .driverDiagnostic = "gti: asynchronous inherited, bounded, isolated, "
                            "working-directory and "
                            "environment processes are "
                            "not supported on Windows yet"};
    return started;
  }
#endif

  std::vector<char *> process = processArguments(arguments);
  std::FILE *capture = options.outputMode == ProcessOutputMode::Capture
                           ? std::tmpfile()
                           : nullptr;
  if (options.outputMode == ProcessOutputMode::Capture && capture == nullptr) {
    started.startFailure = ProcessResult{
        .exitCode = 74,
        .driverDiagnostic = "gti: failed to create " + options.description +
                            " output capture: " + std::strerror(errno)};
    return started;
  }
#if !defined(_WIN32)
  // Keep private descriptors above stdio even if the caller closed a stream;
  // dup2 below must never overwrite the launch handshake. Captures belonging to
  // other active children must not survive a later child's exec either.
  if (capture && fileno(capture) < 3) {
    const int replacement = fcntl(fileno(capture), F_DUPFD_CLOEXEC, 3);
    std::fclose(capture);
    capture = replacement == -1 ? nullptr : fdopen(replacement, "w+b");
    if (!capture && replacement != -1)
      close(replacement);
  }
  if (options.outputMode == ProcessOutputMode::Capture &&
      (!capture || fcntl(fileno(capture), F_SETFD, FD_CLOEXEC) == -1)) {
    if (capture)
      std::fclose(capture);
    started.startFailure = ProcessResult{
        .exitCode = 74,
        .driverDiagnostic = "gti: failed to configure private process capture"};
    return started;
  }
#endif
  std::cout.flush();
  std::cerr.flush();

#if defined(_WIN32)
  // Spawned children inherit the standard handles active at spawn time, so
  // the parent's handles are redirected only for the non-waiting spawn call
  // and restored immediately afterwards.
  const int standardOutput = _fileno(stdout);
  const int standardError = _fileno(stderr);
  const int savedOutput = _dup(standardOutput);
  const int savedError = _dup(standardError);
  if (savedOutput == -1 || savedError == -1 ||
      _dup2(_fileno(capture), standardOutput) != 0 ||
      _dup2(_fileno(capture), standardError) != 0) {
    if (savedOutput != -1) {
      _dup2(savedOutput, standardOutput);
      _close(savedOutput);
    }
    if (savedError != -1) {
      _dup2(savedError, standardError);
      _close(savedError);
    }
    std::fclose(capture);
    started.startFailure =
        ProcessResult{.exitCode = 74,
                      .driverDiagnostic = "gti: failed to redirect " +
                                          options.description + " output"};
    return started;
  }
  const intptr_t spawned = _spawnvp(_P_NOWAIT, process.front(), process.data());
  const int spawnError = errno;
  std::fflush(stdout);
  std::fflush(stderr);
  _dup2(savedOutput, standardOutput);
  _dup2(savedError, standardError);
  _close(savedOutput);
  _close(savedError);
  if (spawned == -1) {
    std::fclose(capture);
    started.startFailure = ProcessResult{
        .exitCode = 127,
        .driverDiagnostic = "gti: failed to execute '" + arguments.front() +
                            "': " + std::strerror(spawnError)};
    return started;
  }
  started.child = spawned;
  started.capture = capture;
  return started;
#else
  // Close-on-exec handshake distinguishes an exec/setup error from a program
  // that deliberately returns 127. No shell or output parsing is involved.
  int errors[2];
  if (pipe(errors) == -1) {
    if (capture)
      std::fclose(capture);
    started.startFailure = ProcessResult{
        .exitCode = 127,
        .driverDiagnostic = "gti: failed to create process launch handshake"};
    return started;
  }
  for (int &descriptor : errors) {
    if (descriptor < 3) {
      const int replacement = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
      close(descriptor);
      descriptor = replacement;
    }
  }
  if (errors[0] == -1 || errors[1] == -1 ||
      fcntl(errors[0], F_SETFD, FD_CLOEXEC) == -1 ||
      fcntl(errors[1], F_SETFD, FD_CLOEXEC) == -1) {
    close(errors[0]);
    close(errors[1]);
    if (capture)
      std::fclose(capture);
    started.startFailure =
        ProcessResult{.exitCode = 127,
                      .driverDiagnostic =
                          "gti: failed to configure process launch handshake"};
    return started;
  }
  const pid_t child = fork();
  if (child == -1) {
    const int forkError = errno;
    close(errors[0]);
    close(errors[1]);
    if (capture)
      std::fclose(capture);
    started.startFailure = ProcessResult{
        .exitCode = 127,
        .driverDiagnostic = "gti: failed to start " + options.description +
                            ": " + std::strerror(forkError)};
    return started;
  }
  if (child == 0) {
    close(errors[0]);
    auto failLaunch = [&](int code) {
      // sizeof(int) is atomic for this pipe; retry interrupted writes only.
      ssize_t sent;
      do {
        sent = write(errors[1], &code, sizeof(code));
      } while (sent == -1 && errno == EINTR);
      _exit(127);
    };
    if (options.isolateProcessGroup && setpgid(0, 0) == -1)
      failLaunch(errno);
    if (capture) {
      const int descriptor = fileno(capture);
      if (descriptor == -1 || dup2(descriptor, STDOUT_FILENO) == -1 ||
          dup2(descriptor, STDERR_FILENO) == -1) {
        const int redirectError = errno;
        std::fprintf(stderr, "gti: failed to redirect %s output: %s\n",
                     options.description.c_str(), std::strerror(redirectError));
        std::fflush(stderr);
        failLaunch(redirectError);
      }
      if (descriptor > STDERR_FILENO)
        close(descriptor);
    }
    if (!applyChildOptions(options)) {
      failLaunch(errno);
    }
    execvp(process.front(), process.data());
    const int executeError = errno;
    std::fprintf(stderr, "gti: failed to execute '%s': %s\n",
                 arguments.front().c_str(), std::strerror(executeError));
    std::fflush(stderr);
    failLaunch(executeError);
  }
  close(errors[1]);
  int launchError = 0;
  ssize_t received;
  do {
    received = read(errors[0], &launchError, sizeof(launchError));
  } while (received == -1 && errno == EINTR);
  close(errors[0]);
  started.child = child;
  started.capture = capture;
  if (received != 0) {
    if (received < 0)
      kill(options.isolateProcessGroup ? -child : child, SIGKILL);
    auto failure = started.wait();
    failure.exitCode = 127;
    failure.driverDiagnostic =
        "gti: failed to launch " + options.description + ": " +
        (received == sizeof(launchError)
             ? std::string(std::strerror(launchError))
             : std::string("invalid process launch handshake"));
    started.startFailure = std::move(failure);
  }
  return started;
#endif
}

} // namespace lang::driver
