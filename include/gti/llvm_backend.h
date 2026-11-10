#pragma once

#include "gti/backend.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace lang {

struct TargetInfo;

enum class LlvmCodegenOptimization {
  None,
  Less,
  Default,
  Aggressive,
};

// UndefinedMemory checks original generated load/store null/alignment only;
// it does not claim complete undefined-behavior instrumentation.
enum class LlvmInstrumentationMode {
  None,
  Address,
  UndefinedMemory,
  AddressAndUndefinedMemory,
  Thread,
};

// Stable policy spelling for cache/provenance records. Invalid values reject.
[[nodiscard]] std::string_view
llvmInstrumentationModeName(LlvmInstrumentationMode mode);

// Policy for the native LLVM backend. LLVM implementation types stay private
// to the backend translation unit so installed GTI headers remain LLVM-free.
struct LlvmBackendOptions {
  LlvmCodegenOptimization optimization = LlvmCodegenOptimization::None;
  bool debugInfo = false;
  LlvmInstrumentationMode instrumentation = LlvmInstrumentationMode::None;
  // Return the module as textual LLVM IR (`.ll`) instead of object bytes: the
  // exact module object emission would hand to target code generation, after
  // GTI's own MIR optimization and the selected LLVM pipeline. Inspection only;
  // never part of object-cache identity.
  bool textualIr = false;
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
  std::string tuneCpu;
  std::string features;
  std::uint64_t privateAbi = 0;
  std::uint64_t relocation = 0;
  std::uint64_t codeModel = 0;
  std::uint64_t codegenOptimization = 0;
  bool debugInfo = false;
  std::string instrumentation;
};

[[nodiscard]] std::optional<LlvmBackendIdentity>
queryLlvmBackendIdentity(const TargetInfo &target, LlvmBackendOptions options,
                         std::string &error);

// Runtime C exports and LLVM's bounded target memory-helper family. The archive
// must be independently selected and compatibility-pinned, not bundle supplied.
[[nodiscard]] std::vector<std::string>
compiledLibrarySupportSymbols(std::string_view archive,
                              const TargetInfo &target);

// A host relocatable object with exactly the declared external definitions and
// explicitly selected GTI imports plus compiledLibrarySupportSymbols.
void verifyCompiledScalarObject(std::string_view bytes,
                                const TargetInfo &target,
                                const std::vector<std::string> &symbols,
                                const std::vector<std::string> &imports = {});

class LlvmBackend final : public Backend {
public:
  explicit LlvmBackend(LlvmBackendOptions options = {});

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] BackendArtifact generate(LoweredProgram program) override;

private:
  LlvmBackendOptions options;
};

} // namespace lang
