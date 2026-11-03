#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

// Native stack for the compiler's recursive compile-time work.
//
// Constant evaluation, staged constexpr analysis, macro expansion and the
// HIR/MIR lowering of expanded bodies recurse on the native stack once per
// nested call or expansion, so how deep they may go is a configurable
// language limit (CompileLimits), never a property of the host thread. Each
// such recursion boundary checks nativeStackNearlyExhausted (or uses
// withSufficientStack): when less than a reserved headroom remains on the
// current stack, the work continues on a fresh, guard-paged stack segment and
// returns to the original stack when it completes. The segment belongs to
// the same thread, so thread-local state, crash recovery, time tracing and
// cooperative cancellation behave exactly as without it; a C++ exception
// thrown on a segment propagates to the caller. The switch is invisible to
// the computation, so results never depend on where it happens.
//
// The switch is implemented for x86-64 and AArch64 on Mach-O and ELF hosts,
// which covers every supported GTI host. Elsewhere nothing switches and
// recursion stays on the thread's own stack (see support.cpp).
[[nodiscard]] bool nativeStackNearlyExhausted();
void runOnFreshNativeStack(void (*work)(void *), void *context);

// Runs work on a fresh segment unconditionally. A recursion boundary that
// re-enters itself there writes
//   if (nativeStackNearlyExhausted())
//     return onFreshNativeStack([&] { return self(arguments...); });
template <typename Work>
auto onFreshNativeStack(Work &&work) -> std::invoke_result_t<Work &> {
  using Result = std::invoke_result_t<Work &>;
  if constexpr (std::is_void_v<Result>) {
    runOnFreshNativeStack(
        [](void *context) {
          (*static_cast<std::remove_reference_t<Work> *>(context))();
        },
        &work);
  } else {
    std::optional<Result> result;
    auto call = [&] { result.emplace(work()); };
    runOnFreshNativeStack(
        [](void *context) { (*static_cast<decltype(call) *>(context))(); },
        &call);
    return std::move(*result);
  }
}

template <typename Work>
auto withSufficientStack(Work &&work) -> std::invoke_result_t<Work &> {
  if (!nativeStackNearlyExhausted())
    return work();
  return onFreshNativeStack(work);
}

// Test seam. `headroom` is the remaining stack below which work moves to a
// fresh segment, `segment` the size of each segment; zero keeps a default.
// The policy is process-wide. Returns the previous policy.
struct NativeStackPolicy {
  std::size_t headroom = 0;
  std::size_t segment = 0;
};
NativeStackPolicy setNativeStackPolicyForTesting(NativeStackPolicy policy);
// Segments this thread has entered, for tests that must prove a switch.
[[nodiscard]] std::size_t nativeStackSegmentsEntered();

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
