#pragma once

#include "cpp_mir_body_emission_plan.h"

#include <cstddef>
#include <string_view>

namespace lang {

enum class CppMirBodyTextForm {
  Plain,
  Failure,
};

// Formatting-only consumer of an immutable body plan. It has no MIR program
// or representation map and therefore cannot recognize or admit a body shape.
class CppMirBodyTextWriter {
public:
  [[nodiscard]] static CppMirBodyEmissionText
  write(const CppMirBodyEmissionPlan &plan, CppMirBodyTextForm form,
        std::string_view familyLabel, std::size_t indentation);
};

} // namespace lang
