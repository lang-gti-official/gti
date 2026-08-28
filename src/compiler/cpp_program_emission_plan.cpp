#include "cpp_program_emission_plan.h"

#include <utility>

namespace lang {

void CppProgramEmissionPlan::append(CppProgramTextRole role, std::string text) {
  if (!text.empty()) {
    fragments_.push_back({.role = role, .text = std::move(text)});
  }
}

} // namespace lang
