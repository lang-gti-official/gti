#pragma once

#include "gti/diagnostic.h"
#include "gti/source_owner.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lang {

struct FrontendOptions;
struct FrontendResult;

// The adapter supplies search authority, not symbol meaning. A quoted root
// stays within its package; an angle root is addressed via its declared alias.
struct IncludeSearchRoot {
  std::filesystem::path directory;
  std::optional<std::string> anglePrefix;
  std::vector<std::filesystem::path> excludedDirectories;
};

struct IncludeDiscoveryCacheLimits {
  // Candidate interfaces and source texts remembered at once (least recently
  // used evicted). Each row is one interface's syntax-level facts.
  std::size_t maximumEntries = 4096;
};

struct IncludeDiscoveryCacheStats {
  std::size_t lexiconEntries = 0;
  std::size_t scanEntries = 0;
  std::size_t lexiconHits = 0;
  std::size_t lexiconMisses = 0;
  std::size_t scanHits = 0;
  std::size_t scanMisses = 0;
  // Speculative frontend analyses run, and those avoided because the
  // candidate surface cannot spell a qualifier the use still needs.
  std::size_t trials = 0;
  std::size_t qualifierSkips = 0;
};

// Missing-import discovery facts that depend only on candidate source text:
// each text's token spellings, and each candidate interface's export surface
// (declaration names and units) from its parse-only load. A row is reused
// only while its context (target, configuration flags, source-owner
// resolution identity, prelude and library roots) is unchanged and every
// source it read still has the same text, so reuse never changes a
// suggestion. Rows never hold semantic identities, and the speculative
// analysis that proves each fix always runs again. Thread-safe; an editor
// shares one instance across its snapshots.
class IncludeDiscoveryCache final {
public:
  class Rows;

  explicit IncludeDiscoveryCache(IncludeDiscoveryCacheLimits limits = {});
  ~IncludeDiscoveryCache();

  IncludeDiscoveryCache(const IncludeDiscoveryCache &) = delete;
  IncludeDiscoveryCache &operator=(const IncludeDiscoveryCache &) = delete;

  // Drops rows that read `canonicalPath`. Reuse already compares source
  // text, so this reclaims memory early rather than preventing stale rows.
  void invalidate(std::string_view canonicalPath);
  void clear();
  [[nodiscard]] IncludeDiscoveryCacheStats stats() const;

  // Storage owned by discovery's implementation.
  [[nodiscard]] Rows &rows() const { return *storage; }

private:
  std::unique_ptr<Rows> storage;
};

struct IncludeSuggestionResult {
  std::vector<Diagnostic> diagnostics;
  std::vector<std::filesystem::path> inputs;
  bool cancelled = false;
};

// Whether this completed snapshot can benefit from import discovery. Editors
// use this before publishing diagnostics, so discovery can follow
// independently.
[[nodiscard]] bool canSuggestIncludes(const FrontendResult &result);

// Reads an immutable analysis and returns only diagnostic enrichment. Semantic
// identities remain in the original/trial snapshots and never escape this call.
[[nodiscard]] IncludeSuggestionResult discoverIncludeSuggestions(
    const FrontendResult &result, const FrontendOptions &options,
    const std::filesystem::path &entryPath,
    const std::vector<std::filesystem::path> &preludePaths,
    const std::unordered_map<std::string, std::string> &sourceOverrides,
    const std::vector<std::filesystem::path> &standardLibraryRoots,
    const std::vector<PackageSourceRoot> &packageSourceRoots);

// Enrich unresolved-name diagnostics using actual source declarations and a
// speculative frontend snapshot. Does not modify files or the semantic model.
void addIncludeSuggestions(
    FrontendResult &result, const FrontendOptions &options,
    const std::filesystem::path &entryPath,
    const std::vector<std::filesystem::path> &preludePaths,
    const std::unordered_map<std::string, std::string> &sourceOverrides,
    const std::vector<std::filesystem::path> &standardLibraryRoots,
    const std::vector<PackageSourceRoot> &packageSourceRoots);

} // namespace lang
