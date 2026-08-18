#pragma once

#include "gti/driver/dependencies.h"
#include "gti/driver/manifest.h"
#include "gti/source_graph.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

enum class ProjectPackageMembership {
  Root,
  Member,
  Dependency,
};

struct ResolvedProjectDependency {
  std::string alias;
  std::string targetIdentity;
  std::filesystem::path targetRoot;
  SourceSpan declaration;
};

struct ResolvedProjectPackage {
  ProjectManifest manifest;
  ProjectPackageMembership membership = ProjectPackageMembership::Dependency;
  std::vector<ResolvedProjectDependency> dependencies;
  // Set for a git-sourced package: the pinned acquisition source and the
  // verified content checksum of its materialized tree.
  std::optional<GitSourceKey> gitSource;
  std::string gitChecksum;

  [[nodiscard]] std::string identity() const;
  // "git+<url>#<revision>#<checksum>" for git-sourced packages.
  [[nodiscard]] std::optional<std::string> sourceIdentity() const;
  [[nodiscard]] bool selectable() const;
};

// How workspace resolution treats pinned git dependencies. Stored trees are
// always checksum-verified before source loading; the policy only selects
// whether gti.lock is the coverage authority and whether a missing checkout
// may be acquired.
struct WorkspaceDependencyPolicy {
  // Verify every git dependency against gti.lock (missing or stale coverage
  // is a diagnostic). Only `gti fetch` resolves without this requirement,
  // because it exists to write the lock.
  bool requireLock = true;
  // Permit running git to materialize a missing checkout. `--offline`,
  // `--locked`, `gti metadata`, and `gti clean` refuse acquisition.
  bool allowAcquisition = true;
};

class ProjectWorkspace;
struct WorkspaceResolutionResult;

[[nodiscard]] WorkspaceResolutionResult resolveProjectWorkspace(
    const std::filesystem::path &startDirectory,
    const std::optional<std::string> &requestedPackage = std::nullopt,
    const WorkspaceDependencyPolicy &dependencyPolicy = {});

// The lock closure `gti fetch` records: one entry per git-sourced package in
// the resolved workspace, with its verified checksum and the names of its
// direct manifest dependencies.
[[nodiscard]] DependencyLock
lockFromWorkspace(const ProjectWorkspace &workspace);

class ProjectWorkspace final {
public:
  [[nodiscard]] const std::filesystem::path &root() const;
  [[nodiscard]] bool declared() const;
  [[nodiscard]] const std::vector<ResolvedProjectPackage> &packages() const;
  [[nodiscard]] const ResolvedProjectPackage &selectedPackage() const;
  [[nodiscard]] const ResolvedProjectPackage *
  findPackage(std::string_view identity) const;
  [[nodiscard]] std::vector<PackageSourceRoot> packageSourceRoots() const;
  [[nodiscard]] std::string modelIdentity() const;

private:
  friend WorkspaceResolutionResult
  resolveProjectWorkspace(const std::filesystem::path &,
                          const std::optional<std::string> &,
                          const WorkspaceDependencyPolicy &);

  ProjectWorkspace(std::filesystem::path root,
                   std::vector<ResolvedProjectPackage> packages,
                   std::string selectedIdentity, bool declaredWorkspace);

  std::filesystem::path workspaceRoot;
  std::vector<ResolvedProjectPackage> resolvedPackages;
  std::string selectedPackageIdentity;
  bool explicitWorkspace = false;
};

enum class WorkspaceResolutionStatus {
  Success,
  DiscoveryFailure,
  ManifestFailure,
  GraphFailure,
  SelectionFailure,
};

struct WorkspaceResolutionResult {
  WorkspaceResolutionStatus status =
      WorkspaceResolutionStatus::DiscoveryFailure;
  std::optional<ProjectWorkspace> workspace;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == WorkspaceResolutionStatus::Success &&
           workspace.has_value();
  }
};

} // namespace lang::driver
