#include "gti/driver/process.h"
#include "gti/driver/test_runner.h"
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace lang::driver;
using namespace std::chrono_literals;
namespace {
int failures = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}
} // namespace
int main(int argc, char **argv) {
#if defined(_WIN32)
  std::cout << "isolated test runner requires POSIX process groups\n";
  return 77;
#else
  if (argc == 2) {
    const std::string mode = argv[1];
    if (mode.starts_with("large")) {
      std::cout << std::string(128 * 1024, 'x');
      return 0;
    }
    if (mode == "closed-streams") {
      close(0);
      close(1);
      close(2);
      const auto nested =
          invokeProcess({std::filesystem::absolute(argv[0]).string(), "b_fast"},
                        {.captureSuccessfulOutput = true,
                         .timeout = 2s,
                         .isolateProcessGroup = true});
      return nested.succeeded() && nested.output == "b_fast ran\n" ? 0 : 1;
    }
    if (mode == "exit127")
      return 127;
    if (mode == "exit143")
      return 143;
    if (mode == "signal") {
      raise(SIGTERM);
      return 0;
    }
    if (mode == "a_slow") {
      for (;;)
        std::this_thread::sleep_for(10ms);
    }
    if (mode == "background" || mode == "background_timeout") {
      if (fork() == 0) {
        std::this_thread::sleep_for(500ms);
        std::ofstream(std::getenv("GTI_RUNNER_MARKER")) << "escaped";
        _exit(0);
      }
      if (mode == "background_timeout")
        for (;;)
          std::this_thread::sleep_for(10ms);
      return 0;
    }
    if (mode == "barrier_a" || mode == "barrier_b") {
      const auto directory =
          std::filesystem::path(std::getenv("GTI_RUNNER_DIRECTORY"));
      std::ofstream(directory / mode) << "started";
      while (!std::filesystem::exists(directory / "barrier_a") ||
             !std::filesystem::exists(directory / "barrier_b"))
        std::this_thread::sleep_for(1ms);
    }
    std::cout << mode << " ran\n";
    return 0;
  }
  const auto executable = std::filesystem::absolute(argv[0]);
  check(invokeProcess({executable.string(), "closed-streams"},
                      {.captureSuccessfulOutput = true,
                       .timeout = 3s,
                       .isolateProcessGroup = true})
            .succeeded(),
        "closed standard streams must not corrupt capture or launch-handshake "
        "descriptors");
  const auto directory = std::filesystem::temp_directory_path() /
                         ("gti-test-runner-" + std::to_string(getpid()));
  std::filesystem::create_directory(directory);
  TestRunOptions options;
  options.timeout = 1s;
  const TestSuiteCommand suite{
      "suite", executable, {"first", "second", "third"}};
  options.listOnly = true;
  options.filter = "second";
  std::vector<TestSelection> inventory;
  auto result = runTestSuites(std::span(&suite, 1), options,
                              {.inventory = [&](auto, auto tests) {
                                inventory.assign(tests.begin(), tests.end());
                              }});
  check(result.summary.complete && result.summary.selected == 1 &&
            result.summary.started == 0 && result.summary.skipped == 2 &&
            inventory.size() == 3,
        "listing must retain complete inventory without executing");
  options.exact = true;
  options.filter = "sec";
  result = runTestSuites(std::span(&suite, 1), options);
  check(result.summary.exitCode != 0 && !result.summary.complete,
        "empty exact selection must fail closed");
  options.allowEmpty = true;
  result = runTestSuites(std::span(&suite, 1), options);
  check(result.summary.exitCode == 0 && result.summary.complete,
        "explicit empty selection must be accepted");
  options = TestRunOptions{};
  options.skips = {"second"};
  result = runTestSuites(std::span(&suite, 1), options);
  check(result.summary.passed == 2 && result.summary.skipped == 1 &&
            result.summary.started == 2 && result.summary.completed == 2,
        "skip must exclude exactly the selected names");
  TestSuiteCommand duplicate{"suite", executable, {"same", "same"}};
  const TestSuiteCommand verbose{
      "suite", executable, {"large1", "large2", "large3"}};
  auto streaming = options;
  streaming.retainOutput = false;
  std::size_t outputBytes = 0;
  const auto streamed = runTestSuites(std::span(&verbose, 1), streaming,
                                      {.finished = [&](const auto &report) {
                                        outputBytes += report.output.size();
                                      }});
  check(outputBytes == 3 * 128 * 1024 && streamed.summary.passed == 3 &&
            std::all_of(streamed.cases.begin(), streamed.cases.end(),
                        [](const auto &report) {
                          return report.output.empty() &&
                                 report.output.capacity() <=
                                     std::string{}.capacity();
                        }),
        "streaming reports must release captured storage after the observer "
        "consumes it");
  result = runTestSuites(std::span(&duplicate, 1), options);
  check(result.summary.exitCode != 0 && result.summary.started == 0,
        "duplicate compiler identities must be rejected before execution");
  options = TestRunOptions{};
  options.jobs = 2;
  options.timeout = 150ms;
  TestSuiteCommand timed{"suite", executable, {"a_slow", "b_fast"}};
  bool cancel = false;
  auto cancelledOptions = options;
  cancelledOptions.jobs = 1;
  auto cancelled =
      runTestSuites(std::span(&timed, 1), cancelledOptions,
                    {.started = [&](auto, auto) { cancel = true; },
                     .interruption = [&] { return cancel ? SIGTERM : 0; }});
  check(!cancelled.summary.complete && cancelled.summary.exitCode == 143 &&
            cancelled.summary.started == 1 &&
            cancelled.summary.completed == 1 && cancelled.cases.size() == 1,
        "interruption must reap active children and not invent unstarted "
        "results");
  std::vector<std::string> completed;
  result = runTestSuites(std::span(&timed, 1), options,
                         {.finished = [&](const auto &report) {
                           completed.push_back(report.name);
                         }});
  check(result.summary.complete && result.summary.timedOut == 1 &&
            result.summary.passed == 1 && result.summary.exitCode == 124 &&
            completed.front() == "b_fast",
        "a hanging test must not block another child or its own deadline");
  TestSuiteCommand barrier{"suite", executable, {"barrier_a", "barrier_b"}};
  options.timeout = 2s;
  options.environment = {{"GTI_RUNNER_DIRECTORY", directory.string()}};
  unsigned active = 0, maximum = 0;
  result = runTestSuites(
      std::span(&barrier, 1), options,
      {.started = [&](auto, auto) { maximum = std::max(maximum, ++active); },
       .finished = [&](const auto &) { --active; }});
  check(result.summary.passed == 2 && maximum == 2 && active == 0,
        "two children must overlap without exceeding the requested job bound");
  TestSuiteCommand kinds{"suite", executable, {"exit127", "exit143", "signal"}};
  result = runTestSuites(std::span(&kinds, 1), options);
  check(result.summary.failed == 2 && result.summary.crashed == 1 &&
            result.summary.runnerErrors == 0 &&
            result.summary.exitCode == 127 && !result.cases[1].signal &&
            result.cases[2].signal == SIGTERM,
        "ordinary statuses including 127 and 128+signal must not be inferred "
        "as launch errors or crashes");
  const TestSuiteCommand missing{"missing", directory / "absent", {"test"}};
  result = runTestSuites(std::span(&missing, 1), options);
  check(result.summary.runnerErrors == 1 && result.summary.failed == 0 &&
            result.summary.completed == 1,
        "failed exec must be a completed runner error, not a program exit");
  auto process = startProcess({executable.string(), "b_fast"},
                              {.captureSuccessfulOutput = true});
  auto moved = std::move(process);
  check(!process.running() && moved.wait().succeeded() &&
            !moved.wait().succeeded(),
        "process ownership must move once and reject a second wait");
  for (const auto *mode : {"background", "background_timeout"}) {
    const auto marker = directory / mode;
    const auto child =
        invokeProcess({executable.string(), mode},
                      {.captureSuccessfulOutput = true,
                       .environment = {{"GTI_RUNNER_MARKER", marker.string()}},
                       .timeout = 100ms,
                       .isolateProcessGroup = true});
    check(child.timedOut == (std::string(mode) == "background_timeout"),
          "timeout fact must reflect the actual deadline");
    std::this_thread::sleep_for(650ms);
    check(!std::filesystem::exists(marker),
          "completion and timeout must both terminate background group "
          "descendants");
  }
  std::filesystem::remove_all(directory);
  return failures == 0 ? 0 : 1;
#endif
}
