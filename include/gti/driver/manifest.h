#pragma once

#include "gti/cpp_emitter.h"
#include "gti/diagnostic.h"
#include "gti/optimizer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

inline constexpr int currentManifestVersion = 1;

struct ProjectPackage {
  std::string name;
  std::string version;
};

struct ProjectTarget {
  std::string name;
  std::filesystem::path root;
  SourceSpan declaration;
};

struct ProjectProfile {
  std::string name;
  OptimizationLevel optimization = OptimizationLevel::O0;
  CppStandard cppStandard = CppStandard::Cpp23;
  bool keepCpp = false;
  SourceSpan declaration;
};

class ProjectManifest final {
public:
  ProjectManifest(std::filesystem::path path, ProjectPackage package,
                  std::vector<ProjectTarget> targets,
                  std::vector<ProjectProfile> profiles);

  [[nodiscard]] const std::filesystem::path &path() const;
  [[nodiscard]] const std::filesystem::path &packageRoot() const;
  [[nodiscard]] const ProjectPackage &package() const;
  [[nodiscard]] const std::vector<ProjectTarget> &targets() const;
  [[nodiscard]] const std::vector<ProjectProfile> &profiles() const;
  [[nodiscard]] const ProjectTarget *findTarget(std::string_view name) const;
  [[nodiscard]] const ProjectProfile *findProfile(std::string_view name) const;

private:
  std::filesystem::path manifestPath;
  std::filesystem::path rootPath;
  ProjectPackage packageIdentity;
  std::vector<ProjectTarget> executableTargets;
  std::vector<ProjectProfile> buildProfiles;
};

enum class ManifestDiscoveryStatus {
  Found,
  NotFound,
  FilesystemFailure,
};

struct ManifestDiscoveryResult {
  ManifestDiscoveryStatus status = ManifestDiscoveryStatus::NotFound;
  std::optional<std::filesystem::path> path;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ManifestDiscoveryStatus::Found && path.has_value();
  }
};

enum class ManifestLoadStatus {
  Success,
  IoFailure,
  ParseFailure,
  ValidationFailure,
};

struct ManifestLoadResult {
  ManifestLoadStatus status = ManifestLoadStatus::ValidationFailure;
  std::optional<ProjectManifest> manifest;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ManifestLoadStatus::Success && manifest.has_value();
  }
};

[[nodiscard]] ManifestDiscoveryResult
discoverProjectManifest(const std::filesystem::path &startDirectory);

[[nodiscard]] ManifestLoadResult
loadProjectManifest(const std::filesystem::path &manifestPath);

} // namespace lang::driver
