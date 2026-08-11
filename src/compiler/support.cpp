#include "gti/support.h"

#include <cstdio>
#include <cstdlib>

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TimeProfiler.h"

#include <utility>

namespace lang {

namespace {

// LLVM support failures are internal toolchain failures, never GTI source
// diagnostics. Report deterministically on stderr and abort; without an
// installed handler LLVM would abort through its own reporting instead.
void reportToolchainFatal(void *, const char *reason, bool) {
  std::fputs("GTI internal toolchain error: ", stderr);
  std::fputs(reason == nullptr ? "unknown fatal error" : reason, stderr);
  std::fputc('\n', stderr);
}

void reportToolchainBadAlloc(void *, const char *reason, bool) {
  std::fputs("GTI internal toolchain error: allocation failed: ", stderr);
  std::fputs(reason == nullptr ? "" : reason, stderr);
  std::fputc('\n', stderr);
}

[[nodiscard]] llvm::StringRef ref(std::string_view text) {
  return llvm::StringRef(text.data(), text.size());
}

} // namespace

void installCrashHandlers(std::string_view toolName) {
  // The signal printer retains the tool name for symbolization; keep a copy
  // with static storage duration.
  static std::string retainedToolName;
  retainedToolName.assign(toolName);
  llvm::install_fatal_error_handler(reportToolchainFatal);
  llvm::install_bad_alloc_error_handler(reportToolchainBadAlloc);
  llvm::sys::PrintStackTraceOnErrorSignal(retainedToolName);
  llvm::CrashRecoveryContext::Enable();
}

bool runGuarded(const std::function<void()> &work) {
  llvm::CrashRecoveryContext context;
  return context.RunSafely([&work] { work(); });
}

void beginTimeTrace(std::string_view toolName) {
  if (!llvm::timeTraceProfilerEnabled()) {
    llvm::timeTraceProfilerInitialize(/*TimeTraceGranularity=*/0,
                                      ref(toolName));
  }
}

bool endTimeTrace(const std::string &path) {
  if (!llvm::timeTraceProfilerEnabled()) {
    return false;
  }
  llvm::Error error =
      llvm::timeTraceProfilerWrite(path, /*FallbackFileName=*/"-");
  const bool failed = static_cast<bool>(error);
  llvm::consumeError(std::move(error));
  llvm::timeTraceProfilerCleanup();
  return !failed;
}

bool timeTraceAvailable() { return true; }

PhaseTimeScope::PhaseTimeScope(std::string_view name) {
  if (llvm::timeTraceProfilerEnabled()) {
    scope = new llvm::TimeTraceScope(ref(name));
  }
}

PhaseTimeScope::PhaseTimeScope(std::string_view name, std::string_view detail) {
  if (llvm::timeTraceProfilerEnabled()) {
    scope = new llvm::TimeTraceScope(ref(name), ref(detail));
  }
}

PhaseTimeScope::~PhaseTimeScope() {
  delete static_cast<llvm::TimeTraceScope *>(scope);
}

} // namespace lang
