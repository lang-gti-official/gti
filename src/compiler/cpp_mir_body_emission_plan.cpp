#include "cpp_mir_body_emission_plan.h"

#include <algorithm>

namespace lang {

const CppMirBodyEmissionPlan *
CppMirProgramEmissionPlan::find(MirBodyAddress address) const {
  const auto found = std::find_if(bodies_.begin(), bodies_.end(),
                                  [&](const CppMirBodyEmissionPlan &plan) {
                                    return plan.body() == address;
                                  });
  return found == bodies_.end() ? nullptr : &*found;
}

CppMirProgramEmissionAnalysis CppMirProgramEmissionPlan::analysis() const {
  CppMirProgramEmissionAnalysis result{.readiness = readiness_,
                                       .issues = issues_};
  result.bodies.reserve(bodies_.size());
  for (const CppMirBodyEmissionPlan &plan : bodies_) {
    result.bodies.push_back(plan.analysis());
  }
  return result;
}

} // namespace lang
