#pragma once

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

  [[nodiscard]] std::string identity() const;
  [[nodiscard]] bool selectable() const;
};

class ProjectWorkspace final {
public:
  ProjectWorkspace(std::filesystem::path root,
                   std::vector<ResolvedProjectPackage> packages,
                   std::string selectedIdentity, bool declaredWorkspace);

  [[nodiscard]] const std::filesystem::path &root() const;
  [[nodiscard]] bool declared() const;
  [[nodiscard]] const std::vector<ResolvedProjectPackage> &packages() const;
  [[nodiscard]] const ResolvedProjectPackage &selectedPackage() const;
  [[nodiscard]] const ResolvedProjectPackage *
  findPackage(std::string_view identity) const;
  [[nodiscard]] std::vector<PackageSourceRoot> packageSourceRoots() const;
  [[nodiscard]] std::string modelIdentity() const;

private:
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

[[nodiscard]] WorkspaceResolutionResult resolveProjectWorkspace(
    const std::filesystem::path &startDirectory,
    const std::optional<std::string> &requestedPackage = std::nullopt);

} // namespace lang::driver
