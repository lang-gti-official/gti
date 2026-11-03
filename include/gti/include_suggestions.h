#pragma once

#include "gti/diagnostic.h"
#include "gti/source_owner.h"

#include <filesystem>
#include <optional>
#include <string>
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
