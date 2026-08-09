#pragma once

#include "gti/driver/artifact.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lang::driver {

class ExecutableBuildRequest final {
public:
  ExecutableBuildRequest(CompilationRequest compilation,
                         ToolchainLayout toolchain,
                         std::filesystem::path generatedSource,
                         std::filesystem::path output,
                         std::string nativeCompiler, NativeInputs nativeInputs,
                         bool keepGeneratedSource, bool createParentDirectories,
                         bool captureSuccessfulNativeOutput);

  [[nodiscard]] const CompilationRequest &compilation() const;
  [[nodiscard]] const ToolchainLayout &toolchain() const;
  [[nodiscard]] const std::filesystem::path &generatedSource() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] const std::string &nativeCompiler() const;
  [[nodiscard]] const NativeInputs &nativeInputs() const;
  [[nodiscard]] bool keepGeneratedSource() const;
  [[nodiscard]] bool createParentDirectories() const;
  [[nodiscard]] bool captureSuccessfulNativeOutput() const;

private:
  CompilationRequest compilationRequest;
  ToolchainLayout toolchainLayout;
  std::filesystem::path generatedSourcePath;
  std::filesystem::path outputPath;
  std::string compilerExecutable;
  NativeInputs additionalNativeInputs;
  bool retainGeneratedSource;
  bool createParents;
  bool captureSuccessfulOutput;
};

enum class ExecutableBuildStatus {
  Success,
  CompilationFailure,
  OutputDirectoryFailure,
  GeneratedArtifactFailure,
  ToolchainConfigurationFailure,
  NativeCompilerFailure,
};

struct ExecutableBuildResult {
  ExecutableBuildStatus status = ExecutableBuildStatus::CompilationFailure;
  CompilationResult compilation;
  std::optional<ArtifactWriteStatus> artifactWriteStatus;
  std::optional<ToolchainResourceError> resourceError;
  std::optional<NativeProcessResult> nativeProcess;
  std::vector<std::string> nativeCommand;
  std::filesystem::path generatedSource;
  std::optional<std::string> driverDiagnostic;
  bool generatedSourceRetained = false;

  [[nodiscard]] bool succeeded() const {
    return status == ExecutableBuildStatus::Success;
  }
};

[[nodiscard]] ExecutableBuildResult
buildExecutable(const ExecutableBuildRequest &request);

} // namespace lang::driver
