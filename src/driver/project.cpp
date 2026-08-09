#include "gti/driver/project.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang::driver {
namespace {

Diagnostic projectDiagnostic(std::string code, SourceSpan span,
                             std::string message) {
  return makeDiagnostic(std::move(code), DiagnosticPhase::Driver,
                        std::move(span), std::move(message));
}

std::size_t editDistance(std::string_view left, std::string_view right) {
  std::vector<std::size_t> previous(right.size() + 1);
  std::vector<std::size_t> current(right.size() + 1);
  for (std::size_t index = 0; index <= right.size(); ++index) {
    previous[index] = index;
  }
  for (std::size_t leftIndex = 0; leftIndex < left.size(); ++leftIndex) {
    current[0] = leftIndex + 1;
    for (std::size_t rightIndex = 0; rightIndex < right.size(); ++rightIndex) {
      current[rightIndex + 1] =
          std::min({current[rightIndex] + 1, previous[rightIndex + 1] + 1,
                    previous[rightIndex] +
                        (left[leftIndex] == right[rightIndex] ? 0U : 1U)});
    }
    previous.swap(current);
  }
  return previous.back();
}

template <typename Value, typename Name>
std::optional<std::string_view> nearestName(std::string_view requested,
                                            const std::vector<Value> &values,
                                            Name name) {
  std::optional<std::string_view> nearest;
  std::size_t distance = std::numeric_limits<std::size_t>::max();
  for (const Value &value : values) {
    const std::string_view candidate = name(value);
    const std::size_t candidateDistance = editDistance(requested, candidate);
    if (candidateDistance < distance) {
      nearest = candidate;
      distance = candidateDistance;
    }
  }
  return distance <= 3 ? nearest : std::nullopt;
}

SourceSpan manifestSpan(const ProjectManifest &manifest) {
  if (!manifest.targets().empty()) {
    return manifest.targets().front().declaration;
  }
  return {manifest.path().string(), 0, 1, 1};
}

std::string outputComponent(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    const bool alphabetic = (character >= 'A' && character <= 'Z') ||
                            (character >= 'a' && character <= 'z');
    const bool digit = character >= '0' && character <= '9';
    result.push_back(alphabetic || digit || character == '_' || character == '-'
                         ? character
                         : '_');
  }
  return result.empty() ? "unknown" : result;
}

bool pathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate) {
  auto rootPart = root.begin();
  auto candidatePart = candidate.begin();
  for (; rootPart != root.end() && candidatePart != candidate.end();
       ++rootPart, ++candidatePart) {
    if (*rootPart != *candidatePart) {
      return false;
    }
  }
  return rootPart == root.end();
}

ProjectBuildPlan makeBuildPlan(const ProjectManifest &manifest,
                               const ProjectTarget &selectedTarget,
                               const ProjectProfile &selectedProfile,
                               const TargetInfo &target,
                               const ProjectBuildOverrides &overrides = {}) {
  const OptimizationLevel optimization =
      overrides.optimization.value_or(selectedProfile.optimization);
  const CppStandard cppStandard =
      overrides.cppStandard.value_or(selectedProfile.cppStandard);
  const bool keepCpp = overrides.keepCpp.value_or(selectedProfile.keepCpp);
  const std::filesystem::path outputDirectory =
      manifest.packageRoot() / "build" / "gti" / selectedProfile.name /
      targetTriple(target);
  std::string executableName = selectedTarget.name;
#if defined(_WIN32)
  executableName += ".exe";
#endif
  const std::filesystem::path output = outputDirectory / executableName;
  const std::filesystem::path generatedSource =
      outputDirectory / "intermediate" / (selectedTarget.name + ".gti.cpp");
  return ProjectBuildPlan(
      manifest.path(), manifest.packageRoot(), manifest.package().name,
      selectedTarget.name, selectedProfile.name, selectedTarget.root, output,
      generatedSource, target, optimization, cppStandard, keepCpp);
}

Diagnostic cleanDiagnostic(const std::filesystem::path &manifest,
                           std::string code, std::string message) {
  return projectDiagnostic(std::move(code), {manifest.string(), 0, 1, 1},
                           std::move(message));
}

} // namespace

ProjectBuildRequest::ProjectBuildRequest(std::filesystem::path startDirectory,
                                         std::optional<std::string> targetName,
                                         std::string profileName,
                                         TargetInfo target,
                                         ProjectBuildOverrides overrides)
    : discoveryStart(std::move(startDirectory)),
      selectedTarget(std::move(targetName)),
      selectedProfile(std::move(profileName)), targetInfo(std::move(target)),
      cliOverrides(std::move(overrides)) {}

const std::filesystem::path &ProjectBuildRequest::startDirectory() const {
  return discoveryStart;
}

const std::optional<std::string> &ProjectBuildRequest::targetName() const {
  return selectedTarget;
}

const std::string &ProjectBuildRequest::profileName() const {
  return selectedProfile;
}

const TargetInfo &ProjectBuildRequest::target() const { return targetInfo; }

const ProjectBuildOverrides &ProjectBuildRequest::overrides() const {
  return cliOverrides;
}

ProjectBuildPlan::ProjectBuildPlan(
    std::filesystem::path manifestPath, std::filesystem::path packageRoot,
    std::string packageName, std::string targetName, std::string profileName,
    std::filesystem::path entry, std::filesystem::path output,
    std::filesystem::path generatedSource, TargetInfo target,
    OptimizationLevel optimization, CppStandard cppStandard, bool keepCpp)
    : projectManifestPath(std::move(manifestPath)),
      projectRoot(std::move(packageRoot)),
      projectPackageName(std::move(packageName)),
      executableTargetName(std::move(targetName)),
      buildProfileName(std::move(profileName)), entryPath(std::move(entry)),
      outputPath(std::move(output)),
      generatedSourcePath(std::move(generatedSource)),
      targetInfo(std::move(target)), optimizationLevel(optimization),
      backendStandard(cppStandard), retainGeneratedSource(keepCpp) {}

const std::filesystem::path &ProjectBuildPlan::manifestPath() const {
  return projectManifestPath;
}

const std::filesystem::path &ProjectBuildPlan::packageRoot() const {
  return projectRoot;
}

const std::string &ProjectBuildPlan::packageName() const {
  return projectPackageName;
}

const std::string &ProjectBuildPlan::targetName() const {
  return executableTargetName;
}

const std::string &ProjectBuildPlan::profileName() const {
  return buildProfileName;
}

const std::filesystem::path &ProjectBuildPlan::entry() const {
  return entryPath;
}

const std::filesystem::path &ProjectBuildPlan::output() const {
  return outputPath;
}

const std::filesystem::path &ProjectBuildPlan::generatedSource() const {
  return generatedSourcePath;
}

const TargetInfo &ProjectBuildPlan::target() const { return targetInfo; }

OptimizationLevel ProjectBuildPlan::optimization() const {
  return optimizationLevel;
}

CppStandard ProjectBuildPlan::cppStandard() const { return backendStandard; }

bool ProjectBuildPlan::keepCpp() const { return retainGeneratedSource; }

ProjectMetadata::ProjectMetadata(ProjectManifest manifest, TargetInfo target,
                                 std::vector<ProjectBuildPlan> plans)
    : projectManifest(std::move(manifest)), targetInfo(std::move(target)),
      buildPlans(std::move(plans)) {}

const ProjectManifest &ProjectMetadata::manifest() const {
  return projectManifest;
}

const TargetInfo &ProjectMetadata::target() const { return targetInfo; }

const std::vector<ProjectBuildPlan> &ProjectMetadata::plans() const {
  return buildPlans;
}

std::string targetTriple(const TargetInfo &target) {
  return outputComponent(target.arch) + "-" + outputComponent(target.vendor) +
         "-" + outputComponent(target.os);
}

ProjectResolutionResult
resolveProjectBuild(const ProjectBuildRequest &request) {
  ProjectResolutionResult result;
  ManifestDiscoveryResult discovery =
      discoverProjectManifest(request.startDirectory());
  if (!discovery.succeeded()) {
    result.status = ProjectResolutionStatus::DiscoveryFailure;
    result.diagnostics = std::move(discovery.diagnostics);
    return result;
  }

  ManifestLoadResult loaded = loadProjectManifest(*discovery.path);
  result.sources = std::move(loaded.sources);
  if (!loaded.succeeded()) {
    result.status = ProjectResolutionStatus::ManifestFailure;
    result.diagnostics = std::move(loaded.diagnostics);
    return result;
  }

  const ProjectManifest &manifest = *loaded.manifest;
  const ProjectTarget *selectedTarget = nullptr;
  if (request.targetName()) {
    selectedTarget = manifest.findTarget(*request.targetName());
    if (selectedTarget == nullptr) {
      Diagnostic diagnostic =
          projectDiagnostic("GTI-B1200", manifestSpan(manifest),
                            "Unknown target '" + *request.targetName() + "'.");
      if (const std::optional<std::string_view> nearest =
              nearestName(*request.targetName(), manifest.targets(),
                          [](const ProjectTarget &target) -> std::string_view {
                            return target.name;
                          })) {
        diagnostic.hints.push_back("Did you mean '" + std::string(*nearest) +
                                   "'?");
      } else if (manifest.findProfile(*request.targetName()) != nullptr) {
        diagnostic.hints.push_back(
            "'" + *request.targetName() +
            "' is a profile; select it with --profile " +
            *request.targetName() +
            (*request.targetName() == "release" ? " or --release." : "."));
      }
      result.diagnostics.push_back(std::move(diagnostic));
    }
  } else if (manifest.targets().size() == 1) {
    selectedTarget = &manifest.targets().front();
  } else {
    Diagnostic diagnostic = projectDiagnostic(
        "GTI-B1201", manifestSpan(manifest),
        "The manifest declares multiple targets; select one explicitly.");
    std::string names;
    for (const ProjectTarget &target : manifest.targets()) {
      if (!names.empty()) {
        names += ", ";
      }
      names += target.name;
    }
    diagnostic.hints.push_back("Available targets: " + names + ".");
    result.diagnostics.push_back(std::move(diagnostic));
  }

  const ProjectProfile *selectedProfile =
      manifest.findProfile(request.profileName());
  if (selectedProfile == nullptr) {
    Diagnostic diagnostic = projectDiagnostic(
        "GTI-B1202", manifestSpan(manifest),
        "Unknown build profile '" + request.profileName() + "'.");
    if (const std::optional<std::string_view> nearest =
            nearestName(request.profileName(), manifest.profiles(),
                        [](const ProjectProfile &profile) -> std::string_view {
                          return profile.name;
                        })) {
      diagnostic.hints.push_back("Did you mean '" + std::string(*nearest) +
                                 "'?");
    }
    result.diagnostics.push_back(std::move(diagnostic));
  }

  if (!result.diagnostics.empty() || selectedTarget == nullptr ||
      selectedProfile == nullptr) {
    result.status = ProjectResolutionStatus::SelectionFailure;
    return result;
  }

  result.status = ProjectResolutionStatus::Success;
  result.plan = makeBuildPlan(manifest, *selectedTarget, *selectedProfile,
                              request.target(), request.overrides());
  return result;
}

ProjectMetadataResult
resolveProjectMetadata(const std::filesystem::path &startDirectory,
                       TargetInfo target) {
  ProjectMetadataResult result;
  ManifestDiscoveryResult discovery = discoverProjectManifest(startDirectory);
  if (!discovery.succeeded()) {
    result.status = ProjectMetadataStatus::DiscoveryFailure;
    result.diagnostics = std::move(discovery.diagnostics);
    return result;
  }

  ManifestLoadResult loaded = loadProjectManifest(*discovery.path);
  result.sources = std::move(loaded.sources);
  if (!loaded.succeeded()) {
    result.status = ProjectMetadataStatus::ManifestFailure;
    result.diagnostics = std::move(loaded.diagnostics);
    return result;
  }

  std::vector<ProjectBuildPlan> plans;
  const ProjectManifest &manifest = *loaded.manifest;
  plans.reserve(manifest.targets().size() * manifest.profiles().size());
  for (const ProjectTarget &projectTarget : manifest.targets()) {
    for (const ProjectProfile &profile : manifest.profiles()) {
      plans.push_back(makeBuildPlan(manifest, projectTarget, profile, target));
    }
  }

  result.status = ProjectMetadataStatus::Success;
  result.metadata.emplace(std::move(*loaded.manifest), std::move(target),
                          std::move(plans));
  return result;
}

ProjectCleanResult cleanProject(const std::filesystem::path &startDirectory) {
  ProjectCleanResult result;
  ManifestDiscoveryResult discovery = discoverProjectManifest(startDirectory);
  if (!discovery.succeeded()) {
    result.status = ProjectCleanStatus::DiscoveryFailure;
    result.diagnostics = std::move(discovery.diagnostics);
    return result;
  }

  const std::filesystem::path manifest = *discovery.path;
  const std::filesystem::path packageRoot = manifest.parent_path();
  const std::filesystem::path buildParent = packageRoot / "build";
  result.buildRoot = buildParent / "gti";

  if (packageRoot.empty() || packageRoot == packageRoot.root_path() ||
      result.buildRoot.filename() != "gti" ||
      result.buildRoot.parent_path().filename() != "build" ||
      !pathIsWithin(packageRoot, result.buildRoot) ||
      result.buildRoot == packageRoot) {
    result.status = ProjectCleanStatus::UnsafePath;
    result.diagnostics.push_back(
        cleanDiagnostic(manifest, "GTI-B1300",
                        "Refusing to clean an unsafe project build path."));
    return result;
  }

  std::error_code error;
  const std::filesystem::file_status buildStatus =
      std::filesystem::symlink_status(buildParent, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to inspect the project build directory: " + error.message() +
            "."));
    return result;
  }
  error.clear();
  const std::filesystem::file_status rootStatus =
      std::filesystem::symlink_status(result.buildRoot, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to inspect the GTI build directory: " + error.message() + "."));
    return result;
  }
  if (std::filesystem::is_symlink(buildStatus) ||
      std::filesystem::is_symlink(rootStatus)) {
    result.status = ProjectCleanStatus::UnsafePath;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1300",
        "Refusing to clean through a symbolic-link build path."));
    return result;
  }

  error.clear();
  const bool exists = std::filesystem::exists(result.buildRoot, error);
  if (error) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to inspect the GTI build directory: " + error.message() + "."));
    return result;
  }
  if (!exists) {
    result.status = ProjectCleanStatus::Success;
    return result;
  }
  if (!std::filesystem::is_directory(result.buildRoot, error) || error) {
    result.status = ProjectCleanStatus::UnsafePath;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1300",
        "Refusing to clean because the GTI build path is not a directory."));
    return result;
  }

  result.removedEntries = std::filesystem::remove_all(result.buildRoot, error);
  if (error) {
    result.status = ProjectCleanStatus::FilesystemFailure;
    result.diagnostics.push_back(cleanDiagnostic(
        manifest, "GTI-B1301",
        "Failed to clean the GTI build directory: " + error.message() + "."));
    return result;
  }
  result.status = ProjectCleanStatus::Success;
  return result;
}

} // namespace lang::driver
