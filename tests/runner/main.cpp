// gti-test: discovers `.gti` test files under a directory and checks each
// against its directives and diagnostic annotations, in worker processes
// that share one analysed standard library per worker. See
// tests/ui/README.md and docs/plans/test-suite-redesign.md.

#include "execute.h"
#include "project.h"
#include "snapshot.h"
#include "test_file.h"

#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace gti_test;

namespace {

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::uint64_t stableHash(const std::string &text) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char c : text) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string displayPath(const fs::path &path) {
  std::error_code error;
  const fs::path relative = fs::relative(path, fs::current_path(), error);
  if (!error && !relative.empty() && *relative.begin() != "..")
    return relative.generic_string();
  return path.generic_string();
}

bool ciEnvironment() {
  const char *value = std::getenv("CI");
  if (value == nullptr)
    return false;
  const std::string text = value;
  return !text.empty() && text != "0" && text != "false";
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer) ||                                        \
    __has_feature(undefined_behavior_sanitizer)
#define GTI_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(GTI_TEST_SANITIZED)
#define GTI_TEST_SANITIZED 1
#endif

double timeScale() {
  const char *value = std::getenv("GTI_TEST_TIME_SCALE");
  if (value == nullptr || *value == '\0') {
#if defined(GTI_TEST_SANITIZED)
    // A sanitized compiler runs several times slower; without an explicit
    // scale the limits would report its ordinary cost as timeouts.
    return 5.0;
#else
    return 1.0;
#endif
  }
  const double scale = std::strtod(value, nullptr);
  return scale > 0 ? scale : 1.0;
}

// GTI_TEST_INSTRUMENTATION=address|undefined|address,undefined|thread.
std::optional<lang::LlvmInstrumentationMode> instrumentation() {
  const char *value = std::getenv("GTI_TEST_INSTRUMENTATION");
  const std::string text = value == nullptr ? "" : value;
  if (text.empty() || text == "none")
    return lang::LlvmInstrumentationMode::None;
  if (text == "address")
    return lang::LlvmInstrumentationMode::Address;
  if (text == "undefined")
    return lang::LlvmInstrumentationMode::UndefinedMemory;
  if (text == "address,undefined" || text == "undefined,address")
    return lang::LlvmInstrumentationMode::AddressAndUndefinedMemory;
  if (text == "thread")
    return lang::LlvmInstrumentationMode::Thread;
  return std::nullopt;
}

struct Options {
  std::vector<std::string> selectors;
  fs::path root = "tests/ui";
  bool rootGiven = false;
  bool list = false;
  bool tidy = false;
  bool bless = false;
  bool isolate = false;
  unsigned jobs = 0;
  unsigned shardIndex = 0;
  unsigned shardCount = 0;
  std::optional<fs::path> json;
  std::optional<fs::path> gti;
  std::optional<fs::path> lsp;
};

void usage(std::ostream &out) {
  out << "usage: gti-test [paths or substrings...] [--root DIR] [--jobs N]\n"
         "                [--shard I/N] [--bless] [--isolate] [--gti PATH] "
         "[--lsp PATH]\n"
         "                [--json FILE] [--list] [--tidy]\n"
         "Runs every .gti test under --root (default tests/ui). A positional\n"
         "argument that names a file or directory selects it; any other\n"
         "selects tests whose path contains it. See tests/ui/README.md.\n";
}

bool underLsp(const fs::path &path) {
  for (const auto &part : path.parent_path())
    if (part == "lsp")
      return true;
  return false;
}

// A `.gti` test, or a `.jsonl` LSP transcript under an `lsp` directory.
// Whether `path` lies inside a project test (a directory, up to a few levels
// above, holding `test.transcript`); its sources belong to that test.
bool insideProject(const fs::path &path) {
  fs::path directory = path.parent_path();
  for (int depth = 0; depth < 6 && !directory.empty(); ++depth) {
    if (fs::exists(directory / "test.transcript"))
      return true;
    if (directory == directory.parent_path())
      break;
    directory = directory.parent_path();
  }
  return false;
}

bool isTestFile(const fs::path &path) {
  if (path.filename() == "test.transcript")
    return true;
  if (insideProject(path))
    return false;
  return path.extension() == ".gti" ||
         (path.extension() == ".jsonl" && underLsp(path));
}

// Every file a test root holds: test files, expected-output companions and
// the README.
std::vector<fs::path> walk(const fs::path &root) {
  std::vector<fs::path> files;
  std::error_code error;
  if (fs::is_regular_file(root, error)) {
    files.push_back(root);
    return files;
  }
  for (fs::recursive_directory_iterator it(root, error), end; it != end;
       it.increment(error)) {
    if (error)
      break;
    if (it->is_regular_file(error))
      files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

int tidy(const fs::path &root) {
  std::vector<std::string> problems;
  const std::vector<fs::path> files = walk(root);
  for (const fs::path &file : files) {
    const std::string display = displayPath(file);
    const fs::path relative = file.lexically_relative(root);
    const std::string extension = file.extension().string();
    if (relative == "README.md")
      continue;
    if (std::distance(relative.begin(), relative.end()) < 2) {
      problems.push_back(display + ": tests live in a subdirectory of " +
                         displayPath(root));
      continue;
    }
    if (extension == ".stdout" || extension == ".stdin") {
      // name.stdout, or name.<revision>.stdout for a declared revision (and
      // the same for .stdin).
      const std::string stem = file.stem().string();
      const std::size_t dot = stem.find('.');
      const fs::path test = file.parent_path() / (stem.substr(0, dot) + ".gti");
      bool known = fs::exists(test);
      if (known && dot != std::string::npos) {
        const TestFile parsed = parseTestFile(test, display);
        known = std::find(parsed.revisions.begin(), parsed.revisions.end(),
                          stem.substr(dot + 1)) != parsed.revisions.end();
      }
      if (!known)
        problems.push_back(display + ": expected output without a test");
      else if (fs::file_size(file) == 0)
        problems.push_back(display + ": empty expected file");
      continue;
    }
    if (extension == ".mir" || extension == ".stderr" ||
        extension == ".formatted") {
      fs::path testPath = file;
      testPath.replace_extension(".gti");
      const Snapshot kind = extension == ".mir"      ? Snapshot::Mir
                            : extension == ".stderr" ? Snapshot::Diagnostics
                                                     : Snapshot::Format;
      if (!fs::exists(testPath)) {
        problems.push_back(display + ": expected output without a test");
        continue;
      }
      const TestFile owner = parseTestFile(testPath, display);
      if (!hasSnapshot(owner, kind) ||
          (kind == Snapshot::Mir && !owner.checks.empty() &&
           !hasSnapshot(owner, Snapshot::LlvmIr)))
        problems.push_back(display + ": expected file without `//@ snapshot: " +
                           snapshotName(kind) + "` in its test");
      const std::optional<std::string> text = readFile(file);
      const std::size_t lineCount =
          text ? static_cast<std::size_t>(
                     std::count(text->begin(), text->end(), '\n'))
               : 0;
      if (text && text->empty())
        problems.push_back(display + ": empty expected file");
      else if (text && (lineCount > snapshotLineLimit ||
                        text->size() > snapshotByteLimit))
        problems.push_back(
            display + ": snapshot is " + std::to_string(lineCount) +
            " lines, over the " + std::to_string(snapshotLineLimit) +
            "-line limit; narrow it (`//@ mir-function:`, `// CHECK:` "
            "patterns, or a smaller test)");
      continue;
    }
    if (file.filename() == "test.transcript") {
      const std::optional<std::string> text = readFile(file);
      if (!text || text->rfind("// ", 0) != 0)
        problems.push_back(display + ": the first line must be a `// ` comment "
                                     "saying what the transcript checks");
      std::vector<std::string> errors;
      (void)parseProjectTranscript(text.value_or(""), errors);
      for (const std::string &error : errors)
        problems.push_back(display + ": " + error);
      continue;
    }
    if (file.filename() == "gti-test.defaults") {
      problems.push_back(display + ": defaults files are for directive-less "
                                   "roots such as examples/; write the "
                                   "directives in each test");
      continue;
    }
    if (insideProject(file))
      continue;
    if (extension == ".jsonl" && underLsp(file)) {
      const std::optional<std::string> text = readFile(file);
      if (!text || text->rfind("// ", 0) != 0)
        problems.push_back(display + ": the first line must be a `// ` comment "
                                     "saying what the transcript checks");
      continue;
    }
    if (extension != ".gti") {
      problems.push_back(display + ": not a test or expected-output file");
      continue;
    }
    const TestFile test = parseTestFile(file, display);
    for (const std::string &error : test.errors)
      problems.push_back(display + ": " + error);
    if (!test.runnerFault.empty())
      problems.push_back(display + ": runner-fault is not allowed in tests");
    for (const Snapshot kind : test.snapshots) {
      const bool patterns = kind == Snapshot::LlvmIr ||
                            (kind == Snapshot::Mir && !test.checks.empty() &&
                             !hasSnapshot(test, Snapshot::LlvmIr));
      if (!patterns && !fs::exists(snapshotPath(test, kind)))
        problems.push_back(display + ": no " +
                           snapshotPath(test, kind).filename().string() +
                           " for `//@ snapshot: " + snapshotName(kind) +
                           "`; run with --bless");
    }
    const std::optional<std::string> text = readFile(file);
    if (!text || text->rfind("// ", 0) != 0 || text->rfind("//@", 0) == 0 ||
        text->rfind("// ---", 0) == 0)
      problems.push_back(display +
                         ": the first line must be a `// ` comment saying "
                         "what the test checks");
    if (text && text->find("\nimport std;") != std::string::npos && !test.slow)
      problems.push_back(display +
                         ": `import std;` is slow; import specific modules");
  }
  for (const std::string &problem : problems)
    std::cout << "TIDY " << problem << '\n';
  std::cout << "RESULT " << (problems.empty() ? "ok" : "fail") << ": tidy, "
            << files.size() << " files, " << problems.size() << " problems\n";
  return problems.empty() ? 0 : 1;
}

// -- Worker protocol ----------------------------------------------------
// Parent -> worker: "<index>\n". Worker -> parent: "<length>\n<payload>",
// payload = "<status> <cpu> <wall>\n<n blessed>\n<blessed>...\n<detail>".

bool writeAll(int fd, const std::string &text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const ssize_t count =
        write(fd, text.data() + written, text.size() - written);
    if (count <= 0) {
      if (count < 0 && errno == EINTR)
        continue;
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

std::string encode(const Outcome &outcome) {
  std::ostringstream out;
  out << static_cast<int>(outcome.status) << ' ' << outcome.cpu << ' '
      << outcome.wall << '\n'
      << outcome.blessed.size() << '\n';
  for (const std::string &file : outcome.blessed)
    out << file << '\n';
  out << outcome.detail;
  const std::string payload = out.str();
  return std::to_string(payload.size()) + "\n" + payload;
}

Outcome decode(const std::string &payload) {
  Outcome outcome;
  std::istringstream in(payload);
  int status = 0;
  std::size_t blessed = 0;
  in >> status >> outcome.cpu >> outcome.wall >> blessed;
  outcome.status = static_cast<Status>(status);
  std::string line;
  std::getline(in, line);
  for (std::size_t index = 0; index < blessed; ++index) {
    std::getline(in, line);
    outcome.blessed.push_back(line);
  }
  std::ostringstream rest;
  rest << in.rdbuf();
  outcome.detail = rest.str();
  return outcome;
}

[[noreturn]] void workerMain(int input, int output,
                             const EngineOptions &options,
                             const std::vector<TestFile> &tests) {
  setpgid(0, 0);
  rlimit core{0, 0};
  setrlimit(RLIMIT_CORE, &core);
  signal(SIGPIPE, SIG_IGN);
  Engine engine(options);
  std::string buffer;
  char chunk[256];
  while (true) {
    const std::size_t newline = buffer.find('\n');
    if (newline == std::string::npos) {
      const ssize_t count = read(input, chunk, sizeof chunk);
      if (count <= 0) {
        if (count < 0 && errno == EINTR)
          continue;
        _exit(0);
      }
      buffer.append(chunk, static_cast<std::size_t>(count));
      continue;
    }
    const std::size_t index = std::stoul(buffer.substr(0, newline));
    buffer.erase(0, newline + 1);
    const Outcome outcome = engine.run(tests.at(index));
    if (!writeAll(output, encode(outcome)))
      _exit(1);
  }
}

struct Worker {
  pid_t pid = -1;
  int toWorker = -1;
  int fromWorker = -1;
  std::string buffer;
  std::optional<std::size_t> running;
  double started = 0;
  std::string group;
  int serial = 0;
};

const char *signalName(int signal) {
  switch (signal) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGABRT:
    return "SIGABRT";
  case SIGBUS:
    return "SIGBUS";
  case SIGILL:
    return "SIGILL";
  case SIGFPE:
    return "SIGFPE";
  case SIGKILL:
    return "SIGKILL";
  case SIGXCPU:
    return "SIGXCPU";
  default:
    return "signal";
  }
}

std::string fixed(double value, int digits = 2) {
  char text[64];
  std::snprintf(text, sizeof text, "%.*f", digits, value);
  return text;
}

std::string jsonString(const std::string &text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\')
      out += std::string("\\") + c;
    else if (static_cast<unsigned char>(c) < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
      out += escaped;
    } else
      out += c;
  }
  return out + "\"";
}

} // namespace

int main(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto value = [&]() -> std::string {
      if (index + 1 >= argc) {
        std::cerr << "gti-test: " << argument << " needs a value\n";
        std::exit(2);
      }
      return argv[++index];
    };
    if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      return 0;
    } else if (argument == "--list") {
      options.list = true;
    } else if (argument == "--tidy") {
      options.tidy = true;
    } else if (argument == "--bless") {
      options.bless = true;
    } else if (argument == "--isolate") {
      options.isolate = true;
    } else if (argument == "--root") {
      options.root = value();
      options.rootGiven = true;
    } else if (argument == "--gti") {
      options.gti = value();
    } else if (argument == "--lsp") {
      options.lsp = value();
    } else if (argument == "--json") {
      options.json = value();
    } else if (argument == "--jobs" || argument == "-j") {
      options.jobs = static_cast<unsigned>(std::stoul(value()));
    } else if (argument == "--shard") {
      const std::string shard = value();
      const std::size_t slash = shard.find('/');
      if (slash == std::string::npos) {
        std::cerr << "gti-test: --shard takes I/N\n";
        return 2;
      }
      options.shardIndex =
          static_cast<unsigned>(std::stoul(shard.substr(0, slash)));
      options.shardCount =
          static_cast<unsigned>(std::stoul(shard.substr(slash + 1)));
      if (options.shardCount == 0 || options.shardIndex == 0 ||
          options.shardIndex > options.shardCount) {
        std::cerr << "gti-test: --shard I/N needs 1 <= I <= N\n";
        return 2;
      }
    } else if (!argument.empty() && argument[0] == '-') {
      std::cerr << "gti-test: unknown option " << argument << '\n';
      usage(std::cerr);
      return 2;
    } else {
      options.selectors.push_back(argument);
    }
  }
  if (options.bless && ciEnvironment()) {
    std::cerr << "gti-test: --bless is refused when CI is set; bless locally "
                 "and commit the result\n";
    return 2;
  }
  if (options.tidy)
    return tidy(options.root);

  // Discovery and selection.
  std::vector<fs::path> roots;
  std::vector<std::string> substrings;
  for (const std::string &selector : options.selectors) {
    if (fs::exists(selector))
      roots.push_back(selector);
    else
      substrings.push_back(selector);
  }
  if (roots.empty())
    roots.push_back(options.root);
  std::set<fs::path> seen;
  std::vector<TestFile> tests;
  for (const fs::path &root : roots) {
    if (!fs::exists(root)) {
      std::cerr << "gti-test: no such test root " << root << '\n';
      return 2;
    }
    for (const fs::path &file : walk(root)) {
      if (!isTestFile(file))
        continue;
      const fs::path absolute = fs::absolute(file).lexically_normal();
      if (!seen.insert(absolute).second)
        continue;
      const std::string display = displayPath(absolute);
      if (!substrings.empty() &&
          std::none_of(substrings.begin(), substrings.end(),
                       [&](const std::string &s) {
                         return display.find(s) != std::string::npos;
                       }))
        continue;
      if (options.shardCount != 0 &&
          stableHash(display) % options.shardCount != options.shardIndex - 1)
        continue;
      tests.push_back(parseTestFile(absolute, display));
    }
  }
  if (options.list) {
    for (const TestFile &test : tests)
      std::cout << test.display << " [" << modeName(test.mode) << "]\n";
    return 0;
  }

  const fs::path self = fs::absolute(argv[0]);
  EngineOptions engineOptions;
  engineOptions.self = self;
  engineOptions.gti = options.gti.value_or(
      std::getenv("GTI_TEST_GTI") ? fs::path(std::getenv("GTI_TEST_GTI"))
                                  : self.parent_path() / "gti");
  engineOptions.lsp = options.lsp.value_or(
      std::getenv("GTI_TEST_LSP") ? fs::path(std::getenv("GTI_TEST_LSP"))
                                  : self.parent_path() / "gti_lsp");
  engineOptions.isolate = options.isolate;
  if (const auto mode = instrumentation()) {
    engineOptions.instrumentation = *mode;
  } else {
    std::cerr << "gti-test: GTI_TEST_INSTRUMENTATION is address, undefined, "
                 "address,undefined or thread\n";
    return 2;
  }
  engineOptions.bless = options.bless;
  const double scale = timeScale();
  engineOptions.limits.check *= scale;
  engineOptions.limits.run *= scale;
  engineOptions.limits.slow *= scale;
  engineOptions.limits.lsp *= scale;
  engineOptions.limits.project *= scale;
  engineOptions.limits.coldAllowance *= scale;
  if (options.isolate && !fs::exists(engineOptions.gti)) {
    std::cerr << "gti-test: --isolate needs gti; not found at "
              << engineOptions.gti << " (use --gti PATH)\n";
    return 2;
  }
  char tempTemplate[] = "/tmp/gti-test.XXXXXX";
  const char *tempRoot = mkdtemp(tempTemplate);
  if (tempRoot == nullptr) {
    std::perror("gti-test: mkdtemp");
    return 2;
  }
  const fs::path workRoot = tempRoot;

  unsigned jobs = options.jobs != 0
                      ? options.jobs
                      : std::max(1U, std::thread::hardware_concurrency());
  jobs = std::max(
      1U, std::min<unsigned>(jobs, static_cast<unsigned>(tests.size())));

  // Group cases by trusted prefix so a worker keeps reusing its analysis.
  std::map<std::string, std::deque<std::size_t>> groups;
  for (std::size_t index = 0; index < tests.size(); ++index)
    groups[tests[index].groupKey].push_back(index);
  std::size_t queued = tests.size();
  const auto next = [&](Worker &worker) -> std::optional<std::size_t> {
    if (queued == 0)
      return std::nullopt;
    auto group = groups.find(worker.group);
    if (group == groups.end() || group->second.empty()) {
      group = std::max_element(groups.begin(), groups.end(),
                               [](const auto &a, const auto &b) {
                                 return a.second.size() < b.second.size();
                               });
      worker.group = group->first;
    }
    const std::size_t index = group->second.front();
    group->second.pop_front();
    --queued;
    return index;
  };

  signal(SIGPIPE, SIG_IGN);
  std::vector<Worker> workers(jobs);
  int serial = 0;
  const auto spawn = [&](Worker &worker) {
    int down[2];
    int up[2];
    if (pipe(down) != 0 || pipe(up) != 0) {
      std::perror("gti-test: pipe");
      std::exit(2);
    }
    worker.serial = serial++;
    EngineOptions workerOptions = engineOptions;
    workerOptions.workDirectory =
        workRoot / ("w" + std::to_string(worker.serial));
    std::cout.flush();
    std::cerr.flush();
    const pid_t pid = fork();
    if (pid == 0) {
      close(down[1]);
      close(up[0]);
      for (const Worker &other : workers) {
        if (other.toWorker >= 0)
          close(other.toWorker);
        if (other.fromWorker >= 0)
          close(other.fromWorker);
      }
      workerMain(down[0], up[1], workerOptions, tests);
    }
    close(down[0]);
    close(up[1]);
    worker.pid = pid;
    worker.toWorker = down[1];
    worker.fromWorker = up[0];
    worker.buffer.clear();
    worker.running.reset();
  };
  for (Worker &worker : workers)
    spawn(worker);

  std::vector<std::optional<Outcome>> outcomes(tests.size());
  std::size_t finished = 0;
  const double start = wallSeconds();
  std::ofstream json;
  if (options.json)
    json.open(*options.json);
  const std::string rerun = argv[0];
  std::size_t counts[6] = {};
  double totalCpu = 0;

  const auto record = [&](std::size_t index, Outcome outcome) {
    const TestFile &test = tests[index];
    ++counts[static_cast<int>(outcome.status)];
    totalCpu += outcome.cpu;
    for (const std::string &file : outcome.blessed)
      std::cout << "BLESSED " << file << '\n';
    if (outcome.status != Status::Pass && outcome.status != Status::Skip) {
      std::cout << statusName(outcome.status) << ' ' << test.display << " ["
                << modeName(test.mode) << "] " << fixed(outcome.cpu) << "s\n";
      std::istringstream detail(outcome.detail);
      for (std::string line; std::getline(detail, line);)
        std::cout << "  " << line << '\n';
      std::cout << "  rerun: " << rerun << ' ' << test.display << '\n';
      // Transcripts have nothing to bless.
      if (outcome.status == Status::Fail && test.mode != Mode::Project &&
          test.mode != Mode::LspTranscript)
        std::cout << "  bless: " << rerun << ' ' << test.display
                  << " --bless\n";
      std::cout.flush();
    }
    if (json.is_open())
      json << "{\"path\":" << jsonString(test.display)
           << ",\"mode\":" << jsonString(modeName(test.mode))
           << ",\"status\":" << jsonString(statusName(outcome.status))
           << ",\"cpu\":" << fixed(outcome.cpu, 3)
           << ",\"wall\":" << fixed(outcome.wall, 3) << "}\n";
    outcomes[index] = std::move(outcome);
    ++finished;
  };

  const auto dispatch = [&](Worker &worker) {
    const std::optional<std::size_t> index = next(worker);
    if (!index)
      return;
    worker.running = index;
    worker.started = wallSeconds();
    if (!writeAll(worker.toWorker, std::to_string(*index) + "\n")) {
      // The worker died before taking the case; the read side reports it.
    }
  };
  for (Worker &worker : workers)
    dispatch(worker);

  const auto backstop = [&](const TestFile &test) {
    const Limits &limits = engineOptions.limits;
    return 4 * limits.forTest(test) + 2 * limits.coldAllowance;
  };
  const auto reap = [&](Worker &worker, bool killedForTime) {
    if (worker.pid > 0) {
      kill(-worker.pid, SIGKILL);
      kill(worker.pid, SIGKILL);
    }
    int status = 0;
    if (worker.pid > 0)
      waitpid(worker.pid, &status, 0);
    close(worker.toWorker);
    close(worker.fromWorker);
    worker.toWorker = worker.fromWorker = -1;
    if (worker.running) {
      const std::size_t index = *worker.running;
      Outcome outcome;
      outcome.wall = wallSeconds() - worker.started;
      const int signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
      if (killedForTime || signal == SIGXCPU) {
        outcome.status = Status::Timeout;
        outcome.detail =
            killedForTime
                ? "no result after " + fixed(outcome.wall) +
                      " s wall time; the worker was killed\n"
                : "exceeded its CPU limit (" +
                      fixed(engineOptions.limits.forTest(tests[index])) +
                      " s); the worker was killed\n";
      } else {
        outcome.status = Status::Crash;
        outcome.detail =
            signal != 0
                ? std::string("the worker died with ") + signalName(signal) +
                      " (" + std::to_string(signal) + ")\n"
                : "the worker exited with status " +
                      std::to_string(WIFEXITED(status) ? WEXITSTATUS(status)
                                                       : -1) +
                      "\n";
      }
      outcome.cpu = outcome.wall;
      worker.running.reset();
      record(index, std::move(outcome));
    }
    worker.pid = -1;
    if (queued > 0) {
      spawn(worker);
      dispatch(worker);
    }
  };

  while (finished < tests.size()) {
    std::vector<pollfd> fds;
    std::vector<Worker *> owners;
    double timeout = 1.0;
    for (Worker &worker : workers) {
      if (worker.pid <= 0)
        continue;
      fds.push_back({worker.fromWorker, POLLIN, 0});
      owners.push_back(&worker);
      if (worker.running)
        timeout = std::min(timeout, worker.started +
                                        backstop(tests[*worker.running]) -
                                        wallSeconds());
    }
    if (fds.empty())
      break;
    const int ready = poll(fds.data(), fds.size(),
                           static_cast<int>(std::max(0.0, timeout) * 1000) + 1);
    if (ready < 0 && errno != EINTR) {
      std::perror("gti-test: poll");
      return 2;
    }
    for (std::size_t slot = 0; slot < fds.size(); ++slot) {
      Worker &worker = *owners[slot];
      if (fds[slot].revents == 0)
        continue;
      char chunk[65536];
      const ssize_t count = read(worker.fromWorker, chunk, sizeof chunk);
      if (count <= 0) {
        if (count < 0 && errno == EINTR)
          continue;
        reap(worker, false);
        continue;
      }
      worker.buffer.append(chunk, static_cast<std::size_t>(count));
      while (true) {
        const std::size_t newline = worker.buffer.find('\n');
        if (newline == std::string::npos)
          break;
        const std::size_t length = std::stoul(worker.buffer.substr(0, newline));
        if (worker.buffer.size() < newline + 1 + length)
          break;
        const std::string payload = worker.buffer.substr(newline + 1, length);
        worker.buffer.erase(0, newline + 1 + length);
        if (worker.running) {
          const std::size_t index = *worker.running;
          worker.running.reset();
          record(index, decode(payload));
        }
        dispatch(worker);
      }
    }
    for (Worker &worker : workers)
      if (worker.pid > 0 && worker.running &&
          wallSeconds() - worker.started > backstop(tests[*worker.running]))
        reap(worker, true);
  }
  for (Worker &worker : workers) {
    if (worker.pid <= 0)
      continue;
    close(worker.toWorker);
    int status = 0;
    waitpid(worker.pid, &status, 0);
    close(worker.fromWorker);
  }
  std::error_code error;
  fs::remove_all(workRoot, error);

  const double wall = wallSeconds() - start;
  std::size_t slowest = tests.size();
  for (std::size_t index = 0; index < tests.size(); ++index)
    if (outcomes[index] && (slowest == tests.size() ||
                            outcomes[index]->cpu > outcomes[slowest]->cpu))
      slowest = index;
  const std::size_t failed = counts[static_cast<int>(Status::Fail)] +
                             counts[static_cast<int>(Status::Ice)] +
                             counts[static_cast<int>(Status::Crash)] +
                             counts[static_cast<int>(Status::Timeout)];
  std::cout << "RESULT " << (failed == 0 ? "ok" : "fail") << ": "
            << counts[static_cast<int>(Status::Pass)] << " passed, "
            << counts[static_cast<int>(Status::Fail)] << " failed, "
            << counts[static_cast<int>(Status::Ice)] << " ice, "
            << counts[static_cast<int>(Status::Timeout)] << " timeout, "
            << counts[static_cast<int>(Status::Crash)] << " crash, "
            << counts[static_cast<int>(Status::Skip)] << " skipped; "
            << fixed(totalCpu, 1) << " cpu-s, " << fixed(wall, 1) << " wall-s, "
            << jobs << " jobs" << (options.isolate ? ", isolated" : "");
  if (slowest < tests.size())
    std::cout << "; slowest " << tests[slowest].display << ' '
              << fixed(outcomes[slowest]->cpu) << 's';
  std::cout << '\n';
  if (tests.empty())
    std::cout << "gti-test: no tests selected\n";
  return failed == 0 && !tests.empty() ? 0 : 1;
}
