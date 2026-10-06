#pragma once

#include "gti/driver/compilation.h"
#include "gti/driver/manifest.h"
#include "gti/driver/workspace.h"
#include "gti/include_suggestions.h"
#include "gti/source_graph.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

// Read-only resolved project facts (B-PROJECT-05).
//
// A consumer that must agree with `gti build` and `gti check` about what a
// project means — an editor, a documentation generator, an external analysis
// tool — needs the same resolved workspace the build path resolves, without
// building, fetching, or mutating project state. Reading `gti.toml` directly
// and reconstructing that meaning creates a second interpretation of manifest
// semantics that drifts from the driver's; the package source-root graph in
// particular cannot be recovered from one manifest, so a consumer that skips
// it silently loses every package include the build path resolves.
//
// These values are that shared answer. Resolution runs no processes and
// acquires nothing: it uses the same policy as `gti metadata`, so a git
// dependency must already be materialized and recorded in `gti.lock`, and a
// stored checkout is still checksum-verified before its source is described.
// An unmaterialized or unlocked dependency is reported as a diagnostic rather
// than acquired.

// One direct manifest edge, as resolved against the workspace graph.
struct ProjectFactsDependency {
  // The alias `#include <alias/unit>` resolves through.
  std::string alias;
  // "name@version" of the package the alias resolves to.
  std::string targetIdentity;
};

// One package in the resolved workspace closure.
struct ProjectFactsPackage {
  std::string identity;
  std::string name;
  std::string version;
  ProjectPackageMembership membership = ProjectPackageMembership::Dependency;
  std::filesystem::path manifestPath;
  std::filesystem::path packageRoot;
  std::filesystem::path sourceRoot;
  // False for a source-only dependency package that declares no targets and
  // therefore cannot be selected as a command target.
  bool selectable = false;
  // Present for a git-sourced package: its pinned provenance and verified
  // content checksum, in the same spelling cache identity uses.
  std::optional<std::string> sourceIdentity;
  std::vector<ProjectFactsDependency> dependencies;
};

// One declared profile's analysis-relevant policy. Native settings and output
// layout are build concerns and are deliberately absent.
struct ProjectFactsProfile {
  std::string name;
  OptimizationLevel optimization = OptimizationLevel::O0;
  CppStandard cppStandard = CppStandard::Cpp23;
  ExecutionProfile executionProfile = ExecutionProfile::SingleThreaded;
  RawPointerAuditMode rawPointerAudit = RawPointerAuditMode::Off;
  WarningPolicy warningPolicy;
  bool dependencyWarnings = true;
};

// One declared target of the selected package.
struct ProjectFactsTarget {
  std::string name;
  ProjectTargetKind kind = ProjectTargetKind::Executable;
  // The target's entry source, resolved against the package source root.
  std::filesystem::path root;
};

class ProjectFacts final {
public:
  [[nodiscard]] const std::filesystem::path &workspaceRoot() const;
  // True when a `[workspace]` table declared the members; false for a
  // standalone package that is its own workspace root.
  [[nodiscard]] bool declaredWorkspace() const;

  [[nodiscard]] const std::vector<ProjectFactsPackage> &packages() const;
  [[nodiscard]] const ProjectFactsPackage &selectedPackage() const;
  [[nodiscard]] const ProjectFactsPackage *
  findPackage(std::string_view identity) const;

  // The package source-root graph the frontend resolves package includes
  // through. Pass this to `Frontend::analyze`; an empty vector would silently
  // reduce every package include to "unit not found".
  [[nodiscard]] const std::vector<PackageSourceRoot> &
  packageSourceRoots() const;

  // The selected package's declared profiles, in manifest order.
  [[nodiscard]] const std::vector<ProjectFactsProfile> &profiles() const;
  [[nodiscard]] const ProjectFactsProfile *
  findProfile(std::string_view name) const;
  // The profile a project command selects when none is named.
  [[nodiscard]] const std::string &defaultProfileName() const;

  [[nodiscard]] const std::vector<ProjectFactsTarget> &targets() const;

  // Package-level `[build] defines`, the same flags the build path seeds a
  // compilation with before CLI overrides apply.
  [[nodiscard]] const ConfigurationFlags &configurationFlags() const;

  // The analysis configuration for one declared profile, assembled exactly as
  // the build path assembles it. Returns nothing when the profile is not
  // declared.
  [[nodiscard]] std::optional<AnalysisConfiguration>
  analysisConfiguration(std::string_view profileName, TargetInfo target,
                        RuntimeProfile runtimeProfile) const;

  // Consumer-selected profile policy, classified by the resolved graph's
  // actual package identities. Dependency manifests cannot hide diagnostics.
  [[nodiscard]] PackageWarningPolicy
  packageWarnings(std::string_view profileName) const;

  // The resolved package graph's identity: equal identities describe the same
  // workspace closure, package versions, and alias edges. A build plan's model
  // identity extends this with its configuration flags and bindgen imports, so
  // this is that identity's base rather than its equal.
  [[nodiscard]] const std::string &modelIdentity() const;

private:
  // Only a successful workspace resolution can establish the selected-package
  // and graph invariants required by selectedPackage() and modelIdentity().
  ProjectFacts() = default;

  friend ProjectFacts projectFactsFromWorkspace(const ProjectWorkspace &);

  std::filesystem::path resolvedWorkspaceRoot;
  bool explicitWorkspace = false;
  std::vector<ProjectFactsPackage> resolvedPackages;
  std::string selectedIdentity;
  std::vector<PackageSourceRoot> resolvedSourceRoots;
  std::vector<ProjectFactsProfile> resolvedProfiles;
  std::string resolvedDefaultProfile;
  std::vector<ProjectFactsTarget> resolvedTargets;
  ConfigurationFlags resolvedConfigurationFlags;
  std::string resolvedModelIdentity;
};

enum class ProjectFactsStatus {
  Success,
  DiscoveryFailure,
  ManifestFailure,
  GraphFailure,
  SelectionFailure,
};

struct ProjectFactsResult {
  ProjectFactsStatus status = ProjectFactsStatus::DiscoveryFailure;
  std::optional<ProjectFacts> facts;
  SourceManager sources;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == ProjectFactsStatus::Success && facts.has_value();
  }
};

// Resolves the project containing `startDirectory` and describes it. Runs no
// native tool, no git process, and no build, and creates no directories.
[[nodiscard]] ProjectFactsResult resolveProjectFacts(
    const std::filesystem::path &startDirectory,
    const std::optional<std::string> &requestedPackage = std::nullopt);

// Describes an already-resolved workspace. Callers that have resolved a
// workspace for another purpose reuse it instead of resolving twice.
[[nodiscard]] ProjectFacts
projectFactsFromWorkspace(const ProjectWorkspace &workspace);

// Search authority for editor include discovery: the selected package and
// declared direct dependency aliases only. No acquisition or recursive walk.
[[nodiscard]] std::vector<IncludeSearchRoot>
projectIncludeSearchRoots(const ProjectFacts &facts);

} // namespace lang::driver
