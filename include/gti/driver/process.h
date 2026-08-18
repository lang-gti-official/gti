#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
};

struct ProcessResult {
  int exitCode = 127;
  std::string output;
  std::optional<std::string> driverDiagnostic;

  [[nodiscard]] bool succeeded() const { return exitCode == 0; }
};

[[nodiscard]] ProcessResult
invokeProcess(const std::vector<std::string> &arguments,
              ProcessInvocationOptions options = {});

// A capture-mode child process that runs concurrently with the caller. The
// handle is single-use: wait() reaps the child and returns its result exactly
// once. Destroying an unwaited started process waits for it first so a child
// can never outlive its capture buffer.
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

private:
  friend StartedProcess startProcess(const std::vector<std::string> &arguments,
                                     ProcessInvocationOptions options);

  StartedProcess() = default;

  std::intptr_t child = -1;
  void *capture = nullptr;
  bool captureSuccessfulOutput = false;
  std::string description;
  std::optional<ProcessResult> startFailure;
};

// Starts a concurrent child with merged stdout/stderr capture. Inherit mode
// is not supported: concurrent children sharing the caller's terminal would
// interleave nondeterministically.
[[nodiscard]] StartedProcess
startProcess(const std::vector<std::string> &arguments,
             ProcessInvocationOptions options = {});

} // namespace lang::driver
