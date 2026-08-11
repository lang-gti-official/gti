#pragma once

#include <functional>
#include <string>
#include <string_view>

// Tool-process support facilities. Declarations are LLVM-free by policy: when
// GTI is built with the LLVM support libraries the implementations in
// src/compiler/support.cpp use them, and otherwise every facility degrades to
// a safe no-op. Public headers must not include llvm/* headers or branch on
// GTI_HAS_LLVM; that macro is private to compiled compiler sources.

namespace lang {

// Installs process-wide crash reporting: fatal-error and allocation-failure
// handlers plus a stack-trace printer on fatal signals when LLVM support is
// available. Call once, first, in every tool entry point. Without LLVM this
// is a no-op and tools keep their previous behavior.
void installCrashHandlers(std::string_view toolName);

// Runs work inside a crash-recovery boundary when one is available and
// returns false when the work crashed instead of completing. Without LLVM
// the work runs directly and a crash terminates the process as before.
// C++ exceptions are not intercepted; existing try/catch recovery still
// applies on top of this boundary.
[[nodiscard]] bool runGuarded(const std::function<void()> &work);

// Compile-time telemetry. While a time trace is active every PhaseTimeScope
// contributes one hierarchical entry; endTimeTrace writes Chrome Trace
// Format JSON. All three are free no-ops without LLVM support.
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
