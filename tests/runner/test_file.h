#pragma once

// One `gti-test` case: a `.gti` file under the test root, its `//@`
// directives, its `//~` diagnostic annotations and its `// --- name` split
// sections. See tests/ui/README.md and docs/plans/test-suite-redesign.md.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gti_test {

enum class Mode { CheckPass, CheckFail, BuildPass, RunPass, RunFail };

[[nodiscard]] const char *modeName(Mode mode);
[[nodiscard]] bool modeRuns(Mode mode);
[[nodiscard]] bool modeBuilds(Mode mode);

enum class Severity { Error, Warning };

[[nodiscard]] const char *severityName(Severity severity);

struct Annotation {
  Severity severity = Severity::Error;
  std::string code;
  // Optional message substring; empty matches any message.
  std::string substring;
  // Section (file) name and 1-based line within it. `unlocated` annotations
  // (`//~?`) match a diagnostic anywhere outside the test's own lines, such
  // as one located in the standard library.
  std::string section;
  int line = 0;
  bool unlocated = false;
  // 1-based line of the test file the annotation is written on.
  int writtenLine = 0;
};

struct Section {
  std::string name;
  // 1-based line of the test file holding the section's first line.
  int firstLine = 1;
  std::string text;
};

struct TestFile {
  std::filesystem::path path;
  // Path relative to the invocation's working directory, for messages.
  std::string display;
  Mode mode = Mode::CheckPass;
  bool modeExplicit = false;
  std::vector<std::string> stdoutLines;
  bool hasInlineStdout = false;
  std::optional<std::filesystem::path> stdoutFile;
  std::vector<std::string> stderrContains;
  std::optional<int> exitCode;
  std::vector<std::string> args;
  std::optional<std::string> stdinText;
  bool optimize = false; // `//@ opt: O3`
  bool slow = false;
  std::string reason;
  std::vector<std::string> only;
  // Self-test fault injection (`//@ runner-fault:`); honoured only when
  // GTI_TEST_SELF_TEST=1, and rejected by tidy.
  std::string runnerFault;
  std::vector<Section> sections;
  std::vector<Annotation> annotations;
  // Malformed directives or annotations. A test with errors fails without
  // being compiled.
  std::vector<std::string> errors;
  // Tests with equal keys analyse the same trusted prefix; workers prefer
  // running them back to back.
  std::string groupKey;
};

[[nodiscard]] TestFile parseTestFile(const std::filesystem::path &path,
                                     std::string display);

// Whether the test's `only:` gates admit this host.
[[nodiscard]] bool hostAdmits(const TestFile &test);

// The whole text of a file, or nullopt when unreadable.
[[nodiscard]] std::optional<std::string>
readFile(const std::filesystem::path &path);
bool writeFile(const std::filesystem::path &path, const std::string &text);

// Rewrites the `//~` annotations of the test's file so that they describe
// exactly `diagnostics` (kept annotations retain their message substrings).
// Returns the new text.
struct LocatedDiagnostic {
  Severity severity = Severity::Error;
  std::string code;
  std::string message;
  std::string section; // empty when outside the test's files
  int line = 0;
  // Where an unlocated diagnostic was reported (path:line), for messages.
  std::string elsewhere;
};
[[nodiscard]] std::string
blessAnnotations(const TestFile &test,
                 const std::vector<LocatedDiagnostic> &diagnostics);

// Replaces inline `//@ stdout:` lines with `output`.
[[nodiscard]] std::string blessInlineStdout(const TestFile &test,
                                            const std::string &output);

} // namespace gti_test
