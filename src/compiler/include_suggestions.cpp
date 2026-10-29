#include "gti/include_suggestions.h"

#include "gti/frontend.h"
#include "gti/language_queries.h"
#include "gti/lexer.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <system_error>

namespace lang {
namespace {

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
  if (unit.moduleDeclaration) {
    // Source loading has already validated the module header. Includes must
    // follow it rather than accidentally preceding the required first token.
    const auto end = source.find(';', unit.moduleDeclaration->position);
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
  for (const auto &root : standardLibraryRoots) {
    roots.push_back({root / "std", "std", {}});
  }
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
  auto searchCandidates = candidates(std::move(roots), overrides);
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
  for (const auto &candidate : searchCandidates) {
    if (entryUnit != nullptr && candidate.path == entryUnit->path)
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
    // Cheap lexical prefilter avoids loading dependencies for unrelated files.
    Lexer lexer;
    const auto tokens = lexer.scan(text, candidate.path.string());
    if (lexer.hadError() ||
        std::none_of(tokens.begin(), tokens.end(), [&](const auto &token) {
          return requested.contains(token.lexeme);
        }))
      continue;

    trialOptions.stopAfter = FrontendPhase::Parsing;
    auto parsed = Frontend(trialOptions)
                      .analyze(candidate.path, text, preludePaths, overrides,
                               standardLibraryRoots, packageSourceRoots);
    for (const auto &unit : parsed.sourceGraph.sourceUnits()) {
      result.includeSuggestionInputs.push_back(unit.path);
    }
    const auto *candidateUnit = std::as_const(parsed.sourceGraph)
                                    .findUnit(parsed.sourceGraph.entryUnit());
    if (!candidateUnit || !candidateUnit->moduleDeclaration ||
        !parsed.syntaxValid)
      continue;
    StmtList declarations;
    const auto &all = parsed.program.declarations();
    for (std::size_t index = candidateUnit->declarationStart;
         index <
         candidateUnit->declarationStart + candidateUnit->declarationCount;
         ++index) {
      declarations.push_back(all[index]);
    }
    std::set<std::string> names;
    declarationNames(declarations, options.target, names);
    std::map<std::string, FrontendResult> trials;
    for (auto &diagnostic : result.diagnostics) {
      if (diagnostic.missingIncludeName.empty() ||
          !names.contains(diagnostic.missingIncludeName))
        continue;
      const auto &sourceName = diagnostic.primary.source;
      const auto *unit =
          std::as_const(result.sourceGraph)
              .findUnit(result.sourceGraph.sourceUnitForPath(sourceName));
      const auto *source = result.sources.find(sourceName);
      if (!unit || !source || unit->path == candidate.path || unit->prelude)
        continue;
      const auto relative =
          candidate.path.lexically_relative(unit->path.parent_path())
              .generic_string();
      std::string spelling = "\"" + relative + "\"";
      if (candidate.angleName) {
        const auto slash = candidate.angleName->find('/');
        if (slash == std::string::npos)
          continue;
        spelling = candidate.angleName->substr(0, slash) + "." +
                   candidateUnit->moduleDeclaration->lexeme;
      }
      if (spelling.find_first_of("\r\n") != std::string::npos ||
          (!candidate.angleName && relative.find('"') != std::string::npos))
        continue;
      const auto insertion = insertionOffset(*source, *unit);
      const std::string newline =
          source->find("\r\n") != std::string::npos ? "\r\n" : "\n";
      const std::string directive = "import " + spelling + ";";
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
      if (!definition || definition->target.source != candidate.path.string())
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
