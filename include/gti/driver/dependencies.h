#pragma once

#include "gti/diagnostic.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang::driver {

inline constexpr int currentDependencyLockVersion = 1;

// One pinned git source: an exact URL plus a full lowercase 40-hex commit.
// Branches, tags, and ranges are deliberately not representable.
struct GitSourceKey {
  std::string url;
  std::string revision;

  friend bool operator==(const GitSourceKey &, const GitSourceKey &) = default;
};

[[nodiscard]] bool isFullGitRevision(std::string_view revision);

// The recorded acquisition facts for one fetched git package. `checksum` is
// the deterministic content identity of the extracted tree
// ("sha256:<64 hex>"), and `dependencies` lists the package names of its
// direct manifest dependencies in sorted order.
struct LockedGitPackage {
  std::string name;
  std::string version;
  std::string url;
  std::string revision;
  std::string checksum;
  std::vector<std::string> dependencies;
};

struct DependencyLock {
  std::vector<LockedGitPackage> packages;

  [[nodiscard]] const LockedGitPackage *find(const GitSourceKey &key) const;
};

enum class DependencyLockLoadStatus {
  Missing,
  Success,
  IoFailure,
  ParseFailure,
};

struct DependencyLockLoadResult {
  DependencyLockLoadStatus status = DependencyLockLoadStatus::Missing;
  DependencyLock lock;
  std::vector<Diagnostic> diagnostics;

  [[nodiscard]] bool succeeded() const {
    return status == DependencyLockLoadStatus::Success;
  }
};

[[nodiscard]] std::filesystem::path
dependencyLockPath(const std::filesystem::path &workspaceRoot);

[[nodiscard]] DependencyLockLoadResult
loadDependencyLock(const std::filesystem::path &workspaceRoot);

// Deterministic lockfile text: entries sorted by package name and every list
// sorted, so regenerating an unchanged closure is byte-identical.
[[nodiscard]] std::string renderDependencyLock(const DependencyLock &lock);

[[nodiscard]] bool
writeDependencyLock(const std::filesystem::path &workspaceRoot,
                    const DependencyLock &lock, std::string &errorMessage);

// Store layout beneath the workspace's managed build tree. The bare object
// database is keyed by URL identity and checkouts by URL identity plus
// revision, so equal aliases across packages share one acquisition.
[[nodiscard]] std::filesystem::path
gitDependencyStoreRoot(const std::filesystem::path &workspaceRoot);

[[nodiscard]] std::filesystem::path
gitCheckoutPath(const std::filesystem::path &workspaceRoot,
                const GitSourceKey &key);

enum class GitFetchStatus {
  Success,
  GitUnavailable,
  FetchFailure,
  RevisionUnavailable,
  TreeRejected,
  StoreFailure,
};

struct GitFetchResult {
  GitFetchStatus status = GitFetchStatus::FetchFailure;
  std::filesystem::path checkout;
  std::string checksum;
  std::string detail;

  [[nodiscard]] bool succeeded() const {
    return status == GitFetchStatus::Success;
  }
};

// Materializes one pinned source into the store. Acquisition never executes
// repository code: history is fetched into a bare database and the tree is
// extracted blob-by-blob with `git ls-tree`/`git cat-file`, so no checkout,
// filter, hook, or submodule machinery runs. Symbolic links, submodules,
// unsafe paths, and case-folded path collisions reject the tree.
//
// The returned checksum is always derived from the immutable object
// database, never from an existing checkout, so a locally modified tree can
// not launder its content into a new lock: a divergent checkout is replaced.
// With `offline`, the network fetch step is skipped and only a database that
// already contains the revision can materialize it; local extraction and
// verification still run.
[[nodiscard]] GitFetchResult
fetchGitSource(const std::filesystem::path &workspaceRoot,
               const GitSourceKey &key, const std::string &gitExecutable,
               bool offline = false);

// Recomputes the deterministic content identity of a materialized checkout
// ("sha256:<64 hex>"). Verification runs before any source loading; a
// mismatch means the store cannot be trusted for this key.
[[nodiscard]] std::optional<std::string>
checkoutChecksum(const std::filesystem::path &checkout,
                 std::string &errorMessage);

// Git discovery follows the toolchain convention: GTI_GIT then `git`.
[[nodiscard]] std::string discoverGitExecutable();

} // namespace lang::driver
