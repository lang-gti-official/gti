#include "execute.h"
#include "lsp.h"
#include "project.h"
#include "snapshot.h"

#include "gti/compile_limits.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/formatter.h"
#include "gti/frontend.h"
#include "gti/frontend_cache.h"
#include "gti/lexer.h"
#include "gti/lowered_program.h"
#include "gti/parser.h"
#include "gti/semantic_preamble.h"
#include "gti/standard_library.h"
#include "gti/target.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace gti_test {

const char *statusName(Status status) {
  switch (status) {
  case Status::Pass:
    return "PASS";
  case Status::Fail:
    return "FAIL";
  case Status::Ice:
    return "ICE";
  case Status::Crash:
    return "CRASH";
  case Status::Timeout:
    return "TIMEOUT";
  case Status::Skip:
    return "SKIP";
  }
  return "?";
}

namespace {

double cpuSeconds(int who) {
  rusage usage{};
  getrusage(who, &usage);
  return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
             1e6;
}

// CPU spent by this process and every child it has reaped.
double totalCpu() {
  return cpuSeconds(RUSAGE_SELF) + cpuSeconds(RUSAGE_CHILDREN);
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void limitCpu(double seconds) {
  rlimit limit{};
  getrlimit(RLIMIT_CPU, &limit);
  limit.rlim_cur =
      seconds <= 0 ? RLIM_INFINITY : static_cast<rlim_t>(std::ceil(seconds));
  setrlimit(RLIMIT_CPU, &limit);
}

// The `check` proof level stops after the verified, optimized MIR and the
// lowered program built from it; nothing is generated.
class CheckBackend final : public lang::Backend {
public:
  [[nodiscard]] std::string_view name() const override {
    return "gti-test-check";
  }
  [[nodiscard]] lang::BackendArtifact
  generate(lang::LoweredProgram program) override {
    if (inspect)
      inspect(program);
    return {};
  }
  // Called with the lowered program, for the mir snapshot.
  std::function<void(const lang::LoweredProgram &)> inspect;
};

bool internalError(const std::string &code, const std::string &message) {
  const std::string prefix = "internal compiler error";
  if (code == "GTI-B0001")
    return true;
  if (message.size() < prefix.size())
    return false;
  for (std::size_t index = 0; index < prefix.size(); ++index)
    if (std::tolower(static_cast<unsigned char>(message[index])) !=
        prefix[index])
      return false;
  return true;
}

std::string tail(const std::string &text, std::size_t lines) {
  std::size_t position = text.size();
  std::size_t count = 0;
  while (position > 0) {
    --position;
    if (text[position] == '\n' && position + 1 < text.size() && ++count > lines)
      return "..." + text.substr(position);
  }
  return text;
}

std::vector<std::string> lines(const std::string &text) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    result.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return result;
}

std::string indent(const std::string &text, const std::string &prefix) {
  std::string result;
  for (const std::string &line : lines(text))
    result += prefix + line + "\n";
  return result;
}

std::string diff(const std::string &expected, const std::string &actual) {
  // Binary output is compared bytewise but shown with escapes.
  if (!inlineSafe(expected + actual))
    return lineDiff(visibleBytes(expected), visibleBytes(actual), 40);
  return lineDiff(expected, actual, 40);
}

// The `//@` lines of a test's text, which bless leaves alone unless
// --bless-directives is given.
std::vector<std::string> directiveLines(const std::string &text) {
  std::vector<std::string> result;
  for (const std::string &line : lines(text)) {
    const std::size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line.compare(first, 3, "//@") == 0)
      result.push_back(line);
  }
  return result;
}

struct ProcessRun {
  bool started = false;
  int exitCode = -1;
  int signal = 0;
  bool timedOut = false;
  std::string out;
  std::string err;
};

using Environment = std::vector<std::pair<std::string, std::string>>;

ProcessRun runProcess(const std::vector<std::string> &argv,
                      const std::optional<std::string> &input, double cpuLimit,
                      double wallLimit, const fs::path &directory,
                      const Environment &environment = {},
                      const std::optional<fs::path> &inputFile = std::nullopt) {
  const fs::path in = inputFile ? *inputFile : directory / ".stdin";
  const fs::path out = directory / ".stdout";
  const fs::path err = directory / ".stderr";
  if (!inputFile)
    writeFile(in, input.value_or(""));
  std::vector<char *> arguments;
  for (const std::string &argument : argv)
    arguments.push_back(const_cast<char *>(argument.c_str()));
  arguments.push_back(nullptr);
  ProcessRun run;
  const pid_t pid = fork();
  if (pid == 0) {
    rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    if (cpuLimit > 0) {
      const auto seconds = static_cast<rlim_t>(std::ceil(cpuLimit));
      rlimit cpu{seconds, seconds + 1};
      setrlimit(RLIMIT_CPU, &cpu);
    }
    const int inFd = open(in.c_str(), O_RDONLY);
    const int outFd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int errFd = open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (inFd < 0 || outFd < 0 || errFd < 0 || chdir(directory.c_str()) != 0)
      _exit(127);
    for (const auto &[name, value] : environment)
      setenv(name.c_str(), value.c_str(), 1);
    dup2(inFd, 0);
    dup2(outFd, 1);
    dup2(errFd, 2);
    execv(arguments[0], arguments.data());
    _exit(127);
  }
  if (pid < 0)
    return run;
  run.started = true;
  const double start = wallSeconds();
  int status = 0;
  while (true) {
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid)
      break;
    if (done < 0)
      return run;
    if (wallSeconds() - start > wallLimit) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      run.timedOut = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (WIFEXITED(status))
    run.exitCode = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    run.signal = WTERMSIG(status);
    if (run.signal == SIGXCPU)
      run.timedOut = true;
  }
  run.out = readFile(out).value_or("");
  run.err = readFile(err).value_or("");
  return run;
}

struct Compiled {
  bool succeeded = false;
  // An internal compiler error or a failure that is not a diagnostic (MIR
  // verification, native link). Never blessed.
  std::optional<std::string> internalFailure;
  std::vector<LocatedDiagnostic> diagnostics;
  std::vector<std::string> diagnosticNotes;
  fs::path executable;
  // The test's own C/C++ sections did not build: a failing test, not an
  // internal error, and not blessable.
  std::optional<std::string> nativeFailure;
};

bool singleFile(const TestFile &test) {
  return test.sections.size() == 1 && test.sections.front().firstLine == 1;
}

std::string where(const TestFile &test, const std::string &section, int line) {
  if (section.empty())
    return "outside the test";
  if (singleFile(test))
    return "line " + std::to_string(line);
  return section + ":" + std::to_string(line);
}

std::string describe(const TestFile &test,
                     const LocatedDiagnostic &diagnostic) {
  std::string text = std::string(severityName(diagnostic.severity)) + " " +
                     diagnostic.code + " at " +
                     where(test, diagnostic.section, diagnostic.line);
  if (diagnostic.section.empty() && !diagnostic.elsewhere.empty())
    text += " (" + diagnostic.elsewhere + ")";
  return text + ": " + diagnostic.message;
}

bool matches(const Annotation &annotation,
             const LocatedDiagnostic &diagnostic) {
  if (annotation.severity != diagnostic.severity ||
      annotation.code != diagnostic.code)
    return false;
  if (annotation.unlocated) {
    if (!diagnostic.section.empty())
      return false;
  } else if (annotation.section != diagnostic.section ||
             annotation.line != diagnostic.line) {
    return false;
  }
  return annotation.substring.empty() ||
         diagnostic.message.find(annotation.substring) != std::string::npos;
}

// Every annotation must match a distinct diagnostic and every error or
// warning must be annotated. Returns the mismatch lines.
std::vector<std::string>
compareDiagnostics(const TestFile &test,
                   const std::vector<LocatedDiagnostic> &diagnostics) {
  std::vector<bool> used(diagnostics.size(), false);
  std::vector<std::string> problems;
  std::vector<std::size_t> order(test.annotations.size());
  for (std::size_t index = 0; index < order.size(); ++index)
    order[index] = index;
  // Annotations with message text first, so a bare one cannot take the
  // diagnostic a more specific one needs.
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) {
                     return !test.annotations[a].substring.empty() &&
                            test.annotations[b].substring.empty();
                   });
  std::vector<std::string> expected;
  for (const std::size_t index : order) {
    const Annotation &annotation = test.annotations[index];
    bool found = false;
    for (std::size_t candidate = 0; candidate < diagnostics.size();
         ++candidate) {
      if (!used[candidate] && matches(annotation, diagnostics[candidate])) {
        used[candidate] = true;
        found = true;
        break;
      }
    }
    if (!found) {
      std::string text =
          "expected: " + std::string(severityName(annotation.severity)) + " " +
          annotation.code + " at " +
          (annotation.unlocated
               ? std::string("outside the test")
               : where(test, annotation.section, annotation.line));
      if (!annotation.substring.empty())
        text += " containing \"" + annotation.substring + "\"";
      problems.push_back(text);
    }
  }
  for (std::size_t index = 0; index < diagnostics.size(); ++index)
    if (!used[index] && !(test.allowWarnings &&
                          diagnostics[index].severity == Severity::Warning))
      problems.push_back("actual:   " + describe(test, diagnostics[index]));
  return problems;
}

} // namespace

struct Engine::State {
  EngineOptions options;
  lang::driver::ToolchainLayout toolchain;
  lang::TargetInfo target;
  lang::TargetInfo concurrentTarget;
  lang::driver::FrontendReuse reuse;
  std::set<std::string> warmGroups;
  // One language server per distinct argument list (`//@
  // execution-profile:` and the like), each kept for every test using it.
  std::map<std::string, std::unique_ptr<LspServer>> lspServers;
  int lspDocuments = 0;
  fs::path caseDirectory;
};

Engine::Engine(EngineOptions options) : state(std::make_unique<State>()) {
  state->options = std::move(options);
  state->toolchain =
      lang::driver::discoverToolchainLayout(state->options.self.c_str());
  state->target =
      lang::driver::AnalysisConfiguration(
          lang::TargetInfo::host(), lang::ExecutionProfile::SingleThreaded,
          lang::driver::RuntimeProfile::Hosted)
          .target();
  state->concurrentTarget =
      lang::driver::AnalysisConfiguration(lang::TargetInfo::host(),
                                          lang::ExecutionProfile::Concurrent,
                                          lang::driver::RuntimeProfile::Hosted)
          .target();
  state->reuse = lang::driver::FrontendReuse{
      .parses = std::make_shared<lang::FrontendCache>(lang::FrontendCacheLimits{
          .maximumEntries = 2048,
          .maximumEstimatedBytes = 768U * 1024U * 1024U}),
      .preambles = std::make_shared<lang::SemanticPreambleCache>(
          lang::SemanticPreambleCacheLimits{.maximumEntries = 4})};
  fs::create_directories(state->options.workDirectory / "case");
  state->caseDirectory = fs::canonical(state->options.workDirectory / "case");
}

Engine::~Engine() = default;

// A project transcript: its commands' CPU, all together, against the
// project limit.
Outcome Engine::runProject(const TestFile &test) {
  Outcome outcome;
  const EngineOptions &options = state->options;
  const double start = lspClock();
  const ProjectResult result =
      runProjectTest(test,
                     ProjectTools{options.gti, options.lsp,
                                  state->toolchain.standardLibrary.root},
                     state->caseDirectory / "project",
                     options.limits.forTest(test), options.bless);
  if (result.blessed)
    outcome.blessed.push_back(test.display);
  outcome.cpu = result.cpu;
  outcome.wall = lspClock() - start;
  if (result.timedOut)
    outcome.status = Status::Timeout;
  else if (!result.failure.empty())
    outcome.status = Status::Fail;
  outcome.detail = result.failure;
  return outcome;
}

// LSP tests are measured in wall time: the work happens in the server
// process. A worker's first LSP test (or any transcript, which starts its own
// server) may also spend the cold allowance analysing the standard library.
Outcome Engine::runLsp(const TestFile &test) {
  Outcome outcome;
  const EngineOptions &options = state->options;
  const double limit = options.limits.forTest(test);
  const double start = lspClock();
  const fs::path directory =
      state->caseDirectory / ("lsp" + std::to_string(++state->lspDocuments));
  std::error_code error;
  fs::remove_all(directory, error);
  bool cold = test.mode == Mode::LspTranscript;
  LspResult result;
  if (test.mode == Mode::LspTranscript) {
    result =
        runLspTranscript(options.lsp, test, directory,
                         start + limit + options.limits.coldAllowance, limit);
  } else {
    std::string key;
    for (const std::string &argument : test.serverArguments)
      key += argument + "\n";
    std::unique_ptr<LspServer> &server = state->lspServers[key];
    if (!server || !server->running()) {
      server = std::make_unique<LspServer>(
          options.lsp,
          options.workDirectory /
              ("gti_lsp" + std::to_string(state->lspServers.size()) + ".log"),
          test.serverArguments);
      std::string failure;
      if (!server->startInitialized(limit + options.limits.coldAllowance,
                                    failure)) {
        outcome.status = Status::Fail;
        std::string command = options.lsp.string();
        for (const std::string &argument : test.serverArguments)
          command += " " + argument;
        outcome.detail = "could not start " + command + ": " + failure + "\n";
        return outcome;
      }
    }
    cold = !server->warm;
    result = runLspTest(
        *server, test, directory, state->toolchain.standardLibrary.root,
        start + limit + (cold ? options.limits.coldAllowance : 0.0));
  }
  outcome.wall = lspClock() - start;
  outcome.cpu = outcome.wall;
  outcome.cold = cold;
  if (result.crashed || result.timedOut) {
    outcome.status = result.crashed ? Status::Crash : Status::Timeout;
    outcome.detail = result.failure;
    return outcome;
  }
  std::string detail = result.failure;
  if (result.published) {
    const std::vector<std::string> problems =
        compareDiagnostics(test, result.diagnostics);
    if (!problems.empty() && options.bless) {
      writeFile(test.path, blessAnnotations(test, result.diagnostics));
      outcome.blessed.push_back(test.display);
    } else {
      for (const std::string &problem : problems)
        detail += problem + "\n";
    }
  }
  if (!detail.empty()) {
    outcome.status = Status::Fail;
    outcome.detail = detail;
  } else if (!cold && outcome.wall > limit) {
    outcome.status = Status::Timeout;
    outcome.detail = "over the time limit: " + std::to_string(outcome.wall) +
                     " s > " + std::to_string(limit) + " s\n";
  }
  return outcome;
}

namespace {

struct Materialized {
  fs::path entry;
  std::map<std::string, std::string> sections; // path string -> name
  // C and C++ sections, built and linked as a project's native sources.
  std::vector<fs::path> cSources;
  std::vector<fs::path> cppSources;
  // A private standard library holding `//@ stdlib-overlay:` sections.
  std::optional<lang::StandardLibraryLayout> standardLibrary;
  std::string failure;
};

// The stdlib module name a `std/a/b.gti` overlay registers (`a.b`), or "".
std::string overlayModule(const std::string &section) {
  if (!section.starts_with("std/") || !section.ends_with(".gti"))
    return "";
  std::string name = section.substr(4, section.size() - 8);
  std::replace(name.begin(), name.end(), '/', '.');
  return name;
}

// Copies the standard library to `copy` and writes the overlay sections over
// it, registering each new `std/...` file in module-map.json the way the
// test-only map helpers did.
std::optional<lang::StandardLibraryLayout>
overlayStandardLibrary(const TestFile &test,
                       const lang::StandardLibraryLayout &original,
                       const fs::path &copy, std::string &failure) {
  std::error_code error;
  fs::remove_all(copy, error);
  fs::create_directories(copy, error);
  for (const auto &entry : fs::directory_iterator(original.root, error)) {
    if (entry.path().filename() == "tests")
      continue;
    fs::copy(entry.path(), copy / entry.path().filename(),
             fs::copy_options::recursive, error);
    if (error) {
      failure = "could not copy the standard library: " + error.message();
      return std::nullopt;
    }
  }
  const fs::path mapPath = copy / "module-map.json";
  for (const Section &section : test.sections) {
    if (std::find(test.stdlibOverlay.begin(), test.stdlibOverlay.end(),
                  section.name) == test.stdlibOverlay.end())
      continue;
    const fs::path path = copy / section.name;
    fs::create_directories(path.parent_path(), error);
    writeFile(path, section.text);
  }
  llvm::Expected<llvm::json::Value> parsed =
      llvm::json::parse(readFile(mapPath).value_or(""));
  if (!parsed) {
    failure = "module-map.json: " + llvm::toString(parsed.takeError());
    return std::nullopt;
  }
  llvm::json::Object *map = parsed->getAsObject();
  llvm::json::Object *modules = map ? map->getObject("modules") : nullptr;
  if (modules == nullptr) {
    failure = "module-map.json has no modules object";
    return std::nullopt;
  }
  bool changed = false;
  for (const std::string &name : test.stdlibOverlay) {
    if (!fs::exists(copy / name)) {
      failure = "no " + name + " in the standard library or the test";
      return std::nullopt;
    }
    const std::string module = overlayModule(name);
    if (module.empty())
      continue;
    bool registered = false;
    for (const auto &[key, value] : *modules)
      registered = registered || value.getAsString() == name;
    if (!registered) {
      (*modules)[module] = name;
      changed = true;
    }
  }
  if (changed) {
    std::string text;
    llvm::raw_string_ostream out(text);
    out << llvm::formatv("{0:2}", llvm::json::Value(std::move(*map)));
    out.flush();
    writeFile(mapPath, text + "\n");
  }
  return lang::StandardLibraryLayout{
      .root = copy,
      .prelude = copy / original.prelude.lexically_relative(original.root)};
}

Materialized materialize(const TestFile &test, const fs::path &directory,
                         const lang::StandardLibraryLayout &standardLibrary) {
  std::error_code error;
  for (const auto &entry : fs::directory_iterator(directory, error))
    fs::remove_all(entry.path(), error);
  Materialized result;
  const fs::path overlayRoot = directory.parent_path() / "stdlib";
  if (!test.stdlibOverlay.empty())
    result.standardLibrary = overlayStandardLibrary(
        test, standardLibrary, overlayRoot, result.failure);
  for (const Section &section : test.sections) {
    const bool overlay =
        std::find(test.stdlibOverlay.begin(), test.stdlibOverlay.end(),
                  section.name) != test.stdlibOverlay.end();
    const fs::path path = overlay
                              ? fs::canonical(overlayRoot, error) / section.name
                              : directory / section.name;
    if (!overlay) {
      fs::create_directories(path.parent_path(), error);
      writeFile(path, section.text);
    }
    result.sections[path.string()] = section.name;
    result.sections[path.lexically_normal().string()] = section.name;
    if (overlay)
      continue;
    if (nativeKind(section.name) == NativeKind::C)
      result.cSources.push_back(path);
    else if (nativeKind(section.name) == NativeKind::Cpp)
      result.cppSources.push_back(path);
  }
  for (const std::string &imported : test.importDirectories)
    fs::copy(test.path.parent_path() / imported, directory / imported,
             fs::copy_options::recursive, error);
  result.entry = directory / test.sections.front().name;
  return result;
}

LocatedDiagnostic locate(const Materialized &files,
                         lang::DiagnosticSeverity severity,
                         const std::string &code, const std::string &message,
                         const std::string &source, int line) {
  LocatedDiagnostic located;
  located.severity = severity == lang::DiagnosticSeverity::Error
                         ? Severity::Error
                         : Severity::Warning;
  located.code = code;
  located.message = message;
  auto found = files.sections.find(source);
  if (found == files.sections.end() && !source.empty()) {
    std::error_code error;
    const fs::path canonical = fs::weakly_canonical(source, error);
    if (!error)
      found = files.sections.find(canonical.string());
  }
  if (found != files.sections.end() && line > 0) {
    located.section = found->second;
    located.line = line;
  } else {
    located.elsewhere = source + ":" + std::to_string(line);
  }
  return located;
}

void collect(Compiled &compiled, const Materialized &files,
             const std::vector<lang::Diagnostic> &diagnostics) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.severity != lang::DiagnosticSeverity::Error &&
        diagnostic.severity != lang::DiagnosticSeverity::Warning)
      continue;
    if (internalError(diagnostic.code, diagnostic.message) &&
        !compiled.internalFailure)
      compiled.internalFailure =
          diagnostic.code + " at " + diagnostic.primary.source + ":" +
          std::to_string(diagnostic.primary.line) + ": " + diagnostic.message;
    for (const auto &related : diagnostic.related)
      compiled.diagnosticNotes.push_back(related.message);
    compiled.diagnostics.push_back(
        locate(files, diagnostic.severity, diagnostic.code, diagnostic.message,
               diagnostic.primary.source, diagnostic.primary.line));
  }
}

void collectCompilation(Compiled &compiled, const Materialized &files,
                        const lang::driver::CompilationResult &result) {
  collect(compiled, files, result.diagnostics);
  if (result.status ==
          lang::driver::CompilationStatus::MirVerificationFailure &&
      !compiled.internalFailure)
    compiled.internalFailure =
        "MIR verification failed: " + (result.mirErrors.empty()
                                           ? std::string("(no detail)")
                                           : result.mirErrors.front().message);
  if (result.status == lang::driver::CompilationStatus::BackendFailure &&
      !compiled.internalFailure &&
      std::none_of(compiled.diagnostics.begin(), compiled.diagnostics.end(),
                   [](const LocatedDiagnostic &d) {
                     return d.severity == Severity::Error;
                   }))
    compiled.internalFailure =
        "backend failed: " + result.backendFailureDetails.value_or("");
}

const std::regex &locatedHeader() {
  static const std::regex pattern(
      R"(^(.+):(\d+):(\d+): (error|warning)\[(GTI-[A-Z]+[0-9]+)\]: (.*)$)");
  return pattern;
}

const std::regex &unlocatedHeader() {
  static const std::regex pattern(
      R"(^(error|warning)\[(GTI-[A-Z]+[0-9]+)\]: (.*)$)");
  return pattern;
}

void collectRendered(Compiled &compiled, const Materialized &files,
                     const std::string &stderrText) {
  std::vector<lang::Diagnostic> diagnostics;
  for (const std::string &line : lines(stderrText)) {
    std::smatch match;
    lang::Diagnostic diagnostic;
    if (std::regex_match(line, match, locatedHeader())) {
      diagnostic.primary.source = match[1].str();
      diagnostic.primary.line = std::stoi(match[2].str());
      diagnostic.severity = match[4].str() == "error"
                                ? lang::DiagnosticSeverity::Error
                                : lang::DiagnosticSeverity::Warning;
      diagnostic.code = match[5].str();
      diagnostic.message = match[6].str();
    } else if (std::regex_match(line, match, unlocatedHeader())) {
      diagnostic.primary.line = 0;
      diagnostic.severity = match[1].str() == "error"
                                ? lang::DiagnosticSeverity::Error
                                : lang::DiagnosticSeverity::Warning;
      diagnostic.code = match[2].str();
      diagnostic.message = match[3].str();
    } else {
      static const std::regex note(R"(^.+:[0-9]+:[0-9]+: note: (.*)$)");
      if (std::regex_match(line, match, note))
        compiled.diagnosticNotes.push_back(match[1].str());
      continue;
    }
    diagnostics.push_back(std::move(diagnostic));
  }
  collect(compiled, files, diagnostics);
}

// parse-pass / parse-fail: the lexer and parser of every section, nothing
// after them.
void parseOnly(Compiled &compiled, const Materialized &files,
               const TestFile &test) {
  std::vector<lang::Diagnostic> diagnostics;
  for (const Section &section : test.sections) {
    if (!section.name.ends_with(".gti"))
      continue;
    lang::Lexer lexer;
    std::vector<lang::Token> tokens = lexer.scan(
        section.text,
        (files.entry.parent_path() / section.name).lexically_normal().string());
    diagnostics.insert(diagnostics.end(), lexer.errors().begin(),
                       lexer.errors().end());
    lang::Parser parser(std::move(tokens), section.text);
    (void)parser.parse();
    diagnostics.insert(diagnostics.end(), parser.errors().begin(),
                       parser.errors().end());
  }
  collect(compiled, files, diagnostics);
  compiled.succeeded = std::none_of(
      compiled.diagnostics.begin(), compiled.diagnostics.end(),
      [](const LocatedDiagnostic &d) { return d.severity == Severity::Error; });
}

// Rendered diagnostics as `gti` prints them, with the test's directory and
// the standard library root replaced by stable names.
std::string normalizeRendered(std::string text, const fs::path &directory,
                              const fs::path &standardLibrary) {
  const auto replace = [&](const std::string &from, const std::string &to) {
    if (from.empty())
      return;
    for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos;
         at += to.size())
      text.replace(at, from.size(), to);
  };
  replace(directory.string() + "/", "");
  replace(standardLibrary.string() + "/", "$STD/");
  return text;
}

} // namespace

// `//@ format-stable`: the formatter reaches a fixed point in one pass and
// the formatted program checks like the original.
void Engine::checkFormatting(const TestFile &test, Outcome &outcome) {
  TestFile formatted = test;
  formatted.formatStable = false;
  // Snapshots and CHECK patterns describe the original text; the formatted
  // copy only has to check.
  formatted.snapshots.clear();
  formatted.checks.clear();
  std::string problems;
  try {
    const lang::Formatter formatter;
    for (Section &section : formatted.sections) {
      const std::string once = formatter.format(section.text);
      std::string twice = formatter.format(once);
      if (test.runnerFault == "format-drift")
        twice += "\n";
      if (once != twice)
        problems += "formatting " + section.name +
                    " is not idempotent (-first +second):\n" +
                    diff(once, twice);
      section.text = once;
    }
  } catch (const std::exception &exception) {
    problems += std::string("the formatter threw: ") + exception.what() + "\n";
  }
  if (problems.empty()) {
    // The copy is never blessed: it shares the original's path.
    const bool bless = state->options.bless;
    state->options.bless = false;
    const Outcome again = run(formatted);
    state->options.bless = bless;
    outcome.cpu += again.cpu;
    outcome.wall += again.wall;
    if (again.status != Status::Pass)
      problems += std::string("the formatted program does not check (") +
                  statusName(again.status) + "):\n" +
                  indent(again.detail, "    ");
  }
  if (!problems.empty()) {
    outcome.status = Status::Fail;
    outcome.detail = problems;
  }
}

Outcome Engine::run(const TestFile &test) {
  Outcome outcome;
  if (!test.errors.empty()) {
    outcome.status = Status::Fail;
    for (const std::string &error : test.errors)
      outcome.detail += "invalid test: " + error + "\n";
    return outcome;
  }
  if (!hostAdmits(test)) {
    outcome.status = Status::Skip;
    return outcome;
  }
  if (test.mode == Mode::Lsp || test.mode == Mode::LspTranscript)
    return runLsp(test);
  if (test.mode == Mode::Project)
    return runProject(test);
  const EngineOptions &options = state->options;
  const double limit = options.limits.forTest(test);
  const bool warm = state->warmGroups.contains(test.groupKey);

  // A stdlib overlay analyses its own library with private caches, so the
  // worker's shared preamble (and its LRU slots) stay with ordinary tests.
  const lang::driver::FrontendReuse reuse =
      test.stdlibOverlay.empty()
          ? state->reuse
          : lang::driver::FrontendReuse{
                .parses = std::make_shared<lang::FrontendCache>(),
                .preambles = std::make_shared<lang::SemanticPreambleCache>()};
  const auto requestFor = [&](const TestFile &file, const Materialized &files) {
    const lang::TargetInfo target =
        file.defines.empty() && file.target.empty()
            ? (file.concurrent ? state->concurrentTarget : state->target)
            : lang::driver::AnalysisConfiguration(
                  file.target.empty() ? lang::TargetInfo::host()
                                      : lang::parseTargetTriple(file.target)
                                            .value_or(lang::TargetInfo::host()),
                  file.concurrent ? lang::ExecutionProfile::Concurrent
                                  : lang::ExecutionProfile::SingleThreaded,
                  lang::driver::RuntimeProfile::Hosted, file.defines)
                  .target();
    lang::CompileLimitOverrides limits;
    for (const auto &[name, value] : file.compileLimits)
      limits.set(*lang::findCompileLimit(name), value);
    auto request =
        lang::driver::CompilationRequest(
            files.entry,
            files.standardLibrary.value_or(state->toolchain.standardLibrary),
            target,
            file.optimize ? lang::OptimizationLevel::O3
                          : lang::OptimizationLevel::O0,
            lang::CppStandard::Cpp23, {}, file.defines, {}, {}, false, {},
            lang::parseRawPointerAuditMode(file.rawPointerAudit)
                .value_or(lang::RawPointerAuditMode::Off),
            {}, {}, file.rawPointerAuditLibraryFindings)
            .withCompileLimits(lang::resolveCompileLimits({}, limits))
            .withFrontendReuse(reuse);
    return file.requireFunctionDefinitions ? request.withDefinitionClosure()
                                           : request;
  };
  // The `gti` options and environment that reproduce the test's analysis.
  const auto gtiArguments = [&](const Materialized &files) {
    std::vector<std::string> argv;
    if (test.concurrent) {
      argv.push_back("--execution-profile");
      argv.push_back("concurrent");
    }
    for (const std::string &flag : test.defines) {
      argv.push_back("-D");
      argv.push_back(flag);
    }
    if (test.rawPointerAudit != "off")
      argv.push_back("--raw-pointer-audit=" + test.rawPointerAudit);
    if (test.rawPointerAuditLibraryFindings)
      argv.push_back("--raw-pointer-audit-library-findings");
    for (const auto &[name, value] : test.compileLimits) {
      argv.push_back("--" + name);
      argv.push_back(std::to_string(value));
    }
    Environment environment;
    if (files.standardLibrary)
      environment.emplace_back("GTI_STDLIB_PATH",
                               files.standardLibrary->root.string());
    return std::pair(argv, environment);
  };

  // `//@ snapshot:` outputs, after the test's other expectations passed.
  // Returns the failure text.
  const auto snapshots = [&](const Materialized &files, bool bless,
                             Outcome &result) {
    std::string problems;
    const auto compare = [&](Snapshot kind, const std::string &actual) {
      if (!test.checks.empty() &&
          (kind == Snapshot::LlvmIr ||
           (kind == Snapshot::Mir && !hasSnapshot(test, Snapshot::LlvmIr)))) {
        for (const std::string &problem : matchChecks(test.checks, actual))
          problems += problem + "\n";
        return;
      }
      bool blessed = false;
      problems += compareSnapshot(test, kind, actual, bless, blessed);
      if (blessed)
        result.blessed.push_back(snapshotPath(test, kind).string());
    };
    for (const Snapshot kind : test.snapshots) {
      if (kind == Snapshot::Format) {
        const lang::Formatter formatter;
        std::string formatted;
        for (const Section &section : test.sections) {
          if (test.sections.size() > 1 || section.firstLine != 1)
            formatted += "// --- " + section.name + "\n";
          formatted += section.name.ends_with(".gti")
                           ? formatter.format(section.text)
                           : section.text;
        }
        compare(kind, formatted);
      } else if (kind == Snapshot::Diagnostics) {
        const fs::path entry =
            files.entry.lexically_relative(state->caseDirectory);
        // As --isolate: --emit-mir unless the definition closure is needed.
        std::vector<std::string> argv{options.gti.string(),
                                      "--color=never",
                                      "--progress",
                                      "never",
                                      entry.string(),
                                      "-o",
                                      "snapshot"};
        if (!modeBuilds(test.mode) && !test.requireFunctionDefinitions)
          argv.push_back("--emit-mir");
        const auto [extra, environment] = gtiArguments(files);
        argv.insert(argv.end(), extra.begin(), extra.end());
        const ProcessRun process =
            runProcess(argv, std::nullopt, limit + options.limits.coldAllowance,
                       4 * (limit + options.limits.coldAllowance),
                       state->caseDirectory, environment);
        if (!process.started || process.timedOut || process.signal != 0) {
          problems += "snapshot diagnostics: gti did not finish (" +
                      std::string(process.timedOut ? "timeout" : "crash") +
                      ")\n";
          continue;
        }
        // Only the diagnostics, without the summary line.
        std::string rendered;
        for (const std::string &line : lines(process.err))
          if (line.rfind("Diagnostics: ", 0) != 0)
            rendered += line + "\n";
        if (rendered.empty())
          problems += "snapshot diagnostics: gti reported no diagnostics\n";
        else
          compare(kind,
                  normalizeRendered(rendered, state->caseDirectory,
                                    state->toolchain.standardLibrary.root));
      } else if (kind == Snapshot::Mir) {
        std::string selected;
        std::string error;
        std::set<std::string> sources;
        for (const auto &[path, name] : files.sections)
          sources.insert(path);
        CheckBackend backend;
        backend.inspect = [&](const lang::LoweredProgram &program) {
          selected = selectMir(program, test.mirFunctions, sources, error);
        };
        const lang::driver::CompilationResult mir =
            lang::driver::compileWithBackend(requestFor(test, files), backend);
        if (mir.status != lang::driver::CompilationStatus::Success)
          problems += "snapshot mir: the program did not reach verified MIR\n";
        else if (!error.empty())
          problems += "snapshot mir: " + error;
        else
          compare(kind, selected);
      } else if (kind == Snapshot::LlvmIr) {
        std::string ir;
        if (options.isolate) {
          std::vector<std::string> argv{options.gti.string(),
                                        "--color=never",
                                        files.entry.string(),
                                        test.optimize ? "-O3" : "-O0",
                                        "--emit-llvm",
                                        "-o",
                                        "case.ll"};
          const auto [extra, environment] = gtiArguments(files);
          argv.insert(argv.end(), extra.begin(), extra.end());
          const ProcessRun process = runProcess(
              argv, std::nullopt, limit + options.limits.coldAllowance,
              4 * (limit + options.limits.coldAllowance), state->caseDirectory,
              environment);
          if (process.exitCode == 0)
            ir = readFile(state->caseDirectory / "case.ll").value_or("");
          else
            problems += "snapshot llvm-ir: gti --emit-llvm failed\n" +
                        indent(tail(process.err, 20), "    ");
        } else {
          const lang::driver::CompilationResult emitted =
              lang::driver::compileToLlvmIr(requestFor(test, files));
          if (emitted.succeeded())
            ir = emitted.artifact->contents;
          else
            problems += "snapshot llvm-ir: LLVM IR emission failed: " +
                        emitted.backendFailureDetails.value_or(
                            emitted.diagnostics.empty()
                                ? std::string("(no detail)")
                                : emitted.diagnostics.front().message) +
                        "\n";
        }
        std::map<std::string, std::string> sources;
        for (const Section &section : test.sections)
          sources[(state->caseDirectory / section.name).string()] =
              section.text;
        if (!ir.empty())
          compare(kind, annotateLlvmIr(ir, sources,
                                       state->caseDirectory.string() + "/"));
      }
    }
    return problems;
  };

  const auto attempt = [&](bool bless) {
    Outcome result;
    const double cpuStart = totalCpu();
    const double wallStart = wallSeconds();
    const std::size_t storesBefore = state->reuse.preambles->stats().stores;
    if (!options.isolate)
      limitCpu(cpuSeconds(RUSAGE_SELF) + limit +
               (warm ? 0.0 : options.limits.coldAllowance) + 1.0);

    const Materialized files = materialize(test, state->caseDirectory,
                                           state->toolchain.standardLibrary);
    Compiled compiled;
    if (!files.failure.empty())
      compiled.internalFailure = "stdlib-overlay: " + files.failure;
    if (test.runnerFault == "crash")
      std::raise(SIGSEGV);
    if (test.runnerFault == "spin") {
      volatile unsigned long spin = 0;
      while (true)
        spin = spin + 1;
    }
    if (test.runnerFault == "hang")
      while (true)
        pause();
    if (test.runnerFault == "three-quarters") {
      // Passes, using three quarters of its limit (the --changed budget).
      volatile unsigned long spin = 0;
      while (totalCpu() - cpuStart < 0.75 * limit)
        spin = spin + 1;
    }
    try {
      if (test.runnerFault == "throw")
        throw std::runtime_error("injected runner fault");
      if (compiled.internalFailure) {
      } else if (test.runnerFault == "ice") {
        compiled.internalFailure =
            "GTI-B0001 at main.gti:1: Internal compiler error: injected";
      } else if (modeParses(test.mode)) {
        parseOnly(compiled, files, test);
      } else if (modeAnalyzes(test.mode)) {
        // In-process even under --isolate: `gti` has no semantic-only stop.
        const lang::driver::CompilationRequest request =
            requestFor(test, files);
        lang::FrontendOptions frontend{
            .target = request.target(),
            .configurationFlags = request.configurationFlags(),
            .declaredConfigurationFlags = request.declaredConfigurationFlags(),
            .stopAfter = lang::FrontendPhase::Semantics,
            .toolingOccurrences = false};
        frontend.rawPointerAudit = request.rawPointerAudit();
        frontend.reportLibraryAuditFindings =
            request.rawPointerAuditLibraryFindings();
        frontend.limits = request.compileLimits();
        frontend.program = request.program();
        frontend.cache = request.frontendReuse().parses;
        frontend.preambleCache = request.frontendReuse().preambles;
        const lang::StandardLibraryLayout &library = request.standardLibrary();
        const lang::FrontendResult analysis = lang::Frontend(frontend).analyze(
            files.entry, std::nullopt, {library.prelude}, {}, {library.root});
        collect(compiled, files, analysis.diagnostics);
        compiled.succeeded = analysis.sourceValid && analysis.syntaxValid &&
                             analysis.semanticValid;
      } else if (options.isolate && files.cSources.empty() &&
                 test.target.empty()) {
        // (Direct `gti` has no --target; target tests stay in-process.)
        // `gti main.gti -- helper.cpp ...` builds C++ sections as the command
        // line does; C sections need a project (`c-sources`), so a test with
        // them builds in-process even under --isolate.
        std::vector<std::string> argv{options.gti.string(), "--color=never",
                                      files.entry.string(),
                                      test.optimize ? "-O3" : "-O0"};
        const auto [extra, environment] = gtiArguments(files);
        argv.insert(argv.end(), extra.begin(), extra.end());
        // `gti --emit-mir` analyses without the definition closure; a test
        // that requires it builds instead.
        const bool emitMir =
            !modeBuilds(test.mode) && !test.requireFunctionDefinitions;
        const fs::path output =
            state->caseDirectory / (emitMir ? "case.mir" : "case");
        if (emitMir)
          argv.push_back("--emit-mir");
        argv.push_back("-o");
        argv.push_back(output.string());
        if (!files.cppSources.empty() || !test.nativeArgs.empty()) {
          argv.push_back("--");
          for (const fs::path &source : files.cppSources)
            argv.push_back(source.string());
          argv.insert(argv.end(), test.nativeArgs.begin(),
                      test.nativeArgs.end());
        }
        const ProcessRun process =
            runProcess(argv, std::nullopt, limit + options.limits.coldAllowance,
                       4 * (limit + options.limits.coldAllowance),
                       state->caseDirectory, environment);
        collectRendered(compiled, files, process.err);
        if (process.timedOut) {
          result.status = Status::Timeout;
          result.detail = "gti exceeded its limit\n";
        } else if (process.exitCode == 0) {
          compiled.succeeded = true;
          compiled.executable = output;
        } else if (process.exitCode != 65 && !compiled.internalFailure) {
          compiled.internalFailure =
              "gti exited with " +
              (process.signal ? "signal " + std::to_string(process.signal)
                              : "status " + std::to_string(process.exitCode)) +
              "\n" + tail(process.err, 20);
        }
      } else {
        const lang::driver::CompilationRequest request =
            requestFor(test, files);
        if (!modeBuilds(test.mode)) {
          CheckBackend backend;
          const lang::driver::CompilationResult result =
              lang::driver::compileWithBackend(request, backend);
          collectCompilation(compiled, files, result);
          compiled.succeeded =
              result.status == lang::driver::CompilationStatus::Success;
        } else {
          const fs::path output = state->caseDirectory / "case";
          // C/C++ sections are a project's native sources (`c-sources`,
          // `cpp-sources`) with the case directory as an include directory;
          // native-args are what `gti ... --` forwards.
          lang::driver::NativeInputs native;
          native.cSources = files.cSources;
          native.cppSources = files.cppSources;
          if (!files.cSources.empty() || !files.cppSources.empty())
            native.includeDirectories.push_back(state->caseDirectory);
          // A C++ file after `gti ... --` is compiled with the forwarded
          // arguments, so the C++ sections are too.
          native.compilerArguments = test.nativeArgs;
          native.trailingArguments = test.nativeArgs;
          lang::driver::ToolchainLayout toolchain = state->toolchain;
          if (files.standardLibrary)
            toolchain.standardLibrary = *files.standardLibrary;
          const lang::driver::ExecutableBuildResult build =
              lang::driver::buildExecutable(
                  lang::driver::ExecutableBuildRequest(
                      request
                          .withBackend(lang::BackendKind::Llvm)
                          // The driver admits instrumentation only with
                          // structured native inputs, so native-args tests
                          // build uninstrumented.
                          .withLlvmInstrumentation(
                              test.nativeArgs.empty()
                                  ? options.instrumentation
                                  : lang::LlvmInstrumentationMode::None),
                      toolchain, state->caseDirectory / "case.o", output,
                      lang::driver::discoverNativeCompiler(), std::move(native),
                      false, false, false, std::nullopt,
                      files.cSources.empty()
                          ? std::nullopt
                          : std::optional(lang::driver::discoverCCompiler())));
          collectCompilation(compiled, files, build.compilation);
          if (build.succeeded()) {
            compiled.succeeded = true;
            compiled.executable = output;
          } else if (build.status != lang::driver::ExecutableBuildStatus::
                                         CompilationFailure &&
                     !compiled.internalFailure) {
            std::string detail = build.driverDiagnostic.value_or("");
            for (const auto &c : build.cCompilations)
              if (c.process.exitCode != 0)
                detail += "\n" + c.source.filename().string() + ":\n" +
                          tail(c.process.output, 20);
            for (const auto &cpp : build.cppCompilations)
              if (cpp.process.exitCode != 0)
                detail += "\n" + cpp.source.filename().string() + ":\n" +
                          tail(cpp.process.output, 20);
            if (build.nativeProcess)
              detail += "\n" + tail(build.nativeProcess->output, 20);
            const bool sections =
                std::any_of(
                    build.cCompilations.begin(), build.cCompilations.end(),
                    [](const auto &c) { return c.process.exitCode != 0; }) ||
                std::any_of(
                    build.cppCompilations.begin(), build.cppCompilations.end(),
                    [](const auto &c) { return c.process.exitCode != 0; });
            if (!detail.empty() && detail.front() == '\n')
              detail.erase(0, 1);
            if (sections)
              compiled.nativeFailure =
                  normalizeRendered(detail, state->caseDirectory,
                                    state->toolchain.standardLibrary.root);
            else
              compiled.internalFailure = "native build failed: " + detail;
          }
        }
      }
    } catch (const std::exception &exception) {
      compiled.internalFailure =
          std::string("exception escaped the compiler: ") + exception.what();
    } catch (...) {
      compiled.internalFailure =
          "a non-standard exception escaped the compiler";
    }
    if (!options.isolate)
      limitCpu(0);

    if (result.status == Status::Timeout) {
      // Reported below with the time.
    } else if (compiled.nativeFailure) {
      result.status = Status::Fail;
      result.detail = "a C/C++ section did not compile:\n" +
                      indent(*compiled.nativeFailure, "    ");
    } else if (compiled.internalFailure) {
      result.status = Status::Ice;
      result.detail = "internal compiler error (never blessable):\n" +
                      indent(*compiled.internalFailure, "    ");
    } else {
      std::vector<std::string> problems =
          compareDiagnostics(test, compiled.diagnostics);
      bool missingNote = false;
      for (const auto &needle : test.diagnosticNotes)
        if (std::none_of(compiled.diagnosticNotes.begin(),
                         compiled.diagnosticNotes.end(), [&](const auto &note) {
                           return note.find(needle) != std::string::npos;
                         })) {
          problems.push_back("expected: diagnostic note containing \"" +
                             needle + "\"");
          missingNote = true;
        }
      if (modeFails(test.mode) && compiled.succeeded)
        problems.insert(problems.begin(),
                        "the program compiled without errors");
      const bool errors =
          std::any_of(compiled.diagnostics.begin(), compiled.diagnostics.end(),
                      [](const LocatedDiagnostic &d) {
                        return d.severity == Severity::Error;
                      });
      // Bless never changes a test's declared proof level: an explicit
      // check-pass, build-pass or run test that now errors stays failing.
      const bool blessable = !missingNote && !(errors && test.modeExplicit &&
                                               !modeFails(test.mode));
      if (!problems.empty()) {
        const std::string text =
            bless && blessable ? blessAnnotations(test, compiled.diagnostics)
                               : std::string();
        const bool directivesKept =
            !bless || options.blessDirectives ||
            directiveLines(text) ==
                directiveLines(readFile(test.path).value_or(""));
        if (bless && blessable && directivesKept) {
          writeFile(test.path, text);
          result.blessed.push_back(test.display);
        } else if (bless && blessable) {
          result.status = Status::Fail;
          for (const std::string &problem : problems)
            result.detail += problem + "\n";
          result.detail += "not blessed: it would change `//@` lines; review "
                           "and rerun with --bless --bless-directives\n";
        } else {
          result.status = Status::Fail;
          for (const std::string &problem : problems)
            result.detail += problem + "\n";
          if (bless)
            result.detail += std::string("not blessed: a ") +
                             modeName(test.mode) +
                             " test cannot expect errors\n";
        }
      } else if (modeRuns(test.mode) && compiled.succeeded) {
        // One execution per revision (or exactly one); every failing
        // revision's problems are reported, prefixed with its name.
        for (const RunSpec &run : test.runs) {
          if (result.status == Status::Timeout)
            break;
          const std::string label =
              run.revision.empty() ? "" : "[" + run.revision + "] ";
          const double remaining =
              std::max(0.5, limit - (totalCpu() - cpuStart));
          std::vector<std::string> argv{compiled.executable.string()};
          argv.insert(argv.end(), run.args.begin(), run.args.end());
          const ProcessRun program =
              runProcess(argv, run.stdinText, remaining, 4 * limit,
                         state->caseDirectory, run.env, run.stdinFile);
          std::string expectedOut;
          if (run.stdoutFile)
            expectedOut = readFile(*run.stdoutFile).value_or("");
          else
            for (const std::string &line : run.stdoutLines)
              expectedOut += line + "\n";
          std::vector<std::string> problems;
          if (!program.started) {
            problems.push_back("could not start the program");
          } else if (program.timedOut) {
            result.status = Status::Timeout;
            result.detail = label + "the program exceeded its limit\n";
          } else if (run.exitSignal) {
            if (program.signal != *run.exitSignal) {
              problems.push_back("expected: terminated by " +
                                 signalText(*run.exitSignal));
              problems.push_back(
                  "actual:   " +
                  (program.signal != 0
                       ? "terminated by " + signalText(program.signal)
                       : "exit code " + std::to_string(program.exitCode)));
            }
          } else if (program.signal != 0) {
            problems.push_back("actual:   terminated by " +
                               signalText(program.signal));
          } else if (run.mode == Mode::RunPass && program.exitCode != 0) {
            problems.push_back("expected: exit code 0");
            problems.push_back("actual:   exit code " +
                               std::to_string(program.exitCode));
          } else if (run.mode == Mode::RunFail && run.exitCode &&
                     program.exitCode != *run.exitCode) {
            problems.push_back("expected: exit code " +
                               std::to_string(*run.exitCode));
            problems.push_back("actual:   exit code " +
                               std::to_string(program.exitCode));
          } else if (run.mode == Mode::RunFail && program.exitCode == 0) {
            problems.push_back("expected: a non-zero exit code");
            problems.push_back("actual:   exit code 0");
          }
          if (run.stderrEmpty && !program.err.empty())
            problems.push_back("expected: empty stderr");
          for (const std::string &needle : run.stderrContains)
            if (program.err.find(needle) == std::string::npos)
              problems.push_back("expected: stderr containing \"" + needle +
                                 "\"");
          const bool stdoutDiffers = program.started && !program.timedOut &&
                                     program.out != expectedOut;
          if (result.status == Status::Timeout) {
          } else if (stdoutDiffers && problems.empty() && bless &&
                     !options.blessDirectives) {
            // `//@ stdout:` lines are directives: without
            // --bless-directives, output goes to the sibling file, and
            // inline expectations stay as written.
            if (run.hasInlineStdout) {
              result.status = Status::Fail;
              result.detail +=
                  label + "stdout differs (-expected +actual):\n" +
                  diff(expectedOut, program.out) + label +
                  "not blessed: the expected output is `//@ stdout:` "
                  "lines; rerun with --bless --bless-directives\n";
            } else {
              const fs::path stdoutPath =
                  stdoutPathFor(test.path, run.revision);
              if (program.out.empty())
                fs::remove(stdoutPath);
              else
                writeFile(stdoutPath, program.out);
              result.blessed.push_back(stdoutPath.string());
            }
          } else if (stdoutDiffers && problems.empty() && bless) {
            // NUL, other control bytes and invalid UTF-8 cannot be written
            // as `//@ stdout:` lines; they go to the sibling file.
            const bool inlineFits =
                !run.stdoutFile && lines(program.out).size() <= 8 &&
                (program.out.empty() || program.out.back() == '\n') &&
                inlineSafe(program.out);
            const fs::path stdoutPath = stdoutPathFor(test.path, run.revision);
            bool trailingSpace = false;
            for (const std::string &line : lines(program.out))
              trailingSpace =
                  trailingSpace ||
                  (!line.empty() &&
                   std::isspace(static_cast<unsigned char>(line.back())));
            if (program.out.empty()) {
              if (run.stdoutFile)
                fs::remove(*run.stdoutFile);
              if (run.hasInlineStdout)
                writeFile(test.path, blessInlineStdout(test, "", run.revision));
            } else if (inlineFits && !trailingSpace) {
              writeFile(test.path,
                        blessInlineStdout(test, program.out, run.revision));
            } else {
              writeFile(stdoutPath, program.out);
              if (run.hasInlineStdout)
                writeFile(test.path, blessInlineStdout(test, "", run.revision));
            }
            if (std::find(result.blessed.begin(), result.blessed.end(),
                          test.display) == result.blessed.end())
              result.blessed.push_back(test.display);
          } else if (stdoutDiffers || !problems.empty()) {
            result.status = Status::Fail;
            for (const std::string &problem : problems)
              result.detail += label + problem + "\n";
            if (stdoutDiffers)
              result.detail += label + "stdout differs (-expected +actual):\n" +
                               diff(expectedOut, program.out);
            if (!problems.empty() && !program.err.empty())
              result.detail +=
                  label + "stderr:\n" + indent(tail(program.err, 20), "    ");
          }
        }
      }
    }
    if (result.status == Status::Pass && !compiled.internalFailure &&
        !test.snapshots.empty()) {
      const std::string problems = snapshots(files, bless, result);
      if (!problems.empty()) {
        result.status = Status::Fail;
        result.detail += problems;
      }
    }
    result.cpu = totalCpu() - cpuStart;
    result.wall = wallSeconds() - wallStart;
    result.cold = state->reuse.preambles->stats().stores != storesBefore;
    return result;
  };

  outcome = attempt(options.bless);
  // Rendered snapshots quote the test's lines: once bless has rewritten the
  // file (annotations, inline stdout), they are blessed again from the new
  // text, so one --bless leaves a passing test.
  if (options.bless && !test.snapshots.empty() &&
      std::find(outcome.blessed.begin(), outcome.blessed.end(), test.display) !=
          outcome.blessed.end()) {
    TestFile rewritten = parseTestFile(test.path, test.display);
    rewritten.optimize = test.optimize;
    if (rewritten.errors.empty()) {
      Outcome second = run(rewritten);
      for (const std::string &file : outcome.blessed)
        if (std::find(second.blessed.begin(), second.blessed.end(), file) ==
            second.blessed.end())
          second.blessed.push_back(file);
      return second;
    }
  }
  // The limit applies to warm execution: a case that paid for analysing a
  // standard-library prefix is measured again once that prefix is cached.
  // The verdict stays the first run's.
  if (!options.isolate && outcome.cold &&
      outcome.cpu > limit * options.budgetFraction &&
      outcome.status != Status::Timeout) {
    const Outcome warmRun = attempt(false);
    outcome.cpu = warmRun.cpu;
    outcome.wall = warmRun.wall;
    outcome.cold = false;
  }
  outcome.inProcess =
      options.isolate &&
      (modeParses(test.mode) || modeAnalyzes(test.mode) ||
       !test.target.empty() || hasSnapshot(test, Snapshot::Mir) ||
       std::any_of(test.sections.begin(), test.sections.end(),
                   [](const Section &section) {
                     return nativeKind(section.name) == NativeKind::C;
                   }));
  if (outcome.status != Status::Skip)
    state->warmGroups.insert(test.groupKey);
  if (test.formatStable && outcome.status == Status::Pass)
    checkFormatting(test, outcome);
  if (!options.isolate && outcome.cpu > limit &&
      (outcome.status == Status::Pass || outcome.status == Status::Fail)) {
    outcome.status = Status::Timeout;
    outcome.detail = "over the CPU limit: " + std::to_string(outcome.cpu) +
                     " s > " + std::to_string(limit) + " s\n" + outcome.detail;
  }
  return outcome;
}

} // namespace gti_test
