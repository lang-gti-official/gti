#include "gti/driver/workspace.h"

#include "gti/driver/dependencies.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang::driver {
namespace {

constexpr std::string_view manifestFilename = "gti.toml";

Diagnostic workspaceDiagnostic(std::string code, SourceSpan span,
                               std::string message) {
  return makeDiagnostic(std::move(code), DiagnosticPhase::Driver,
                        std::move(span), std::move(message));
}

SourceSpan manifestSpan(const ProjectManifest &manifest) {
  if (!manifest.targets().empty()) {
    return manifest.targets().front().declaration;
  }
  return {manifest.path().string(), 0, 1, 1};
}

void mergeSources(SourceManager &destination, const SourceManager &source) {
  for (const std::string &name : source.names()) {
    if (const std::string *text = source.find(name)) {
      destination.set(name, *text);
    }
  }
}

int membershipRank(ProjectPackageMembership membership) {
  switch (membership) {
  case ProjectPackageMembership::Root:
    return 2;
  case ProjectPackageMembership::Member:
    return 1;
  case ProjectPackageMembership::Dependency:
    return 0;
  }
  return 0;
}

bool hasNativeInputs(const NativeInputs &inputs) {
  return !inputs.includeDirectories.empty() || !inputs.cSources.empty() ||
         !inputs.cppSources.empty() || !inputs.libraryDirectories.empty() ||
         !inputs.libraryFiles.empty() || !inputs.libraries.empty() ||
         !inputs.frameworks.empty() || !inputs.compilerArguments.empty() ||
         !inputs.cCompilerArguments.empty() ||
         !inputs.linkerArguments.empty() || !inputs.trailingArguments.empty() ||
         inputs.cStandard.has_value();
}

bool hasNativeInputs(const ProjectNativeSettings &settings) {
  if (hasNativeInputs(settings.inputs)) {
    return true;
  }
  return std::any_of(settings.platforms.begin(), settings.platforms.end(),
                     [](const ProjectNativePlatform &platform) {
                       return hasNativeInputs(platform.inputs);
                     });
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

} // namespace

std::string ResolvedProjectPackage::identity() const {
  return manifest.package().name + "@" + manifest.package().version;
}

std::optional<std::string> ResolvedProjectPackage::sourceIdentity() const {
  if (!gitSource) {
    return std::nullopt;
  }
  return "git+" + gitSource->url + "#" + gitSource->revision + "#" +
         gitChecksum;
}

bool ResolvedProjectPackage::selectable() const {
  return membership != ProjectPackageMembership::Dependency;
}

ProjectWorkspace::ProjectWorkspace(std::filesystem::path root,
                                   std::vector<ResolvedProjectPackage> packages,
                                   std::string selectedIdentity,
                                   bool declaredWorkspace)
    : workspaceRoot(std::move(root)), resolvedPackages(std::move(packages)),
      selectedPackageIdentity(std::move(selectedIdentity)),
      explicitWorkspace(declaredWorkspace) {}

const std::filesystem::path &ProjectWorkspace::root() const {
  return workspaceRoot;
}

bool ProjectWorkspace::declared() const { return explicitWorkspace; }

const std::vector<ResolvedProjectPackage> &ProjectWorkspace::packages() const {
  return resolvedPackages;
}

const ResolvedProjectPackage &ProjectWorkspace::selectedPackage() const {
  return *findPackage(selectedPackageIdentity);
}

const ResolvedProjectPackage *
ProjectWorkspace::findPackage(std::string_view identity) const {
  const auto found =
      std::find_if(resolvedPackages.begin(), resolvedPackages.end(),
                   [identity](const ResolvedProjectPackage &package) {
                     return package.identity() == identity;
                   });
  return found == resolvedPackages.end() ? nullptr : &*found;
}

std::vector<PackageSourceRoot> ProjectWorkspace::packageSourceRoots() const {
  std::vector<PackageSourceRoot> roots;
  roots.reserve(resolvedPackages.size());
  for (const ResolvedProjectPackage &package : resolvedPackages) {
    PackageSourceRoot root{.identity = package.identity(),
                           .name = package.manifest.package().name,
                           .packageRoot = package.manifest.packageRoot(),
                           .sourceRoot = package.manifest.package().sourceRoot};
    root.dependencies.reserve(package.dependencies.size());
    for (const ResolvedProjectDependency &dependency : package.dependencies) {
      root.dependencies.push_back(
          {.alias = dependency.alias,
           .targetIdentity = dependency.targetIdentity});
    }
    roots.push_back(std::move(root));
  }
  return roots;
}

std::string ProjectWorkspace::modelIdentity() const {
  std::string identity = "gti-workspace-v1";
  for (const ResolvedProjectPackage &package : resolvedPackages) {
    identity += "\npackage:" + package.identity();
    if (const std::optional<std::string> source = package.sourceIdentity()) {
      // A git-sourced package's exact acquisition identity participates so
      // two revisions of one name@version can never share build state.
      identity += "@" + *source;
    }
    for (const ResolvedProjectDependency &dependency : package.dependencies) {
      identity += "\ndependency:" + package.identity() + ":" +
                  dependency.alias + "=" + dependency.targetIdentity;
    }
  }
  return identity;
}

WorkspaceResolutionResult
resolveProjectWorkspace(const std::filesystem::path &startDirectory,
                        const std::optional<std::string> &requestedPackage,
                        const WorkspaceDependencyPolicy &dependencyPolicy) {
  WorkspaceResolutionResult result;
  ManifestDiscoveryResult discovery = discoverProjectManifest(startDirectory);
  if (!discovery.succeeded()) {
    result.status = WorkspaceResolutionStatus::DiscoveryFailure;
    result.diagnostics = std::move(discovery.diagnostics);
    return result;
  }

  ManifestLoadResult nearestLoad = loadProjectManifest(*discovery.path);
  mergeSources(result.sources, nearestLoad.sources);
  if (!nearestLoad.succeeded()) {
    result.status = WorkspaceResolutionStatus::ManifestFailure;
    result.diagnostics = std::move(nearestLoad.diagnostics);
    return result;
  }
  const std::filesystem::path nearestRoot = nearestLoad.manifest->packageRoot();

  std::filesystem::path workspaceManifest = nearestLoad.manifest->path();
  bool declaredWorkspace = nearestLoad.manifest->workspace().has_value();
  std::filesystem::path parent = nearestRoot.parent_path();
  while (!parent.empty() && parent != parent.parent_path()) {
    const std::filesystem::path candidate = parent / manifestFilename;
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) && !error) {
      ManifestLoadResult candidateLoad = loadProjectManifest(candidate);
      if (candidateLoad.succeeded() && candidateLoad.manifest->workspace()) {
        const auto &members = candidateLoad.manifest->workspace()->members;
        if (std::find(members.begin(), members.end(), nearestRoot) !=
            members.end()) {
          workspaceManifest = candidateLoad.manifest->path();
          declaredWorkspace = true;
          break;
        }
      }
    }
    parent = parent.parent_path();
  }

  std::vector<ResolvedProjectPackage> packages;
  std::unordered_map<std::string, std::size_t> packageByRoot;
  const auto addLoadedManifest =
      [&](ManifestLoadResult loaded, ProjectPackageMembership membership,
          const std::optional<std::filesystem::path> &declaredRoot,
          const std::optional<SourceSpan> &declaration,
          std::string_view relationship) -> std::optional<std::size_t> {
    mergeSources(result.sources, loaded.sources);
    if (!loaded.succeeded()) {
      result.diagnostics.insert(result.diagnostics.end(),
                                loaded.diagnostics.begin(),
                                loaded.diagnostics.end());
      return std::nullopt;
    }
    if (declaredRoot && loaded.manifest->packageRoot() != *declaredRoot) {
      Diagnostic diagnostic = workspaceDiagnostic(
          "GTI-B1608", declaration.value_or(manifestSpan(*loaded.manifest)),
          std::string(relationship) + " manifest resolves to package root '" +
              loaded.manifest->packageRoot().string() +
              "', which differs from its declared package directory '" +
              declaredRoot->string() + "'.");
      diagnostic.related.push_back(
          {manifestSpan(*loaded.manifest),
           "The redirected package manifest is located here."});
      diagnostic.hints.push_back(
          "Keep gti.toml within the declared package directory; a manifest "
          "symbolic link cannot redirect package ownership.");
      result.diagnostics.push_back(std::move(diagnostic));
      return std::nullopt;
    }
    const std::string loadedRoot = loaded.manifest->packageRoot().string();
    if (const auto existing = packageByRoot.find(loadedRoot);
        existing != packageByRoot.end()) {
      ResolvedProjectPackage &package = packages[existing->second];
      if (membershipRank(membership) > membershipRank(package.membership)) {
        package.membership = membership;
      }
      return existing->second;
    }
    const std::size_t index = packages.size();
    packageByRoot.emplace(loadedRoot, index);
    packages.push_back(
        {.manifest = std::move(*loaded.manifest), .membership = membership});
    return index;
  };

  ManifestLoadResult workspaceLoad =
      workspaceManifest == nearestLoad.manifest->path()
          ? std::move(nearestLoad)
          : loadProjectManifest(workspaceManifest);
  const std::optional<std::size_t> rootIndex = addLoadedManifest(
      std::move(workspaceLoad), ProjectPackageMembership::Root, std::nullopt,
      std::nullopt, "Workspace root");
  if (!rootIndex) {
    result.status = WorkspaceResolutionStatus::ManifestFailure;
    return result;
  }
  const std::filesystem::path workspaceRoot =
      packages[*rootIndex].manifest.packageRoot();

  if (declaredWorkspace) {
    // Loading a member appends to `packages` and may reallocate it. Retain an
    // owned manifest value so member iteration never refers into that vector.
    const ProjectWorkspaceManifest workspace =
        *packages[*rootIndex].manifest.workspace();
    for (std::size_t index = 0; index < workspace.members.size(); ++index) {
      const std::filesystem::path &memberRoot = workspace.members[index];
      if (!addLoadedManifest(loadProjectManifest(memberRoot / manifestFilename),
                             ProjectPackageMembership::Member, memberRoot,
                             workspace.memberDeclarations[index],
                             "Workspace member")) {
        result.status = WorkspaceResolutionStatus::ManifestFailure;
        return result;
      }
    }
  }

  // Pinned git dependencies materialize through the verified store. The lock
  // is loaded once on first use and its parse diagnostics are reported once.
  std::optional<DependencyLockLoadResult> lockLoad;
  bool lockDiagnosticsReported = false;
  std::string gitExecutable;
  const auto dependencyLock = [&]() -> const DependencyLockLoadResult & {
    if (!lockLoad) {
      lockLoad = loadDependencyLock(workspaceRoot);
    }
    return *lockLoad;
  };

  for (std::size_t packageIndex = 0; packageIndex < packages.size();
       ++packageIndex) {
    if (packageIndex != *rootIndex &&
        packages[packageIndex].manifest.workspace()) {
      result.diagnostics.push_back(workspaceDiagnostic(
          "GTI-B1600", packages[packageIndex].manifest.workspace()->declaration,
          "Nested workspace declarations are not supported; only the root "
          "gti.toml may declare [workspace]."));
      continue;
    }

    std::set<std::string> dependencyRoots;
    const std::vector<ProjectDependency> dependencies =
        packages[packageIndex].manifest.dependencies();
    for (const ProjectDependency &dependency : dependencies) {
      std::filesystem::path targetRoot = dependency.packageRoot;
      std::optional<GitSourceKey> gitSource;
      std::string gitChecksum;
      const LockedGitPackage *locked = nullptr;
      if (dependency.git) {
        const GitSourceKey key{.url = dependency.git->url,
                               .revision = dependency.git->revision};
        if (!dependencyRoots.insert("git+" + key.url + "#" + key.revision)
                 .second) {
          result.diagnostics.push_back(workspaceDiagnostic(
              "GTI-B1602", dependency.pathDeclaration,
              "Dependency alias '" + dependency.alias +
                  "' resolves to a package already declared by this "
                  "package."));
          continue;
        }

        if (dependencyPolicy.requireLock) {
          const DependencyLockLoadResult &lock = dependencyLock();
          if (lock.status == DependencyLockLoadStatus::Missing) {
            Diagnostic diagnostic = workspaceDiagnostic(
                "GTI-B1701", dependency.pathDeclaration,
                "Git dependency '" + dependency.alias +
                    "' is declared, but the workspace has no gti.lock.");
            diagnostic.hints.push_back(
                "Run `gti fetch` to acquire pinned git dependencies and "
                "write gti.lock.");
            result.diagnostics.push_back(std::move(diagnostic));
            continue;
          }
          if (!lock.succeeded()) {
            if (!lockDiagnosticsReported) {
              result.diagnostics.insert(result.diagnostics.end(),
                                        lock.diagnostics.begin(),
                                        lock.diagnostics.end());
              lockDiagnosticsReported = true;
            }
            continue;
          }
          locked = lock.lock.find(key);
          if (locked == nullptr) {
            Diagnostic diagnostic = workspaceDiagnostic(
                "GTI-B1701", dependency.pathDeclaration,
                "gti.lock does not record git dependency '" + dependency.alias +
                    "' at " + key.url + "#" + key.revision + ".");
            diagnostic.hints.push_back(
                "The manifest changed after the last `gti fetch`; run it "
                "again to update gti.lock.");
            result.diagnostics.push_back(std::move(diagnostic));
            continue;
          }
        }

        const std::filesystem::path checkout =
            gitCheckoutPath(workspaceRoot, key);
        std::string checksum;
        std::error_code checkoutError;
        if (!dependencyPolicy.requireLock) {
          // Fetch mode re-derives truth from the immutable object database
          // even for materialized checkouts, so a locally modified tree can
          // never launder its content into a freshly written lock.
          if (gitExecutable.empty()) {
            gitExecutable = discoverGitExecutable();
          }
          const GitFetchResult fetched =
              fetchGitSource(workspaceRoot, key, gitExecutable,
                             !dependencyPolicy.allowAcquisition);
          if (!fetched.succeeded()) {
            result.diagnostics.push_back(workspaceDiagnostic(
                "GTI-B1705", dependency.pathDeclaration,
                "Cannot acquire git dependency '" + dependency.alias +
                    "' from " + key.url + ": " + fetched.detail + "."));
            continue;
          }
          checksum = fetched.checksum;
        } else if (std::filesystem::is_directory(checkout, checkoutError) &&
                   !checkoutError) {
          std::string checksumError;
          const std::optional<std::string> verified =
              checkoutChecksum(checkout, checksumError);
          if (!verified) {
            result.diagnostics.push_back(workspaceDiagnostic(
                "GTI-B1704", dependency.pathDeclaration,
                "Cannot verify the stored checkout for git dependency '" +
                    dependency.alias + "': " + checksumError + "."));
            continue;
          }
          checksum = *verified;
        } else if (!dependencyPolicy.allowAcquisition) {
          Diagnostic diagnostic = workspaceDiagnostic(
              "GTI-B1703", dependency.pathDeclaration,
              "Git dependency '" + dependency.alias + "' at " + key.url + "#" +
                  key.revision +
                  " is not materialized, and this command does not acquire "
                  "dependencies.");
          diagnostic.hints.push_back(
              "Run `gti fetch` first, or rerun without --offline/--locked.");
          result.diagnostics.push_back(std::move(diagnostic));
          continue;
        } else {
          if (gitExecutable.empty()) {
            gitExecutable = discoverGitExecutable();
          }
          const GitFetchResult fetched =
              fetchGitSource(workspaceRoot, key, gitExecutable);
          if (!fetched.succeeded()) {
            result.diagnostics.push_back(workspaceDiagnostic(
                "GTI-B1705", dependency.pathDeclaration,
                "Cannot acquire git dependency '" + dependency.alias +
                    "' from " + key.url + ": " + fetched.detail + "."));
            continue;
          }
          checksum = fetched.checksum;
        }

        if (locked != nullptr && checksum != locked->checksum) {
          Diagnostic diagnostic = workspaceDiagnostic(
              "GTI-B1704", dependency.pathDeclaration,
              "Git dependency '" + dependency.alias +
                  "' does not match its gti.lock checksum; refusing to load "
                  "unverified source.");
          diagnostic.hints.push_back(
              "Delete build/gti/deps and run `gti fetch` to re-acquire, or "
              "investigate how the stored tree changed.");
          result.diagnostics.push_back(std::move(diagnostic));
          continue;
        }
        targetRoot = checkout;
        gitSource = key;
        gitChecksum = checksum;
      } else if (!dependencyRoots.insert(dependency.packageRoot.string())
                      .second) {
        result.diagnostics.push_back(workspaceDiagnostic(
            "GTI-B1602", dependency.pathDeclaration,
            "Dependency alias '" + dependency.alias +
                "' resolves to a package already declared by this package."));
        continue;
      }

      // A fetched package's own path dependencies must stay inside its
      // verified checkout; escaping the store would load unlocked source.
      if (!dependency.git && packages[packageIndex].gitSource &&
          !pathIsWithin(packages[packageIndex].manifest.packageRoot(),
                        targetRoot)) {
        Diagnostic diagnostic = workspaceDiagnostic(
            "GTI-B1707", dependency.pathDeclaration,
            "Path dependency '" + dependency.alias +
                "' of a git-sourced package escapes its own checkout.");
        diagnostic.hints.push_back(
            "A fetched package may only use path dependencies contained in "
            "its repository; declare external packages as git dependencies "
            "with pinned revisions.");
        result.diagnostics.push_back(std::move(diagnostic));
        continue;
      }

      std::optional<std::size_t> dependencyIndex;
      if (const auto existing = packageByRoot.find(targetRoot.string());
          existing != packageByRoot.end()) {
        dependencyIndex = existing->second;
      } else {
        dependencyIndex = addLoadedManifest(
            loadProjectManifest(targetRoot / manifestFilename),
            ProjectPackageMembership::Dependency, targetRoot,
            dependency.pathDeclaration,
            dependency.git ? "Git dependency" : "Path dependency");
      }
      if (!dependencyIndex) {
        continue;
      }
      const ProjectManifest &target = packages[*dependencyIndex].manifest;
      if (locked != nullptr && (target.package().name != locked->name ||
                                target.package().version != locked->version)) {
        Diagnostic diagnostic = workspaceDiagnostic(
            "GTI-B1704", dependency.pathDeclaration,
            "Git dependency '" + dependency.alias + "' resolves to package " +
                target.package().name + "@" + target.package().version +
                ", but gti.lock records " + locked->name + "@" +
                locked->version + ".");
        diagnostic.hints.push_back(
            "Run `gti fetch` to update gti.lock for the pinned revision's "
            "actual package identity.");
        result.diagnostics.push_back(std::move(diagnostic));
        continue;
      }
      if (gitSource) {
        packages[*dependencyIndex].gitSource = std::move(gitSource);
        packages[*dependencyIndex].gitChecksum = std::move(gitChecksum);
      }
      std::error_code error;
      if (!std::filesystem::is_directory(target.package().sourceRoot, error) ||
          error) {
        result.diagnostics.push_back(workspaceDiagnostic(
            "GTI-B1601", dependency.pathDeclaration,
            "Dependency package '" + target.package().name +
                "' has no existing source root at '" +
                target.package().sourceRoot.string() + "'."));
      }
      if (hasNativeInputs(target.package().native)) {
        Diagnostic diagnostic = workspaceDiagnostic(
            "GTI-B1606", dependency.pathDeclaration,
            "Dependency '" + dependency.alias +
                "' declares package-level native inputs, which are not yet "
                "composed across package boundaries.");
        diagnostic.hints.push_back(
            "Keep this dependency source-only for now, or move the native "
            "inputs to the selected application package.");
        result.diagnostics.push_back(std::move(diagnostic));
      }
      packages[packageIndex].dependencies.push_back(
          {.alias = dependency.alias,
           .targetIdentity = packages[*dependencyIndex].identity(),
           .targetRoot = targetRoot,
           .declaration = dependency.pathDeclaration});
    }
  }

  std::unordered_map<std::string, std::size_t> packageByName;
  for (std::size_t index = 0; index < packages.size(); ++index) {
    const std::string &name = packages[index].manifest.package().name;
    const auto [found, inserted] =
        packageByName.emplace(portableProjectNameKey(name), index);
    if (!inserted && packages[found->second].manifest.packageRoot() !=
                         packages[index].manifest.packageRoot()) {
      Diagnostic diagnostic = workspaceDiagnostic(
          "GTI-B1604", manifestSpan(packages[index].manifest),
          "Package name '" + name +
              "' collides with another package under portable "
              "case-insensitive identity.");
      diagnostic.related.push_back(
          {manifestSpan(packages[found->second].manifest),
           "The first package with this name is declared here."});
      diagnostic.hints.push_back(
          "Workspace package names must be unique so selection and import "
          "identity remain deterministic.");
      result.diagnostics.push_back(std::move(diagnostic));
    }
  }

  if (!result.diagnostics.empty()) {
    result.status = WorkspaceResolutionStatus::GraphFailure;
    return result;
  }

  std::unordered_map<std::string, std::size_t> packageByIdentity;
  for (std::size_t index = 0; index < packages.size(); ++index) {
    packageByIdentity.emplace(packages[index].identity(), index);
  }
  std::vector<int> colors(packages.size(), 0);
  std::vector<std::size_t> stack;
  std::function<void(std::size_t)> visit = [&](std::size_t index) {
    colors[index] = 1;
    stack.push_back(index);
    for (const ResolvedProjectDependency &dependency :
         packages[index].dependencies) {
      const auto target = packageByIdentity.find(dependency.targetIdentity);
      if (target == packageByIdentity.end()) {
        continue;
      }
      if (colors[target->second] == 0) {
        visit(target->second);
      } else if (colors[target->second] == 1) {
        Diagnostic diagnostic = workspaceDiagnostic(
            "GTI-B1603", dependency.declaration,
            "Package dependency cycle closes at '" +
                packages[target->second].manifest.package().name + "'.");
        const auto cycleStart =
            std::find(stack.begin(), stack.end(), target->second);
        for (auto current = cycleStart; current != stack.end(); ++current) {
          diagnostic.related.push_back(
              {manifestSpan(packages[*current].manifest),
               "Package '" + packages[*current].manifest.package().name +
                   "' participates in this cycle."});
        }
        result.diagnostics.push_back(std::move(diagnostic));
      }
    }
    stack.pop_back();
    colors[index] = 2;
  };
  for (std::size_t index = 0; index < packages.size(); ++index) {
    if (colors[index] == 0) {
      visit(index);
    }
  }

  if (!result.diagnostics.empty()) {
    result.status = WorkspaceResolutionStatus::GraphFailure;
    return result;
  }

  const ResolvedProjectPackage *selected = nullptr;
  if (requestedPackage) {
    for (const ResolvedProjectPackage &package : packages) {
      if (package.selectable() &&
          package.manifest.package().name == *requestedPackage) {
        selected = &package;
        break;
      }
    }
    if (selected == nullptr) {
      Diagnostic diagnostic = workspaceDiagnostic(
          "GTI-B1607", manifestSpan(packages[*rootIndex].manifest),
          "Unknown workspace package '" + *requestedPackage + "'.");
      std::string available;
      for (const ResolvedProjectPackage &package : packages) {
        if (!package.selectable()) {
          continue;
        }
        if (!available.empty()) {
          available += ", ";
        }
        available += package.manifest.package().name;
      }
      diagnostic.hints.push_back("Available packages: " + available + ".");
      result.diagnostics.push_back(std::move(diagnostic));
    }
  } else {
    const auto nearest = packageByRoot.find(nearestRoot.string());
    if (nearest != packageByRoot.end() &&
        packages[nearest->second].selectable()) {
      selected = &packages[nearest->second];
    } else {
      selected = &packages[*rootIndex];
    }
  }

  if (selected == nullptr) {
    result.status = WorkspaceResolutionStatus::SelectionFailure;
    return result;
  }
  const std::string selectedIdentity = selected->identity();
  std::sort(
      packages.begin(), packages.end(),
      [](const ResolvedProjectPackage &left,
         const ResolvedProjectPackage &right) {
        if (left.manifest.package().name != right.manifest.package().name) {
          return left.manifest.package().name < right.manifest.package().name;
        }
        return left.manifest.packageRoot() < right.manifest.packageRoot();
      });
  result.workspace = ProjectWorkspace(workspaceRoot, std::move(packages),
                                      selectedIdentity, declaredWorkspace);
  result.status = WorkspaceResolutionStatus::Success;
  return result;
}

DependencyLock lockFromWorkspace(const ProjectWorkspace &workspace) {
  DependencyLock lock;
  for (const ResolvedProjectPackage &package : workspace.packages()) {
    if (!package.gitSource) {
      continue;
    }
    LockedGitPackage locked{.name = package.manifest.package().name,
                            .version = package.manifest.package().version,
                            .url = package.gitSource->url,
                            .revision = package.gitSource->revision,
                            .checksum = package.gitChecksum,
                            .dependencies = {}};
    for (const ResolvedProjectDependency &dependency : package.dependencies) {
      const std::string &identity = dependency.targetIdentity;
      locked.dependencies.push_back(identity.substr(0, identity.find('@')));
    }
    std::sort(locked.dependencies.begin(), locked.dependencies.end());
    locked.dependencies.erase(
        std::unique(locked.dependencies.begin(), locked.dependencies.end()),
        locked.dependencies.end());
    lock.packages.push_back(std::move(locked));
  }
  std::sort(lock.packages.begin(), lock.packages.end(),
            [](const LockedGitPackage &left, const LockedGitPackage &right) {
              return left.name < right.name;
            });
  return lock;
}

} // namespace lang::driver
