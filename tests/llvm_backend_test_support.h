#pragma once

// Native-emission oracle for general tests: the shipped LLVM backend consumes
// the same verified lowered program the driver builds. Link gti_llvm_backend.

#include "gti/backend.h"
#include "gti/llvm_backend.h"

#include "lowered_program_test_support.h"

#include <string>
#include <utility>

namespace gti_test {

// Emits a native object for `frontend` through the production optimization
// pipeline and lowering. Throws whatever lowering or the backend throws.
inline lang::BackendArtifact
emitLlvm(const lang::FrontendResult &frontend,
         lang::OptimizationLevel level = lang::OptimizationLevel::O0,
         const lang::TargetInfo &target = lang::TargetInfo::host(),
         lang::LlvmBackendOptions options = {}) {
  return lang::LlvmBackend(options).generate(
      lowerOptimizedProgram(frontend, level, target));
}

// True when the shipped backend produces a non-empty native object. On
// failure `error` (if supplied) receives the exception text.
inline bool
llvmEmits(const lang::FrontendResult &frontend,
          lang::OptimizationLevel level = lang::OptimizationLevel::O0,
          const lang::TargetInfo &target = lang::TargetInfo::host(),
          std::string *error = nullptr) {
  try {
    const lang::BackendArtifact artifact = emitLlvm(frontend, level, target);
    return artifact.kind == lang::BackendArtifactKind::Object &&
           !artifact.contents.empty();
  } catch (const std::exception &failure) {
    if (error != nullptr)
      *error = failure.what();
    return false;
  }
}

} // namespace gti_test
