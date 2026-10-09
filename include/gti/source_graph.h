#pragma once

#include "gti/diagnostic.h"
#include "gti/token.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
  Import,
  ExportImport,
  // An interface's deliberate republication of one legacy include unit.
  ExportInclude,
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
  // Exact source-text identity used by caches after loading. A zero digest
  // means the graph was supplied by a caller that did not establish one.
  std::uint64_t sourceDigest = 0;
  // Identity of the exact token stream presented to this unit's parser after
  // includes and configuration directives have been resolved. This changes
  // when dependency-provided flags select different syntax even if this
  // unit's own source bytes do not.
  std::uint64_t syntaxDigest = 0;
  // Token-granular inactive source spans are retained after parsing so editor
  // tooling can fade unselected branches without analysing them semantically.
  std::vector<SourceSpan> inactiveSpans;
  // Configuration syntax is resolved by the source loader before semantic
  // analysis. Retain its exact roles so tooling does not need to reconstruct
  // directive grammar from neighbouring lexical tokens.
  std::vector<ConfigurationToken> configurationTokens;
  // Source-level interfaces only; no backend or binary-module identity.
  std::optional<Token> moduleDeclaration;
  // Parser-derived declaration ranges carrying an explicit/inherited export.
  // Namespace names are retained separately so exporting one child does not
  // expose its private siblings. Invalid exports are rejected by semantics.
  std::vector<SourceSpan> exportedDeclarations;
  std::vector<SourceSpan> exportedNamespaces;
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
  // Public package-local module name -> absolute, contained interface path.
  std::vector<std::pair<std::string, std::filesystem::path>> modules;
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
  [[nodiscard]] bool isDeclarationVisible(SourceUnitId requester,
                                          SourceUnitId declaration,
                                          std::size_t nameOffset) const;
  [[nodiscard]] bool isExported(SourceUnitId declaration,
                                std::size_t nameOffset) const;
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

// The exact source-text identity recorded as `SourceUnit::sourceDigest` for
// loaded text. It is a stable FNV-1a digest, never zero in practice; callers
// that must not accept a collision compare the bytes as well.
[[nodiscard]] std::uint64_t sourceTextDigest(std::string_view source);

} // namespace lang
