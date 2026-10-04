#pragma once

#include "gti/driver/artifact.h"
#include "gti/driver/compilation.h"
#include "gti/driver/native_toolchain.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

struct ManagedOutputPolicy {
  std::filesystem::path trustedRoot;
  std::filesystem::path outputRoot;
};

// Validates an already-published managed executable before a build-skipping
// run. The complete parent chain and artifact must remain inside the trusted
// managed subtree without symbolic-link traversal, and the leaf must be a
// regular executable file. Returns a user-facing driver diagnostic on failure.
[[nodiscard]] std::optional<std::string>
managedExecutableDiagnostic(const ManagedOutputPolicy &policy,
                            const std::filesystem::path &executable);

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

enum class NativeLibraryResolutionStatus {
  // Exactly one modeled library file for the name exists in the first
  // declared search directory that holds any candidate for it.
  Resolved,
  // No declared search directory holds a candidate, so the name would resolve
  // through the linker's implicit system directories instead.
  Unresolved,
  // A directory holds more than one modeled candidate, or a spelling this
  // driver does not model. Which file the linker would select is a guess.
  Ambiguous,
  // The name is not a plain library name (a `-l:file` exact-name selection or
  // a path separator), whose meaning differs between linkers.
  UnsupportedSpelling,
  // A declared search directory cannot be enumerated, so absence of a
  // candidate cannot be established.
  DirectoryUnreadable,
};

struct NativeLibraryResolution {
  NativeLibraryResolutionStatus status =
      NativeLibraryResolutionStatus::Unresolved;
  // The resolved file, when the status is Resolved.
  std::filesystem::path file;
  // The directory that produced the status, when one did.
  std::filesystem::path directory;
};

// Resolves one `-l<name>` operand against the declared native library search
// directories, in the exact order the link command emits them. A declared
// `-L` directory precedes every implicit toolchain directory on the command
// line, so the first declared directory holding a candidate is the directory
// the linker would select from. Any directory holding more than one modeled
// candidate, or a spelling outside the modeled set, resolves ambiguously
// rather than guessing which file the linker would prefer.
[[nodiscard]] NativeLibraryResolution resolveNativeLibraryName(
    std::string_view name,
    const std::vector<std::filesystem::path> &searchDirectories);

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

// Schema 2 adds the produced artifact's content identity and the optional
// project section that `gti build --build-report` records. Schema 1 records
// carry neither, so verification does not interpret them.
inline constexpr int currentBuildProvenanceSchema = 2;

enum class CompilerSourceState {
  Clean,
  Dirty,
  Unavailable,
};

[[nodiscard]] std::string_view
compilerSourceStateName(CompilerSourceState state);

struct CompilerBuildIdentity {
  CompilerSourceState state = CompilerSourceState::Unavailable;
  std::optional<std::string> commit;
};

// Identity compiled into the driver at build time. A clean or dirty state has
// an exact source commit; unavailable means the build could not establish both
// facts and therefore cannot support exact-head evidence.
[[nodiscard]] CompilerBuildIdentity currentCompilerBuildIdentity();

struct BuildFileProvenance {
  std::filesystem::path canonicalPath;
  std::uintmax_t bytes = 0;
  std::string sha256;
};

struct BuildSourceUnitProvenance {
  SourceUnitId id = 0;
  SourceUnitRole role = SourceUnitRole::Application;
  BuildFileProvenance material;
  std::optional<std::string> logicalIdentity;
  std::optional<std::string> standardLibraryName;
  std::optional<std::string> packageIdentity;
  std::optional<std::string> packageRelativePath;
  bool entry = false;
  bool prelude = false;
};

struct BuildSourceDependencyProvenance {
  SourceUnitId source = 0;
  SourceUnitId target = 0;
  SourceDependencyKind kind = SourceDependencyKind::Include;
  std::string includeSpelling;
  std::size_t includeOccurrence = 0;
};

struct BuildSourceProvenance {
  SourceUnitId entryRoot = 0;
  std::vector<SourceUnitId> preludeRoots;
  std::vector<SourceUnitId> compilationOrder;
  std::vector<BuildSourceUnitProvenance> units;
  std::vector<BuildSourceDependencyProvenance> dependencies;
  std::string materialsSha256;
};

enum class BuildRuntimeHeaderKind {
  Runtime,
  Compatibility,
};

[[nodiscard]] std::string_view
buildRuntimeHeaderKindName(BuildRuntimeHeaderKind kind);

struct BuildRuntimeHeaderProvenance {
  BuildRuntimeHeaderKind kind = BuildRuntimeHeaderKind::Runtime;
  std::string logicalIdentity;
  BuildFileProvenance material;
};

struct BuildRuntimeProvenance {
  std::vector<BuildRuntimeHeaderProvenance> headers;
  BuildFileProvenance archive;
  std::string materialsSha256;
};

// Project-mode selection facts. Direct mode has no concept of them, so the
// section is absent from a direct record. `gti verify` re-resolves exactly
// this package, target, and profile before rebuilding, and compares the
// project model identity that produced the recorded request.
struct BuildProjectProvenance {
  std::string packageName;
  std::string packageVersion;
  std::string targetName;
  // "executable" or "test"; spelled as a string so the record stays
  // independent of the manifest model's enumeration.
  std::string targetKind;
  std::string profileName;
  // Normalized GTI triple of the selected target. `gti verify` re-resolves
  // the recorded build from this rather than assuming the build host, so a
  // cross build stays verifiable.
  std::string targetTriple;
  std::string modelIdentity;
};

struct BuildProvenance {
  int schema = currentBuildProvenanceSchema;
  // Additive schema-2 policy field. Older reports imply false.
  bool debugInfo = false;
  BuildFileProvenance producerExecutable;
  CompilerBuildIdentity compilerBuild;
  std::optional<BuildProjectProvenance> project;
  BuildSourceProvenance source;
  BuildFileProvenance nativeCompilerExecutable;
  std::vector<std::string> nativeCommand;
  BuildRuntimeProvenance runtime;
  // Content identity of the published executable. Captured after the native
  // link and publication, so it describes the exact bytes this invocation
  // produced.
  BuildFileProvenance artifact;
};

struct BuildReportRequest {
  std::filesystem::path output;
  std::filesystem::path producerExecutable;
  // Supplied by project commands; direct compilation leaves it empty.
  std::optional<BuildProjectProvenance> project;
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
      std::optional<BuildCachePolicy> cache = std::nullopt,
      std::optional<BuildReportRequest> buildReport = std::nullopt);

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
  [[nodiscard]] const std::optional<BuildReportRequest> &buildReport() const;

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
  std::optional<BuildReportRequest> requestedBuildReport;
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
  BuildReportCaptureFailure,
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
  std::optional<BuildProvenance> provenance;
  bool generatedSourceRetained = false;

  [[nodiscard]] bool succeeded() const {
    return status == ExecutableBuildStatus::Success;
  }
};

[[nodiscard]] ExecutableBuildResult
buildExecutable(const ExecutableBuildRequest &request);

} // namespace lang::driver
