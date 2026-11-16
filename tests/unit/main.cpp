// gti_unit_tests: runs the GTI_UNIT_TEST cases linked from tests/unit, each
// in its own child process so that exits, aborts, crashes, signal handlers
// and environment changes stay inside one case. See unit_test.h.

#include "unit_test.h"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr const char *caseEnvironment = "GTI_UNIT_TEST_CASE";
constexpr int skipStatus = 77;

struct Case {
  std::string name;
  std::string file;
  gti_unit::CaseFunction function;
};

std::vector<Case> &registry() {
  static std::vector<Case> cases;
  return cases;
}

const Case *findCase(std::string_view name) {
  for (const Case &entry : registry())
    if (entry.name == name)
      return &entry;
  return nullptr;
}

std::string displayFile(const std::string &file) {
  const std::string_view marker = "tests/unit/";
  const std::size_t at = file.rfind(marker);
  return at == std::string::npos ? file : file.substr(at);
}

void usage(std::ostream &out) {
  out << "usage: gti_unit_tests [--list] [--test NAME]... [--timeout SECONDS]\n"
         "                      [--no-fork] [--test NAME -- ARGS...]\n"
         "Runs every unit case, or the named ones, each in a child process.\n"
         "A case returns 0 to pass and 77 to skip.\n";
}

double timeScale() {
  const char *value = std::getenv("GTI_TEST_TIME_SCALE");
  if (value == nullptr || *value == '\0')
    return 1.0;
  const double scale = std::strtod(value, nullptr);
  return scale > 0 ? scale : 1.0;
}

struct Outcome {
  enum class Kind { Pass, Skip, Fail } kind = Kind::Fail;
  std::string reason;
  std::string output;
  double seconds = 0;
};

[[noreturn]] void runChild(const Case &entry, std::vector<char *> &arguments,
                           int output) {
  setpgid(0, 0);
  dup2(output, STDOUT_FILENO);
  dup2(output, STDERR_FILENO);
  close(output);
  setenv(caseEnvironment, entry.name.c_str(), 1);
  const int status =
      entry.function(static_cast<int>(arguments.size()) - 1, arguments.data());
  std::fflush(nullptr);
  std::cout.flush();
  std::cerr.flush();
  std::exit(status);
}

Outcome runIsolated(const Case &entry, std::vector<char *> &arguments,
                    double timeoutSeconds) {
  Outcome outcome;
  const auto start = std::chrono::steady_clock::now();
  int channel[2];
  if (pipe(channel) != 0) {
    outcome.reason = std::string("pipe failed: ") + std::strerror(errno);
    return outcome;
  }
  std::fflush(nullptr);
  const pid_t child = fork();
  if (child < 0) {
    outcome.reason = std::string("fork failed: ") + std::strerror(errno);
    close(channel[0]);
    close(channel[1]);
    return outcome;
  }
  if (child == 0) {
    close(channel[0]);
    runChild(entry, arguments, channel[1]);
  }
  close(channel[1]);
  bool timedOut = false;
  char buffer[4096];
  while (true) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    const double remaining = timeoutSeconds - elapsed;
    if (remaining <= 0) {
      timedOut = true;
      kill(-child, SIGKILL);
      kill(child, SIGKILL);
      break;
    }
    pollfd descriptor{channel[0], POLLIN, 0};
    const int ready =
        poll(&descriptor, 1, static_cast<int>(remaining * 1000) + 1);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready <= 0)
      continue;
    const ssize_t count = read(channel[0], buffer, sizeof buffer);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    outcome.output.append(buffer, static_cast<std::size_t>(count));
  }
  close(channel[0]);
  int status = 0;
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  // Reap anything the case left running in its process group.
  kill(-child, SIGKILL);
  outcome.seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  if (timedOut) {
    outcome.reason = "timed out after " + std::to_string(timeoutSeconds) + " s";
  } else if (WIFSIGNALED(status)) {
    outcome.reason = std::string("killed by signal ") +
                     std::to_string(WTERMSIG(status)) + " (" +
                     strsignal(WTERMSIG(status)) + ")";
  } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    outcome.kind = Outcome::Kind::Pass;
  } else if (WIFEXITED(status) && WEXITSTATUS(status) == skipStatus) {
    outcome.kind = Outcome::Kind::Skip;
  } else {
    outcome.reason = "exit status " + std::to_string(WEXITSTATUS(status));
  }
  return outcome;
}

void printIndented(const std::string &text) {
  std::size_t begin = 0;
  while (begin < text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos)
      end = text.size();
    std::cout << "  | " << std::string_view(text).substr(begin, end - begin)
              << '\n';
    begin = end + 1;
  }
}

} // namespace

gti_unit::Registration::Registration(const char *name, const char *file,
                                     CaseFunction function) {
  registry().push_back({name, file, function});
}

int main(int argc, char **argv) {
  // A case that re-executes argv[0] reaches its own code again.
  if (const char *name = std::getenv(caseEnvironment)) {
    const Case *entry = findCase(name);
    if (entry == nullptr) {
      std::cerr << "gti_unit_tests: unknown case " << name << '\n';
      return 2;
    }
    return entry->function(argc, argv);
  }

  std::sort(registry().begin(), registry().end(),
            [](const Case &left, const Case &right) {
              return left.name < right.name;
            });
  for (std::size_t index = 1; index < registry().size(); ++index)
    if (registry()[index].name == registry()[index - 1].name) {
      std::cerr << "gti_unit_tests: duplicate case " << registry()[index].name
                << '\n';
      return 2;
    }

  bool list = false;
  bool fork = true;
  double timeoutSeconds = 60;
  std::vector<std::string> selected;
  std::vector<std::string> caseArguments;
  bool argumentsGiven = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--list") {
      list = true;
    } else if (argument == "--no-fork") {
      fork = false;
    } else if (argument == "--test" && index + 1 < argc) {
      selected.emplace_back(argv[++index]);
    } else if (argument == "--timeout" && index + 1 < argc) {
      timeoutSeconds = std::strtod(argv[++index], nullptr);
    } else if (argument == "--") {
      argumentsGiven = true;
      for (++index; index < argc; ++index)
        caseArguments.emplace_back(argv[index]);
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      return 0;
    } else {
      usage(std::cerr);
      return 2;
    }
  }
  if (argumentsGiven && selected.size() != 1) {
    std::cerr << "gti_unit_tests: arguments after -- need exactly one --test\n";
    return 2;
  }

  std::vector<const Case *> cases;
  if (selected.empty()) {
    for (const Case &entry : registry())
      cases.push_back(&entry);
  } else {
    for (const std::string &name : selected) {
      const Case *entry = findCase(name);
      if (entry == nullptr) {
        std::cerr << "gti_unit_tests: no case named " << name
                  << " (see --list)\n";
        return 2;
      }
      cases.push_back(entry);
    }
  }
  if (list) {
    for (const Case *entry : cases)
      std::cout << entry->name << ' ' << displayFile(entry->file) << '\n';
    return 0;
  }

  // argv[0] stays the executable so that cases can re-execute it.
  std::string self = argv[0];
  std::error_code error;
  const auto absolute = std::filesystem::absolute(self, error);
  if (!error)
    self = absolute.string();
  std::vector<char *> arguments{self.data()};
  for (std::string &argument : caseArguments)
    arguments.push_back(argument.data());
  arguments.push_back(nullptr);

  if (!fork) {
    int worst = 0;
    for (const Case *entry : cases) {
      setenv(caseEnvironment, entry->name.c_str(), 1);
      const int status = entry->function(static_cast<int>(arguments.size()) - 1,
                                         arguments.data());
      std::cout << (status == 0 ? "PASS " : "FAIL ") << entry->name << '\n';
      if (status != 0)
        worst = status;
    }
    return worst;
  }

  timeoutSeconds *= timeScale();
  std::size_t passed = 0, failed = 0, skipped = 0;
  double total = 0;
  for (const Case *entry : cases) {
    const Outcome outcome = runIsolated(*entry, arguments, timeoutSeconds);
    total += outcome.seconds;
    char seconds[32];
    std::snprintf(seconds, sizeof seconds, "%.2fs", outcome.seconds);
    switch (outcome.kind) {
    case Outcome::Kind::Pass:
      ++passed;
      std::cout << "PASS " << entry->name << ' ' << seconds << '\n';
      break;
    case Outcome::Kind::Skip:
      ++skipped;
      std::cout << "SKIP " << entry->name << ' ' << seconds << '\n';
      printIndented(outcome.output);
      break;
    case Outcome::Kind::Fail:
      ++failed;
      std::cout << "FAIL " << entry->name << " (" << displayFile(entry->file)
                << "): " << outcome.reason << ' ' << seconds << '\n';
      printIndented(outcome.output);
      std::cout << "  rerun: gti_unit_tests --test " << entry->name << '\n';
      break;
    }
    std::cout.flush();
  }
  char seconds[32];
  std::snprintf(seconds, sizeof seconds, "%.1f", total);
  std::cout << "RESULT " << (failed == 0 ? "ok" : "fail") << ": " << passed
            << " passed, " << failed << " failed, " << skipped << " skipped; "
            << seconds << " wall-s\n";
  return failed == 0 ? 0 : 1;
}
