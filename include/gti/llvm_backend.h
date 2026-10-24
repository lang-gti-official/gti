#pragma once

#include "gti/backend.h"

#include <string_view>

namespace lang {

enum class LlvmCodegenOptimization {
  None,
  Less,
  Default,
  Aggressive,
};

// Policy for the native LLVM backend. LLVM implementation types stay private
// to the backend translation unit so installed GTI headers remain LLVM-free.
struct LlvmBackendOptions {
  LlvmCodegenOptimization optimization = LlvmCodegenOptimization::None;
  bool debugInfo = false;
};

class LlvmBackend final : public Backend {
public:
  explicit LlvmBackend(LlvmBackendOptions options = {});

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] BackendArtifact generate(LoweredProgram program) override;

private:
  LlvmBackendOptions options;
};

} // namespace lang
