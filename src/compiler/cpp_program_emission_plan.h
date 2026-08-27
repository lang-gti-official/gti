#pragma once

#include "gti/cpp_standard.h"

#include <string>
#include <vector>

namespace lang {

class LoweredProgram;
class CppProgramEmissionPlanner;

enum class CppProgramTextRole {
  SupportPreamble,
  FailureAndLifecycleSupport,
  NativeAndThreadAdapter,
  DeclarationTypeMemberLifecycle,
  InitializerAndModuleAdapter,
  HostedEntryAdapter,
};

struct CppProgramTextFragment {
  CppProgramTextRole role = CppProgramTextRole::SupportPreamble;
  std::string text;
};

// Pointer-free ordered artifact plan. All lowered-program lookup and spelling
// decisions are complete before this value crosses into the final writers.
class CppProgramEmissionPlan {
public:
  [[nodiscard]] const std::vector<CppProgramTextFragment> &fragments() const {
    return fragments_;
  }
  [[nodiscard]] bool empty() const { return fragments_.empty(); }

private:
  void append(CppProgramTextRole role, std::string text);

  std::vector<CppProgramTextFragment> fragments_;

  friend class CppProgramEmissionPlanner;
};

[[nodiscard]] CppProgramEmissionPlan
buildCppProgramEmissionPlan(const LoweredProgram &program,
                            CppStandard standard);

} // namespace lang
