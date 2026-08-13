#pragma once

#include "gti/driver/manifest.h"
#include "gti/target.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lang::driver {

struct ProjectBuildOverrides {
  std::optional<OptimizationLevel> optimization;
  std::optional<CppStandard> cppStandard;
  std::optional<ExecutionProfile> executionProfile;
  std::optional<bool> keepCpp;
};

class ProjectBuildRequest final {
public:
  ProjectBuildRequest(std::filesystem::path startDirectory,
                      std::optional<std::string> targetName,
                      std::string profileName, TargetInfo target,
                      ProjectBuildOverrides overrides = {});

  [[nodiscard]] const std::filesystem::path &startDirectory() const;
  [[nodiscard]] const std::optional<std::string> &targetName() const;
  [[nodiscard]] const std::string &profileName() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] const ProjectBuildOverrides &overrides() const;

private:
  std::filesystem::path discoveryStart;
  std::optional<std::string> selectedTarget;
  std::string selectedProfile;
  TargetInfo targetInfo;
  ProjectBuildOverrides cliOverrides;
};

class ProjectBuildPlan final {
public:
  ProjectBuildPlan(std::filesystem::path manifestPath,
                   std::filesystem::path packageRoot, std::string packageName,
                   std::string targetName, ProjectTargetKind targetKind,
                   std::string profileName, std::filesystem::path entry,
                   std::filesystem::path output,
                   std::filesystem::path generatedSource, TargetInfo target,
                   OptimizationLevel optimization, CppStandard cppStandard,
                   bool keepCpp, NativeInputs nativeInputs);

  [[nodiscard]] const std::filesystem::path &manifestPath() const;
  [[nodiscard]] const std::filesystem::path &packageRoot() const;
  [[nodiscard]] const std::string &packageName() const;
  [[nodiscard]] const std::string &targetName() const;
  [[nodiscard]] ProjectTargetKind targetKind() const;
  [[nodiscard]] const std::string &profileName() const;
  [[nodiscard]] const std::filesystem::path &entry() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] const std::filesystem::path &generatedSource() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] CppStandard cppStandard() const;
  [[nodiscard]] bool keepCpp() const;
  [[nodiscard]] const NativeInputs &nativeInputs() const;

private:
  std::filesystem::path projectManifestPath;
  std::filesystem::path projectRoot;
  std::string projectPackageName;
  std::string projectTargetName;
  ProjectTargetKind projectTargetKind;
  std::string buildProfileName;
  std::filesystem::path entryPath;
  std::filesystem::path outputPath;
  std::filesystem::path generatedSourcePath;
  TargetInfo targetInfo;
  OptimizationLevel optimizationLevel;
  CppStandard backendStandard;
  bool retainGeneratedSource;
  NativeInputs resolvedNativeInputs;
};

class ProjectMetadata final {
public:
  ProjectMetadata(ProjectManifest manifest, TargetInfo target,
                  std::vector<ProjectBuildPlan> plans);

  [[nodiscard]] const ProjectManifest &manifest() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] const std::vector<ProjectBuildPlan> &plans() const;

private:
  ProjectManifest projectManifest;
  TargetInfo targetInfo;
  std::vector<ProjectBuildPlan> buildPlans;
};

enum class ProjectResolutionStatus {
  Success,
  DiscoveryFailure,
  ManifestFailure,
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

enum class ProjectMetadataStatus {
  Success,
  DiscoveryFailure,
  ManifestFailure,
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

class ProjectScaffoldRequest final {
public:
  ProjectScaffoldRequest(ProjectScaffoldMode mode,
                         std::filesystem::path destination,
                         std::optional<std::string> packageName = std::nullopt);

  [[nodiscard]] ProjectScaffoldMode mode() const;
  [[nodiscard]] const std::filesystem::path &destination() const;
  [[nodiscard]] const std::optional<std::string> &packageName() const;

private:
  ProjectScaffoldMode scaffoldMode;
  std::filesystem::path destinationPath;
  std::optional<std::string> requestedPackageName;
};

enum class ProjectScaffoldStatus {
  Success,
  InvalidRequest,
  Conflict,
  FilesystemFailure,
};

struct ProjectScaffoldResult {
  ProjectScaffoldStatus status = ProjectScaffoldStatus::InvalidRequest;
  std::filesystem::path packageRoot;
  std::string packageName;
  bool createdSource = false;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectScaffoldStatus::Success;
  }
};

[[nodiscard]] std::string targetTriple(const TargetInfo &target);

[[nodiscard]] ProjectResolutionResult
resolveProjectBuild(const ProjectBuildRequest &request);

[[nodiscard]] ProjectTestResolutionResult
resolveProjectTests(const ProjectBuildRequest &request);

[[nodiscard]] ProjectMetadataResult
resolveProjectMetadata(const std::filesystem::path &startDirectory,
                       TargetInfo target);

[[nodiscard]] ProjectCleanResult
cleanProject(const std::filesystem::path &startDirectory);

[[nodiscard]] ProjectScaffoldResult
scaffoldProject(const ProjectScaffoldRequest &request);

} // namespace lang::driver
