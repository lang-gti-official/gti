#pragma once

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

} // namespace lang::driver
