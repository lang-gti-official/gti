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
  // Stop after semantic analysis (FrontendOptions::stopAfter = Semantics):
  // no HIR, MIR or lowering.
  AnalyzePass,
  AnalyzeFail,
  // A `.gti` file under an `lsp` directory: requests at markers against a
  // language server (tests/lsp/README.md).
  Lsp,
  // A `.jsonl` request/response transcript under an `lsp` directory.
  LspTranscript,
  // A `test.transcript` of commands run in a copy of its directory
  // (tests/project/README.md).
  Project,
};

[[nodiscard]] const char *modeName(Mode mode);
[[nodiscard]] bool modeRuns(Mode mode);
[[nodiscard]] bool modeBuilds(Mode mode);
[[nodiscard]] bool modeParses(Mode mode);
[[nodiscard]] bool modeAnalyzes(Mode mode);
[[nodiscard]] bool modeFails(Mode mode);

// Output a test asserts besides diagnostics and run results (`//@ snapshot:`).
enum class Snapshot { Mir, LlvmIr, Diagnostics, Format, OptimizationReport };

[[nodiscard]] const char *snapshotName(Snapshot snapshot);
// The sibling expected file of a blessed snapshot (`name.mir`, `name.stderr`,
// `name.formatted`); empty for llvm-ir and optimization-report, which are
// checked by patterns only.
[[nodiscard]] std::string snapshotExtension(Snapshot snapshot);

// One FileCheck-style `// CHECK:` line.
struct CheckPattern {
  enum class Kind { Check, Next, Not };
  Kind kind = Kind::Check;
  std::string pattern;
  int writtenLine = 0;
};

// Information and Hint are LSP publications (`//~ INFO`, `//~ HINT`).
enum class Severity { Error, Warning, Info, Hint };

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
  bool stderrEmpty = false;
  std::optional<int> exitCode;
  std::vector<std::string> args;
  std::optional<std::string> stdinText;
  // A sibling `name.stdin` (`name.<revision>.stdin`): bytes, which may hold
  // NUL or non-UTF-8 data that a `//@ stdin:` line cannot.
  std::optional<std::filesystem::path> stdinFile;
  // `//@ exit-signal: SIGABRT`: the program must die of this signal.
  std::optional<int> exitSignal;
  // `//@ env: NAME=value`: added to the program's environment.
  std::vector<std::pair<std::string, std::string>> env;
  // `//@ stdout-closed`: stdout is a pipe whose reader closed before launch.
  bool stdoutClosed = false;
};

// The signal number of a `SIGABRT`-style name, or nullopt.
[[nodiscard]] std::optional<int> signalNumber(const std::string &name);
[[nodiscard]] std::string signalText(int signal);

// How a non-`.gti` section takes part in a build (`// --- helper.c`).
enum class NativeKind { None, C, Cpp, Header };
[[nodiscard]] NativeKind nativeKind(const std::string &sectionName);

// An LSP edit step's section name, `step N` or `step N path.gti`: its number
// and the file it replaces (empty: the entry file).
struct LspStep {
  int number = 0;
  std::string file;
};
[[nodiscard]] std::optional<LspStep> lspStep(const std::string &sectionName);

// `//@ hover @a: text` and the other LSP expectations: the request, its
// arguments (marker, new name) and the expected value.
struct LspDirective {
  std::vector<std::string> words;
  std::string value;
  int writtenLine = 0;
  // The edit step it is asked of: 0 once every file is open, k after the
  // k-th `// --- step N` section.
  int step = 0;
  // The section the directive is written in (its home document); a bare
  // `@marker` is looked up there first. Directives above the first section
  // belong to the entry file.
  int section = 0;
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
  bool stderrEmpty = false;
  std::vector<std::string> diagnosticNotes;
  std::optional<int> exitCode;
  std::optional<int> exitSignal;
  std::vector<std::string> args;
  std::optional<std::string> stdinText;
  std::vector<std::pair<std::string, std::string>> env;
  bool stdoutClosed = false;
  bool optimize = false; // `//@ opt: O3`
  // `//@ opt:` was written, so a global `--opt`/GTI_TEST_OPT leaves it.
  bool optPinned = false;
  // Compile limits named as the compiler's options (`//@ constexpr-depth:
  // 16` is `gti --constexpr-depth 16`), in written order.
  std::vector<std::pair<std::string, std::size_t>> compileLimits;
  // `//@ target: x86_64-unknown-linux-gnu`: analyse for another target
  // (check and analyze modes; nothing is linked).
  std::string target;
  bool slow = false;
  // `//@ no-prelude` (analyze modes): analyse without the standard prelude.
  bool noPrelude = false;
  // The only problem is a fail mode without `//~ ERROR` annotations, which
  // `--bless` may seed.
  bool unannotatedFailMode = false;
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
  // `//@ define: NAME` (gti -D NAME): valueless configuration flags.
  std::vector<std::string> defines;
  // `//@ raw-pointer-audit: off|warn|strict` (gti --raw-pointer-audit=) and
  // `//@ raw-pointer-audit-library-findings`.
  std::string rawPointerAudit = "off";
  bool rawPointerAuditLibraryFindings = false;
  // `//@ native-args: ...`: what follows `--` on a gti command line, passed
  // to the native build (link flags, sanitizer flags for C/C++ sections).
  std::vector<std::string> nativeArgs;
  // `//@ stdlib-overlay: std/x.gti ...`: sections written over a private
  // copy of the standard library instead of beside the test.
  std::vector<std::string> stdlibOverlay;
  // `//@ allow-warnings` (gti-test.defaults only): unannotated warnings do
  // not fail the test; annotated ones must still match.
  bool allowWarnings = false;
  // Sibling directories a single-file test imports from (`import
  // "dir/x.gti"`), copied beside it.
  std::vector<std::string> importDirectories;
  std::vector<Snapshot> snapshots;
  // `//@ mir-function: name`; empty snapshots every function of the test.
  std::vector<std::string> mirFunctions;
  // `// CHECK:` patterns, against the llvm-ir snapshot when there is one,
  // else the mir snapshot.
  std::vector<CheckPattern> checks;
  std::vector<LspDirective> lspDirectives;
  // LSP tests: the `gti_lsp` options of `//@ execution-profile:` and
  // `//@ raw-pointer-audit:`, in that order. A worker keeps one server per
  // distinct list.
  std::vector<std::string> serverArguments;
  // `//@ format-options: tabSize=4 insertSpaces=true`: the FormattingOptions
  // of `formatting` requests (default tabSize=2 insertSpaces=true); values
  // are integers, true/false or strings.
  std::vector<std::pair<std::string, std::string>> formatOptions;
  // `//@ disk-only: lib.gti ...`: sections written beside the test but not
  // opened as documents.
  std::vector<std::string> diskOnly;
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

// The `//@` lines of the gti-test.defaults files above `test` that apply to
// it, outermost first; `origin` is `path:line`. Malformed lines go to
// `errors`.
struct DefaultDirective {
  std::string text;
  std::string origin;
};
[[nodiscard]] std::vector<DefaultDirective>
defaultDirectives(const std::filesystem::path &test,
                  std::vector<std::string> &errors);

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
// `name.stdin`, or `name.<revision>.stdin`.
[[nodiscard]] std::filesystem::path
stdinPathFor(const std::filesystem::path &test, const std::string &revision);

// Whether `text` can be written as inline `//@ stdout:` lines: printable
// UTF-8 without NUL or other control bytes (tabs and CR included).
[[nodiscard]] bool inlineSafe(const std::string &text);
// `text` with NUL, other control bytes and invalid UTF-8 written as `\xNN`,
// for failure messages.
[[nodiscard]] std::string visibleBytes(const std::string &text);

} // namespace gti_test
