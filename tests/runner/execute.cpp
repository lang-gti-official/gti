#include "execute.h"

#include "gti/compile_limits.h"
#include "gti/driver/build.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"
#include "gti/frontend_cache.h"
#include "gti/lowered_program.h"
#include "gti/semantic_preamble.h"

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
  [[nodiscard]] lang::BackendArtifact generate(lang::LoweredProgram) override {
    return {};
  }
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

// A line diff of expected and actual output, capped at `limit` lines.
std::string diff(const std::string &expected, const std::string &actual,
                 std::size_t limit = 40) {
  const std::vector<std::string> a = lines(expected);
  const std::vector<std::string> b = lines(actual);
  std::vector<std::string> out;
  if (a.size() * b.size() > 4'000'000) {
    out.push_back("(output too large to diff; " + std::to_string(a.size()) +
                  " expected lines, " + std::to_string(b.size()) +
                  " actual lines)");
  } else {
    std::vector<std::vector<int>> table(a.size() + 1,
                                        std::vector<int>(b.size() + 1, 0));
    for (std::size_t i = a.size(); i-- > 0;)
      for (std::size_t j = b.size(); j-- > 0;)
        table[i][j] = a[i] == b[j] ? table[i + 1][j + 1] + 1
                                   : std::max(table[i + 1][j], table[i][j + 1]);
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < a.size() || j < b.size()) {
      if (i < a.size() && j < b.size() && a[i] == b[j]) {
        out.push_back(" " + a[i]);
        ++i;
        ++j;
      } else if (i < a.size() &&
                 (j == b.size() || table[i + 1][j] >= table[i][j + 1])) {
        out.push_back("-" + a[i++]);
      } else {
        out.push_back("+" + b[j++]);
      }
    }
  }
  std::string result;
  for (std::size_t index = 0; index < out.size() && index < limit; ++index)
    result += "    " + out[index] + "\n";
  if (out.size() > limit)
    result +=
        "    ... (" + std::to_string(out.size() - limit) + " more lines)\n";
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

ProcessRun runProcess(const std::vector<std::string> &argv,
                      const std::optional<std::string> &input, double cpuLimit,
                      double wallLimit, const fs::path &directory) {
  const fs::path in = directory / ".stdin";
  const fs::path out = directory / ".stdout";
  const fs::path err = directory / ".stderr";
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
  fs::path executable;
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
    if (!used[index])
      problems.push_back("actual:   " + describe(test, diagnostics[index]));
  return problems;
}

} // namespace

struct Engine::State {
  EngineOptions options;
  lang::driver::ToolchainLayout toolchain;
  lang::TargetInfo target;
  lang::driver::FrontendReuse reuse;
  std::set<std::string> warmGroups;
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

namespace {

struct Materialized {
  fs::path entry;
  std::map<std::string, std::string> sections; // path string -> name
};

Materialized materialize(const TestFile &test, const fs::path &directory) {
  std::error_code error;
  for (const auto &entry : fs::directory_iterator(directory, error))
    fs::remove_all(entry.path(), error);
  Materialized result;
  for (const Section &section : test.sections) {
    const fs::path path = directory / section.name;
    fs::create_directories(path.parent_path(), error);
    writeFile(path, section.text);
    result.sections[path.string()] = section.name;
    result.sections[path.lexically_normal().string()] = section.name;
  }
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
      continue;
    }
    diagnostics.push_back(std::move(diagnostic));
  }
  collect(compiled, files, diagnostics);
}

} // namespace

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
  const EngineOptions &options = state->options;
  const double limit = options.limits.forTest(test);
  const bool warm = state->warmGroups.contains(test.groupKey);

  const auto attempt = [&](bool bless) {
    Outcome result;
    const double cpuStart = totalCpu();
    const double wallStart = wallSeconds();
    const std::size_t storesBefore = state->reuse.preambles->stats().stores;
    if (!options.isolate)
      limitCpu(cpuSeconds(RUSAGE_SELF) + limit +
               (warm ? 0.0 : options.limits.coldAllowance) + 1.0);

    const Materialized files = materialize(test, state->caseDirectory);
    Compiled compiled;
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
    try {
      if (test.runnerFault == "throw")
        throw std::runtime_error("injected runner fault");
      if (test.runnerFault == "ice") {
        compiled.internalFailure =
            "GTI-B0001 at main.gti:1: Internal compiler error: injected";
      } else if (options.isolate) {
        std::vector<std::string> argv{options.gti.string(), "--color=never",
                                      files.entry.string(),
                                      test.optimize ? "-O3" : "-O0"};
        const fs::path output = state->caseDirectory /
                                (modeBuilds(test.mode) ? "case" : "case.mir");
        if (!modeBuilds(test.mode))
          argv.push_back("--emit-mir");
        argv.push_back("-o");
        argv.push_back(output.string());
        const ProcessRun process = runProcess(
            argv, std::nullopt, limit + options.limits.coldAllowance,
            4 * (limit + options.limits.coldAllowance), state->caseDirectory);
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
        const auto request =
            lang::driver::CompilationRequest(
                files.entry, state->toolchain.standardLibrary, state->target,
                test.optimize ? lang::OptimizationLevel::O3
                              : lang::OptimizationLevel::O0,
                lang::CppStandard::Cpp23)
                .withCompileLimits(lang::resolveCompileLimits({}, {}))
                .withFrontendReuse(state->reuse);
        if (!modeBuilds(test.mode)) {
          CheckBackend backend;
          const lang::driver::CompilationResult result =
              lang::driver::compileWithBackend(request, backend);
          collectCompilation(compiled, files, result);
          compiled.succeeded =
              result.status == lang::driver::CompilationStatus::Success;
        } else {
          const fs::path output = state->caseDirectory / "case";
          const lang::driver::ExecutableBuildResult build =
              lang::driver::buildExecutable(
                  lang::driver::ExecutableBuildRequest(
                      request.withBackend(lang::BackendKind::Llvm),
                      state->toolchain, state->caseDirectory / "case.o", output,
                      lang::driver::discoverNativeCompiler(), {}, false, false,
                      false));
          collectCompilation(compiled, files, build.compilation);
          if (build.succeeded()) {
            compiled.succeeded = true;
            compiled.executable = output;
          } else if (build.status != lang::driver::ExecutableBuildStatus::
                                         CompilationFailure &&
                     !compiled.internalFailure) {
            compiled.internalFailure =
                "native build failed: " + build.driverDiagnostic.value_or("") +
                (build.nativeProcess
                     ? "\n" + tail(build.nativeProcess->output, 20)
                     : std::string());
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
    } else if (compiled.internalFailure) {
      result.status = Status::Ice;
      result.detail = "internal compiler error (never blessable):\n" +
                      indent(*compiled.internalFailure, "    ");
    } else {
      std::vector<std::string> problems =
          compareDiagnostics(test, compiled.diagnostics);
      if (test.mode == Mode::CheckFail && compiled.succeeded)
        problems.insert(problems.begin(),
                        "the program compiled without errors");
      const bool errors =
          std::any_of(compiled.diagnostics.begin(), compiled.diagnostics.end(),
                      [](const LocatedDiagnostic &d) {
                        return d.severity == Severity::Error;
                      });
      // Bless never changes a test's declared proof level: an explicit
      // check-pass, build-pass or run test that now errors stays failing.
      const bool blessable =
          !(errors && test.modeExplicit && test.mode != Mode::CheckFail);
      if (!problems.empty()) {
        if (bless && blessable) {
          const std::string text = blessAnnotations(test, compiled.diagnostics);
          writeFile(test.path, text);
          result.blessed.push_back(test.display);
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
        const double remaining = std::max(0.5, limit - (totalCpu() - cpuStart));
        std::vector<std::string> argv{compiled.executable.string()};
        argv.insert(argv.end(), test.args.begin(), test.args.end());
        const ProcessRun program = runProcess(argv, test.stdinText, remaining,
                                              4 * limit, state->caseDirectory);
        std::string expectedOut;
        if (test.stdoutFile)
          expectedOut = readFile(*test.stdoutFile).value_or("");
        else
          for (const std::string &line : test.stdoutLines)
            expectedOut += line + "\n";
        std::vector<std::string> problems;
        if (!program.started) {
          problems.push_back("could not start the program");
        } else if (program.timedOut) {
          result.status = Status::Timeout;
          result.detail = "the program exceeded its limit\n";
        } else if (program.signal != 0) {
          problems.push_back("actual:   terminated by signal " +
                             std::to_string(program.signal));
        } else if (test.mode == Mode::RunPass && program.exitCode != 0) {
          problems.push_back("expected: exit code 0");
          problems.push_back("actual:   exit code " +
                             std::to_string(program.exitCode));
        } else if (test.mode == Mode::RunFail && test.exitCode &&
                   program.exitCode != *test.exitCode) {
          problems.push_back("expected: exit code " +
                             std::to_string(*test.exitCode));
          problems.push_back("actual:   exit code " +
                             std::to_string(program.exitCode));
        } else if (test.mode == Mode::RunFail && program.exitCode == 0) {
          problems.push_back("expected: a non-zero exit code");
          problems.push_back("actual:   exit code 0");
        }
        for (const std::string &needle : test.stderrContains)
          if (program.err.find(needle) == std::string::npos)
            problems.push_back("expected: stderr containing \"" + needle +
                               "\"");
        const bool stdoutDiffers =
            program.started && !program.timedOut && program.out != expectedOut;
        if (result.status == Status::Timeout) {
        } else if (stdoutDiffers && problems.empty() && bless) {
          const bool inlineFits =
              !test.stdoutFile && lines(program.out).size() <= 8 &&
              (program.out.empty() || program.out.back() == '\n') &&
              std::none_of(program.out.begin(), program.out.end(),
                           [](char c) { return c == '\r' || c == '\t'; });
          fs::path stdoutPath = test.path;
          stdoutPath.replace_extension(".stdout");
          bool trailingSpace = false;
          for (const std::string &line : lines(program.out))
            trailingSpace =
                trailingSpace ||
                (!line.empty() &&
                 std::isspace(static_cast<unsigned char>(line.back())));
          if (program.out.empty()) {
            if (test.stdoutFile)
              fs::remove(*test.stdoutFile);
            if (test.hasInlineStdout)
              writeFile(test.path, blessInlineStdout(test, ""));
          } else if (inlineFits && !trailingSpace) {
            writeFile(test.path, blessInlineStdout(test, program.out));
          } else {
            writeFile(stdoutPath, program.out);
            if (test.hasInlineStdout)
              writeFile(test.path, blessInlineStdout(test, ""));
          }
          result.blessed.push_back(test.display);
        } else if (stdoutDiffers || !problems.empty()) {
          result.status = Status::Fail;
          for (const std::string &problem : problems)
            result.detail += problem + "\n";
          if (stdoutDiffers)
            result.detail += "stdout differs (-expected +actual):\n" +
                             diff(expectedOut, program.out);
          if (!problems.empty() && !program.err.empty())
            result.detail +=
                "stderr:\n" + indent(tail(program.err, 20), "    ");
        }
      }
    }
    result.cpu = totalCpu() - cpuStart;
    result.wall = wallSeconds() - wallStart;
    result.cold = state->reuse.preambles->stats().stores != storesBefore;
    return result;
  };

  outcome = attempt(options.bless);
  // The limit applies to warm execution: a case that paid for analysing a
  // standard-library prefix is measured again once that prefix is cached.
  // The verdict stays the first run's.
  if (!options.isolate && outcome.cold && outcome.cpu > limit &&
      outcome.status != Status::Timeout) {
    const Outcome warmRun = attempt(false);
    outcome.cpu = warmRun.cpu;
    outcome.wall = warmRun.wall;
  }
  if (outcome.status != Status::Skip)
    state->warmGroups.insert(test.groupKey);
  if (!options.isolate && outcome.cpu > limit &&
      (outcome.status == Status::Pass || outcome.status == Status::Fail)) {
    outcome.status = Status::Timeout;
    outcome.detail = "over the CPU limit: " + std::to_string(outcome.cpu) +
                     " s > " + std::to_string(limit) + " s\n" + outcome.detail;
  }
  return outcome;
}

} // namespace gti_test
