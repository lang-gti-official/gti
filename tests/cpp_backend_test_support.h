#pragma once

#include "gti/backend.h"
#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/optimizer.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace gti_test {

inline lang::LoweredProgram
lowerProgram(const lang::FrontendResult &frontend,
             const lang::MirProgram &sourceMir,
             const lang::MirProgram &optimizedMir,
             const lang::TargetInfo &target = lang::TargetInfo::host()) {
  lang::LoweredProgramBuild lowered = lang::LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, sourceMir,
      optimizedMir, target);
  if (!lowered.valid()) {
    throw std::logic_error(
        lowered.issues.empty()
            ? "test fixture did not produce a lowered program"
            : "test fixture did not produce a lowered program: " +
                  lowered.issues.front().detail);
  }
  return std::move(*lowered.program);
}

inline lang::BackendArtifact
emitCpp(const lang::FrontendResult &frontend,
        lang::CppStandard standard = lang::CppStandard::Cpp23,
        lang::TargetInfo target = lang::TargetInfo::host(),
        lang::OptimizationLevel level = lang::OptimizationLevel::O0,
        const lang::OptimizationResult *compatibility = nullptr) {
  std::optional<lang::OptimizationResult> ownedCompatibility;
  if (compatibility == nullptr) {
    ownedCompatibility.emplace(
        lang::OptimizationPipeline().run(frontend.hir, level));
    compatibility = &*ownedCompatibility;
  }
  const lang::OptimizedProgram optimized =
      lang::OptimizationPipeline().run({.hir = frontend.hir,
                                        .mir = frontend.mir,
                                        .level = level,
                                        .compatibility = compatibility});
  if (!optimized.valid()) {
    throw std::logic_error(
        "test fixture did not produce a coherent optimized MIR program");
  }
  const lang::LoweredProgram lowered =
      lowerProgram(frontend, optimized.sourceMir, optimized.mir, target);
  return lang::CppBackend(standard).generate({.program = frontend.program,
                                              .semantics = frontend.semantics,
                                              .hir = frontend.hir,
                                              .mir = optimized.mir,
                                              .sourceMir = &frontend.mir,
                                              .optimizations = *compatibility,
                                              .target = std::move(target),
                                              .loweredProgram = &lowered});
}

inline std::string
emitCppText(const lang::FrontendResult &frontend,
            lang::CppStandard standard = lang::CppStandard::Cpp23,
            lang::TargetInfo target = lang::TargetInfo::host(),
            lang::OptimizationLevel level = lang::OptimizationLevel::O0,
            const lang::OptimizationResult *compatibility = nullptr) {
  return emitCpp(frontend, standard, std::move(target), level, compatibility)
      .contents;
}

} // namespace gti_test
