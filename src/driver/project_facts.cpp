#include "gti/driver/project_facts.h"

#include <algorithm>
#include <utility>

namespace lang::driver {

std::vector<IncludeSearchRoot>
projectIncludeSearchRoots(const ProjectFacts &facts) {
  const auto &selected = facts.selectedPackage();
  IncludeSearchRoot own{selected.packageRoot, std::nullopt, {}};
  for (const auto &package : facts.packages()) {
    if (package.identity != selected.identity) {
      const auto relative =
          package.packageRoot.lexically_relative(selected.packageRoot);
      if (!relative.empty() && *relative.begin() != ".." && relative != ".") {
        own.excludedDirectories.push_back(package.packageRoot);
      }
    }
  }
  std::vector<IncludeSearchRoot> roots{std::move(own)};
  for (const auto &dependency : selected.dependencies) {
    if (const auto *package = facts.findPackage(dependency.targetIdentity)) {
      roots.push_back({package->sourceRoot, dependency.alias, {}});
    }
  }
  return roots;
}

namespace {

// The profile a project command selects when none is named. Derived from the
// resolved manifest rather than asserted, so this never disagrees with what
// the manifest actually declares.
std::string
selectDefaultProfileName(const std::vector<ProjectProfile> &declared) {
  const auto dev = std::find_if(
      declared.begin(), declared.end(),
      [](const ProjectProfile &profile) { return profile.name == "dev"; });
  if (dev != declared.end()) {
    return dev->name;
  }
  return declared.empty() ? std::string() : declared.front().name;
}

ProjectFactsPackage describePackage(const ResolvedProjectPackage &package) {
  ProjectFactsPackage described;
  described.identity = package.identity();
  described.name = package.manifest.package().name;
  described.version = package.manifest.package().version;
  described.membership = package.membership;
  described.manifestPath = package.manifest.path();
  described.packageRoot = package.manifest.packageRoot();
  described.sourceRoot = package.manifest.package().sourceRoot;
  described.selectable = package.selectable();
  described.sourceIdentity = package.sourceIdentity();
  described.dependencies.reserve(package.dependencies.size());
  for (const ResolvedProjectDependency &dependency : package.dependencies) {
    described.dependencies.push_back(
        ProjectFactsDependency{.alias = dependency.alias,
                               .targetIdentity = dependency.targetIdentity});
  }
  return described;
}

} // namespace

const std::filesystem::path &ProjectFacts::workspaceRoot() const {
  return resolvedWorkspaceRoot;
}

bool ProjectFacts::declaredWorkspace() const { return explicitWorkspace; }

const std::vector<ProjectFactsPackage> &ProjectFacts::packages() const {
  return resolvedPackages;
}

const ProjectFactsPackage &ProjectFacts::selectedPackage() const {
  const ProjectFactsPackage *selected = findPackage(selectedIdentity);
  // Resolution only produces facts for a workspace with a selected package,
  // so this cannot be absent.
  return *selected;
}

const ProjectFactsPackage *
ProjectFacts::findPackage(std::string_view identity) const {
  const auto match =
      std::find_if(resolvedPackages.begin(), resolvedPackages.end(),
                   [&](const ProjectFactsPackage &package) {
                     return package.identity == identity;
                   });
  return match == resolvedPackages.end() ? nullptr : &*match;
}

const std::vector<PackageSourceRoot> &ProjectFacts::packageSourceRoots() const {
  return resolvedSourceRoots;
}

const std::vector<ProjectFactsProfile> &ProjectFacts::profiles() const {
  return resolvedProfiles;
}

const ProjectFactsProfile *
ProjectFacts::findProfile(std::string_view name) const {
  const auto match = std::find_if(
      resolvedProfiles.begin(), resolvedProfiles.end(),
      [&](const ProjectFactsProfile &profile) { return profile.name == name; });
  return match == resolvedProfiles.end() ? nullptr : &*match;
}

const std::string &ProjectFacts::defaultProfileName() const {
  return resolvedDefaultProfile;
}

const std::vector<ProjectFactsTarget> &ProjectFacts::targets() const {
  return resolvedTargets;
}

const ConfigurationFlags &ProjectFacts::configurationFlags() const {
  return resolvedConfigurationFlags;
}

std::optional<AnalysisConfiguration>
ProjectFacts::analysisConfiguration(std::string_view profileName,
                                    TargetInfo target,
                                    RuntimeProfile runtimeProfile) const {
  const ProjectFactsProfile *profile = findProfile(profileName);
  if (profile == nullptr) {
    return std::nullopt;
  }
  return AnalysisConfiguration(std::move(target), profile->executionProfile,
                               runtimeProfile, resolvedConfigurationFlags);
}

const std::string &ProjectFacts::modelIdentity() const {
  return resolvedModelIdentity;
}

PackageWarningPolicy
ProjectFacts::packageWarnings(std::string_view profileName) const {
  const ProjectFactsProfile *profile = findProfile(profileName);
  if (profile == nullptr) {
    return {};
  }
  std::vector<std::string> dependencies;
  for (const ProjectFactsPackage &package : resolvedPackages) {
    if (package.identity != selectedIdentity) {
      dependencies.push_back(package.identity);
    }
  }
  return PackageWarningPolicy(profile->dependencyWarnings, selectedIdentity,
                              std::move(dependencies));
}

ProjectFacts projectFactsFromWorkspace(const ProjectWorkspace &workspace) {
  ProjectFacts facts;
  facts.resolvedWorkspaceRoot = workspace.root();
  facts.explicitWorkspace = workspace.declared();
  facts.resolvedModelIdentity = workspace.modelIdentity();
  facts.resolvedSourceRoots = workspace.packageSourceRoots();

  facts.resolvedPackages.reserve(workspace.packages().size());
  for (const ResolvedProjectPackage &package : workspace.packages()) {
    facts.resolvedPackages.push_back(describePackage(package));
  }

  const ResolvedProjectPackage &selected = workspace.selectedPackage();
  const ProjectManifest &manifest = selected.manifest;
  facts.selectedIdentity = selected.identity();
  facts.resolvedConfigurationFlags = manifest.build().defines;
  facts.resolvedDefaultProfile = selectDefaultProfileName(manifest.profiles());

  facts.resolvedProfiles.reserve(manifest.profiles().size());
  for (const ProjectProfile &profile : manifest.profiles()) {
    facts.resolvedProfiles.push_back(
        ProjectFactsProfile{.name = profile.name,
                            .optimization = profile.optimization,
                            .cppStandard = profile.cppStandard,
                            .executionProfile = profile.executionProfile,
                            .warningPolicy = profile.warningPolicy,
                            .dependencyWarnings = profile.dependencyWarnings});
  }

  facts.resolvedTargets.reserve(manifest.targets().size());
  for (const ProjectTarget &target : manifest.targets()) {
    facts.resolvedTargets.push_back(ProjectFactsTarget{
        .name = target.name, .kind = target.kind, .root = target.root});
  }
  return facts;
}

ProjectFactsResult
resolveProjectFacts(const std::filesystem::path &startDirectory,
                    const std::optional<std::string> &requestedPackage) {
  ProjectFactsResult result;
  // The read-only policy `gti metadata` uses: a stored checkout is still
  // checksum-verified and gti.lock remains the coverage authority, but no git
  // process runs and nothing is acquired.
  WorkspaceResolutionResult resolved = resolveProjectWorkspace(
      startDirectory, requestedPackage, {.allowAcquisition = false});
  result.sources = std::move(resolved.sources);
  result.diagnostics = std::move(resolved.diagnostics);
  if (!resolved.succeeded()) {
    switch (resolved.status) {
    case WorkspaceResolutionStatus::DiscoveryFailure:
      result.status = ProjectFactsStatus::DiscoveryFailure;
      break;
    case WorkspaceResolutionStatus::ManifestFailure:
      result.status = ProjectFactsStatus::ManifestFailure;
      break;
    case WorkspaceResolutionStatus::GraphFailure:
      result.status = ProjectFactsStatus::GraphFailure;
      break;
    case WorkspaceResolutionStatus::SelectionFailure:
      result.status = ProjectFactsStatus::SelectionFailure;
      break;
    case WorkspaceResolutionStatus::Success:
      result.status = ProjectFactsStatus::GraphFailure;
      break;
    }
    return result;
  }

  result.status = ProjectFactsStatus::Success;
  result.facts = projectFactsFromWorkspace(*resolved.workspace);
  return result;
}

} // namespace lang::driver
