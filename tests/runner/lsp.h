#pragma once

// The LSP test kinds. A worker keeps one `gti_lsp` process for all of its
// `.gti` LSP tests (so the standard library is analysed once per worker, as
// for compiler tests), opening each test as a fresh document; a transcript
// gets a fresh server of its own because protocol sequencing is its subject.
// See tests/lsp/README.md.

#include "test_file.h"

#include "llvm/Support/JSON.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace gti_test {

class LspServer {
public:
  // `arguments` follow the executable on its command line (`//@
  // execution-profile:` and `//@ raw-pointer-audit:`).
  LspServer(std::filesystem::path executable, std::filesystem::path log,
            std::vector<std::string> arguments = {});
  ~LspServer();
  LspServer(const LspServer &) = delete;
  LspServer &operator=(const LspServer &) = delete;

  // Starts the process; `initialize` is sent separately so a transcript can
  // drive it.
  bool start(std::string &error);
  // start() plus initialize/initialized with the capabilities tests need.
  bool startInitialized(double seconds, std::string &error);
  [[nodiscard]] bool running() const { return pid > 0; }
  void stop();

  void send(const llvm::json::Value &message);
  // The next message from the server, answering server-to-client requests
  // with a null result; nullopt on timeout or exit.
  std::optional<llvm::json::Value> receive(double deadline);
  // Sends a request and returns its response, keeping other messages for
  // waitFor.
  std::optional<llvm::json::Object>
  request(const std::string &method, llvm::json::Value params, double deadline);
  // The first message (queued or new) that satisfies `predicate`.
  std::optional<llvm::json::Object>
  waitFor(const std::function<bool(const llvm::json::Object &)> &predicate,
          double deadline);
  [[nodiscard]] std::string logTail() const;
  // Moves every message already readable without waiting into the queue
  // waitFor and latest search.
  void drain();
  // The newest queued message that satisfies `predicate` (removing it and
  // any older match), after draining.
  std::optional<llvm::json::Object>
  latest(const std::function<bool(const llvm::json::Object &)> &predicate);
  // CPU seconds the server process has used so far, or a negative value
  // where that cannot be measured.
  [[nodiscard]] double cpuSeconds() const;

  std::vector<std::string> tokenTypes;
  // Whether this server has already analysed a test (its standard library
  // is warm).
  bool warm = false;
  // The exit status once the process has exited (128 + signal if killed).
  int exitCode = -1;

private:
  std::filesystem::path executable;
  std::filesystem::path log;
  std::vector<std::string> arguments;
  pid_t pid = -1;
  int input = -1;
  int output = -1;
  std::string buffer;
  std::deque<llvm::json::Object> pending;
  long nextId = 1000;
};

struct LspResult {
  // Failure lines; empty on success.
  std::string failure;
  // The diagnostics published for the document, for annotation matching.
  std::vector<LocatedDiagnostic> diagnostics;
  bool published = false;
  bool timedOut = false;
  // The server exited during the test.
  bool crashed = false;
  // CPU the server spent on the test, when it could be read.
  std::optional<double> serverCpu;
};

// Writes the test's file sections into `directory`, opens each `.gti` one
// (except `//@ disk-only:` ones) as a document, applies the edit steps and
// checks each expectation directive. `standardLibrary` is the root that
// `@std:path` expectations are relative to. `hangGuard` is the wall time
// allowed for each awaited response or publication (the test fails as hung
// past it); the CPU budget is the caller's.
[[nodiscard]] LspResult runLspTest(LspServer &server, const TestFile &test,
                                   const std::filesystem::path &directory,
                                   const std::filesystem::path &standardLibrary,
                                   double hangGuard);

// Runs a `.jsonl` transcript against a fresh server. An expectation fails,
// naming what it awaited, once the server has been silent and idle (no CPU)
// for a second, or, after it has published diagnostics, silent for `quiet`
// seconds.
[[nodiscard]] LspResult
runLspTranscript(const std::filesystem::path &executable, const TestFile &test,
                 const std::filesystem::path &directory, double deadline,
                 double quiet);

// Whether these `gti_lsp` options run the raw-pointer audit, which publishes
// each version's diagnostics twice: the edit path's, then the audit's.
[[nodiscard]] bool auditing(const std::vector<std::string> &serverArguments);

// Wall-clock seconds, the clock deadlines use.
[[nodiscard]] double lspClock();

} // namespace gti_test
