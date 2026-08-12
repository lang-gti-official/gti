#pragma once

#include "gti/cpp_emitter.h"
#include "gti/diagnostic.h"
#include "gti/driver/native_toolchain.h"
#include "gti/optimizer.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

inline constexpr int currentManifestVersion = 1;

[[nodiscard]] bool isPortableProjectName(std::string_view name);

struct ProjectNativePlatform {
  std::optional<std::string> os;
  std::optional<std::string> vendor;
  std::optional<std::string> arch;
  NativeInputs inputs;
  std::vector<SourceSpan> includeDirectoryDeclarations;
  std::vector<SourceSpan> cSourceDeclarations;
  std::vector<SourceSpan> libraryDirectoryDeclarations;
  std::vector<SourceSpan> libraryFileDeclarations;
  std::vector<SourceSpan> frameworkDeclarations;
  SourceSpan declaration;
};

struct ProjectNativeSettings {
  NativeInputs inputs;
  std::vector<SourceSpan> includeDirectoryDeclarations;
  std::vector<SourceSpan> cSourceDeclarations;
  std::vector<SourceSpan> libraryDirectoryDeclarations;
  std::vector<SourceSpan> libraryFileDeclarations;
  std::vector<SourceSpan> frameworkDeclarations;
  SourceSpan declaration;
  std::vector<ProjectNativePlatform> platforms;
};

struct ProjectPackage {
  std::string name;
  std::string version;
  ProjectNativeSettings native;
};

struct ProjectTarget {
  std::string name;
  std::filesystem::path root;
  SourceSpan declaration;
  ProjectNativeSettings native;
};

struct ProjectProfile {
  std::string name;
  OptimizationLevel optimization = OptimizationLevel::O0;
  CppStandard cppStandard = CppStandard::Cpp23;
  bool keepCpp = false;
  SourceSpan declaration;
  ProjectNativeSettings native;
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
