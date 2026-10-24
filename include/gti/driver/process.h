#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lang::driver {

enum class ProcessOutputMode {
  Capture,
  Inherit,
};

struct ProcessInvocationOptions {
  ProcessOutputMode outputMode = ProcessOutputMode::Capture;
  bool captureSuccessfulOutput = false;
  std::string description = "process";
  std::optional<std::filesystem::path> workingDirectory;
  // Environment entries override inherited values. Build scripting currently
  // does not claim sandboxing; inheritance is intentional and documented.
  std::vector<std::pair<std::string, std::string>> environment;
  // Deadlines are measured from launch, including time spent waiting behind
  // another child. Zero/unset means no deadline.
  std::optional<std::chrono::milliseconds> timeout;
  bool isolateProcessGroup = false;
};

struct ProcessResult {
  int exitCode = 127;
  std::string output;
  std::optional<std::string> driverDiagnostic;
  std::optional<int> signal;
  bool timedOut = false;

  [[nodiscard]] bool succeeded() const {
    return exitCode == 0 && !driverDiagnostic && !timedOut;
  }
};

[[nodiscard]] ProcessResult
invokeProcess(const std::vector<std::string> &arguments,
              ProcessInvocationOptions options = {});

// A child process that runs concurrently with the caller. The
// handle is single-use: wait() reaps the child and returns its result exactly
// once. Destroying an unwaited started process waits for it first so a child
// can never outlive its capture buffer.
// The caller must retain exclusive wait ownership: other reapers and SIGCHLD
// auto-reaping are unsupported. Lost ownership reports a driver error without
// signalling the released numeric identity.
class StartedProcess final {
public:
  StartedProcess(const StartedProcess &) = delete;
  StartedProcess &operator=(const StartedProcess &) = delete;
  StartedProcess(StartedProcess &&other) noexcept;
  StartedProcess &operator=(StartedProcess &&other) noexcept;
  ~StartedProcess();

  // True while the child is running and unreaped; false after wait() or when
  // the start itself failed (wait() then reports the start failure).
  [[nodiscard]] bool running() const;

  [[nodiscard]] ProcessResult wait();
  // Returns a completed result once, or nullopt while the child is running.
  [[nodiscard]] std::optional<ProcessResult> poll();
  // Requests immediate termination of a POSIX child and its isolated group;
  // the owner must still poll/wait to reap it and retrieve captured output.
  void terminate();

private:
  friend StartedProcess startProcess(const std::vector<std::string> &arguments,
                                     ProcessInvocationOptions options);

  StartedProcess() = default;

  std::intptr_t child = -1;
  void *capture = nullptr;
  bool captureSuccessfulOutput = false;
  std::string description;
  std::optional<ProcessResult> startFailure;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  bool isolatedProcessGroup = false;
};

// Starts a concurrent child with merged stdout/stderr capture, or inherited
// streams on POSIX. Inherited streams can interleave with concurrent children.
[[nodiscard]] StartedProcess
startProcess(const std::vector<std::string> &arguments,
             ProcessInvocationOptions options = {});

} // namespace lang::driver
