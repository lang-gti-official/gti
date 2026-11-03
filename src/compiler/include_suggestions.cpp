#include "gti/include_suggestions.h"

#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"
#include "gti/source_owner.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

namespace lang {

class IncludeDiscoveryCache::Rows {
public:
  // Every token spelling of one exact source text. Rows are immutable.
  struct Lexicon {
    std::uint64_t digest = 0;
    std::vector<std::string> spellings; // sorted and unique
    std::size_t reexports = 0;
    bool hadError = false;

    [[nodiscard]] bool spells(std::string_view name) const {
      return std::binary_search(spellings.begin(), spellings.end(), name);
    }
  };

  // The parse-only load of one candidate interface and its export surface.
  struct Scan {
    struct Input {
      std::string path;
      std::uint64_t digest = 0;
    };
    std::string context;
    // Every unit the load read, with the digest of the text it read.
    std::vector<Input> inputs;
    // A syntactically valid module interface.
    bool usable = false;
    std::set<std::string> names;
    std::set<std::string> surfacePaths;
    // Token spellings of every surface unit; incomplete when a surface text
    // was unavailable, and then no use is ruled out by its qualifiers.
    std::vector<std::shared_ptr<const Lexicon>> surfaceLexicons;
    bool surfaceSpellingsComplete = true;
  };

  explicit Rows(IncludeDiscoveryCacheLimits limits) : limits(limits) {}

  [[nodiscard]] std::shared_ptr<const Lexicon> lexicon(const std::string &path,
                                                       std::uint64_t digest) {
    const std::lock_guard lock(mutex);
    const auto found = lexicons.find(path);
    if (found == lexicons.end() || found->second.row->digest != digest) {
      ++counters.lexiconMisses;
      return nullptr;
    }
    ++counters.lexiconHits;
    found->second.used = ++clock;
    return found->second.row;
  }

  void store(const std::string &path, std::shared_ptr<const Lexicon> row) {
    const std::lock_guard lock(mutex);
    lexicons[path] = {std::move(row), ++clock};
    evictLocked(lexicons);
  }

  [[nodiscard]] std::shared_ptr<const Scan> scan(const std::string &path) {
    const std::lock_guard lock(mutex);
    const auto found = scans.find(path);
    if (found == scans.end())
      return nullptr;
    found->second.used = ++clock;
    return found->second.row;
  }

  void store(const std::string &path, std::shared_ptr<const Scan> row) {
    const std::lock_guard lock(mutex);
    scans[path] = {std::move(row), ++clock};
    evictLocked(scans);
  }

  void countScan(bool reused) {
    const std::lock_guard lock(mutex);
    ++(reused ? counters.scanHits : counters.scanMisses);
  }

  void countTrial(bool skipped) {
    const std::lock_guard lock(mutex);
    ++(skipped ? counters.qualifierSkips : counters.trials);
  }

  void invalidate(std::string_view path) {
    const std::lock_guard lock(mutex);
    lexicons.erase(std::string(path));
    std::erase_if(scans, [&](const auto &entry) {
      return std::any_of(entry.second.row->inputs.begin(),
                         entry.second.row->inputs.end(),
                         [&](const auto &input) { return input.path == path; });
    });
  }

  void clear() {
    const std::lock_guard lock(mutex);
    lexicons.clear();
    scans.clear();
  }

  [[nodiscard]] IncludeDiscoveryCacheStats stats() const {
    const std::lock_guard lock(mutex);
    auto result = counters;
    result.lexiconEntries = lexicons.size();
    result.scanEntries = scans.size();
    return result;
  }

private:
  template <typename Row> struct Slot {
    std::shared_ptr<const Row> row;
    std::uint64_t used = 0;
  };

  template <typename Row>
  void evictLocked(std::unordered_map<std::string, Slot<Row>> &rows) {
    while (rows.size() > std::max<std::size_t>(limits.maximumEntries, 1)) {
      rows.erase(std::min_element(rows.begin(), rows.end(),
                                  [](const auto &left, const auto &right) {
                                    return left.second.used < right.second.used;
                                  }));
    }
  }

  mutable std::mutex mutex;
  IncludeDiscoveryCacheLimits limits;
  std::uint64_t clock = 0;
  std::unordered_map<std::string, Slot<Lexicon>> lexicons;
  std::unordered_map<std::string, Slot<Scan>> scans;
  IncludeDiscoveryCacheStats counters;
};

IncludeDiscoveryCache::IncludeDiscoveryCache(IncludeDiscoveryCacheLimits limits)
    : storage(std::make_unique<Rows>(limits)) {}

IncludeDiscoveryCache::~IncludeDiscoveryCache() = default;

void IncludeDiscoveryCache::invalidate(std::string_view canonicalPath) {
  storage->invalidate(canonicalPath);
}

void IncludeDiscoveryCache::clear() { storage->clear(); }

IncludeDiscoveryCacheStats IncludeDiscoveryCache::stats() const {
  return storage->stats();
}

namespace {

using Lexicon = IncludeDiscoveryCache::Rows::Lexicon;
using CandidateScan = IncludeDiscoveryCache::Rows::Scan;

std::filesystem::path canonicalSourcePath(const std::filesystem::path &path) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : result;
}

bool within(const std::filesystem::path &path,
            const std::filesystem::path &root) {
  auto part = path.begin();
  for (const auto &component : root) {
    if (part == path.end() || *part++ != component) {
      return false;
    }
  }
  return true;
}

bool excluded(const std::filesystem::path &path,
              const IncludeSearchRoot &root) {
  if (!within(path, root.directory)) {
    return true;
  }
  for (const auto &directory : root.excludedDirectories) {
    if (within(path, directory)) {
      return true;
    }
  }
  for (const auto &component : path.lexically_relative(root.directory)) {
    const auto name = component.string();
    if (name.starts_with('.') || name == "build" ||
        name.starts_with("build-") || name.starts_with("cmake-build-")) {
      return true;
    }
  }
  return false;
}

struct Candidate {
  std::filesystem::path path;
  std::optional<std::string> angleName;
};

std::optional<std::string> importSpelling(const SourceGraph &graph,
                                          const SourceUnit &requester,
                                          const std::filesystem::path &target) {
  const auto registry = graph.sourceOwners();
  const auto *owner =
      registry ? registry->ownerForPath(requester.path) : nullptr;
  const auto *targetOwner = registry ? registry->ownerForPath(target) : nullptr;
  if (!registry ||
      (owner && targetOwner && owner->identity == targetOwner->identity)) {
    const auto relative =
        target.lexically_relative(requester.path.parent_path())
            .generic_string();
    if (relative.find_first_of("\"\r\n") != std::string::npos)
      return std::nullopt;
    return "\"" + relative + "\"";
  }
  std::set<std::string> names;
  const auto add = [&](const SourceOwnerDescriptor &selected,
                       const std::string &alias) {
    for (const auto &[name, path] : selected.modules) {
      if (path == target) {
        names.insert(alias + "." + name);
        if (name == selected.defaultModule)
          names.insert(alias);
      }
    }
  };
  if (owner)
    for (const auto &dependency : owner->dependencies)
      if (const auto *selected = registry->find(dependency.targetIdentity))
        add(*selected, dependency.alias);
  for (const auto &selected : registry->owners())
    if (selected.implicitAlias)
      add(selected, *selected.implicitAlias);
  if (names.empty())
    return std::nullopt;
  return *names.begin();
}

std::vector<Candidate>
candidates(std::vector<IncludeSearchRoot> roots,
           const std::unordered_map<std::string, std::string> &overrides) {
  std::map<std::pair<std::string, std::string>, Candidate> found;
  for (auto &root : roots) {
    root.directory = canonicalSourcePath(root.directory);
    for (auto &directory : root.excludedDirectories) {
      directory = canonicalSourcePath(directory);
    }
    const auto add = [&](const std::filesystem::path &spelling) {
      const auto path = canonicalSourcePath(spelling);
      if (path.extension() != ".gti" || excluded(path, root)) {
        return;
      }
      for (auto parent = path.parent_path();
           parent != root.directory && !parent.empty() &&
           parent != parent.parent_path();
           parent = parent.parent_path()) {
        std::error_code error;
        if (std::filesystem::exists(parent / "gti.toml", error))
          return;
      }
      std::optional<std::string> angle;
      if (root.anglePrefix) {
        auto relative = path.lexically_relative(root.directory);
        relative.replace_extension();
        angle = *root.anglePrefix + "/" + relative.generic_string();
      }
      found.emplace(std::pair(path.string(), angle.value_or("")),
                    Candidate{path, std::move(angle)});
    };
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        root.directory,
        std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && it != end) {
      const auto path = it->path();
      if (excluded(canonicalSourcePath(path), root)) {
        it.disable_recursion_pending();
      } else if (it->is_directory(error) &&
                 std::filesystem::exists(path / "gti.toml", error)) {
        // Nested packages are separate search authorities; a declared direct
        // dependency receives its own root and angle alias from the driver.
        it.disable_recursion_pending();
      } else if (it->is_regular_file(error)) {
        add(path);
      }
      it.increment(error);
    }
    // New, unsaved project files participate even before they exist on disk.
    for (const auto &[path, source] : overrides) {
      (void)source;
      add(path);
    }
  }
  std::vector<Candidate> result;
  for (auto &[key, candidate] : found) {
    (void)key;
    result.push_back(std::move(candidate));
  }
  return result;
}

// This is only a syntax-level candidate filter. It never resolves a name,
// evaluates a constraint, or grants access. The speculative frontend below
// proves the actual declaration identity at the unresolved source occurrence.
void declarationNames(const StmtList &statements, const TargetInfo &target,
                      std::set<std::string> &names) {
  for (const auto &statement : statements) {
    if (const auto *decl = stmtAs<NamespaceDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
      declarationNames(decl->declarations(), target, names);
    } else if (const auto *decl = stmtAs<ClassDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
      declarationNames(decl->members(), target, names);
    } else if (const auto *decl = stmtAs<EnumDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
      for (const auto &enumerator : decl->enumerators()) {
        names.insert(enumerator.name.lexeme);
      }
    } else if (const auto *decl = stmtAs<FunctionDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
    } else if (const auto *decl = stmtAs<VariableDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
    } else if (const auto *decl = stmtAs<TypeAliasDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
    } else if (const auto *decl = stmtAs<ConceptDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
    } else if (const auto *decl = stmtAs<NamespaceAliasDecl>(statement.get())) {
      names.insert(decl->name().lexeme);
    } else if (const auto *decl = stmtAs<ExternCDecl>(statement.get())) {
      declarationNames(decl->declarations(), target, names);
    } else if (const auto *decl = stmtAs<ConditionalStmt>(statement.get())) {
      if (const auto *active = decl->activeBranch(target)) {
        declarationNames(*active, target, names);
      }
    }
  }
}

std::size_t insertionOffset(const std::string &source, const SourceUnit &unit) {
  std::size_t offset = source.starts_with("\xef\xbb\xbf") ? 3 : 0;
  const auto &header = unit.moduleDeclaration ? unit.moduleDeclaration
                                              : unit.implementationDeclaration;
  if (header) {
    // Source loading has already validated the module header. Imports must
    // follow it rather than accidentally preceding the required first token.
    const auto end = source.find(';', header->position);
    if (end != std::string::npos) {
      offset = end + 1;
      if (offset < source.size() && source[offset] == '\r')
        ++offset;
      if (offset < source.size() && source[offset] == '\n')
        ++offset;
    }
  }
  return offset;
}

bool introducesError(const FrontendResult &trial, const FrontendResult &base,
                     std::string_view editedSource, std::size_t insertion,
                     std::size_t added) {
  for (const auto &diagnostic : trial.diagnostics) {
    if (diagnostic.severity != DiagnosticSeverity::Error)
      continue;
    auto span = diagnostic.primary;
    if (span.source == editedSource && span.start >= insertion) {
      if (span.start < insertion + added)
        return true;
      span.start -= added;
      span.end -= added;
    }
    const bool existed = std::any_of(
        base.diagnostics.begin(), base.diagnostics.end(), [&](const auto &old) {
          return old.code == diagnostic.code &&
                 old.message == diagnostic.message &&
                 old.primary.source == span.source &&
                 old.primary.start == span.start && old.primary.end == span.end;
        });
    if (!existed)
      return true;
  }
  return false;
}

// The text a speculative load would read for each path in this discovery:
// the frozen snapshot or an unsaved buffer first, otherwise the file.
class CurrentSources {
public:
  explicit CurrentSources(
      const std::unordered_map<std::string, std::string> &overrides)
      : overrides(overrides) {}

  void remember(const std::string &path, std::uint64_t digest) {
    digests.insert_or_assign(path, digest);
  }

  [[nodiscard]] std::optional<std::uint64_t> digest(const std::string &path) {
    if (const auto found = digests.find(path); found != digests.end())
      return found->second;
    std::optional<std::uint64_t> result;
    if (const auto found = overrides.find(path); found != overrides.end()) {
      result = sourceTextDigest(found->second);
    } else if (std::ifstream input(path, std::ios::binary); input) {
      const std::string text(std::istreambuf_iterator<char>(input), {});
      if (!input.bad())
        result = sourceTextDigest(text);
    }
    digests.emplace(path, result);
    return result;
  }

private:
  const std::unordered_map<std::string, std::string> &overrides;
  std::unordered_map<std::string, std::optional<std::uint64_t>> digests;
};

std::shared_ptr<const Lexicon> lexiconFor(IncludeDiscoveryCache::Rows &rows,
                                          const std::string &path,
                                          const std::string &text,
                                          std::uint64_t digest) {
  if (auto row = rows.lexicon(path, digest))
    return row;
  Lexer lexer;
  const auto tokens = lexer.scan(text, path);
  auto row = std::make_shared<Lexicon>();
  row->digest = digest;
  row->hadError = lexer.hadError();
  for (std::size_t index = 1; index < tokens.size(); ++index)
    if (tokens[index - 1].kind == TokenKind::EXPORT &&
        tokens[index].kind == TokenKind::IMPORT)
      ++row->reexports;
  row->spellings.reserve(tokens.size());
  for (const auto &token : tokens)
    row->spellings.push_back(token.lexeme);
  std::sort(row->spellings.begin(), row->spellings.end());
  row->spellings.erase(
      std::unique(row->spellings.begin(), row->spellings.end()),
      row->spellings.end());
  rows.store(path, row);
  return row;
}

// Everything besides source text that a candidate's parse-only load reads:
// syntax configuration, prelude and library roots, and the source owners
// that resolve module names. Owner identity is root-relative, so the roots
// themselves are part of the context.
std::string
scanContext(const FrontendOptions &options, const SourceGraph &graph,
            const std::vector<std::filesystem::path> &preludePaths,
            const std::vector<std::filesystem::path> &standardLibraryRoots) {
  std::string result = frontendCacheContext(options);
  const auto append = [&](std::string_view label, std::string_view value) {
    result += '\n';
    result += label;
    result += std::to_string(value.size());
    result += ':';
    result += value;
  };
  for (const auto &path : preludePaths)
    append("prelude:", canonicalSourcePath(path).string());
  for (const auto &root : standardLibraryRoots)
    append("library:", canonicalSourcePath(root).string());
  if (const auto registry = graph.sourceOwners()) {
    append("direct:", registry->isDirectMode() ? "1" : "0");
    append("owners:", registry->resolutionIdentity());
    for (const auto &owner : registry->owners()) {
      append("owner:", owner.identity);
      append("package-root:", owner.packageRoot.string());
      append("source-root:", owner.sourceRoot.string());
      for (const auto &root : owner.additionalRoots)
        append("additional-root:", root.string());
    }
  }
  return result;
}

bool reusable(const CandidateScan &scan, const std::string &context,
              CurrentSources &current) {
  return scan.context == context &&
         std::all_of(scan.inputs.begin(), scan.inputs.end(),
                     [&](const auto &input) {
                       return current.digest(input.path) == input.digest;
                     });
}

struct QualifierComponent {
  std::string name;
  std::size_t position = 0;
};

// The identifiers written before a missing name, `a::b::name` giving `b`
// and `a`. Stops at the first component that is not a plain identifier,
// such as a generic argument list or a global `::`.
std::vector<QualifierComponent>
writtenQualifiers(const std::vector<Token> &tokens,
                  const Diagnostic &diagnostic) {
  const auto use =
      std::lower_bound(tokens.begin(), tokens.end(), diagnostic.primary.start,
                       [](const Token &token, std::size_t start) {
                         return token.position < start;
                       });
  if (use == tokens.end() || use->position != diagnostic.primary.start ||
      use->lexeme != diagnostic.missingIncludeName)
    return {};
  std::vector<QualifierComponent> result;
  for (auto index = static_cast<std::size_t>(use - tokens.begin());
       index >= 2 && tokens[index - 1].kind == TokenKind::SCOPE &&
       tokens[index - 2].kind == TokenKind::IDENTIFIER;
       index -= 2)
    result.push_back({tokens[index - 2].lexeme, tokens[index - 2].position});
  return result;
}

// For each diagnostic, the written qualifiers that no source of the analysed
// program spells anywhere except as a qualifier of a use being repaired.
// Such a name resolves after an import only through a declaration in the
// imported surface, which must then spell it. A qualifier the program
// spells elsewhere (its own namespace, an alias, a local declaration) may
// already resolve and imposes nothing.
std::vector<std::vector<std::string>>
unavailableQualifiers(const FrontendResult &result,
                      IncludeDiscoveryCache::Rows &rows) {
  std::vector<std::vector<std::string>> required(result.diagnostics.size());
  std::vector<std::vector<QualifierComponent>> written(
      result.diagnostics.size());
  std::map<std::string, std::vector<Token>> requesters;
  std::map<std::string, std::set<std::size_t>> qualifierPositions;
  std::set<std::string> components;
  for (std::size_t index = 0; index < result.diagnostics.size(); ++index) {
    const auto &diagnostic = result.diagnostics[index];
    if (diagnostic.missingIncludeName.empty())
      continue;
    const auto *source = result.sources.find(diagnostic.primary.source);
    if (!source)
      continue;
    auto [tokens, inserted] = requesters.try_emplace(diagnostic.primary.source);
    if (inserted)
      tokens->second = Lexer().scan(*source, diagnostic.primary.source);
    written[index] = writtenQualifiers(tokens->second, diagnostic);
    for (const auto &component : written[index]) {
      qualifierPositions[diagnostic.primary.source].insert(component.position);
      components.insert(component.name);
    }
  }
  if (components.empty())
    return required;
  std::set<std::string> spelled;
  std::set<std::string> visited;
  for (const auto &unit : result.sourceGraph.sourceUnits()) {
    const auto path = unit.path.string();
    const auto *text = result.sources.find(path);
    if (!text)
      return required; // An unknown program text keeps every candidate.
    visited.insert(path);
    if (const auto tokens = requesters.find(path); tokens != requesters.end()) {
      const auto &positions = qualifierPositions[path];
      for (const auto &token : tokens->second)
        if (components.contains(token.lexeme) &&
            !positions.contains(token.position))
          spelled.insert(token.lexeme);
      continue;
    }
    const auto lexicon = lexiconFor(rows, path, *text, sourceTextDigest(*text));
    for (const auto &component : components)
      if (lexicon->spells(component))
        spelled.insert(component);
  }
  for (std::size_t index = 0; index < written.size(); ++index) {
    if (!visited.contains(result.diagnostics[index].primary.source))
      continue;
    for (const auto &component : written[index])
      if (!spelled.contains(component.name))
        required[index].push_back(component.name);
  }
  return required;
}

// Loads a candidate interface only through parsing and records its export
// surface: the interface plus everything it re-exports. A row is stored
// only for a complete load whose imports all resolved and whose units all
// still have the text they were loaded from, so no absent file, racing
// write or cancelled load is remembered.
std::shared_ptr<const CandidateScan>
scanCandidate(IncludeDiscoveryCache::Rows &rows, CurrentSources &current,
              const std::string &context, FrontendOptions options,
              const std::filesystem::path &candidate, const std::string &text,
              const std::vector<std::filesystem::path> &preludePaths,
              const std::unordered_map<std::string, std::string> &overrides,
              const std::vector<std::filesystem::path> &standardLibraryRoots,
              const std::vector<PackageSourceRoot> &packageSourceRoots) {
  const TargetInfo target = options.target;
  options.stopAfter = FrontendPhase::Parsing;
  const auto parsed = Frontend(std::move(options))
                          .analyze(candidate, text, preludePaths, overrides,
                                   standardLibraryRoots, packageSourceRoots);
  auto scan = std::make_shared<CandidateScan>();
  scan->context = context;
  for (const auto &unit : parsed.sourceGraph.sourceUnits())
    scan->inputs.push_back({unit.path.string(), unit.sourceDigest});
  const auto *candidateUnit = std::as_const(parsed.sourceGraph)
                                  .findUnit(parsed.sourceGraph.entryUnit());
  scan->usable =
      candidateUnit && candidateUnit->moduleDeclaration && parsed.syntaxValid;
  if (scan->usable) {
    StmtList declarations;
    const auto &all = parsed.program.declarations();
    std::set<SourceUnitId> surface{candidateUnit->id};
    std::vector<SourceUnitId> pending{candidateUnit->id};
    while (!pending.empty()) {
      const auto id = pending.back();
      pending.pop_back();
      const auto *unit = std::as_const(parsed.sourceGraph).findUnit(id);
      if (!unit)
        continue;
      const auto path = unit->path.string();
      scan->surfacePaths.insert(path);
      if (const auto *source = parsed.sources.find(path))
        scan->surfaceLexicons.push_back(
            lexiconFor(rows, path, *source, sourceTextDigest(*source)));
      else
        scan->surfaceSpellingsComplete = false;
      for (std::size_t index = unit->declarationStart;
           index < unit->declarationStart + unit->declarationCount; ++index)
        declarations.push_back(all[index]);
      for (const auto &edge : parsed.sourceGraph.dependencyEdges())
        if (edge.source == id &&
            edge.kind == SourceDependencyKind::ExportImport &&
            surface.insert(edge.target).second)
          pending.push_back(edge.target);
    }
    declarationNames(declarations, target, scan->names);
  }
  const auto &imports = parsed.sourceGraph.imports();
  if (!parsed.cancelled && parsed.sourceValid &&
      std::all_of(imports.begin(), imports.end(),
                  [](const auto &import) {
                    return import.resolution ==
                           SourceImportResolution::Resolved;
                  }) &&
      std::all_of(scan->inputs.begin(), scan->inputs.end(),
                  [&](const auto &input) {
                    return current.digest(input.path) == input.digest;
                  }))
    rows.store(candidate.string(), scan);
  return scan;
}

} // namespace

void addIncludeSuggestions(
    FrontendResult &result, const FrontendOptions &options,
    const std::filesystem::path &entryPath,
    const std::vector<std::filesystem::path> &preludePaths,
    const std::unordered_map<std::string, std::string> &sourceOverrides,
    const std::vector<std::filesystem::path> &standardLibraryRoots,
    const std::vector<PackageSourceRoot> &packageSourceRoots) {
  std::set<std::string> requested;
  for (const auto &diagnostic : result.diagnostics) {
    if (!diagnostic.missingIncludeName.empty()) {
      requested.insert(diagnostic.missingIncludeName);
    }
  }
  // Adding an import cannot repair syntax in the frozen source graph. Every
  // speculative edit below already requires valid syntax, so recovered parser
  // errors cannot produce a fix and must not trigger a workspace-wide search.
  if (requested.empty() || !result.sourceValid || !result.syntaxValid)
    return;
  result.includeSuggestionsRequested = true;

  auto roots = options.includeSearchRoots;
  // Cross-owner discovery is defined by public maps, not a directory scan.
  roots.erase(std::remove_if(roots.begin(), roots.end(),
                             [](const auto &root) {
                               return root.anglePrefix.has_value();
                             }),
              roots.end());
  auto overrides = sourceOverrides;
  // Freeze the original loaded graph, not a mixture of a new disk snapshot
  // and the source that produced the diagnostic being repaired.
  for (const auto &name : result.sources.names()) {
    overrides[name] = *result.sources.find(name);
  }
  FrontendOptions trialOptions = options;
  // Header probes and proposed-fix checks are internal diagnostic work, not
  // additional user compilations. Keep their phases out of the caller's sink.
  trialOptions.progress = nullptr;
  trialOptions.suggestIncludes = false;
  trialOptions.requireEntryPoint = false;
  trialOptions.toolingOccurrences = true;
  trialOptions.workMetrics = nullptr;
  trialOptions.completionOffset.reset();
  // Reuse the bounded parsed-unit cache, never semantic identities.
  if (!trialOptions.cache)
    trialOptions.cache = std::make_shared<FrontendCache>();
  // Candidate facts that depend only on source text may come from earlier
  // discoveries; every row is checked against the text read now.
  const auto cache = options.includeDiscoveryCache
                         ? options.includeDiscoveryCache
                         : std::make_shared<IncludeDiscoveryCache>();
  auto &rows = cache->rows();
  CurrentSources current(overrides);
  trialOptions.stopAfter = FrontendPhase::Parsing;
  const std::string context = scanContext(trialOptions, result.sourceGraph,
                                          preludePaths, standardLibraryRoots);
  const auto qualifiers = unavailableQualifiers(result, rows);
  auto searchCandidates = candidates(std::move(roots), overrides);
  if (const auto registry = result.sourceGraph.sourceOwners()) {
    for (const auto &owner : registry->owners()) {
      for (const auto &[name, path] : owner.modules) {
        (void)name;
        if (std::none_of(
                searchCandidates.begin(), searchCandidates.end(),
                [&](const auto &candidate) { return candidate.path == path; }))
          searchCandidates.push_back({path, std::nullopt});
      }
    }
  }
  // Loaded-but-invisible declarations already belong to this compilation's
  // source authority, including virtual/overlay-only compiler-library inputs.
  // Keep their module-import advice even when no project search was requested.
  for (const auto &unit : result.sourceGraph.sourceUnits()) {
    if (unit.entry || unit.prelude || !unit.moduleDeclaration)
      continue;
    if (std::none_of(searchCandidates.begin(), searchCandidates.end(),
                     [&](const auto &candidate) {
                       return candidate.path == unit.path;
                     })) {
      searchCandidates.push_back({unit.path, unit.standardLibraryName});
    }
  }
  std::sort(searchCandidates.begin(), searchCandidates.end(),
            [](const auto &a, const auto &b) {
              return std::tie(a.path, a.angleName) <
                     std::tie(b.path, b.angleName);
            });
  // The analysis entry is never proposed: importing it from a unit it reaches
  // forms a cycle, and the program root is not a library unit. It usually
  // spells the missing name, so probing it would reload and parse its whole
  // graph on every such edit.
  const SourceUnit *entryUnit = std::as_const(result.sourceGraph)
                                    .findUnit(result.sourceGraph.entryUnit());
  std::vector<std::pair<std::size_t, Candidate>> prepared;
  for (const auto &candidate : searchCandidates) {
    if (entryUnit != nullptr && candidate.path == entryUnit->path)
      continue;
    const bool accessible = std::any_of(
        result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto &diagnostic) {
          const auto *requester =
              std::as_const(result.sourceGraph)
                  .findUnit(result.sourceGraph.sourceUnitForPath(
                      diagnostic.primary.source));
          return requester &&
                 importSpelling(result.sourceGraph, *requester, candidate.path)
                     .has_value();
        });
    if (!accessible)
      continue;
    result.includeSuggestionInputs.push_back(candidate.path);
    std::string text;
    if (const auto found = overrides.find(candidate.path.string());
        found != overrides.end()) {
      text = found->second;
    } else {
      std::ifstream input(candidate.path, std::ios::binary);
      if (!input)
        continue;
      text.assign(std::istreambuf_iterator<char>(input), {});
      overrides[candidate.path.string()] = text;
    }
    const auto digest = sourceTextDigest(text);
    current.remember(candidate.path.string(), digest);
    // Cheap lexical prefilter avoids loading dependencies for unrelated files.
    const auto lexicon =
        lexiconFor(rows, candidate.path.string(), text, digest);
    if (lexicon->hadError ||
        (!lexicon->reexports &&
         std::none_of(requested.begin(), requested.end(),
                      [&](const auto &name) { return lexicon->spells(name); })))
      continue;

    prepared.emplace_back(lexicon->reexports, candidate);
  }
  // Prefer direct public APIs and smaller facades. Once a precise import is
  // proved, loading an aggregate facade adds visibility and expensive trials
  // without repairing anything else at that occurrence.
  std::sort(prepared.begin(), prepared.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.first, left.second.path) <
                     std::tie(right.first, right.second.path);
            });
  for (const auto &[reexports, candidate] : prepared) {
    if (reexports &&
        std::none_of(result.diagnostics.begin(), result.diagnostics.end(),
                     [](const auto &diagnostic) {
                       return !diagnostic.missingIncludeName.empty() &&
                              diagnostic.fixes.empty();
                     }))
      continue;
    const auto &text = overrides.at(candidate.path.string());

    auto scan = rows.scan(candidate.path.string());
    const bool reused = scan && reusable(*scan, context, current);
    rows.countScan(reused);
    if (!reused)
      scan = scanCandidate(rows, current, context, trialOptions, candidate.path,
                           text, preludePaths, overrides, standardLibraryRoots,
                           packageSourceRoots);
    for (const auto &input : scan->inputs) {
      result.includeSuggestionInputs.push_back(input.path);
    }
    if (!scan->usable)
      continue;
    const auto &names = scan->names;
    const auto &surfacePaths = scan->surfacePaths;
    std::map<std::string, FrontendResult> trials;
    for (std::size_t index = 0; index < result.diagnostics.size(); ++index) {
      auto &diagnostic = result.diagnostics[index];
      if (diagnostic.missingIncludeName.empty() ||
          (reexports && !diagnostic.fixes.empty()) ||
          !names.contains(diagnostic.missingIncludeName))
        continue;
      // A qualifier the program cannot otherwise resolve must come from this
      // surface, so a surface that never spells it cannot repair the use.
      if (scan->surfaceSpellingsComplete &&
          std::any_of(qualifiers[index].begin(), qualifiers[index].end(),
                      [&](const auto &name) {
                        return std::none_of(scan->surfaceLexicons.begin(),
                                            scan->surfaceLexicons.end(),
                                            [&](const auto &lexicon) {
                                              return lexicon->spells(name);
                                            });
                      })) {
        rows.countTrial(true);
        continue;
      }
      const auto &sourceName = diagnostic.primary.source;
      const auto *unit =
          std::as_const(result.sourceGraph)
              .findUnit(result.sourceGraph.sourceUnitForPath(sourceName));
      const auto *source = result.sources.find(sourceName);
      if (!unit || !source || unit->path == candidate.path || unit->prelude)
        continue;
      const auto spelling =
          importSpelling(result.sourceGraph, *unit, candidate.path);
      if (!spelling)
        continue;
      const auto insertion = insertionOffset(*source, *unit);
      const std::string newline =
          source->find("\r\n") != std::string::npos ? "\r\n" : "\n";
      const std::string directive = "import " + *spelling + ";";
      const std::string replacement =
          (insertion && (*source)[insertion - 1] == ';' ? newline : "") +
          directive + newline;
      auto found = trials.find(sourceName);
      if (found == trials.end()) {
        auto modified = overrides;
        modified[sourceName] = *source;
        modified[sourceName].insert(insertion, replacement);
        trialOptions.stopAfter = options.stopAfter == FrontendPhase::Mir
                                     ? FrontendPhase::Hir
                                     : options.stopAfter;
        auto trial =
            Frontend(trialOptions)
                .analyze(entryPath, std::nullopt, preludePaths, modified,
                         standardLibraryRoots, packageSourceRoots);
        rows.countTrial(false);
        found = trials.emplace(sourceName, std::move(trial)).first;
      }
      const auto &trial = found->second;
      if (!trial.sourceValid || !trial.syntaxValid ||
          introducesError(trial, result, sourceName, insertion,
                          replacement.size()))
        continue;
      const auto useOffset =
          diagnostic.primary.start +
          (diagnostic.primary.start >= insertion ? replacement.size() : 0);
      const auto definition = LanguageQueries().definition(
          trial, trial.sourceGraph.sourceUnitForPath(sourceName), useOffset);
      if (!definition)
        continue;
      const auto *symbol =
          trial.semantics.database().findSymbol(definition->symbol);
      const SourceSpan &selected =
          symbol ? symbol->nameSpan : definition->target;
      const auto selectedUnit =
          trial.sourceGraph.sourceUnitForPath(selected.source);
      if (!surfacePaths.contains(selected.source) ||
          !trial.sourceGraph.isExported(selectedUnit, selected.start))
        continue;
      diagnostic.fixes.push_back(
          {.span = {sourceName, insertion, insertion,
                    static_cast<int>(
                        result.sources
                            .locate({sourceName, insertion, insertion, 1})
                            .line)},
           .replacement = replacement,
           .message = "Add " + directive});
    }
  }
  for (auto &diagnostic : result.diagnostics) {
    if (diagnostic.missingIncludeName.empty() || diagnostic.fixes.empty())
      continue;
    if (diagnostic.code != "GTI-S2024")
      diagnostic.code = "GTI-S2089";
    std::sort(diagnostic.fixes.begin(), diagnostic.fixes.end(),
              [](const auto &left, const auto &right) {
                return left.replacement < right.replacement;
              });
    for (auto &fix : diagnostic.fixes) {
      fix.preferred = diagnostic.fixes.size() == 1;
      const std::string hint = fix.message + " to this source file.";
      if (std::find(diagnostic.hints.begin(), diagnostic.hints.end(), hint) ==
          diagnostic.hints.end()) {
        diagnostic.hints.push_back(hint);
      }
    }
  }
  std::sort(result.includeSuggestionInputs.begin(),
            result.includeSuggestionInputs.end());
  result.includeSuggestionInputs.erase(
      std::unique(result.includeSuggestionInputs.begin(),
                  result.includeSuggestionInputs.end()),
      result.includeSuggestionInputs.end());
}

} // namespace lang
