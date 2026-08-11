// ARCHIVED: excluded from the GTI build. See archive/compiler/README.md.

#include "gti/support.h"

namespace lang {

void installCrashHandlers(std::string_view) {}

bool runGuarded(const std::function<void()> &work) {
  work();
  return true;
}

void beginTimeTrace(std::string_view) {}

bool endTimeTrace(const std::string &) { return false; }

bool timeTraceAvailable() { return false; }

PhaseTimeScope::PhaseTimeScope(std::string_view) {}

PhaseTimeScope::PhaseTimeScope(std::string_view, std::string_view) {}

PhaseTimeScope::~PhaseTimeScope() = default;

} // namespace lang
