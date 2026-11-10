#pragma once

// C++-backend emission helpers. Only the frozen C++ backend suites
// (cpp_mir_*, cpp_debug_artifacts, lowered_program_corpus_census) may include
// this header; general tests use lowered_program_test_support.h and
// llvm_backend_test_support.h.

#include "gti/backend.h"
#include "gti/cpp_backend.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

#include "lowered_program_test_support.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gti_test {

inline lang::BackendArtifact
emitCpp(const lang::FrontendResult &frontend,
        lang::CppStandard standard = lang::CppStandard::Cpp23,
        lang::TargetInfo target = lang::TargetInfo::host(),
        lang::OptimizationLevel level = lang::OptimizationLevel::O0,
        const lang::OptimizationResult *compatibility = nullptr) {
  const lang::OptimizedProgram optimized =
      optimizeProgram(frontend, level, compatibility);
  const lang::LoweredProgram lowered =
      lowerProgram(frontend, optimized.sourceMir, optimized.mir, target,
                   optimized.report.transformations);
  return lang::CppBackend(standard).generate(lowered);
}

inline lang::BackendArtifact
emitCpp(const lang::FrontendResult &frontend, const lang::MirProgram &sourceMir,
        const lang::MirProgram &optimizedMir,
        lang::CppStandard standard = lang::CppStandard::Cpp23,
        lang::TargetInfo target = lang::TargetInfo::host()) {
  const lang::LoweredProgram lowered =
      lowerProgram(frontend, sourceMir, optimizedMir, target);
  return lang::CppBackend(standard).generate(lowered);
}

inline lang::BackendArtifact
emitCpp(const lang::FrontendResult &frontend, const lang::MirProgram &sourceMir,
        const lang::MirProgram &optimizedMir,
        const std::vector<lang::MirTransformRecord> &transformations,
        lang::CppStandard standard = lang::CppStandard::Cpp23,
        lang::TargetInfo target = lang::TargetInfo::host()) {
  const lang::LoweredProgram lowered =
      lowerProgram(frontend, sourceMir, optimizedMir, target, transformations);
  return lang::CppBackend(standard).generate(lowered);
}

inline lang::BackendArtifact
emitCpp(const lang::FrontendResult &frontend, const lang::MirProgram &sourceMir,
        const lang::MirProgram &optimizedMir,
        const lang::OptimizationResult &compatibility,
        lang::CppStandard standard = lang::CppStandard::Cpp23,
        lang::TargetInfo target = lang::TargetInfo::host()) {
  static_cast<void>(compatibility);
  return emitCpp(frontend, sourceMir, optimizedMir, standard,
                 std::move(target));
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
