#pragma once

#include "gti/diagnostic.h"
#include "gti/token.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

using SourceUnitId = std::size_t;

enum class SourceDependencyKind {
  Include,
  Prelude,
  StandardLibrary,
  Package,
};

enum class SourceUnitRole {
  Application,
  Prelude,
  StandardLibrary,
};

enum class ConfigurationTokenKind {
  Operator,
  Flag,
};

struct ConfigurationToken {
  ConfigurationTokenKind kind = ConfigurationTokenKind::Flag;
  SourceSpan span;
};

struct SourceDependency {
  SourceUnitId source = 0;
  SourceUnitId target = 0;
  SourceDependencyKind kind = SourceDependencyKind::Include;
  std::optional<SourceSpan> directive;
  std::string includeSpelling;
  std::size_t includeOccurrence = 0;
};

struct SourceUnit {
  SourceUnitId id = 0;
  std::filesystem::path path;
  std::vector<Token> tokens;
  // Token-granular inactive source spans are retained after parsing so editor
  // tooling can fade unselected branches without analysing them semantically.
  std::vector<SourceSpan> inactiveSpans;
  // Configuration syntax is resolved by the source loader before semantic
  // analysis. Retain its exact roles so tooling does not need to reconstruct
  // directive grammar from neighbouring lexical tokens.
  std::vector<ConfigurationToken> configurationTokens;
  std::size_t declarationStart = 0;
  std::size_t declarationCount = 0;
  std::optional<std::string> standardLibraryName;
  // Present for application units loaded through a manifest-owned package
  // graph. Package provenance is never compiler trust; it supplies stable
  // cache identity and direct package-include lookup only.
  std::optional<std::string> packageIdentity;
  std::optional<std::string> packageRelativePath;
  SourceUnitRole role = SourceUnitRole::Application;
  bool entry = false;
  bool prelude = false;
};

struct PackageSourceDependency {
  std::string alias;
  std::string targetIdentity;
};

struct PackageSourceRoot {
  std::string identity;
  std::string name;
  std::filesystem::path packageRoot;
  std::filesystem::path sourceRoot;
  std::vector<PackageSourceDependency> dependencies;
};

class SourceGraph {
public:
  [[nodiscard]] SourceUnitId entryUnit() const;
  [[nodiscard]] const std::vector<SourceUnit> &sourceUnits() const;
  [[nodiscard]] const std::vector<SourceDependency> &dependencyEdges() const;
  [[nodiscard]] const std::vector<SourceUnitId> &preludeRoots() const;
  [[nodiscard]] const SourceUnit *findUnit(SourceUnitId id) const;
  [[nodiscard]] SourceUnitId sourceUnitForPath(std::string_view path) const;
  [[nodiscard]] bool isCompilerTrusted(SourceUnitId id) const;
  [[nodiscard]] bool isVisible(SourceUnitId requester,
                               SourceUnitId declaration) const;
  [[nodiscard]] std::vector<SourceUnitId> compilationOrder() const;

private:
  friend class Frontend;
  friend class SourceLoader;

  [[nodiscard]] SourceUnit *findUnit(SourceUnitId id);
  void clear();

  SourceUnitId
  addUnit(std::filesystem::path path, bool isEntry, bool isPrelude,
          std::optional<std::string> standardLibraryName = std::nullopt,
          SourceUnitRole role = SourceUnitRole::Application,
          std::optional<std::string> packageIdentity = std::nullopt,
          std::optional<std::string> packageRelativePath = std::nullopt);

  void addDependency(SourceDependency dependency);

  void addPreludeRoot(SourceUnitId id);

  [[nodiscard]] bool hasDirectDependency(SourceUnitId source,
                                         SourceUnitId target) const;

  [[nodiscard]] bool hasDependencyPath(SourceUnitId source,
                                       SourceUnitId target) const;

  std::vector<SourceUnit> units;
  std::vector<SourceDependency> dependencies;
  std::vector<SourceUnitId> configuredPreludeRoots;
  std::unordered_map<std::string, SourceUnitId> unitsByPath;
  SourceUnitId entry = 0;
};

} // namespace lang
