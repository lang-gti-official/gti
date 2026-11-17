#pragma once

// Executes one gti-test case, either in this process through the compiler
// libraries with the analysed standard library shared across cases, or
// (`--isolate`) through a fresh `gti` process.

#include "test_file.h"

#include "gti/llvm_backend.h"

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
  // The case built a standard-library preamble in this worker, and `cpu`
  // was not re-measured warm.
  bool cold = false;
  // Under --isolate, the case still ran in-process (parse modes, mir
  // snapshots, C sections).
  bool inProcess = false;
};

struct Limits {
  double check = 2.0;
  double run = 5.0;
  double slow = 120.0;
  // CPU the language server spends on one LSP test's requests.
  double lsp = 3.0;
  // Wall time an LSP test may wait for any one response or publication
  // before it is reported as hung (not a budget: a loaded machine waits).
  double lspHang = 30.0;
  // CPU of a project transcript's commands, all together.
  double project = 20.0;
  // Extra CPU a case may spend while its worker first analyses a standard
  // library prefix; the case is then re-measured warm.
  double coldAllowance = 30.0;

  [[nodiscard]] double forTest(const TestFile &test) const {
    if (test.slow)
      return slow;
    if (test.mode == Mode::Lsp || test.mode == Mode::LspTranscript)
      return lsp;
    if (test.mode == Mode::Project)
      return project;
    return modeBuilds(test.mode) ? run : check;
  }
};

struct EngineOptions {
  std::filesystem::path self; // gti-test executable
  std::filesystem::path gti;  // gti executable (--isolate)
  std::filesystem::path lsp;  // gti_lsp executable (LSP tests)
  std::filesystem::path workDirectory;
  bool isolate = false;
  bool bless = false;
  // `--bless-directives`: bless may rewrite `//@` lines (inline stdout);
  // without it such a bless is refused and new output goes to a file.
  bool blessDirectives = false;
  // A cold case is re-measured warm when it uses more than this fraction of
  // its limit (half under --changed, whose budget rule needs a warm figure).
  double budgetFraction = 1.0;
  // GTI_TEST_INSTRUMENTATION: build every program with LLVM sanitizer
  // instrumentation (the sanitizer tier); off by default.
  lang::LlvmInstrumentationMode instrumentation =
      lang::LlvmInstrumentationMode::None;
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
  Outcome runLsp(const TestFile &test);
  Outcome runProject(const TestFile &test);

  struct State;
  std::unique_ptr<State> state;
};

} // namespace gti_test
