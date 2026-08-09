#pragma once

#include "gti/driver/manifest.h"
#include "gti/target.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lang::driver {

struct ProjectBuildOverrides {
  std::optional<OptimizationLevel> optimization;
  std::optional<CppStandard> cppStandard;
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
                   std::string targetName, std::string profileName,
                   std::filesystem::path entry, std::filesystem::path output,
                   std::filesystem::path generatedSource, TargetInfo target,
                   OptimizationLevel optimization, CppStandard cppStandard,
                   bool keepCpp);

  [[nodiscard]] const std::filesystem::path &manifestPath() const;
  [[nodiscard]] const std::filesystem::path &packageRoot() const;
  [[nodiscard]] const std::string &packageName() const;
  [[nodiscard]] const std::string &targetName() const;
  [[nodiscard]] const std::string &profileName() const;
  [[nodiscard]] const std::filesystem::path &entry() const;
  [[nodiscard]] const std::filesystem::path &output() const;
  [[nodiscard]] const std::filesystem::path &generatedSource() const;
  [[nodiscard]] const TargetInfo &target() const;
  [[nodiscard]] OptimizationLevel optimization() const;
  [[nodiscard]] CppStandard cppStandard() const;
  [[nodiscard]] bool keepCpp() const;

private:
  std::filesystem::path projectManifestPath;
  std::filesystem::path projectRoot;
  std::string projectPackageName;
  std::string executableTargetName;
  std::string buildProfileName;
  std::filesystem::path entryPath;
  std::filesystem::path outputPath;
  std::filesystem::path generatedSourcePath;
  TargetInfo targetInfo;
  OptimizationLevel optimizationLevel;
  CppStandard backendStandard;
  bool retainGeneratedSource;
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

[[nodiscard]] std::string targetTriple(const TargetInfo &target);

[[nodiscard]] ProjectResolutionResult
resolveProjectBuild(const ProjectBuildRequest &request);

} // namespace lang::driver
