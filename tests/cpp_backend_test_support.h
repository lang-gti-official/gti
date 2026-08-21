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
  return lang::CppBackend(standard).generate({.program = frontend.program,
                                              .semantics = frontend.semantics,
                                              .hir = frontend.hir,
                                              .mir = optimized.mir,
                                              .sourceMir = &frontend.mir,
                                              .optimizations = *compatibility,
                                              .target = std::move(target)});
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
