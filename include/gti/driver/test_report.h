#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace lang::driver {

// The runner owns lifecycle validation. These records only project its facts
// into a versioned JSON Lines protocol; no test-language semantics live here.
enum class TestCaseStatus {
  Passed,
  Failed,
  Crashed,
  TimedOut,
  RunnerError,
  Skipped
};

struct TestSelection {
  std::string name;
  bool selected = false;
  std::string skipReason;
};

struct TestCaseReport {
  std::string target;
  std::string name;
  TestCaseStatus status = TestCaseStatus::RunnerError;
  int exitCode = 0;
  std::optional<int> signal;
  std::uint64_t durationMs = 0;
  std::string output;
  std::optional<std::string> diagnostic;
};

struct TestRunSummary {
  std::size_t discovered = 0;
  std::size_t selected = 0;
  std::size_t started = 0;
  std::size_t completed = 0;
  std::size_t passed = 0;
  std::size_t failed = 0;
  std::size_t crashed = 0;
  std::size_t timedOut = 0;
  std::size_t runnerErrors = 0;
  // Includes every discovered-but-unselected case (filtered or explicitly
  // skipped).
  std::size_t skipped = 0;
  bool listOnly = false;
  bool complete = false;
  int exitCode = 0;
  std::optional<std::string> diagnostic;
};

[[nodiscard]] std::string_view testCaseStatusName(TestCaseStatus status);
void writeTestInventoryJson(std::ostream &out, std::string_view target,
                            std::span<const TestSelection> tests);
void writeTestStartedJson(std::ostream &out, std::string_view target,
                          std::string_view name);
void writeTestFinishedJson(std::ostream &out, const TestCaseReport &report);
void writeTestSummaryJson(std::ostream &out, const TestRunSummary &summary);

} // namespace lang::driver
