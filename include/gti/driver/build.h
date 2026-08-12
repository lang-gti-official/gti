#pragma once

#include "gti/driver/artifact.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lang::driver {

struct ManagedOutputPolicy {
  std::filesystem::path trustedRoot;
  std::filesystem::path outputRoot;
};

class ExecutableBuildRequest final {
public:
  ExecutableBuildRequest(
      CompilationRequest compilation, ToolchainLayout toolchain,
      std::filesystem::path generatedSource, std::filesystem::path output,
      std::string nativeCompiler, NativeInputs nativeInputs,
      bool keepGeneratedSource, bool createParentDirectories,
      bool captureSuccessfulNativeOutput,
      std::optional<ManagedOutputPolicy> managedOutput = std::nullopt,
      std::optional<std::string> cCompiler = std::nullopt);

  [[nodiscard]] const CompilationRequest &compilation() const;
  [[nodiscard]] const ToolchainLayout &toolchain() const;
  [[nodiscard]] const std::filesystem::path &generatedSource() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] const std::string &nativeCompiler() const;
  [[nodiscard]] const std::optional<std::string> &cCompiler() const;
  [[nodiscard]] const NativeInputs &nativeInputs() const;
  [[nodiscard]] bool keepGeneratedSource() const;
  [[nodiscard]] bool createParentDirectories() const;
  [[nodiscard]] bool captureSuccessfulNativeOutput() const;
  [[nodiscard]] const std::optional<ManagedOutputPolicy> &managedOutput() const;

private:
  CompilationRequest compilationRequest;
  ToolchainLayout toolchainLayout;
  std::filesystem::path generatedSourcePath;
  std::filesystem::path outputPath;
  std::string compilerExecutable;
  std::optional<std::string> cCompilerExecutable;
  NativeInputs additionalNativeInputs;
  bool retainGeneratedSource;
  bool createParents;
  bool captureSuccessfulOutput;
  std::optional<ManagedOutputPolicy> managedOutputPolicy;
};

enum class ExecutableBuildStatus {
  Success,
  CompilationFailure,
  OutputDirectoryFailure,
  GeneratedArtifactFailure,
  ToolchainConfigurationFailure,
  NativeCCompilerFailure,
  NativeObjectPublicationFailure,
  NativeCompilerFailure,
  ArtifactPublicationFailure,
  ArtifactPathConflict,
};

struct NativeCCompilationResult {
  std::filesystem::path source;
  std::filesystem::path object;
  std::vector<std::string> command;
  NativeProcessResult process;
  std::optional<ArtifactPublishResult> artifactPublishResult;
};

struct ExecutableBuildResult {
  ExecutableBuildStatus status = ExecutableBuildStatus::CompilationFailure;
  CompilationResult compilation;
  std::optional<ArtifactWriteStatus> artifactWriteStatus;
  std::optional<ArtifactPublishResult> artifactPublishResult;
  std::optional<ToolchainResourceError> resourceError;
  std::vector<NativeCCompilationResult> cCompilations;
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
