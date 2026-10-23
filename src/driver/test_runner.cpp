#include "gti/driver/test_runner.h"
#include "gti/driver/process.h"
#include <algorithm>
#include <set>
#include <thread>

namespace lang::driver {
TestRunResult runTestSuites(std::span<const TestSuiteCommand> suites,
                            const TestRunOptions &options,
                            const TestRunObserver &observer) {
  TestRunResult result;
  auto &summary = result.summary;
  summary.listOnly = options.listOnly;
  auto invalid = [&](std::string message) {
    summary.diagnostic = std::move(message);
    summary.exitCode = 1;
    ++summary.runnerErrors;
  };
  if (options.jobs == 0 || options.timeout.count() < 0 ||
      (options.exact && !options.filter)) {
    invalid("invalid test execution options");
    return result;
  }
  struct Selected {
    const TestSuiteCommand *suite;
    std::string name;
    bool legacy;
  };
  std::vector<Selected> selected;
  std::set<std::pair<std::string, std::string>> identities;
  for (const auto &suite : suites) {
    auto names = suite.tests.empty() ? std::vector<std::string>{suite.target}
                                     : suite.tests;
    std::sort(names.begin(), names.end());
    std::vector<TestSelection> inventory;
    for (const auto &name : names) {
      if (name.empty() || !identities.emplace(suite.target, name).second) {
        invalid("empty or duplicate test identity in compiler catalog");
        return result;
      }
      const bool matches =
          !options.filter ||
          (options.exact ? name == *options.filter
                         : name.find(*options.filter) != std::string::npos);
      const bool skipped = std::any_of(
          options.skips.begin(), options.skips.end(), [&](const auto &skip) {
            return name.find(skip) != std::string::npos;
          });
      const bool run = matches && !skipped;
      inventory.push_back({name, run,
                           !matches  ? "filtered"
                           : skipped ? "skipped"
                                     : ""});
      ++summary.discovered;
      if (run)
        selected.push_back({&suite, name, suite.tests.empty()});
      else
        ++summary.skipped;
    }
    if (observer.inventory)
      observer.inventory(suite.target, inventory);
  }
  summary.selected = selected.size();
  if (selected.empty() && !options.allowEmpty) {
    invalid(
        "no tests selected; use --allow-empty to accept an empty selection");
    return result;
  }
  if (options.listOnly || selected.empty()) {
    summary.complete = true;
    return result;
  }
  result.cases.resize(selected.size());
  struct Active {
    std::size_t index;
    std::chrono::steady_clock::time_point began;
    StartedProcess process;
  };
  std::vector<Active> active;
  active.reserve(std::min(options.jobs, selected.size()));
  std::size_t next = 0;
  int interrupted = 0;
  while (next < selected.size() || !active.empty()) {
    if (!interrupted && observer.interruption)
      interrupted = observer.interruption();
    if (interrupted) {
      next = selected.size();
      for (auto &child : active)
        child.process.terminate();
    }
    while (!interrupted && next < selected.size() &&
           active.size() < options.jobs) {
      if (observer.interruption && observer.interruption())
        break;
      const auto &item = selected[next];
      std::vector<std::string> arguments{item.suite->executable.string()};
      if (!item.legacy)
        arguments.push_back(item.name);
      if (observer.started)
        observer.started(item.suite->target, item.name);
      ++summary.started;
      const auto began = std::chrono::steady_clock::now();
      auto process = startProcess(
          arguments, {.captureSuccessfulOutput = true,
                      .description = "test case '" + item.name + "'",
                      .environment = options.environment,
                      .timeout = options.timeout,
                      .isolateProcessGroup = true});
      active.push_back({next++, began, std::move(process)});
    }
    bool progress = false;
    for (std::size_t i = 0; i < active.size();) {
      auto process = active[i].process.poll();
      if (!process) {
        ++i;
        continue;
      }
      progress = true;
      const auto index = active[i].index;
      auto &report = result.cases[index];
      report.target = selected[index].suite->target;
      report.name = selected[index].name;
      report.exitCode = process->exitCode;
      report.signal = process->signal;
      report.output = std::move(process->output);
      report.diagnostic = std::move(process->driverDiagnostic);
      report.durationMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - active[i].began)
              .count();
      if (interrupted && process->signal) {
        report.status = TestCaseStatus::RunnerError;
        report.exitCode = 128 + interrupted;
        report.diagnostic =
            "test interrupted by signal " + std::to_string(interrupted);
        ++summary.runnerErrors;
      } else if (process->timedOut) {
        report.status = TestCaseStatus::TimedOut;
        ++summary.timedOut;
      } else if (report.diagnostic) {
        report.status = TestCaseStatus::RunnerError;
        ++summary.runnerErrors;
      } else if (process->signal) {
        report.status = TestCaseStatus::Crashed;
        ++summary.crashed;
      } else if (process->exitCode != 0) {
        report.status = TestCaseStatus::Failed;
        ++summary.failed;
      } else {
        report.status = TestCaseStatus::Passed;
        ++summary.passed;
      }
      ++summary.completed;
      if (observer.finished)
        observer.finished(report);
      active.erase(active.begin() + i);
    }
    if (!progress && !active.empty())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!interrupted && observer.interruption)
    interrupted = observer.interruption();
  if (interrupted) {
    std::erase_if(result.cases,
                  [](const auto &report) { return report.name.empty(); });
    summary.exitCode = 128 + interrupted;
    summary.diagnostic =
        "test run interrupted by signal " + std::to_string(interrupted);
    summary.complete = false;
    return result;
  }
  summary.complete = summary.started == summary.selected &&
                     summary.completed == summary.selected &&
                     summary.passed + summary.failed + summary.crashed +
                             summary.timedOut + summary.runnerErrors ==
                         summary.completed;
  for (const auto &report : result.cases) {
    if (report.status != TestCaseStatus::Passed) {
      summary.exitCode = report.exitCode == 0 ? 127 : report.exitCode;
      break;
    }
  }
  if (!summary.complete)
    invalid("test execution ended with missing or inconsistent results");
  return result;
}
} // namespace lang::driver
