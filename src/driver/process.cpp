#include "gti/driver/process.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#else
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

std::string readCapture(std::FILE *capture) {
  if (std::fseek(capture, 0, SEEK_END) != 0) {
    return {};
  }
  const long capturedSize = std::ftell(capture);
  if (capturedSize <= 0 || std::fseek(capture, 0, SEEK_SET) != 0) {
    return {};
  }

  std::string output(static_cast<std::size_t>(capturedSize), '\0');
  std::size_t total = 0;
  while (total < output.size()) {
    const std::size_t size = std::fread(output.data() + total, sizeof(char),
                                        output.size() - total, capture);
    total += size;
    if (size == 0 || std::feof(capture) != 0 || std::ferror(capture) != 0) {
      break;
    }
  }
  output.resize(total);
  return output;
}

ProcessResult finishCapturedProcess(std::FILE *capture, int status,
                                    bool captureSuccessfulOutput) {
  ProcessResult result{.exitCode = status};
  if (status != 0 || captureSuccessfulOutput) {
    result.output = readCapture(capture);
  }
  std::fclose(capture);
  return result;
}

#if !defined(_WIN32)
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
    return finishCapturedProcess(capture, exitCode, captureSuccessfulOutput);
  }
  return {.exitCode = exitCode};
}
#endif

ProcessResult invokeInherited(const std::vector<std::string> &arguments,
                              std::string_view description) {
  std::vector<char *> process = processArguments(arguments);
  std::cout.flush();
  std::cerr.flush();

#if defined(_WIN32)
  const intptr_t status = _spawnvp(_P_WAIT, process.front(), process.data());
  if (status == -1) {
    return {.exitCode = 127,
            .driverDiagnostic = "gti: failed to execute '" + arguments.front() +
                                "' as " + std::string(description) + ": " +
                                std::strerror(errno)};
  }
  return {.exitCode = static_cast<int>(status)};
#else
  const pid_t child = fork();
  if (child == -1) {
    return {.exitCode = 127,
            .driverDiagnostic = "gti: failed to start " +
                                std::string(description) + ": " +
                                std::strerror(errno)};
  }
  if (child == 0) {
    execvp(process.front(), process.data());
    const int executeError = errno;
    std::fprintf(stderr, "gti: failed to execute '%s': %s\n",
                 arguments.front().c_str(), std::strerror(executeError));
    std::fflush(stderr);
    _exit(127);
  }
  return waitForProcess(child, description, nullptr, false);
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
  if (arguments.empty() || arguments.front().empty()) {
    return {.exitCode = 127,
            .driverDiagnostic =
                "gti: " + options.description + " command is empty"};
  }
  if (options.outputMode == ProcessOutputMode::Inherit) {
    return invokeInherited(arguments, options.description);
  }
  return invokeCaptured(arguments, options);
}

} // namespace lang::driver
