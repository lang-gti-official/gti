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

struct BuildCachePolicy {
  // A project-owned cache root, normally <package>/build/gti/cache. The build
  // driver owns its versioned layout beneath this directory.
  std::filesystem::path root;
  // Stable root used to describe GTI source paths without embedding the
  // checkout's absolute location in content identities.
  std::filesystem::path sourceRoot;
  // Shipped compiler identity supplied by the frontend executable (currently
  // the GTI release version).
  std::string compilerIdentity;
  // Schema/policy identity for the project model that produced the effective
  // request. Raw manifest formatting and non-output metadata are excluded.
  std::string projectModelIdentity;
};

enum class BuildCacheStatus {
  NotConfigured,
  Hit,
  Miss,
  RecoveredCorruption,
  Bypassed,
};

// Parses a make-style dependency report written by a native compiler's
// -MD/-MF option: one rule whose prerequisites are every file the
// preprocessor opened. Handles backslash line continuations, escaped spaces,
// `$$`, and a Windows drive colon inside the rule target. Returns every
// prerequisite path in report order, or nullopt with a diagnostic when the
// report has no rule separator.
[[nodiscard]] std::optional<std::vector<std::filesystem::path>>
parseNativeDependencyFile(std::string_view contents, std::string &errorMessage);

// Classification of an exact native link-input file for cache identity. Only
// inputs whose bytes fully determine their contribution to the link are
// content-complete; a thin archive, linker script, or shared library can pull
// in members or transitive dependencies that its own content identity cannot
// record, so those classes must bypass the cache.
enum class NativeLinkInputClass {
  StaticArchive,
  RelocatableObject,
  RequiresDependencyDiscovery,
  Unreadable,
};

[[nodiscard]] NativeLinkInputClass
classifyNativeLinkInput(const std::filesystem::path &path);

// Deterministic preprocessing policy: __DATE__, __TIME__, and __TIMESTAMP__
// make object content depend on ambient build time, which no input content
// identity can record. The check is a conservative byte scan; a spelling in a
// comment or string bypasses the cache rather than risking a stale hit.
[[nodiscard]] bool containsTimeSensitivePreprocessorUse(std::string_view text);

struct BuildCacheResult {
  BuildCacheStatus status = BuildCacheStatus::NotConfigured;
  std::string key;
  std::filesystem::path entry;
  std::optional<std::string> detail;
  std::optional<std::string> warning;
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
      std::optional<std::string> cCompiler = std::nullopt,
      std::optional<BuildCachePolicy> cache = std::nullopt);

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
  [[nodiscard]] const std::optional<BuildCachePolicy> &cache() const;

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
  std::optional<BuildCachePolicy> buildCachePolicy;
};

enum class ExecutableBuildStatus {
  Success,
  CompilationFailure,
  OutputDirectoryFailure,
  GeneratedArtifactFailure,
  ToolchainConfigurationFailure,
  NativeCCompilerFailure,
  NativeCppCompilerFailure,
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

struct NativeCppCompilationResult {
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
  std::vector<NativeCppCompilationResult> cppCompilations;
  std::optional<NativeProcessResult> nativeProcess;
  std::vector<std::string> nativeCommand;
  std::filesystem::path generatedSource;
  std::optional<std::string> driverDiagnostic;
  BuildCacheResult cache;
  bool generatedSourceRetained = false;

  [[nodiscard]] bool succeeded() const {
    return status == ExecutableBuildStatus::Success;
  }
};

[[nodiscard]] ExecutableBuildResult
buildExecutable(const ExecutableBuildRequest &request);

} // namespace lang::driver
