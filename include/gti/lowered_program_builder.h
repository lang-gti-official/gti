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

// Checks the caller has already run on exactly the programs and records it
// passes to build(). Verification depends only on program content, so the
// builder skips only that repeated check (see mir_detail::verifyVerified*);
// every other verification and every result is unchanged. The driver passes
// the optimization pipeline's own input verification and output coherence
// results; tests and other callers leave both false and verify everything.
struct LoweredProgramVerifiedInputs {
  bool sourceMirVerified = false;
  bool optimizationCoherenceVerified = false;
};

class LoweredProgramBuilder final {
public:
  [[nodiscard]] LoweredProgramBuild
  build(const Program &program, const SemanticModel &semantics,
        const HirProgram &hir, const MirProgram &sourceMir,
        const MirProgram &optimizedMir,
        const std::vector<MirTransformRecord> &transformations,
        const TargetInfo &target, bool captureSourceLocations = false,
        LoweredProgramVerifiedInputs verified = {}) const;
};

} // namespace lang
