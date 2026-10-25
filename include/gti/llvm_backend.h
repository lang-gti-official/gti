#pragma once

#include "gti/backend.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace lang {

struct TargetInfo;

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

// Resolved producer facts for whole-program object caches. LLVM types remain
// private; an unavailable fact disables reuse, not executable generation.
struct LlvmBackendIdentity {
  std::string schema;
  std::string llvmVersion;
  std::string compilerExecutable;
  std::string triple;
  std::string dataLayout;
  std::string cpu;
  std::string features;
  std::uint64_t privateAbi = 0;
  std::uint64_t relocation = 0;
  std::uint64_t codeModel = 0;
  std::uint64_t codegenOptimization = 0;
};

[[nodiscard]] std::optional<LlvmBackendIdentity>
queryLlvmBackendIdentity(const TargetInfo &target, LlvmBackendOptions options,
                         std::string &error);

class LlvmBackend final : public Backend {
public:
  explicit LlvmBackend(LlvmBackendOptions options = {});

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] BackendArtifact generate(LoweredProgram program) override;

private:
  LlvmBackendOptions options;
};

} // namespace lang
