#pragma once

// One `gti-test` case: a `.gti` file under the test root, its `//@`
// directives, its `//~` diagnostic annotations and its `// --- name` split
// sections. See tests/ui/README.md and docs/plans/test-suite-redesign.md.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace gti_test {

enum class Mode {
  CheckPass,
  CheckFail,
  BuildPass,
  RunPass,
  RunFail,
  // Stop after the lexer and parser: syntax and recovery tests.
  ParsePass,
  ParseFail,
  // A `.gti` file under an `lsp` directory: requests at markers against a
  // language server (tests/lsp/README.md).
  Lsp,
  // A `.jsonl` request/response transcript under an `lsp` directory.
  LspTranscript,
};

[[nodiscard]] const char *modeName(Mode mode);
[[nodiscard]] bool modeRuns(Mode mode);
[[nodiscard]] bool modeBuilds(Mode mode);
[[nodiscard]] bool modeParses(Mode mode);
[[nodiscard]] bool modeFails(Mode mode);

// Output a test asserts besides diagnostics and run results (`//@ snapshot:`).
enum class Snapshot { Mir, LlvmIr, Diagnostics, Format };

[[nodiscard]] const char *snapshotName(Snapshot snapshot);
// The sibling expected file of a blessed snapshot (`name.mir`, `name.stderr`,
// `name.formatted`); empty for llvm-ir, which is checked by patterns only.
[[nodiscard]] std::string snapshotExtension(Snapshot snapshot);

// One FileCheck-style `// CHECK:` line.
struct CheckPattern {
  enum class Kind { Check, Next, Not };
  Kind kind = Kind::Check;
  std::string pattern;
  int writtenLine = 0;
};

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

// One execution of a run test's program: what it is given and what it must
// produce. `revision` is empty for a test without `//@ revisions:`.
struct RunSpec {
  std::string revision;
  Mode mode = Mode::RunPass;
  std::vector<std::string> stdoutLines;
  bool hasInlineStdout = false;
  std::optional<std::filesystem::path> stdoutFile;
  std::vector<std::string> stderrContains;
  std::optional<int> exitCode;
  std::vector<std::string> args;
  std::optional<std::string> stdinText;
};

// `//@ hover @a: text` and the other LSP expectations: the request, its
// arguments (marker, new name) and the expected value.
struct LspDirective {
  std::vector<std::string> words;
  std::string value;
  int writtenLine = 0;
};

struct TestFile {
  std::filesystem::path path;
  // Path relative to the invocation's working directory, for messages.
  std::string display;
  Mode mode = Mode::CheckPass;
  bool modeExplicit = false;
  // `//@ revisions: a b`: the program is built once and run once per
  // revision. Only run inputs and expectations vary (`//@[a] args: ...`).
  std::vector<std::string> revisions;
  // The executions of a run test, in order: one per revision, or exactly one.
  std::vector<RunSpec> runs;
  // The shared (unprefixed) run directives.
  std::vector<std::string> stdoutLines;
  bool hasInlineStdout = false;
  std::optional<std::filesystem::path> stdoutFile;
  std::vector<std::string> stderrContains;
  std::optional<int> exitCode;
  std::vector<std::string> args;
  std::optional<std::string> stdinText;
  bool optimize = false; // `//@ opt: O3`
  bool slow = false;
  // `//@ format-stable`: formatting the test is idempotent and the formatted
  // program also checks (check-pass tests without annotations only).
  bool formatStable = false;
  // `//@ execution-profile: concurrent` (the compiler's
  // `--execution-profile`); single-threaded by default.
  bool concurrent = false;
  // `//@ require-function-definitions`: analyse with the definition-closure
  // requirement executable backends apply (FrontendOptions'
  // requireFunctionDefinitions); always on for build and run tests.
  bool requireFunctionDefinitions = false;
  std::vector<Snapshot> snapshots;
  // `//@ mir-function: name`; empty snapshots every function of the test.
  std::vector<std::string> mirFunctions;
  // `// CHECK:` patterns, against the llvm-ir snapshot when there is one,
  // else the mir snapshot.
  std::vector<CheckPattern> checks;
  std::vector<LspDirective> lspDirectives;
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

[[nodiscard]] bool hasSnapshot(const TestFile &test, Snapshot snapshot);
// The expected file of a blessed snapshot.
[[nodiscard]] std::filesystem::path snapshotPath(const TestFile &test,
                                                 Snapshot snapshot);

// Replaces inline `//@ stdout:` lines (`//@[revision] stdout:` for a
// revision) with `output`.
[[nodiscard]] std::string blessInlineStdout(const TestFile &test,
                                            const std::string &output,
                                            const std::string &revision = "");

// The expected-output file of a run: `name.stdout`, or
// `name.<revision>.stdout`.
[[nodiscard]] std::filesystem::path
stdoutPathFor(const std::filesystem::path &test, const std::string &revision);

} // namespace gti_test
