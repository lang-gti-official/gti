#include "gti/driver/test_report.h"

#include <iostream>
#include <limits>
#include <locale>
#include <vector>

namespace {
class GroupedNumbers final : public std::numpunct<char> {
  char do_thousands_sep() const override { return '_'; }
  std::string do_grouping() const override { return "\3"; }
};
} // namespace

int main() {
  using namespace lang::driver;
  std::cout.imbue(std::locale(std::locale::classic(), new GroupedNumbers));
  const std::vector<TestSelection> cases{{"app::unicode_\xc3\xa9", true, ""},
                                         {"app::filtered", false, "filtered"},
                                         {"app::skipped", false, "skipped"}};
  writeTestInventoryJson(std::cout, "suite\"\\", cases);
  writeTestStartedJson(std::cout, "suite\"\\", cases[0].name);
  std::string output = "quote\" slash\\ newline\n tab\t null";
  output.push_back('\0');
  output += " unicode \xc3\xa9 \xf0\x9f\x90\x88 invalid ";
  for (unsigned char c :
       {0xc0, 0xaf, 0xed, 0xa0, 0x80, 0xf4, 0x90, 0x80, 0x80, 0xe2})
    output.push_back(static_cast<char>(c));
  writeTestFinishedJson(std::cout, {.target = "suite\"\\",
                                    .name = cases[0].name,
                                    .status = TestCaseStatus::Passed,
                                    .exitCode = 0,
                                    .durationMs = 1234567,
                                    .output = output});
  for (auto status : {TestCaseStatus::Failed, TestCaseStatus::Crashed,
                      TestCaseStatus::TimedOut, TestCaseStatus::RunnerError,
                      TestCaseStatus::Skipped}) {
    writeTestFinishedJson(
        std::cout, {.target = "other",
                    .name = std::string(testCaseStatusName(status)),
                    .status = status,
                    .exitCode = 137,
                    .signal = 9,
                    .durationMs = std::numeric_limits<std::uint64_t>::max(),
                    .diagnostic = "line\n\"message\""});
  }
  writeTestSummaryJson(std::cout, {.discovered = 10000,
                                   .selected = 9999,
                                   .started = 9999,
                                   .completed = 9999,
                                   .passed = 9995,
                                   .failed = 1,
                                   .crashed = 1,
                                   .timedOut = 1,
                                   .runnerErrors = 1,
                                   .skipped = 1,
                                   .complete = true,
                                   .exitCode = 70});
  writeTestSummaryJson(std::cout, {.discovered = 3,
                                   .selected = 0,
                                   .skipped = 3,
                                   .listOnly = true,
                                   .complete = true,
                                   .diagnostic = "No selected tests"});
}
