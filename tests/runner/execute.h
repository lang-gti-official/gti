#pragma once

// Executes one gti-test case, either in this process through the compiler
// libraries with the analysed standard library shared across cases, or
// (`--isolate`) through a fresh `gti` process.

#include "test_file.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gti_test {

enum class Status { Pass, Fail, Ice, Crash, Timeout, Skip };

[[nodiscard]] const char *statusName(Status status);

struct Outcome {
  Status status = Status::Pass;
  double cpu = 0;
  double wall = 0;
  // Failure explanation, one item per line, without the FAIL header.
  std::string detail;
  std::vector<std::string> blessed;
  // The case built a standard-library preamble in this worker.
  bool cold = false;
};

struct Limits {
  double check = 2.0;
  double run = 5.0;
  double slow = 120.0;
  // Extra CPU a case may spend while its worker first analyses a standard
  // library prefix; the case is then re-measured warm.
  double coldAllowance = 30.0;

  [[nodiscard]] double forTest(const TestFile &test) const {
    if (test.slow)
      return slow;
    return modeBuilds(test.mode) ? run : check;
  }
};

struct EngineOptions {
  std::filesystem::path self; // gti-test executable
  std::filesystem::path gti;  // gti executable (--isolate)
  std::filesystem::path workDirectory;
  bool isolate = false;
  bool bless = false;
  Limits limits;
};

class Engine {
public:
  explicit Engine(EngineOptions options);
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  // Runs one case. May raise SIGXCPU (the caller's CPU limit) or crash; the
  // parent process attributes either to the case.
  Outcome run(const TestFile &test);

private:
  void checkFormatting(const TestFile &test, Outcome &outcome);

  struct State;
  std::unique_ptr<State> state;
};

} // namespace gti_test
