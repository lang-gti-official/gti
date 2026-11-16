#include "project.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace gti_test {
namespace {

double childCpu() {
  rusage usage{};
  getrusage(RUSAGE_CHILDREN, &usage);
  return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
         static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) /
             1e6;
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string trim(const std::string &text) {
  const std::size_t begin = text.find_first_not_of(" \t\r");
  if (begin == std::string::npos)
    return "";
  const std::size_t end = text.find_last_not_of(" \t\r");
  return text.substr(begin, end - begin + 1);
}

// Whitespace-separated words; double quotes group, `\"` escapes.
std::vector<std::string> words(const std::string &text, bool &ok) {
  std::vector<std::string> result;
  std::string current;
  bool quoted = false;
  bool any = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char c = text[index];
    if (c == '\\' && quoted && index + 1 < text.size()) {
      current += text[++index];
    } else if (c == '"') {
      quoted = !quoted;
      any = true;
    } else if ((c == ' ' || c == '\t') && !quoted) {
      if (any || !current.empty())
        result.push_back(current);
      current.clear();
      any = false;
    } else {
      current += c;
    }
  }
  if (any || !current.empty())
    result.push_back(current);
  ok = !quoted;
  return result;
}

void replaceAll(std::string &text, const std::string &from,
                const std::string &to) {
  if (from.empty())
    return;
  for (std::size_t at = 0; (at = text.find(from, at)) != std::string::npos;
       at += to.size())
    text.replace(at, from.size(), to);
}

struct Run {
  int exitCode = -1;
  int signal = 0;
  bool timedOut = false;
  std::string out;
  std::string err;
};

Run runCommand(const std::vector<std::string> &argv, const fs::path &cwd,
               const fs::path &home, double cpuLimit, double wallLimit) {
  const fs::path out = home / ".command-stdout";
  const fs::path err = home / ".command-stderr";
  std::vector<char *> arguments;
  for (const std::string &argument : argv)
    arguments.push_back(const_cast<char *>(argument.c_str()));
  arguments.push_back(nullptr);
  Run run;
  const pid_t pid = fork();
  if (pid == 0) {
    rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    const auto seconds =
        static_cast<rlim_t>(std::max(1.0, std::ceil(cpuLimit)));
    rlimit cpu{seconds, seconds + 1};
    setrlimit(RLIMIT_CPU, &cpu);
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CACHE_HOME", (home / ".cache").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (home / ".config").c_str(), 1);
    setenv("NO_COLOR", "1", 1);
    setenv("TERM", "dumb", 1);
    unsetenv("GTI_TEST_SELF_TEST");
    const int inFd = open("/dev/null", O_RDONLY);
    const int outFd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int errFd = open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (inFd < 0 || outFd < 0 || errFd < 0 || chdir(cwd.c_str()) != 0)
      _exit(127);
    dup2(inFd, 0);
    dup2(outFd, 1);
    dup2(errFd, 2);
    execvp(arguments[0], arguments.data());
    _exit(127);
  }
  if (pid < 0)
    return run;
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
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (WIFEXITED(status))
    run.exitCode = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    run.signal = WTERMSIG(status);
    run.timedOut = run.timedOut || run.signal == SIGXCPU;
  }
  run.out = readFile(out).value_or("");
  run.err = readFile(err).value_or("");
  return run;
}

std::string excerpt(const std::string &text) {
  std::string result;
  std::istringstream lines(text);
  int count = 0;
  for (std::string line; std::getline(lines, line) && count < 20; ++count)
    result += "      " + line + "\n";
  if (count == 20)
    result += "      ...\n";
  return result.empty() ? "      (empty)\n" : result;
}

} // namespace

std::vector<ProjectCommand>
parseProjectTranscript(const std::string &text,
                       std::vector<std::string> &errors) {
  std::vector<ProjectCommand> commands;
  std::istringstream lines(text);
  int number = 0;
  for (std::string raw; std::getline(lines, raw);) {
    ++number;
    const std::string line = trim(raw);
    if (line.empty() || line.rfind("//", 0) == 0)
      continue;
    const auto error = [&](const std::string &message) {
      errors.push_back("line " + std::to_string(number) + ": " + message);
    };
    if (line.rfind("$ ", 0) == 0) {
      bool ok = true;
      ProjectCommand command;
      command.argv = words(line.substr(2), ok);
      command.writtenLine = number;
      if (!ok || command.argv.empty())
        error("a command is `$ program arguments...`");
      else if (command.argv.front() == "cd" && command.argv.size() != 2)
        error("`$ cd` takes one directory");
      commands.push_back(std::move(command));
      continue;
    }
    if (commands.empty()) {
      error("expectations follow a `$ command` line");
      continue;
    }
    ProjectCommand &command = commands.back();
    const auto value = [&](std::size_t prefix) {
      const std::string text = trim(line.substr(prefix));
      if (text.empty())
        error("expected text after '" + line.substr(0, prefix) + "'");
      return text;
    };
    if (line.rfind("? ", 0) == 0) {
      char *end = nullptr;
      const std::string code = trim(line.substr(2));
      const long parsed = std::strtol(code.c_str(), &end, 10);
      if (code.empty() || *end != '\0' || parsed < 0 || parsed > 255)
        error("exit codes are `? N` with N from 0 to 255");
      command.exitCode = static_cast<int>(parsed);
    } else if (line.rfind("stdout:", 0) == 0) {
      command.stdoutContains.push_back(value(7));
    } else if (line.rfind("stderr:", 0) == 0) {
      command.stderrContains.push_back(value(7));
    } else if (line.rfind("!stdout:", 0) == 0) {
      command.stdoutLacks.push_back(value(8));
    } else if (line.rfind("!stderr:", 0) == 0) {
      command.stderrLacks.push_back(value(8));
    } else {
      error("expected `$ command`, `? N`, `stdout:`, `stderr:`, `!stdout:` "
            "or `!stderr:`");
    }
  }
  if (commands.empty() && errors.empty())
    errors.push_back("the transcript runs no command");
  return commands;
}

ProjectResult runProjectTest(const TestFile &test, const ProjectTools &tools,
                             const fs::path &work, double limit, bool bless) {
  ProjectResult result;
  std::vector<std::string> errors;
  const std::string transcript = readFile(test.path).value_or("");
  const std::vector<ProjectCommand> commands =
      parseProjectTranscript(transcript, errors);
  // What --bless learns per command line: the exit code and diagnostics.
  struct Observed {
    int exitCode = 0;
    std::vector<std::string> diagnostics;
  };
  std::map<int, Observed> observed;
  std::string unblessable;
  for (const std::string &error : errors)
    result.failure += "invalid test: " + error + "\n";
  if (!errors.empty())
    return result;

  std::error_code error;
  fs::remove_all(work, error);
  const fs::path project = work / "project";
  const fs::path home = work / "home";
  fs::create_directories(home);
  fs::copy(test.path.parent_path(), project, fs::copy_options::recursive,
           error);
  fs::remove(project / test.path.filename(), error);
  if (error) {
    result.failure = "could not copy the project: " + error.message() + "\n";
    return result;
  }
  const fs::path canonical = fs::canonical(project);
  const auto normalize = [&](std::string text) {
    replaceAll(text, canonical.string(), "$DIR");
    replaceAll(text, project.string(), "$DIR");
    if (!tools.standardLibrary.empty())
      replaceAll(text, tools.standardLibrary.string(), "$STD");
    replaceAll(text, tools.gti.string(), "gti");
    static const std::regex timing(R"(\b[0-9]+(\.[0-9]+)?\s?(ms|s)\b)");
    return std::regex_replace(text, timing, "<time>");
  };

  const double cpuStart = childCpu();
  fs::path cwd = canonical;
  for (const ProjectCommand &command : commands) {
    const std::string where =
        "line " + std::to_string(command.writtenLine) + ": $";
    std::string shown;
    for (const std::string &word : command.argv)
      shown += " " + word;
    if (command.argv.front() == "cd") {
      cwd = (cwd / command.argv[1]).lexically_normal();
      if (!fs::is_directory(cwd)) {
        result.failure += where + shown + ": no such directory\n";
        break;
      }
      continue;
    }
    std::vector<std::string> argv;
    for (std::string word : command.argv) {
      replaceAll(word, "$DIR", canonical.string());
      argv.push_back(word);
    }
    if (argv.front() == "gti")
      argv.front() = tools.gti.string();
    else if (argv.front() == "gti_lsp")
      argv.front() = tools.lsp.string();
    const double remaining = limit - (childCpu() - cpuStart);
    if (remaining <= 0) {
      result.timedOut = true;
      result.failure += where + shown + ": the CPU budget is spent\n";
      break;
    }
    const Run run = runCommand(argv, cwd, home, remaining, 4 * limit);
    if (run.timedOut) {
      result.timedOut = true;
      result.failure += where + shown + ": over the CPU limit (" +
                        std::to_string(limit) + " s for the transcript)\n";
      break;
    }
    const std::string out = normalize(run.out);
    const std::string err = normalize(run.err);
    std::vector<std::string> problems;
    if (bless && run.signal == 0) {
      Observed &seen = observed[command.writtenLine];
      seen.exitCode = run.exitCode;
      std::istringstream lines(err);
      for (std::string line; std::getline(lines, line);)
        if ((line.find("error[GTI-") != std::string::npos ||
             line.find("warning[GTI-") != std::string::npos) &&
            seen.diagnostics.size() < 5)
          seen.diagnostics.push_back(trim(line));
    }
    if (run.signal != 0)
      problems.push_back("terminated by signal " + std::to_string(run.signal));
    else if (run.exitCode != command.exitCode && !bless)
      problems.push_back("exit code " + std::to_string(run.exitCode) +
                         ", expected " + std::to_string(command.exitCode));
    for (const std::string &needle : command.stdoutContains)
      if (out.find(needle) == std::string::npos)
        problems.push_back("stdout lacks \"" + needle + "\"");
    for (const std::string &needle : command.stderrContains)
      if (err.find(needle) == std::string::npos)
        problems.push_back("stderr lacks \"" + needle + "\"");
    for (const std::string &needle : command.stdoutLacks)
      if (out.find(needle) != std::string::npos)
        problems.push_back("stdout contains \"" + needle + "\"");
    for (const std::string &needle : command.stderrLacks)
      if (err.find(needle) != std::string::npos)
        problems.push_back("stderr contains \"" + needle + "\"");
    if (!problems.empty()) {
      std::string &target = bless ? unblessable : result.failure;
      target += where + shown + "\n";
      for (const std::string &problem : problems)
        target += "  " + problem + "\n";
      target += "  stdout (normalised):\n" + excerpt(out) +
                "  stderr (normalised):\n" + excerpt(err);
      if (!bless)
        break;
    }
  }
  result.cpu = childCpu() - cpuStart;
  if (bless && !result.timedOut) {
    std::map<int, const ProjectCommand *> byLine;
    for (const ProjectCommand &command : commands)
      byLine[command.writtenLine] = &command;
    std::string blessed;
    std::istringstream lines(transcript);
    int number = 0;
    for (std::string raw; std::getline(lines, raw);) {
      ++number;
      if (trim(raw).rfind("? ", 0) == 0)
        continue;
      blessed += raw + "\n";
      const auto command = byLine.find(number);
      const auto seen = observed.find(number);
      if (command == byLine.end() || seen == observed.end())
        continue;
      if (seen->second.exitCode != 0)
        blessed += "? " + std::to_string(seen->second.exitCode) + "\n";
      const ProjectCommand &written = *command->second;
      if (written.stdoutContains.empty() && written.stderrContains.empty() &&
          written.stdoutLacks.empty() && written.stderrLacks.empty())
        for (const std::string &diagnostic : seen->second.diagnostics)
          blessed += "stderr: " + diagnostic + "\n";
    }
    if (blessed != transcript) {
      writeFile(test.path, blessed);
      result.blessed = true;
    }
    if (!unblessable.empty())
      result.failure = unblessable +
                       "not blessed: stdout/stderr expectations are written "
                       "by hand\n";
  }
  return result;
}

} // namespace gti_test
