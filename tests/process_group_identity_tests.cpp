#include "gti/driver/process.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
bool inspectKills = false;
bool interruptWait = false;
bool failWait = false;
unsigned kills = 0;
unsigned releasedIdentities = 0;
pid_t lastKilled = 0;

template <class Function> Function original(const char *name) {
  auto function = reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
  if (!function)
    std::abort();
  return function;
}
constexpr bool killNoexcept = noexcept(::kill(pid_t{}, 0));
constexpr bool waitNoexcept =
    noexcept(::waitid(P_PID, id_t{}, std::declval<siginfo_t *>(), WEXITED));

bool lastChildReaped() {
  static auto observe = original<decltype(&::waitid)>("waitid");
  siginfo_t info{};
  return lastKilled > 0 &&
         observe(P_PID, static_cast<id_t>(lastKilled), &info,
                 WEXITED | WNOHANG | WNOWAIT) == -1 &&
         errno == ECHILD;
}
} // namespace

extern "C" int waitid(idtype_t type, id_t id, siginfo_t *info,
                      int options) noexcept(waitNoexcept) {
  static auto real = original<decltype(&::waitid)>("waitid");
  if (interruptWait || failWait) {
    errno = interruptWait ? EINTR : EIO;
    interruptWait = false;
    failWait = false;
    return -1;
  }
  return real(type, id, info, options);
}

extern "C" int kill(pid_t target, int signal) noexcept(killNoexcept) {
  static auto real = original<decltype(&::kill)>("kill");
  static auto observe = original<decltype(&::waitid)>("waitid");
  if (inspectKills && target != 0 && signal == SIGKILL) {
    const int saved = errno;
    siginfo_t info{};
    int result;
    do {
      result = observe(P_PID, static_cast<id_t>(target < 0 ? -target : target),
                       &info, WEXITED | WNOHANG | WNOWAIT);
    } while (result == -1 && errno == EINTR);
    ++kills;
    lastKilled = target < 0 ? -target : target;
    if (result == -1)
      ++releasedIdentities;
    errno = saved;
  }
  return real(target, signal);
}
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  return 77;
#else
  using namespace lang::driver;
  using namespace std::chrono_literals;
  if (argc == 2) {
    const std::string mode = argv[1];
    if (mode == "pid") {
      std::ofstream(std::getenv("GTI_GROUP_IDENTITY_PATH")) << getpid();
      return 0;
    }
    std::cout << "started\n" << std::flush;
    if (mode == "loop")
      for (;;)
        std::this_thread::sleep_for(10ms);
    if (mode == "signal")
      raise(SIGTERM);
    return mode == "exit23" ? 23 : 0;
  }
  int failures = 0;
  auto check = [&](bool condition, const char *message) {
    if (!condition) {
      std::cerr << message << '\n';
      ++failures;
    }
  };
  const auto executable = std::filesystem::absolute(argv[0]).string();
  inspectKills = true;
  for (const auto *mode : {"exit0", "exit23", "signal"}) {
    const auto result =
        invokeProcess({executable, mode}, {.captureSuccessfulOutput = true,
                                           .timeout = 2s,
                                           .isolateProcessGroup = true});
    check(result.exitCode == (std::string(mode) == "exit0"    ? 0
                              : std::string(mode) == "exit23" ? 23
                                                              : 143) &&
              !result.driverDiagnostic && result.output == "started\n",
          "cleanup must preserve the child status and output");
    check(lastChildReaped(), "completed children must actually be reaped");
  }
  auto timed = invokeProcess({executable, "loop"},
                             {.timeout = 30ms, .isolateProcessGroup = true});
  check(timed.timedOut && timed.exitCode == 124,
        "timeout must still kill and reap the owned child");
  check(lastChildReaped(), "timed-out children must actually be reaped");
  auto terminated = startProcess({executable, "loop"},
                                 {.timeout = 2s, .isolateProcessGroup = true});
  terminated.terminate();
  check(terminated.wait().exitCode == 137,
        "explicit termination must still reap the child");
  check(lastChildReaped(), "terminated children must actually be reaped");
  interruptWait = true;
  auto interrupted = invokeProcess(
      {executable, "exit0"}, {.timeout = 2s, .isolateProcessGroup = true});
  check(interrupted.succeeded() && !interruptWait,
        "an interrupted wait observation must retry without losing ownership");
  check(lastChildReaped(), "interrupted waits must still reap their child");
  failWait = true;
  auto failed = invokeProcess({executable, "loop"},
                              {.timeout = 200ms, .isolateProcessGroup = true});
  check(failed.driverDiagnostic && !failWait,
        "an observation error must report failure and clean the owned child");
  check(lastChildReaped(), "observation errors must still reap owned children");
  failWait = interruptWait = false;

  std::string temporary =
      (std::filesystem::temp_directory_path() / "gti-group-identity-XXXXXX")
          .string();
  char *directory = mkdtemp(temporary.data());
  if (!directory)
    return 1;
  const auto marker = std::filesystem::path(directory) / "pid";
  auto lost = startProcess(
      {executable, "pid"},
      {.environment = {{"GTI_GROUP_IDENTITY_PATH", marker.string()}},
       .timeout = 2s,
       .isolateProcessGroup = true});
  const auto limit = std::chrono::steady_clock::now() + 2s;
  pid_t pid = 0;
  while (!pid && std::chrono::steady_clock::now() < limit) {
    std::ifstream(marker) >> pid;
    if (!pid)
      std::this_thread::sleep_for(1ms);
  }
  int status = 0;
  if (pid > 0 && waitpid(pid, &status, 0) == pid) {
    const auto before = kills;
    lost.terminate();
    auto result = lost.wait();
    check(result.driverDiagnostic && kills == before,
          "externally reaped children must fail without signalling a recycled "
          "numeric identity");
  } else {
    check(false, "external reaping fixture must obtain its child identity");
  }
  std::filesystem::remove_all(directory);
  check(kills >= 5 && releasedIdentities == 0,
        "every cleanup signal must retain an unreaped child identity; reaping "
        "first makes PID reuse unsafe");
  inspectKills = false;
  return failures == 0 ? 0 : 1;
#endif
}
