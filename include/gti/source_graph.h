#pragma once

#include "gti/source_owner.h"

#include "gti/diagnostic.h"
#include "gti/token.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lang {

class SourceOwnerRegistry;

using SourceUnitId = std::size_t;
using ModuleId = std::size_t;

// A language identity shared by an interface and its registered definitions.
// SourceUnitId remains the identity of a file (including source-local statics).
// Trust and physical containment belong to source ownership, not ModuleId.
struct SourceModule {
  ModuleId id = 0;
  std::string ownerIdentity;
  std::string name;
  SourceUnitId interfaceUnit = 0;
};

enum class SourceDependencyKind {
  Prelude,
  Import,
  ExportImport,
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
  SourceDependencyKind kind = SourceDependencyKind::Import;
  std::optional<SourceSpan> directive;
  std::string includeSpelling;
  std::size_t includeOccurrence = 0;
};

enum class SourceImportResolution { Resolved, Unresolved, Incomplete };

// Source loading retains imports independently of successful resolution so
// compiler queries can explain incomplete documents using the same graph.
struct SourceImport {
  SourceUnitId requester = 0;
  SourceSpan declaration;
  SourceSpan name;
  std::string spelling;
  std::vector<std::string> segments;
  std::vector<SourceSpan> segmentSpans;
  std::optional<std::string> completionPrefix;
  bool quoted = false;
  bool reexport = false;
  // Authorized canonical lookup, including an interface that does not yet
  // exist. Editors track its creation/overlay without inventing a target.
  std::optional<std::filesystem::path> lookupPath;
  std::optional<SourceUnitId> target;
  SourceImportResolution resolution = SourceImportResolution::Unresolved;
};

struct SourceUnit {
  SourceUnitId id = 0;
  ModuleId moduleId = 0;
  std::filesystem::path path;
  // Post-directive tokens for the parser. Unchanged units share them with
  // the frontend cache, so they are immutable here.
  std::shared_ptr<const std::vector<Token>> tokens;
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
  std::optional<SourceSpan> moduleDeclarationSpan;
  std::optional<Token> implementationDeclaration;
  std::optional<SourceSpan> implementationDeclarationSpan;
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

// Loading provenance, not a dependency or visibility edge. An interface may
// register private implementation roots which themselves import that interface.
struct SourceImplementation {
  SourceUnitId interfaceUnit = 0;
  SourceUnitId implementationUnit = 0;
};

class SourceGraph {
public:
  [[nodiscard]] SourceUnitId entryUnit() const;
  [[nodiscard]] const std::vector<SourceUnit> &sourceUnits() const;
  [[nodiscard]] const std::vector<SourceModule> &sourceModules() const;
  [[nodiscard]] const SourceModule *findModule(ModuleId id) const;
  [[nodiscard]] ModuleId moduleForUnit(SourceUnitId id) const;
  [[nodiscard]] bool sameModule(SourceUnitId left, SourceUnitId right) const;
  [[nodiscard]] const std::vector<SourceImport> &imports() const;
  [[nodiscard]] const std::shared_ptr<const SourceOwnerRegistry> &
  sourceOwners() const;
  [[nodiscard]] const std::vector<SourceDependency> &dependencyEdges() const;
  [[nodiscard]] const std::vector<SourceImplementation> &
  moduleImplementations() const;
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
  void addImport(SourceImport import);
  void setSourceOwners(std::shared_ptr<const SourceOwnerRegistry> owners);

  ModuleId registerModule(SourceUnitId interfaceUnit, std::string ownerIdentity,
                          std::string name);
  bool attachModuleImplementation(SourceUnitId interfaceUnit,
                                  SourceUnitId implementationUnit);

  void addPreludeRoot(SourceUnitId id);

  [[nodiscard]] bool hasDirectDependency(SourceUnitId source,
                                         SourceUnitId target) const;

  [[nodiscard]] bool hasDependencyPath(SourceUnitId source,
                                       SourceUnitId target) const;

  std::vector<SourceUnit> units;
  std::vector<SourceModule> modules;
  std::vector<SourceImport> sourceImports;
  std::shared_ptr<const SourceOwnerRegistry> owners;
  std::vector<SourceDependency> dependencies;
  std::vector<SourceUnitId> configuredPreludeRoots;
  std::vector<SourceImplementation> implementationRoots;
  std::unordered_map<std::string, SourceUnitId> unitsByPath;
  SourceUnitId entry = 0;
};

// The exact source-text identity recorded as `SourceUnit::sourceDigest` for
// loaded text. It is a stable FNV-1a digest, never zero in practice; callers
// that must not accept a collision compare the bytes as well.
[[nodiscard]] std::uint64_t sourceTextDigest(std::string_view source);

} // namespace lang
