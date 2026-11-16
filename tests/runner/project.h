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
  std::vector<std::string> argv;
  int writtenLine = 0;
  int exitCode = 0;
  // `stdout: text`, `stderr: text`, and the negated `!stdout:`/`!stderr:`.
  std::vector<std::string> stdoutContains;
  std::vector<std::string> stderrContains;
  std::vector<std::string> stdoutLacks;
  std::vector<std::string> stderrLacks;
};

// Parses a transcript; problems go to `errors` as "line N: ...".
[[nodiscard]] std::vector<ProjectCommand>
parseProjectTranscript(const std::string &text,
                       std::vector<std::string> &errors);

struct ProjectResult {
  std::string failure;
  bool timedOut = false;
  double cpu = 0;
};

struct ProjectTools {
  std::filesystem::path gti;
  std::filesystem::path lsp;
  std::filesystem::path standardLibrary;
};

// Copies the test's directory to `work` and runs its commands there under a
// total CPU budget of `limit` seconds.
[[nodiscard]] ProjectResult runProjectTest(const TestFile &test,
                                           const ProjectTools &tools,
                                           const std::filesystem::path &work,
                                           double limit);

} // namespace gti_test
