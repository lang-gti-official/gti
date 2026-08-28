#pragma once

#include "cpp_program_emission_plan.h"

#include <string>

namespace lang {

// Formatting-only final pass over an immutable ordered program plan.
// Responsibility-specific writers are private to the implementation.
[[nodiscard]] std::string
writeCppProgramEmissionPlan(const CppProgramEmissionPlan &plan);

} // namespace lang
