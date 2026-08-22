#pragma once

#include "gti/lowered_program.h"

#include <optional>
#include <vector>

namespace lang {

class HirProgram;
class Program;
class SemanticModel;

struct LoweredProgramBuild {
  std::optional<LoweredProgram> program;
  std::vector<LoweredProgramIssue> issues;

  [[nodiscard]] bool valid() const {
    return program.has_value() && issues.empty();
  }
};

class LoweredProgramBuilder final {
public:
  [[nodiscard]] LoweredProgramBuild
  build(const Program &program, const SemanticModel &semantics,
        const HirProgram &hir, const MirProgram &sourceMir,
        const MirProgram &optimizedMir, const TargetInfo &target) const;
};

} // namespace lang
