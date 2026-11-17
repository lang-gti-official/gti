#include "project.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
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

// A pseudo-terminal for `$ tty:` commands: the controller's descriptor and
// the terminal's path, 80 columns by 24 rows, with output post-processing
// off so a newline stays a newline. -1 on failure.
int openTerminal(std::string &name) {
  const int controller = posix_openpt(O_RDWR | O_NOCTTY);
  if (controller < 0)
    return -1;
  const char *path = nullptr;
  if (grantpt(controller) != 0 || unlockpt(controller) != 0 ||
      (path = ptsname(controller)) == nullptr) {
    close(controller);
    return -1;
  }
  name = path;
  return controller;
}

// The environment's changes for one command: `unset` removes a variable.
using Changes = std::vector<std::pair<std::string, std::optional<std::string>>>;

Run runCommand(const std::vector<std::string> &argv, const fs::path &cwd,
               const fs::path &home, double cpuLimit, double wallLimit,
               const Changes &changes, bool terminal) {
  const fs::path out = home / ".command-stdout";
  const fs::path err = home / ".command-stderr";
  std::vector<char *> arguments;
  for (const std::string &argument : argv)
    arguments.push_back(const_cast<char *>(argument.c_str()));
  arguments.push_back(nullptr);
  Run run;
  std::string terminalName;
  const int controller = terminal ? openTerminal(terminalName) : -1;
  if (terminal && controller < 0) {
    run.err = "gti-test: could not open a pseudo-terminal\n";
    return run;
  }
  const pid_t pid = fork();
  if (pid == 0) {
    rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    const auto seconds =
        static_cast<rlim_t>(std::max(1.0, std::ceil(cpuLimit)));
    rlimit cpu{seconds, seconds + 1};
    setrlimit(RLIMIT_CPU, &cpu);
    signal(SIGPIPE, SIG_DFL);
    setenv("HOME", home.c_str(), 1);
    setenv("XDG_CACHE_HOME", (home / ".cache").c_str(), 1);
    setenv("XDG_CONFIG_HOME", (home / ".config").c_str(), 1);
    setenv("NO_COLOR", "1", 1);
    setenv("TERM", "dumb", 1);
    unsetenv("GTI_TEST_SELF_TEST");
    for (const auto &[name, value] : changes) {
      if (value)
        setenv(name.c_str(), value->c_str(), 1);
      else
        unsetenv(name.c_str());
    }
    const int inFd = open("/dev/null", O_RDONLY);
    int outFd = -1;
    int errFd = -1;
    if (terminal) {
      // A session of its own whose controlling terminal is the pty.
      close(controller);
      setsid();
      outFd = open(terminalName.c_str(), O_RDWR);
      errFd = outFd;
#if defined(TIOCSCTTY)
      if (outFd >= 0)
        ioctl(outFd, TIOCSCTTY, 0);
#endif
      termios mode{};
      if (outFd >= 0 && tcgetattr(outFd, &mode) == 0) {
        mode.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        tcsetattr(outFd, TCSANOW, &mode);
      }
      winsize size{};
      size.ws_row = 24;
      size.ws_col = 80;
      if (outFd >= 0)
        ioctl(outFd, TIOCSWINSZ, &size);
    } else {
      outFd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      errFd = open(err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (inFd < 0 || outFd < 0 || errFd < 0 || chdir(cwd.c_str()) != 0)
      _exit(127);
    dup2(inFd, 0);
    dup2(outFd, 1);
    dup2(errFd, 2);
    if (outFd > 2)
      close(outFd);
    if (errFd > 2 && errFd != outFd)
      close(errFd);
    execvp(arguments[0], arguments.data());
    _exit(127);
  }
  if (pid < 0) {
    if (controller >= 0)
      close(controller);
    return run;
  }
  const double start = wallSeconds();
  int status = 0;
  // The terminal's output, read as it comes so the program never blocks on
  // a full buffer; reading fails (EIO) once every writer has gone.
  std::string screen;
  const auto readTerminal = [&](int milliseconds) {
    pollfd fd{controller, POLLIN, 0};
    while (controller >= 0 && poll(&fd, 1, milliseconds) > 0) {
      char chunk[4096];
      const ssize_t count = read(controller, chunk, sizeof chunk);
      if (count <= 0)
        break;
      screen.append(chunk, static_cast<std::size_t>(count));
      milliseconds = 0;
    }
  };
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
    if (controller >= 0)
      readTerminal(2);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (controller >= 0) {
    readTerminal(0);
    close(controller);
  }
  if (WIFEXITED(status))
    run.exitCode = WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    run.signal = WTERMSIG(status);
    run.timedOut = run.timedOut || run.signal == SIGXCPU;
  }
  if (terminal) {
    run.out = std::move(screen);
  } else {
    run.out = readFile(out).value_or("");
    run.err = readFile(err).value_or("");
  }
  return run;
}

// An expectation's text: `\e` is the escape character (ESC, for colour
// sequences) and `\\` a backslash; anything else is literal.
std::string unescape(const std::string &text) {
  std::string result;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\\' && index + 1 < text.size() &&
        (text[index + 1] == 'e' || text[index + 1] == '\\')) {
      result += text[index + 1] == 'e' ? '\x1b' : '\\';
      ++index;
    } else {
      result += text[index];
    }
  }
  return result;
}

bool variableName(const std::string &name) {
  return !name.empty() &&
         !std::isdigit(static_cast<unsigned char>(name.front())) &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
         });
}

// Text for a failure message, with the escape character written `\e`.
std::string visible(std::string text) {
  replaceAll(text, "\x1b", "\\e");
  return text;
}

std::string excerpt(const std::string &raw) {
  const std::string text = visible(raw);
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

ProjectTranscript parseProjectTranscript(const std::string &text,
                                         std::vector<std::string> &errors) {
  ProjectTranscript transcript;
  std::vector<ProjectCommand> &commands = transcript.commands;
  bool runs = false;
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
    if (line == "slow") {
      if (transcript.slow)
        error("duplicate `slow`");
      transcript.slow = true;
      continue;
    }
    if (line.rfind("env ", 0) == 0 || line.rfind("unset ", 0) == 0) {
      ProjectCommand change;
      change.writtenLine = number;
      const bool set = line.rfind("env ", 0) == 0;
      const std::string rest = trim(line.substr(set ? 4 : 6));
      change.kind =
          set ? ProjectCommand::Kind::SetEnv : ProjectCommand::Kind::Unset;
      const std::size_t equals = set ? rest.find('=') : std::string::npos;
      change.variable = set ? rest.substr(0, equals) : rest;
      if (set && equals != std::string::npos)
        change.value = rest.substr(equals + 1);
      if (!variableName(change.variable) ||
          (set && equals == std::string::npos))
        error(set ? "environment lines are `env NAME=value`"
                  : "`unset NAME` names one variable");
      commands.push_back(std::move(change));
      continue;
    }
    if (line.rfind("$ ", 0) == 0) {
      bool ok = true;
      ProjectCommand command;
      std::string rest = line.substr(2);
      if (rest.rfind("tty:", 0) == 0) {
        command.terminal = true;
        rest = rest.substr(4);
      }
      command.argv = words(rest, ok);
      command.writtenLine = number;
      if (!ok || command.argv.empty())
        error("a command is `$ program arguments...` or `$ tty: program "
              "arguments...`");
      else if (command.argv.front() == "cd" &&
               (command.argv.size() != 2 || command.terminal))
        error("`$ cd` takes one directory");
      runs = runs || (ok && !command.argv.empty());
      commands.push_back(std::move(command));
      continue;
    }
    if (commands.empty() || commands.back().kind != ProjectCommand::Kind::Run) {
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
      command.stdoutContains.push_back(unescape(value(7)));
    } else if (line.rfind("stderr:", 0) == 0) {
      if (command.terminal)
        error("a `$ tty:` command's output is all `stdout:` (one terminal)");
      command.stderrContains.push_back(unescape(value(7)));
    } else if (line.rfind("!stdout:", 0) == 0) {
      command.stdoutLacks.push_back(unescape(value(8)));
    } else if (line.rfind("!stderr:", 0) == 0) {
      if (command.terminal)
        error("a `$ tty:` command's output is all `stdout:` (one terminal)");
      command.stderrLacks.push_back(unescape(value(8)));
    } else {
      error("expected `$ command`, `? N`, `stdout:`, `stderr:`, `!stdout:`, "
            "`!stderr:`, `env NAME=value`, `unset NAME` or `slow`");
    }
  }
  if (!runs && errors.empty())
    errors.push_back("the transcript runs no command");
  return transcript;
}

ProjectResult runProjectTest(const TestFile &test, const ProjectTools &tools,
                             const fs::path &work, double limit, bool bless) {
  ProjectResult result;
  std::vector<std::string> errors;
  const std::string transcript = readFile(test.path).value_or("");
  const std::vector<ProjectCommand> commands =
      parseProjectTranscript(transcript, errors).commands;
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
  const fs::path canonicalHome = fs::canonical(home);
  const auto normalize = [&](std::string text) {
    replaceAll(text, canonical.string(), "$DIR");
    replaceAll(text, project.string(), "$DIR");
    replaceAll(text, canonicalHome.string(), "$HOME");
    replaceAll(text, home.string(), "$HOME");
    if (!tools.standardLibrary.empty())
      replaceAll(text, tools.standardLibrary.string(), "$STD");
    replaceAll(text, tools.gti.string(), "gti");
    static const std::regex timing(R"(\b[0-9]+(\.[0-9]+)?\s?(ms|s)\b)");
    return std::regex_replace(text, timing, "<time>");
  };

  const double cpuStart = childCpu();
  fs::path cwd = canonical;
  // `env`/`unset` lines, in order; `$PATH` in a value is the PATH later
  // commands would otherwise see.
  Changes changes;
  const auto currentPath = [&]() -> std::string {
    for (auto it = changes.rbegin(); it != changes.rend(); ++it)
      if (it->first == "PATH")
        return it->second.value_or("");
    const char *inherited = std::getenv("PATH");
    return inherited == nullptr ? "" : inherited;
  };
  const auto expand = [&](std::string text) {
    replaceAll(text, "$DIR", canonical.string());
    replaceAll(text, "$HOME", canonicalHome.string());
    return text;
  };
  for (const ProjectCommand &command : commands) {
    if (command.kind == ProjectCommand::Kind::SetEnv) {
      std::string value = command.value;
      replaceAll(value, "$PATH", currentPath());
      changes.emplace_back(command.variable, expand(value));
      continue;
    }
    if (command.kind == ProjectCommand::Kind::Unset) {
      changes.emplace_back(command.variable, std::nullopt);
      continue;
    }
    const std::string where = "line " + std::to_string(command.writtenLine) +
                              ": $" + (command.terminal ? " tty:" : "");
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
    for (const std::string &word : command.argv)
      argv.push_back(expand(word));
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
    const Run run = runCommand(argv, cwd, canonicalHome, remaining, 4 * limit,
                               changes, command.terminal);
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
        problems.push_back("stdout lacks \"" + visible(needle) + "\"");
    for (const std::string &needle : command.stderrContains)
      if (err.find(needle) == std::string::npos)
        problems.push_back("stderr lacks \"" + visible(needle) + "\"");
    for (const std::string &needle : command.stdoutLacks)
      if (out.find(needle) != std::string::npos)
        problems.push_back("stdout contains \"" + visible(needle) + "\"");
    for (const std::string &needle : command.stderrLacks)
      if (err.find(needle) != std::string::npos)
        problems.push_back("stderr contains \"" + visible(needle) + "\"");
    if (!problems.empty()) {
      std::string &target = bless ? unblessable : result.failure;
      target += where + shown + "\n";
      for (const std::string &problem : problems)
        target += "  " + problem + "\n";
      if (command.terminal)
        target += "  terminal (normalised):\n" + excerpt(out);
      else
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
