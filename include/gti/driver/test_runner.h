#pragma once

#include "gti/driver/test_report.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lang::driver {
struct TestRunOptions {
  std::optional<std::string> filter;
  bool exact = false;
  std::vector<std::string> skips;
  bool listOnly = false;
  bool allowEmpty = false;
  // Observers consume each captured output before it is released. Callers
  // needing output again in result.cases keep the default true.
  bool retainOutput = true;
  std::size_t jobs = 1;
  std::chrono::milliseconds timeout{300000};
  std::vector<std::pair<std::string, std::string>> environment;
};
struct TestSuiteCommand {
  std::string target;
  std::filesystem::path executable;
  // Empty catalog represents a legacy executable, named after its target.
  std::vector<std::string> tests;
};
struct TestRunObserver {
  std::function<void(std::string_view, std::span<const TestSelection>)>
      inventory;
  std::function<void(std::string_view, std::string_view)> started;
  std::function<void(const TestCaseReport &)> finished;
  // Zero means continue; a POSIX signal number requests interruption. The
  // caller owns signal-handler policy; the driver owns terminating/reaping.
  std::function<int()> interruption;
};
struct TestRunResult {
  TestRunSummary summary;
  // Declaration order, independent of completion order or --jobs.
  std::vector<TestCaseReport> cases;
};

// Scheduling and process lifecycle belong to the reusable driver. Presentation
// observers run synchronously on the parent thread and must not throw.
[[nodiscard]] TestRunResult
runTestSuites(std::span<const TestSuiteCommand> suites,
              const TestRunOptions &options,
              const TestRunObserver &observer = {});
} // namespace lang::driver
