#pragma once

#include "gti/driver/compilation.h"
#include "gti/driver/workspace.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lang::driver {

struct ProjectBuildOverrides {
  std::optional<OptimizationLevel> optimization;
  std::optional<CppStandard> cppStandard;
  std::optional<ExecutionProfile> executionProfile;
  std::optional<RawPointerAuditMode> rawPointerAudit;
  std::optional<bool> keepCpp;
  std::optional<bool> debugInfo;
  ConfigurationFlags configurationFlags;
  WarningPolicy warningPolicy;
};

// Native compiler selections supplied by the caller rather than the
// manifest. The compiler strings stay with the invocation layer; resolution
// needs only their availability to enforce fail-closed cross-toolchain rules.
struct ProjectToolchainOverrides {
  bool cCompiler = false;
};

// How project resolution treats a selected target that is not the build
// host.
enum class ToolchainSelectionPolicy {
  // Native compiler discovery keeps its existing host order. This is the
  // compiler-library contract: a caller that assembles a TargetInfo to
  // analyze code for another target is not asking the driver to configure a
  // native toolchain, and `gti check` performs no native compilation.
  HostTools,
  // A target that is not the build host must resolve a declared
  // `[toolchains.<triple>]` table. Project commands that produce a native
  // artifact select this so a cross build can never silently link host
  // tools and publish a wrong binary.
  DeclaredForNonHostTarget,
};

// The native toolchain a plan builds with. `declared()` is false for a host
// build, where native compiler discovery keeps its existing
// `--cxx`/`GTI_CXX`/`CXX` order.
struct ResolvedProjectToolchain {
  // The manifest's declared triple spelling, empty for a host build.
  std::string triple;
  std::string cxx;
  std::string cc;
  std::filesystem::path sysroot;
  // The sysroot exactly as spelled in the manifest.
  std::string declaredSysroot;
  // The native toolchain's own triple spelling, emitted as `--target=`.
  std::string nativeTarget;
  // The `[toolchains.<triple>]` key span, for diagnostics anchored at the
  // declaration rather than the manifest as a whole.
  SourceSpan declaration;

  [[nodiscard]] bool declared() const { return !triple.empty(); }
};

class ProjectBuildRequest final {
public:
  ProjectBuildRequest(std::filesystem::path startDirectory,
                      std::optional<std::string> targetName,
                      std::string profileName, TargetInfo target,
                      ProjectBuildOverrides overrides = {},
                      std::optional<std::string> packageName = std::nullopt,
                      WorkspaceDependencyPolicy dependencyPolicy = {},
                      RuntimeProfile runtimeProfile = RuntimeProfile::None,
                      ToolchainSelectionPolicy toolchainPolicy =
                          ToolchainSelectionPolicy::HostTools,
                      ProjectToolchainOverrides toolchainOverrides = {},
                      ManifestFeaturePolicy manifestFeatures = {});

  [[nodiscard]] const std::filesystem::path &startDirectory() const;
  [[nodiscard]] const std::optional<std::string> &targetName() const;
  [[nodiscard]] const std::string &profileName() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] const ProjectBuildOverrides &overrides() const;
  [[nodiscard]] const std::optional<std::string> &packageName() const;
  [[nodiscard]] const WorkspaceDependencyPolicy &dependencyPolicy() const;
  [[nodiscard]] RuntimeProfile runtimeProfile() const;
  [[nodiscard]] ToolchainSelectionPolicy toolchainPolicy() const;
  [[nodiscard]] const ProjectToolchainOverrides &toolchainOverrides() const;
  [[nodiscard]] ManifestFeaturePolicy manifestFeatures() const;

private:
  std::filesystem::path discoveryStart;
  std::optional<std::string> selectedTarget;
  std::string selectedProfile;
  TargetInfo targetInfo;
  ProjectBuildOverrides cliOverrides;
  std::optional<std::string> selectedPackage;
  WorkspaceDependencyPolicy workspaceDependencyPolicy;
  RuntimeProfile selectedRuntimeProfile;
  ToolchainSelectionPolicy selectedToolchainPolicy;
  ProjectToolchainOverrides nativeToolchainOverrides;
  ManifestFeaturePolicy enabledManifestFeatures;
};

// One resolved bindgen import carried by a build plan: every value the
// generation step needs, fixed before frontend entry. `outputDirectory` is
// the synthetic package source root whose `bindings.gti` the plan's source
// graph resolves through the alias.
struct BindgenImportPlan {
  std::string packageName;
  std::string alias;
  std::filesystem::path header;
  std::string declaredHeader;
  std::string namespaceName;
  std::vector<std::filesystem::path> includeDirectories;
  std::vector<std::string> macroArguments;
  // Opt-in import policy (C-07), sorted by the manifest loader.
  std::vector<std::string> allowPatterns;
  std::vector<std::string> denyPatterns;
  std::vector<std::pair<std::string, std::string>> renames;
  std::vector<std::pair<std::string, std::string>> arrayReturns;
  std::filesystem::path outputDirectory;
  std::filesystem::path manifestPath;
  SourceSpan declaration;
};

// One approved host build program, resolved before frontend entry but not run
// by metadata/check/fetch operations. Dependency scripts are ordered
// dependency-first so generated native artifacts exist before dependents run.
struct BuildScriptPlan {
  std::string packageIdentity;
  std::filesystem::path packageRoot;
  std::vector<std::string> command;
  std::optional<std::filesystem::path> source;
  std::vector<std::filesystem::path> inputs;
  std::vector<std::string> declaredInputs;
  std::filesystem::path outputDirectory;
  SourceSpan declaration;
  bool selectedPackage = false;
};

class ProjectBuildPlan final {
public:
  ProjectBuildPlan(
      std::filesystem::path manifestPath, std::filesystem::path packageRoot,
      std::filesystem::path workspaceRoot, std::string packageName,
      std::string packageVersion, std::string targetName,
      ProjectTargetKind targetKind, std::string profileName,
      std::filesystem::path entry, std::filesystem::path output,
      std::filesystem::path generatedSource, TargetInfo target,
      OptimizationLevel optimization, CppStandard cppStandard, bool keepCpp,
      NativeInputs nativeInputs,
      std::vector<PackageSourceRoot> packageSourceRoots,
      ConfigurationFlags configurationFlags, WarningPolicy warningPolicy,
      std::string projectModelIdentity,
      std::vector<BindgenImportPlan> bindgenImports = {},
      std::vector<BuildScriptPlan> buildScripts = {},
      ResolvedProjectToolchain toolchain = {}, bool debugInfo = false,
      PackageWarningPolicy packageWarnings = {},
      RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off,
      ProjectTargetLinkage targetLinkage = ProjectTargetLinkage::None,
      std::vector<ProjectExport> exports = {});

  [[nodiscard]] const std::filesystem::path &manifestPath() const;
  [[nodiscard]] const std::filesystem::path &packageRoot() const;
  [[nodiscard]] const std::filesystem::path &workspaceRoot() const;
  [[nodiscard]] const std::string &packageName() const;
  [[nodiscard]] const std::string &packageVersion() const;
  [[nodiscard]] const std::string &targetName() const;
  [[nodiscard]] ProjectTargetKind targetKind() const;
  [[nodiscard]] ProjectTargetLinkage targetLinkage() const;
  [[nodiscard]] const std::vector<ProjectExport> &exports() const;
  [[nodiscard]] ProgramRequest programRequest() const;
  [[nodiscard]] const std::string &profileName() const;
  [[nodiscard]] const std::filesystem::path &entry() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] const std::filesystem::path &generatedSource() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] CppStandard cppStandard() const;
  [[nodiscard]] bool keepCpp() const;
  [[nodiscard]] bool debugInfo() const;
  [[nodiscard]] RawPointerAuditMode rawPointerAudit() const;
  [[nodiscard]] const NativeInputs &nativeInputs() const;
  [[nodiscard]] const std::vector<PackageSourceRoot> &packageSources() const;
  [[nodiscard]] const ConfigurationFlags &configurationFlags() const;
  [[nodiscard]] const WarningPolicy &warnings() const;
  [[nodiscard]] const PackageWarningPolicy &packageWarnings() const;
  [[nodiscard]] const std::string &projectModelIdentity() const;
  [[nodiscard]] const std::vector<BindgenImportPlan> &bindgenImports() const;
  [[nodiscard]] const std::vector<BuildScriptPlan> &buildScripts() const;
  [[nodiscard]] const ResolvedProjectToolchain &toolchain() const;

private:
  std::filesystem::path projectManifestPath;
  std::filesystem::path projectRoot;
  std::filesystem::path projectWorkspaceRoot;
  std::string projectPackageName;
  std::string projectPackageVersion;
  std::string projectTargetName;
  ProjectTargetKind projectTargetKind;
  ProjectTargetLinkage projectTargetLinkage;
  std::string buildProfileName;
  std::filesystem::path entryPath;
  std::filesystem::path outputPath;
  std::filesystem::path generatedSourcePath;
  TargetInfo targetInfo;
  OptimizationLevel optimizationLevel;
  CppStandard backendStandard;
  bool retainGeneratedSource;
  NativeInputs resolvedNativeInputs;
  std::vector<PackageSourceRoot> resolvedPackageSources;
  ConfigurationFlags resolvedConfigurationFlags;
  WarningPolicy resolvedWarningPolicy;
  PackageWarningPolicy resolvedPackageWarningPolicy;
  std::string resolvedProjectModelIdentity;
  std::vector<BindgenImportPlan> resolvedBindgenImports;
  std::vector<BuildScriptPlan> resolvedBuildScripts;
  ResolvedProjectToolchain resolvedToolchain;
  std::vector<ProjectExport> resolvedExports;
  bool sourceDebugInfo = false;
  RawPointerAuditMode rawPointerAuditMode = RawPointerAuditMode::Off;
};

class ProjectMetadata final {
public:
  ProjectMetadata(ProjectWorkspace workspace, TargetInfo target,
                  std::vector<ProjectBuildPlan> plans);

  [[nodiscard]] const ProjectManifest &manifest() const;
  [[nodiscard]] const ProjectWorkspace &workspace() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] const std::vector<ProjectBuildPlan> &plans() const;

private:
  ProjectWorkspace projectWorkspace;
  TargetInfo targetInfo;
  std::vector<ProjectBuildPlan> buildPlans;
};

enum class ProjectResolutionStatus {
  Success,
  DiscoveryFailure,
  ManifestFailure,
  GraphFailure,
  SelectionFailure,
};

struct ProjectResolutionResult {
  ProjectResolutionStatus status = ProjectResolutionStatus::DiscoveryFailure;
  std::optional<ProjectBuildPlan> plan;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectResolutionStatus::Success && plan.has_value();
  }
};

struct ProjectTestResolutionResult {
  ProjectResolutionStatus status = ProjectResolutionStatus::DiscoveryFailure;
  std::vector<ProjectBuildPlan> plans;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectResolutionStatus::Success && !plans.empty();
  }
};

struct ProjectTargetSetResolutionResult {
  ProjectResolutionStatus status = ProjectResolutionStatus::DiscoveryFailure;
  std::vector<ProjectBuildPlan> plans;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectResolutionStatus::Success && !plans.empty();
  }
};

enum class ProjectMetadataStatus {
  Success,
  DiscoveryFailure,
  ManifestFailure,
  GraphFailure,
  SelectionFailure,
};

struct ProjectMetadataResult {
  ProjectMetadataStatus status = ProjectMetadataStatus::DiscoveryFailure;
  std::optional<ProjectMetadata> metadata;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectMetadataStatus::Success && metadata.has_value();
  }
};

enum class ProjectCleanStatus {
  Success,
  DiscoveryFailure,
  UnsafePath,
  FilesystemFailure,
};

struct ProjectCleanResult {
  ProjectCleanStatus status = ProjectCleanStatus::DiscoveryFailure;
  std::filesystem::path buildRoot;
  std::uintmax_t removedEntries = 0;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectCleanStatus::Success;
  }
};

enum class ProjectScaffoldMode {
  NewPackage,
  ExistingDirectory,
};

struct ProjectScaffoldGit final {
  std::string executable;
};

class ProjectScaffoldRequest final {
public:
  ProjectScaffoldRequest(ProjectScaffoldMode mode,
                         std::filesystem::path destination,
                         std::optional<std::string> packageName = std::nullopt,
                         std::optional<ProjectScaffoldGit> git = std::nullopt);

  [[nodiscard]] ProjectScaffoldMode mode() const;
  [[nodiscard]] const std::filesystem::path &destination() const;
  [[nodiscard]] const std::optional<std::string> &packageName() const;
  [[nodiscard]] const std::optional<ProjectScaffoldGit> &git() const;

private:
  ProjectScaffoldMode scaffoldMode;
  std::filesystem::path destinationPath;
  std::optional<std::string> requestedPackageName;
  std::optional<ProjectScaffoldGit> requestedGit;
};

enum class ProjectScaffoldStatus {
  Success,
  InvalidRequest,
  Conflict,
  FilesystemFailure,
  ToolFailure,
};

struct ProjectScaffoldResult {
  ProjectScaffoldStatus status = ProjectScaffoldStatus::InvalidRequest;
  std::filesystem::path packageRoot;
  std::string packageName;
  bool createdSource = false;
  bool initializedGit = false;
  bool createdGitIgnore = false;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectScaffoldStatus::Success;
  }
};

enum class FormatConfigScaffoldStatus {
  Success,
  InvalidRequest,
  Conflict,
  FilesystemFailure,
};

struct FormatConfigScaffoldResult {
  FormatConfigScaffoldStatus status =
      FormatConfigScaffoldStatus::InvalidRequest;
  std::filesystem::path configPath;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == FormatConfigScaffoldStatus::Success;
  }
};

[[nodiscard]] std::string targetTriple(const TargetInfo &target);

[[nodiscard]] ProjectResolutionResult
resolveProjectBuild(const ProjectBuildRequest &request);

[[nodiscard]] ProjectTestResolutionResult
resolveProjectTests(const ProjectBuildRequest &request);

// Resolves one build plan for every enabled declared target of
// the selected package, in the manifest's deterministic target-name order.
// The request must not name a single target.
[[nodiscard]] ProjectTargetSetResolutionResult
resolveAllProjectTargets(const ProjectBuildRequest &request);

[[nodiscard]] ProjectMetadataResult
resolveProjectMetadata(const std::filesystem::path &startDirectory,
                       TargetInfo target,
                       std::optional<std::string> packageName = std::nullopt,
                       RuntimeProfile runtimeProfile = RuntimeProfile::None,
                       ManifestFeaturePolicy manifestFeatures = {});

[[nodiscard]] ProjectCleanResult
cleanProject(const std::filesystem::path &startDirectory);

[[nodiscard]] ProjectScaffoldResult
scaffoldProject(const ProjectScaffoldRequest &request);

[[nodiscard]] FormatConfigScaffoldResult
scaffoldFormatConfig(const std::filesystem::path &destinationDirectory);

} // namespace lang::driver
