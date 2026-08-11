#pragma once

#include <functional>
#include <string>
#include <string_view>

// Tool-process support facilities. Declarations are LLVM-free by policy even
// though the compiled implementation requires LLVM support libraries. Public
// headers must not include llvm/* headers.

namespace lang {

// Installs process-wide crash reporting: fatal-error and allocation-failure
// handlers plus a stack-trace printer on fatal signals. Call once, first, in
// every tool entry point.
void installCrashHandlers(std::string_view toolName);

// Runs work inside LLVM's crash-recovery boundary and returns false when work
// crashed instead of completing. The callback must catch its own C++
// exceptions so none cross an LLVM frame, and it must not own shared locks or
// other resources that require stack unwinding if crash recovery transfers
// control. This is best-effort in-process recovery, not process isolation.
[[nodiscard]] bool runGuarded(const std::function<void()> &work);

// Compile-time telemetry. While a time trace is active every PhaseTimeScope
// contributes one hierarchical entry; endTimeTrace writes Chrome Trace
// Format JSON using LLVM's compile-time profiler.
void beginTimeTrace(std::string_view toolName);
[[nodiscard]] bool endTimeTrace(const std::string &path);
[[nodiscard]] bool timeTraceAvailable();

// One named region in the compile-time profile. Scopes nest naturally and
// cost nothing when tracing is inactive.
class PhaseTimeScope {
public:
  explicit PhaseTimeScope(std::string_view name);
  PhaseTimeScope(std::string_view name, std::string_view detail);
  PhaseTimeScope(const PhaseTimeScope &) = delete;
  PhaseTimeScope &operator=(const PhaseTimeScope &) = delete;
  ~PhaseTimeScope();

private:
  void *scope = nullptr;
};

} // namespace lang
