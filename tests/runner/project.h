#pragma once

// The project/driver kind: a directory `tests/project/<name>/` holding a
// small project and a `test.transcript` of commands with their expected exit
// codes and output, run in a temporary copy of the directory. See
// tests/project/README.md.

#include "test_file.h"

#include <filesystem>
#include <string>
#include <vector>

namespace gti_test {

struct ProjectCommand {
  // `$ program ...` runs (`$ cd dir` moves); `env NAME=value` and
  // `unset NAME` change the environment of later commands.
  enum class Kind { Run, SetEnv, Unset };
  Kind kind = Kind::Run;
  std::vector<std::string> argv;
  // SetEnv/Unset: the variable and (SetEnv) its value, before `$DIR`,
  // `$HOME` and `$PATH` are expanded.
  std::string variable;
  std::string value;
  // `$ tty: program ...`: stdout and stderr are one pseudo-terminal, whose
  // output the `stdout:` expectations match.
  bool terminal = false;
  int writtenLine = 0;
  int exitCode = 0;
  // `stdout: text`, `stderr: text`, and the negated `!stdout:`/`!stderr:`.
  std::vector<std::string> stdoutContains;
  std::vector<std::string> stderrContains;
  std::vector<std::string> stdoutLacks;
  std::vector<std::string> stderrLacks;
};

struct ProjectTranscript {
  std::vector<ProjectCommand> commands;
  // A `slow` line: the transcript has the slow tier's limit (only under a
  // slow/ directory).
  bool slow = false;
};

// Parses a transcript; problems go to `errors` as "line N: ...".
[[nodiscard]] ProjectTranscript
parseProjectTranscript(const std::string &text,
                       std::vector<std::string> &errors);

struct ProjectResult {
  std::string failure;
  bool timedOut = false;
  double cpu = 0;
  // `--bless` rewrote the transcript.
  bool blessed = false;
};

struct ProjectTools {
  std::filesystem::path gti;
  std::filesystem::path lsp;
  std::filesystem::path standardLibrary;
};

// Copies the test's directory to `work` and runs its commands there under a
// total CPU budget of `limit` seconds. With `bless`, every command runs and
// the transcript is rewritten: each `? N` becomes the actual exit code, and a
// command without stdout/stderr expectations gains a `stderr:` line per
// diagnostic header it printed. Other expectations are never rewritten.
[[nodiscard]] ProjectResult runProjectTest(const TestFile &test,
                                           const ProjectTools &tools,
                                           const std::filesystem::path &work,
                                           double limit, bool bless = false);

} // namespace gti_test
