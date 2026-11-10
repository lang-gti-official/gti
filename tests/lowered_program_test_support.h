#pragma once

// Backend-neutral lowering helpers for tests. Nothing here depends on a code
// generator: a test that only needs the verified lowered program (or the
// optimized MIR a backend would consume) links gti_compiler alone.

#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gti_test {

inline lang::LoweredProgram lowerProgram(
    const lang::FrontendResult &frontend, const lang::MirProgram &sourceMir,
    const lang::MirProgram &optimizedMir,
    const lang::TargetInfo &target = lang::TargetInfo::host(),
    const std::vector<lang::MirTransformRecord> &transformations = {}) {
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, sourceMir,
      optimizedMir, transformations, target);
  if (!lowered.valid()) {
    throw std::logic_error(
        lowered.issues.empty()
            ? "test fixture did not produce a lowered program"
            : "test fixture did not produce a lowered program: " +
                  lowered.issues.front().detail);
  }
  return std::move(*lowered.program);
}

// Runs the production optimization pipeline exactly as the driver does before
// handing MIR to a backend. Throws when the optimized program is incoherent.
inline lang::OptimizedProgram
optimizeProgram(const lang::FrontendResult &frontend,
                lang::OptimizationLevel level = lang::OptimizationLevel::O0,
                const lang::OptimizationResult *compatibility = nullptr) {
  std::optional<lang::OptimizationResult> ownedCompatibility;
  if (compatibility == nullptr) {
    ownedCompatibility.emplace(
        lang::OptimizationPipeline().run(frontend.hir, level));
    compatibility = &*ownedCompatibility;
  }
  lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = level,
                                        .compatibility = compatibility});
  if (!optimized.valid()) {
    throw std::logic_error(
        "test fixture did not produce a coherent optimized MIR program");
  }
  return optimized;
}

// The verified lowered program every backend consumes, built from the
// optimized MIR at `level`. Throws when lowering rejects the program.
inline lang::LoweredProgram lowerOptimizedProgram(
    const lang::FrontendResult &frontend,
    lang::OptimizationLevel level = lang::OptimizationLevel::O0,
    const lang::TargetInfo &target = lang::TargetInfo::host()) {
  const lang::OptimizedProgram optimized = optimizeProgram(frontend, level);
  return lowerProgram(frontend, optimized.sourceMir, optimized.mir, target,
                      optimized.report.transformations);
}

} // namespace gti_test
