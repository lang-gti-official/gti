#include "gti/source_loader.h"

#include "diagnostic_registry.h"

#include "configuration_directive_processor.h"
#include "loaded_lexical_unit.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace lang {
namespace {

void digestInteger(std::uint64_t &digest, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    digest ^= static_cast<unsigned char>(value >> shift);
    digest *= 1099511628211ULL;
  }
}

void digestText(std::uint64_t &digest, std::string_view value) {
  digestInteger(digest, value.size());
  for (const unsigned char byte : value) {
    digest ^= byte;
    digest *= 1099511628211ULL;
  }
}

void digestLiteral(std::uint64_t &digest, const Literal &literal) {
  digestInteger(digest, literal.index());
  std::visit(
      [&](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::uint64_t>) {
          digestInteger(digest, value);
        } else if constexpr (std::is_same_v<Value, BinaryFloat>) {
          digestInteger(digest, value.bits);
          digestInteger(digest, static_cast<std::uint64_t>(value.format));
        } else if constexpr (std::is_same_v<Value, CharacterLiteral>) {
          digestInteger(digest, value.value);
        } else if constexpr (std::is_same_v<Value, std::string>) {
          digestText(digest, value);
        } else if constexpr (std::is_same_v<Value, bool>) {
          digestInteger(digest, value ? 1 : 0);
        }
      },
      literal);
}

std::uint64_t syntaxDigest(const std::vector<Token> &tokens) {
  std::uint64_t result = 14695981039346656037ULL;
  for (const Token &token : tokens) {
    digestInteger(result, static_cast<std::uint64_t>(token.kind));
    digestInteger(result, token.position);
    digestInteger(result, static_cast<std::uint64_t>(token.line));
    digestText(result, token.lexeme);
    digestLiteral(result, token.literal);
    digestInteger(result, token.completion ? 1 : 0);
    digestInteger(result, token.generated ? 1 : 0);
    digestInteger(result, token.configurationFlagReference ? 1 : 0);
    digestInteger(result, token.configurationFlagDefined.has_value() ? 1 : 0);
    if (token.configurationFlagDefined) {
      digestInteger(result, *token.configurationFlagDefined ? 1 : 0);
    }
    if (token.leadingDocumentationMarkdown) {
      digestInteger(result, 1);
      digestText(result, *token.leadingDocumentationMarkdown);
    } else {
      digestInteger(result, 0);
    }
  }
  return result;
}

bool hasErrors(const std::vector<Diagnostic> &diagnostics) {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic &diagnostic) {
                       return diagnostic.severity == DiagnosticSeverity::Error;
                     });
}

} // namespace

SourceGraph SourceLoader::load(
    const std::filesystem::path &entryPath,
    std::optional<std::string> entrySource,
    const std::vector<std::filesystem::path> &preludePaths,
    const std::unordered_map<std::string, std::string> &sourceOverrides,
    const std::vector<std::filesystem::path> &standardLibraryRoots,
    std::optional<std::size_t> completionOffset,
    const std::vector<PackageSourceRoot> &packageSourceRoots, TargetInfo target,
    ConfigurationFlags configurationFlags,
    ConfigurationFlags declaredConfigurationFlags) {
  diagnostics.clear();
  states.clear();
  graph.clear();
  sourceManager.clear();
  this->entrySource = std::move(entrySource);
  this->completionOffset = completionOffset;
  targetInfo = std::move(target);
  normalizeConfigurationFlags(configurationFlags);
  activeFlags.clear();
  declaredFlags.clear();
  activeFlags.insert(configurationFlags.begin(), configurationFlags.end());
  declaredFlags.insert(configurationFlags.begin(), configurationFlags.end());
  declaredFlags.insert(declaredConfigurationFlags.begin(),
                       declaredConfigurationFlags.end());
  initialFlags = activeFlags;
  configurationDomain = 0;
  this->sourceOverrides = &sourceOverrides;
  this->standardLibraryRoots.clear();
  this->standardLibraryRoots.reserve(standardLibraryRoots.size());
  for (const std::filesystem::path &root : standardLibraryRoots) {
    this->standardLibraryRoots.emplace_back(canonicalPath(root));
  }
  this->packageSourceRoots = packageSourceRoots;
  for (PackageSourceRoot &package : this->packageSourceRoots) {
    package.packageRoot = canonicalPath(package.packageRoot);
    package.sourceRoot = canonicalPath(package.sourceRoot);
    std::sort(package.dependencies.begin(), package.dependencies.end(),
              [](const PackageSourceDependency &left,
                 const PackageSourceDependency &right) {
                return left.alias < right.alias;
              });
  }
  entrySourceConsumed = false;
  std::vector<SourceUnitId> preludes;
  const std::filesystem::path canonicalEntry = canonicalPath(entryPath);
  for (const std::filesystem::path &preludePath : preludePaths) {
    const SourceUnitId prelude =
        loadFile(canonicalPath(preludePath), false, true, nullptr, std::nullopt,
                 SourceUnitRole::Prelude);
    preludes.push_back(prelude);
    graph.addPreludeRoot(prelude);
  }
  const std::optional<std::string> entryStandardLibraryName =
      standardLibraryNameForExistingPath(canonicalEntry);
  const SourceUnitId entry =
      loadFile(canonicalEntry, true, false, nullptr, entryStandardLibraryName,
               entryStandardLibraryName ? SourceUnitRole::StandardLibrary
                                        : SourceUnitRole::Application);

  // Complete the private implementation closure only after ordinary imports
  // finish. A body can import its interface without creating an artificial
  // interface -> implementation -> interface import cycle.
  std::unordered_set<std::string> loadedImplementations;
  for (std::size_t index = 0; index < graph.sourceUnits().size(); ++index) {
    const auto moduleDeclaration = graph.sourceUnits()[index].moduleDeclaration;
    const auto unitPath = graph.sourceUnits()[index].path;
    if (!moduleDeclaration || !graph.sourceUnits()[index].packageIdentity)
      continue;
    const auto *package = packageForPath(unitPath);
    if (package == nullptr)
      continue;
    const auto registered =
        std::find_if(package->modules.begin(), package->modules.end(),
                     [&](const auto &entry) {
                       return entry.first == moduleDeclaration->lexeme &&
                              canonicalPath(entry.second) == unitPath;
                     });
    if (registered == package->modules.end())
      continue;
    for (const auto &[name, paths] : package->moduleImplementations) {
      if (name != registered->first)
        continue;
      for (const auto &path : paths) {
        const auto canonical = canonicalPath(path);
        if (packageForPath(canonical) != package ||
            canonical.extension() != ".gti") {
          report(
              *moduleDeclaration,
              "A module implementation must remain inside its owning package.",
              "GTI-I0012");
          continue;
        }
        if (!loadedImplementations.insert(canonical.string()).second) {
          graph.implementationRoots.push_back(
              {.interfaceUnit = graph.sourceUnits()[index].id,
               .implementationUnit =
                   graph.sourceUnitForPath(canonical.string())});
          continue;
        }
        auto savedFlags = activeFlags;
        const auto savedDomain = configurationDomain;
        activeFlags = initialFlags;
        const auto existing = graph.sourceUnitForPath(canonical.string());
        configurationDomain =
            existing != 0 ? existing : graph.sourceUnits().size() + 1;
        const auto implementation =
            loadFile(canonical, false, false, &*moduleDeclaration);
        activeFlags = std::move(savedFlags);
        configurationDomain = savedDomain;
        if (graph.findUnit(implementation)->moduleDeclaration) {
          report(*moduleDeclaration,
                 "A registered implementation is an ordinary source file, not "
                 "another module interface.",
                 "GTI-I0012");
          continue;
        }
        graph.implementationRoots.push_back(
            {.interfaceUnit = graph.sourceUnits()[index].id,
             .implementationUnit = implementation});
      }
    }
  }

  // Preludes are layered in order. A unit that an earlier prelude already
  // reaches, or that prelude itself, lies below every later prelude and never
  // depends on one; a later prelude root sees the earlier ones. Otherwise an
  // extra prelude, such as a test suite's, becomes reachable from the core
  // prelude, and the cycle check below then hides the core prelude from the
  // units that extra prelude includes.
  std::unordered_map<SourceUnitId, std::size_t> preludeLayer;
  for (const SourceUnit &unit : graph.sourceUnits()) {
    for (std::size_t index = 0; index < preludes.size(); ++index) {
      if (unit.id == preludes[index] ||
          graph.hasDependencyPath(preludes[index], unit.id)) {
        preludeLayer.emplace(unit.id, index);
        break;
      }
    }
  }
  for (const SourceUnit &unit : graph.sourceUnits()) {
    const auto layer = preludeLayer.find(unit.id);
    const std::size_t visible =
        layer == preludeLayer.end() ? preludes.size() : layer->second;
    for (std::size_t index = 0; index < visible; ++index) {
      const SourceUnitId prelude = preludes[index];
      if (unit.id != prelude && !graph.hasDirectDependency(unit.id, prelude) &&
          !graph.hasDependencyPath(prelude, unit.id)) {
        graph.addDependency({.source = unit.id,
                             .target = prelude,
                             .kind = SourceDependencyKind::Prelude});
      }
    }
  }
  if (entry != 0) {
    graph.entry = entry;
    if (SourceUnit *entryUnit = graph.findUnit(entry)) {
      entryUnit->entry = true;
    }
  }
  for (SourceUnit &unit : graph.units) {
    for (Token &token : unit.tokens) {
      if (!token.configurationFlagReference) {
        continue;
      }
      if (declaredFlags.contains(token.lexeme)) {
        continue;
      }
      Diagnostic diagnostic = makeDiagnostic(
          DiagnosticId::UnknownConfigurationFlag, token, {token.lexeme});
      diagnostic.hints.emplace_back(
          "Check the spelling or define the flag in source, with '-D " +
          token.lexeme + "', or in manifest build/profile/target 'defines'.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
  return std::move(graph);
}

bool SourceLoader::hadError() const { return hasErrors(diagnostics); }

const std::vector<SourceDiagnostic> &SourceLoader::errors() const {
  return diagnostics;
}

const SourceManager &SourceLoader::sources() const { return sourceManager; }

std::filesystem::path
SourceLoader::canonicalPath(const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::path absolute = std::filesystem::absolute(path, error);
  if (error) {
    absolute = path;
    error.clear();
  }

  std::filesystem::path canonical =
      std::filesystem::weakly_canonical(absolute, error);
  return error ? absolute.lexically_normal() : canonical;
}

bool SourceLoader::isImportPathSegment(std::string_view segment) {
  if (segment.empty()) {
    return false;
  }
  const auto validStart = [](char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalpha(character) != 0 || value == '_';
  };
  const auto validPart = [](char value) {
    const unsigned char character = static_cast<unsigned char>(value);
    return std::isalnum(character) != 0 || value == '_';
  };
  return validStart(segment.front()) &&
         std::all_of(segment.begin() + 1, segment.end(), validPart);
}

bool SourceLoader::isImportPathSegment(const Token &token) {
  return isImportPathSegment(token.lexeme);
}

bool SourceLoader::pathIsWithin(const std::filesystem::path &root,
                                const std::filesystem::path &candidate) {
  auto rootPart = root.begin();
  auto candidatePart = candidate.begin();
  for (; rootPart != root.end() && candidatePart != candidate.end();
       ++rootPart, ++candidatePart) {
    if (*rootPart != *candidatePart) {
      return false;
    }
  }
  return rootPart == root.end();
}

const PackageSourceRoot *
SourceLoader::packageForPath(const std::filesystem::path &path) const {
  const PackageSourceRoot *selected = nullptr;
  std::size_t selectedDepth = 0;
  for (const PackageSourceRoot &package : packageSourceRoots) {
    if (!pathIsWithin(package.packageRoot, path)) {
      continue;
    }
    const std::size_t depth = static_cast<std::size_t>(
        std::distance(package.packageRoot.begin(), package.packageRoot.end()));
    if (selected == nullptr || depth > selectedDepth) {
      selected = &package;
      selectedDepth = depth;
    }
  }
  return selected;
}

const PackageSourceRoot *
SourceLoader::packageByIdentity(std::string_view identity) const {
  const auto found =
      std::find_if(packageSourceRoots.begin(), packageSourceRoots.end(),
                   [identity](const PackageSourceRoot &package) {
                     return package.identity == identity;
                   });
  return found == packageSourceRoots.end() ? nullptr : &*found;
}

std::optional<std::string> SourceLoader::standardLibraryNameForExistingPath(
    const std::filesystem::path &path) const {
  for (const std::filesystem::path &root : standardLibraryRoots) {
    const std::filesystem::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
      continue;
    }

    std::vector<std::string> segments;
    bool contained = true;
    for (const std::filesystem::path &component : relative) {
      const std::string segment = component.string();
      if (segment.empty() || segment == ".") {
        continue;
      }
      if (segment == "..") {
        contained = false;
        break;
      }
      segments.push_back(segment);
    }
    if (!contained || segments.size() < 2 || segments.front() != "std") {
      continue;
    }

    std::filesystem::path leaf(segments.back());
    if (leaf.extension() != ".gti") {
      continue;
    }
    segments.back() = leaf.stem().string();
    if (std::any_of(segments.begin(), segments.end(),
                    [](const std::string &segment) {
                      return !isImportPathSegment(segment);
                    })) {
      continue;
    }

    // Most application units are outside every configured standard-library
    // root. Establish lexical containment first so those common lookups do
    // not issue filesystem status calls; retain the same existence checks for
    // the only paths that could be trusted library units.
    std::error_code error;
    if (!std::filesystem::is_directory(root, error)) {
      continue;
    }
    error.clear();
    if (!std::filesystem::is_regular_file(path, error)) {
      continue;
    }

    std::string importName;
    for (const std::string &segment : segments) {
      if (!importName.empty()) {
        importName += '/';
      }
      importName += segment;
    }
    return importName;
  }
  return std::nullopt;
}

std::optional<std::string> SourceLoader::standardModuleNameForExistingPath(
    const std::filesystem::path &path) const {
  for (const std::filesystem::path &root : standardLibraryRoots) {
    if (path == canonicalPath(root / "std.gti")) {
      return "std";
    }
    const std::filesystem::path directory = canonicalPath(root / "modules");
    if (path.parent_path() != directory || path.extension() != ".gti" ||
        !isImportPathSegment(path.stem().string())) {
      continue;
    }
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) ||
        sourceOverrides->contains(path.string())) {
      return "modules/" + path.stem().string();
    }
  }
  return std::nullopt;
}

SourceUnitId
SourceLoader::loadFile(const std::filesystem::path &path, bool isEntry,
                       bool isPrelude, const Token *includeToken,
                       std::optional<std::string> standardLibraryName,
                       SourceUnitRole role) {
  const std::string key = path.string();
  SourceUnitId unitId = 0;
  bool replayConfiguration = false;
  if (const auto state = states.find(key); state != states.end()) {
    unitId = state->second.unit;
    if (state->second.state == LoadState::Visiting && includeToken != nullptr) {
      report(*includeToken, "Include cycle detected for '" + key + "'.",
             "GTI-I0001");
    }
    if (SourceUnit *unit = graph.findUnit(unitId)) {
      unit->entry = unit->entry || isEntry;
      unit->prelude = unit->prelude || isPrelude;
      if (role == SourceUnitRole::Prelude ||
          (role == SourceUnitRole::StandardLibrary &&
           unit->role == SourceUnitRole::Application)) {
        unit->role = role;
      }
      if (standardLibraryName) {
        unit->standardLibraryName = std::move(standardLibraryName);
      }
      // A new module domain replays the unit's directives under its own
      // flags. Incoming flags may differ when the unit never observes them;
      // the canonical-syntax comparison after replay rejects any difference
      // that selects other tokens, here or in a nested include.
      if (!unit->moduleDeclaration && includeToken != nullptr &&
          state->second.state == LoadState::Loaded &&
          !state->second.configurationDomains.contains(configurationDomain)) {
        replayConfiguration = true;
        state->second.state = LoadState::Visiting;
        state->second.configurationDomains.insert(configurationDomain);
      }
    }
    if (isEntry) {
      graph.entry = unitId;
    }
    if (!replayConfiguration) {
      return unitId;
    }
  } else {
    std::optional<std::string> packageIdentity;
    std::optional<std::string> packageRelativePath;
    if (role == SourceUnitRole::Application) {
      if (const PackageSourceRoot *package = packageForPath(path)) {
        packageIdentity = package->identity;
        packageRelativePath =
            path.lexically_relative(package->packageRoot).generic_string();
      }
    }
    unitId = graph.addUnit(
        path, isEntry, isPrelude, std::move(standardLibraryName), role,
        std::move(packageIdentity), std::move(packageRelativePath));
    states.emplace(key,
                   FileState{.state = LoadState::Visiting,
                             .unit = unitId,
                             .configurationDomains = {configurationDomain}});
  }
  std::optional<std::string_view> exactSource;
  std::optional<std::size_t> unitCompletionOffset;
  if (replayConfiguration) {
    // Replay the exact snapshot already loaded, including unsaved overlays.
    // Reuse the directive processor rather than approximating nested include
    // effects with a saved output flag set: a child may already be included in
    // this domain and must not reapply an earlier #define after an #undef.
    if (const std::string *source = sourceManager.find(key)) {
      exactSource = *source;
    }
  } else if (isEntry && entrySource && !entrySourceConsumed) {
    entrySourceConsumed = true;
    exactSource = *entrySource;
    unitCompletionOffset = completionOffset;
  } else if (const auto override = sourceOverrides->find(key);
             override != sourceOverrides->end()) {
    exactSource = override->second;
  }
  std::uint64_t generation = 0;
  if (sourceGenerations != nullptr) {
    if (const auto found = sourceGenerations->find(key);
        found != sourceGenerations->end()) {
      generation = found->second;
    }
  }
  std::uint64_t knownSourceDigest = 0;
  if (!exactSource && sourceDigests != nullptr) {
    if (const auto found = sourceDigests->find(key);
        found != sourceDigests->end()) {
      knownSourceDigest = found->second;
    }
  }
  LoadedLexicalUnit lexical =
      LoadedLexicalUnitLoader(workMetrics, cache)
          .load({.path = path,
                 .canonicalPath = key,
                 .exactSource = exactSource,
                 .completionOffset = unitCompletionOffset,
                 .sourceGeneration = generation,
                 .knownSourceDigest = knownSourceDigest,
                 .cacheContext = cacheContext});
  const std::uint64_t loadedSourceDigest = lexical.sourceDigest;
  sourceManager.set(key, lexical.source);
  if (SourceUnit *unit = graph.findUnit(unitId)) {
    unit->sourceDigest = loadedSourceDigest;
  }

  for (const Diagnostic &diagnostic : lexical.diagnostics) {
    Diagnostic forwarded = diagnostic;
    if (includeToken != nullptr) {
      forwarded.related.push_back(
          {tokenSpan(*includeToken), "Included from here."});
    }
    diagnostics.emplace_back(std::move(forwarded));
  }
  if (lexical.hadError) {
    states[key].state = LoadState::Loaded;
    return unitId;
  }
  std::vector<Token> fileTokens = std::move(lexical.tokens);

  const bool moduleInterface = fileTokens.size() >= 4 &&
                               fileTokens[0].kind == TokenKind::EXPORT &&
                               fileTokens[1].kind == TokenKind::MODULE &&
                               fileTokens[2].kind == TokenKind::IDENTIFIER &&
                               fileTokens[3].kind == TokenKind::SEMICOLON;
  std::unordered_set<std::string> importerFlags;
  const SourceUnitId importerDomain = configurationDomain;
  if (moduleInterface) {
    graph.findUnit(unitId)->moduleDeclaration = fileTokens[2];
    importerFlags = activeFlags;
    activeFlags = initialFlags;
    configurationDomain = unitId;
    // Module names are owned by one package, the standard library, or the
    // direct-compilation application; the standard library's `vector` does
    // not collide with an application's interface of that name.
    const SourceUnit &interfaceUnit = *graph.findUnit(unitId);
    const auto standardOwned = [](const SourceUnit &unit) {
      return unit.role == SourceUnitRole::StandardLibrary;
    };
    for (const SourceUnit &other : graph.sourceUnits()) {
      if (other.id != unitId && other.moduleDeclaration &&
          other.packageIdentity == interfaceUnit.packageIdentity &&
          standardOwned(other) == standardOwned(interfaceUnit) &&
          other.moduleDeclaration->lexeme == fileTokens[2].lexeme) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-I0012", DiagnosticPhase::SourceLoading, fileTokens[2],
            "Module '" + fileTokens[2].lexeme +
                "' has more than one interface in this package.");
        diagnostic.related.push_back(
            {tokenSpan(*other.moduleDeclaration), "First interface is here."});
        diagnostics.push_back(std::move(diagnostic));
      }
    }
  }

  const bool requiresDirectiveProcessing =
      std::any_of(fileTokens.begin(), fileTokens.end(), [](const Token &token) {
        return isDirectiveToken(token.kind) ||
               token.kind == TokenKind::IMPORT ||
               token.kind == TokenKind::MODULE ||
               token.kind == TokenKind::EXPORT ||
               (token.kind == TokenKind::IDENTIFIER &&
                token.lexeme == "include");
      });
  if (!requiresDirectiveProcessing) {
    if (!replayConfiguration) {
      SourceUnit *loadedUnit = graph.findUnit(unitId);
      loadedUnit->syntaxDigest = loadedSourceDigest;
      loadedUnit->tokens = std::move(fileTokens);
    }
    states[key].state = LoadState::Loaded;
    return unitId;
  }

  std::vector<Token> output;
  int braceDepth = 0;
  std::size_t includeOccurrence = 0;
  ConfigurationDirectiveProcessor directiveProcessor(
      fileTokens, targetInfo, activeFlags, declaredFlags, diagnostics);
  for (std::size_t index = moduleInterface ? 4 : 0; index < fileTokens.size();
       ++index) {
    Token &token = fileTokens[index];
    if (token.kind == TokenKind::END_OF_FILE) {
      output.push_back(std::move(token));
      continue;
    }

    const ConfigurationDirectiveStep directive =
        directiveProcessor.process(index);
    if (directive.consumed) {
      index = directive.lastConsumedIndex;
      continue;
    }

    const bool startsLine =
        index == 0 || fileTokens[index - 1].line != token.line;
    const bool legacyInclude =
        braceDepth == 0 && startsLine && token.kind == TokenKind::IDENTIFIER &&
        token.lexeme == "include" && index + 1 < fileTokens.size() &&
        (fileTokens[index + 1].kind == TokenKind::STRING_LITERAL ||
         fileTokens[index + 1].kind == TokenKind::LESS);
    if (legacyInclude) {
      report(token,
             "GTI include directives use '#include'; plain 'include' is "
             "not a directive.",
             "GTI-I0009");
    }

    const bool exportImport = token.kind == TokenKind::EXPORT &&
                              index + 1 < fileTokens.size() &&
                              fileTokens[index + 1].kind == TokenKind::IMPORT;
    if (token.kind == TokenKind::IMPORT || exportImport) {
      if (exportImport && !moduleInterface) {
        report(token,
               "Re-exporting an import requires an 'export module name;' "
               "interface.",
               "GTI-I0012");
      }
      const ResolvedInclude imported = resolveImport(
          fileTokens, index + (exportImport ? 1 : 0), path, braceDepth,
          directiveProcessor.conditionalDepth(), exportImport);
      // The loader resolves dependency identity, but the parser owns whether
      // this directive is between complete source-unit declarations. Erasing
      // it here could turn malformed parameters or initializers into valid
      // source. Keep its original tokens and spans in the unit syntax cache.
      for (std::size_t part = index; part <= imported.directiveEnd; ++part) {
        output.push_back(fileTokens[part]);
      }
      index = imported.directiveEnd;
      if (imported.dependency != 0 && !replayConfiguration) {
        graph.addDependency({.source = unitId,
                             .target = imported.dependency,
                             .kind = imported.kind,
                             .directive = tokenSpan(token),
                             .includeSpelling = imported.includeSpelling,
                             .includeOccurrence = includeOccurrence++});
      }
      continue;
    }
    if (token.kind == TokenKind::MODULE ||
        (token.kind == TokenKind::EXPORT && index + 1 < fileTokens.size() &&
         fileTokens[index + 1].kind == TokenKind::MODULE)) {
      report(token,
             "A module interface must begin with exactly one 'export module "
             "name;' declaration.",
             "GTI-I0012");
      while (index + 1 < fileTokens.size() &&
             fileTokens[index].kind != TokenKind::SEMICOLON &&
             fileTokens[index + 1].kind != TokenKind::END_OF_FILE) {
        ++index;
      }
      continue;
    }

    // `export #include` keeps include resolution and direct visibility for
    // this interface, then deliberately republishes that one legacy unit to
    // importers. The unit's own nested includes are not republished.
    const bool exportInclude =
        token.kind == TokenKind::EXPORT && index + 1 < fileTokens.size() &&
        fileTokens[index + 1].kind == TokenKind::HASH_INCLUDE;
    if (exportInclude && !moduleInterface) {
      report(token,
             "Republishing an include requires an 'export module name;' "
             "interface.",
             "GTI-I0012");
    }
    if (token.kind == TokenKind::HASH_INCLUDE || exportInclude) {
      const Token &includeToken = fileTokens[index + (exportInclude ? 1 : 0)];
      const std::size_t occurrence = includeOccurrence++;
      const ResolvedInclude include =
          resolveInclude(fileTokens, index + (exportInclude ? 1 : 0), path,
                         braceDepth, directiveProcessor.conditionalDepth());
      index = include.directiveEnd;
      if (include.dependency != 0) {
        if (graph.findUnit(include.dependency)->moduleDeclaration) {
          report(includeToken,
                 exportInclude
                     ? "A module interface is republished with 'export "
                       "import', not 'export #include'."
                     : "A module interface must be imported, not included; "
                       "use 'import' to respect its exports.",
                 "GTI-I0012");
        }
        if (!replayConfiguration) {
          graph.addDependency({.source = unitId,
                               .target = include.dependency,
                               .kind = exportInclude && moduleInterface
                                           ? SourceDependencyKind::ExportInclude
                                           : include.kind,
                               .directive = tokenSpan(includeToken),
                               .includeSpelling = include.includeSpelling,
                               .includeOccurrence = occurrence});
        }
      }
      continue;
    }

    if (!directiveProcessor.currentPathActive() &&
        !directive.conditionalBoundary) {
      directiveProcessor.retainInactive(token);
    }

    if (token.kind == TokenKind::LEFT_BRACE) {
      ++braceDepth;
    } else if (token.kind == TokenKind::RIGHT_BRACE && braceDepth > 0) {
      --braceDepth;
    }
    output.push_back(std::move(token));
  }

  directiveProcessor.finish();

  if (output.empty() || output.back().kind != TokenKind::END_OF_FILE) {
    output.emplace_back(TokenKind::END_OF_FILE, "", std::monostate{}, 0, 1,
                        key);
  }
  SourceUnit *loadedUnit = graph.findUnit(unitId);
  const std::uint64_t selectedSyntax = syntaxDigest(output);
  if (replayConfiguration) {
    if (loadedUnit->syntaxDigest != selectedSyntax) {
      report(*includeToken,
             "A shared legacy source unit selects different syntax across "
             "module configuration domains; use common include ordering and "
             "flags or separate interfaces.",
             "GTI-I0012");
    }
  } else {
    loadedUnit->syntaxDigest = selectedSyntax;
    loadedUnit->tokens = std::move(output);
    loadedUnit->inactiveSpans = directiveProcessor.takeInactiveSpans();
    loadedUnit->configurationTokens =
        directiveProcessor.takeConfigurationTokens();
  }
  states[key].state = LoadState::Loaded;
  if (moduleInterface) {
    activeFlags = std::move(importerFlags);
    configurationDomain = importerDomain;
  }
  return unitId;
}

SourceLoader::ResolvedInclude
SourceLoader::resolveInclude(std::vector<Token> &tokens, std::size_t index,
                             const std::filesystem::path &includingFile,
                             int braceDepth, int conditionalDepth) {
  const Token includeToken = tokens[index];
  const bool hasRelativePath =
      index + 1 < tokens.size() &&
      tokens[index + 1].kind == TokenKind::STRING_LITERAL;
  const bool hasStandardPath =
      index + 1 < tokens.size() && tokens[index + 1].kind == TokenKind::LESS;
  const bool hasPath = hasRelativePath || hasStandardPath;
  std::size_t directiveEnd = hasPath ? index + 1 : index;
  if (hasRelativePath && index + 2 < tokens.size() &&
      tokens[index + 2].kind == TokenKind::SEMICOLON) {
    directiveEnd = index + 2;
  }

  if (braceDepth != 0) {
    report(includeToken, "Include directives are only allowed at top level.",
           "GTI-I0004");
    return {.directiveEnd = directiveEnd};
  }
  if (conditionalDepth != 0) {
    report(includeToken,
           "Include directives cannot appear inside '#if' blocks.",
           "GTI-I0004");
    return {.directiveEnd = directiveEnd};
  }
  if (!hasPath) {
    report(includeToken,
           "Expect a quoted .gti path, <std/name>, or a declared "
           "<dependency/name> after '#include'.",
           "GTI-I0005");
    return {.directiveEnd = directiveEnd};
  }

  if (hasStandardPath) {
    ResolvedInclude result =
        resolveAngleInclude(tokens, index, includeToken, includingFile);
    if (result.dependency != 0) {
      for (std::size_t spellingIndex = index + 1;
           spellingIndex <= result.directiveEnd &&
           tokens[spellingIndex].kind != TokenKind::SEMICOLON;
           ++spellingIndex) {
        result.includeSpelling += tokens[spellingIndex].lexeme;
      }
    }
    return result;
  }

  const Token &pathToken = tokens[index + 1];
  const auto *pathText = std::get_if<std::string>(&pathToken.literal);
  if (pathText == nullptr || pathText->empty()) {
    report(pathToken, "Include path cannot be empty.", "GTI-I0006");
    return {.directiveEnd = directiveEnd};
  }

  const std::filesystem::path requestedPath(*pathText);
  if (requestedPath.is_absolute()) {
    report(pathToken, "Include path must be relative to the including file.",
           "GTI-I0006");
    return {.directiveEnd = directiveEnd};
  }
  if (requestedPath.extension() != ".gti") {
    report(pathToken, "Included source file must use the .gti extension.",
           "GTI-I0006");
    return {.directiveEnd = directiveEnd};
  }

  const std::filesystem::path resolved =
      canonicalPath(includingFile.parent_path() / requestedPath);
  if (!packageSourceRoots.empty()) {
    const PackageSourceRoot *includingPackage =
        packageForPath(canonicalPath(includingFile));
    const PackageSourceRoot *targetPackage = packageForPath(resolved);
    if (includingPackage != nullptr && targetPackage != includingPackage) {
      report(pathToken,
             "A quoted include cannot cross the owning package boundary; "
             "declare the package dependency and use its angle-include "
             "alias.",
             "GTI-I0010");
      return {.directiveEnd = directiveEnd};
    }
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(resolved, error) &&
      !sourceOverrides->contains(resolved.string())) {
    report(pathToken,
           "Included source file '" + requestedPath.generic_string() +
               "' was not found.",
           "GTI-I0008");
    return {.directiveEnd = directiveEnd};
  }
  const std::optional<std::string> trustedImport =
      standardLibraryNameForExistingPath(resolved);
  return {.directiveEnd = directiveEnd,
          .dependency =
              loadFile(resolved, false, false, &includeToken, trustedImport,
                       trustedImport ? SourceUnitRole::StandardLibrary
                                     : SourceUnitRole::Application),
          .includeSpelling = pathToken.lexeme};
}

SourceLoader::ResolvedInclude
SourceLoader::resolveAngleInclude(std::vector<Token> &tokens, std::size_t index,
                                  const Token &includeToken,
                                  const std::filesystem::path &includingFile) {
  std::size_t current = index + 2;
  std::vector<std::string> segments;
  while (current < tokens.size() && isImportPathSegment(tokens[current])) {
    segments.emplace_back(tokens[current].lexeme);
    ++current;
    if (current >= tokens.size() || tokens[current].kind != TokenKind::SLASH) {
      break;
    }
    ++current;
  }

  const bool closed =
      current < tokens.size() && tokens[current].kind == TokenKind::GREATER;
  std::size_t directiveEnd = closed ? current : index + 1;
  if (closed && current + 1 < tokens.size() &&
      tokens[current + 1].kind == TokenKind::SEMICOLON) {
    directiveEnd = current + 1;
  }

  if (!closed || segments.size() < 2) {
    report(includeToken,
           "Angle includes use a standard-library or declared package path "
           "such as '#include <std/array>' or '#include <math/vector>'.",
           "GTI-I0007");
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::StandardLibrary};
  }

  if (segments.front() != "std") {
    return resolvePackageInclude(segments, directiveEnd, includeToken,
                                 includingFile);
  }
  if (standardLibraryRoots.empty()) {
    report(includeToken,
           "Cannot resolve standard-library include because no standard "
           "library root is configured.",
           "GTI-I0007");
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::StandardLibrary};
  }

  std::filesystem::path relative;
  std::string importName;
  for (const std::string &segment : segments) {
    relative /= segment;
    if (!importName.empty()) {
      importName += '/';
    }
    importName += segment;
  }
  relative += ".gti";

  for (const std::filesystem::path &root : standardLibraryRoots) {
    const std::filesystem::path candidate = canonicalPath(root / relative);
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error) ||
        sourceOverrides->contains(candidate.string())) {
      const std::optional<std::string> trustedImport =
          standardLibraryNameForExistingPath(candidate);
      return {.directiveEnd = directiveEnd,
              .dependency = loadFile(
                  candidate, false, false, &includeToken, trustedImport,
                  trustedImport ? SourceUnitRole::StandardLibrary
                                : SourceUnitRole::Application),
              .kind = SourceDependencyKind::StandardLibrary};
    }
  }

  report(includeToken,
         "Standard-library unit '<" + importName + ">' was not found.",
         "GTI-I0007");
  return {.directiveEnd = directiveEnd,
          .kind = SourceDependencyKind::StandardLibrary};
}

SourceLoader::ResolvedInclude SourceLoader::resolvePackageInclude(
    const std::vector<std::string> &segments, std::size_t directiveEnd,
    const Token &includeToken, const std::filesystem::path &includingFile) {
  const PackageSourceRoot *includingPackage =
      packageForPath(canonicalPath(includingFile));
  if (includingPackage == nullptr) {
    report(includeToken,
           "Package include '<" + segments.front() +
               "/...>' is unavailable because this compilation has no "
               "owning package graph.",
           "GTI-I0010");
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::Package};
  }

  const auto dependency =
      std::find_if(includingPackage->dependencies.begin(),
                   includingPackage->dependencies.end(),
                   [&segments](const PackageSourceDependency &candidate) {
                     return candidate.alias == segments.front();
                   });
  if (dependency == includingPackage->dependencies.end()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-I0010", DiagnosticPhase::SourceLoading, includeToken,
        "Package '" + includingPackage->name +
            "' has no direct dependency alias '" + segments.front() + "'.");
    if (!includingPackage->dependencies.empty()) {
      std::string aliases;
      for (const PackageSourceDependency &candidate :
           includingPackage->dependencies) {
        if (!aliases.empty()) {
          aliases += ", ";
        }
        aliases += candidate.alias;
      }
      diagnostic.hints.push_back(
          "Declared direct dependency aliases: " + aliases + ".");
    }
    diagnostics.push_back(std::move(diagnostic));
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::Package};
  }

  const PackageSourceRoot *target =
      packageByIdentity(dependency->targetIdentity);
  if (target == nullptr) {
    report(includeToken,
           "Dependency alias '" + dependency->alias +
               "' refers to a package that is absent from the resolved "
               "package graph.",
           "GTI-I0010");
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::Package};
  }

  std::filesystem::path relative;
  for (std::size_t index = 1; index < segments.size(); ++index) {
    relative /= segments[index];
  }
  relative += ".gti";
  const std::filesystem::path candidate =
      canonicalPath(target->sourceRoot / relative);
  if (!pathIsWithin(target->sourceRoot, candidate)) {
    report(includeToken,
           "Package include escapes dependency source root for alias '" +
               dependency->alias + "'.",
           "GTI-I0010");
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::Package};
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(candidate, error) &&
      !sourceOverrides->contains(candidate.string())) {
    std::string importName;
    for (const std::string &segment : segments) {
      if (!importName.empty()) {
        importName += '/';
      }
      importName += segment;
    }
    report(includeToken,
           "Package unit '<" + importName + ">' was not found beneath '" +
               target->sourceRoot.string() + "'.",
           "GTI-I0010");
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::Package};
  }

  return {.directiveEnd = directiveEnd,
          .dependency = loadFile(candidate, false, false, &includeToken),
          .kind = SourceDependencyKind::Package};
}

SourceLoader::ResolvedInclude
SourceLoader::resolveImport(const std::vector<Token> &tokens, std::size_t index,
                            const std::filesystem::path &includingFile,
                            int braceDepth, int conditionalDepth,
                            bool reexport) {
  const Token &keyword = tokens[index];
  std::size_t current = index + 1;
  std::vector<std::string> names;
  const bool quoted = current < tokens.size() &&
                      tokens[current].kind == TokenKind::STRING_LITERAL;
  if (quoted) {
    ++current;
  } else {
    while (current < tokens.size() &&
           tokens[current].kind == TokenKind::IDENTIFIER) {
      names.push_back(tokens[current++].lexeme);
      if (current >= tokens.size() || tokens[current].kind != TokenKind::DOT) {
        break;
      }
      ++current;
    }
  }
  const bool terminated =
      current < tokens.size() && tokens[current].kind == TokenKind::SEMICOLON;
  const std::size_t end = terminated ? current : current - 1;
  ResolvedInclude result{.directiveEnd = end,
                         .kind = reexport ? SourceDependencyKind::ExportImport
                                          : SourceDependencyKind::Import};
  const auto reject = [&](std::string message) {
    report(keyword, std::move(message), "GTI-I0012");
    return result;
  };
  if (!terminated ||
      (!quoted && (names.empty() || names.size() > 2 ||
                   tokens[current - 1].kind == TokenKind::DOT))) {
    return reject("Expect 'import name;', 'import dependency.name;', or "
                  "'import \"path.gti\";'.");
  }
  if (braceDepth != 0 || conditionalDepth != 0) {
    return reject(
        "Module imports are unconditional source-unit-level declarations.");
  }
  for (std::size_t part = index + 1; part < current; ++part) {
    result.includeSpelling += tokens[part].lexeme;
  }
  const PackageSourceRoot *owner = packageForPath(includingFile);
  std::filesystem::path path;
  std::optional<std::string> expectedName;
  std::optional<std::string> standardModule;
  if (!quoted && names.front() == "std") {
    // The configured standard library owns the reserved `std` module
    // namespace in direct and project compilation alike. Its interfaces have
    // one fixed location beneath each root, so a name never triggers search.
    const bool aggregate = names.size() == 1;
    const std::string &name = aggregate ? names.front() : names[1];
    for (const std::filesystem::path &root : standardLibraryRoots) {
      const std::filesystem::path candidate = canonicalPath(
          aggregate ? root / "std.gti" : root / "modules" / (name + ".gti"));
      std::error_code error;
      if (std::filesystem::is_regular_file(candidate, error) ||
          sourceOverrides->contains(candidate.string())) {
        path = candidate;
        break;
      }
    }
    if (path.empty()) {
      return reject(
          standardLibraryRoots.empty()
              ? "Cannot resolve standard-library module because no standard "
                "library root is configured."
          : aggregate
              ? "Standard-library aggregate module 'std' was not found; "
                "install a standard library providing std.gti or import "
                "individual units."
              : "Standard-library module 'std." + name +
                    "' was not found; each public <std/name> unit is "
                    "imported as 'std.name'.");
    }
    expectedName = name;
    standardModule = standardModuleNameForExistingPath(path);
  } else if (quoted) {
    const auto *spelling = std::get_if<std::string>(&tokens[index + 1].literal);
    if (spelling == nullptr || spelling->empty() ||
        spelling->find('\0') != std::string::npos) {
      return reject("A quoted module import requires a nonempty .gti path.");
    }
    const std::filesystem::path relative(*spelling);
    if (relative.is_absolute() || relative.extension() != ".gti") {
      return reject("A quoted module import must name a relative .gti file.");
    }
    path = canonicalPath(includingFile.parent_path() / relative);
    if (!packageSourceRoots.empty() &&
        (owner == nullptr || packageForPath(path) != owner)) {
      return reject("A quoted import cannot cross a package boundary; use a "
                    "declared dependency alias.");
    }
    standardModule = standardModuleNameForExistingPath(path);
  } else {
    if (owner == nullptr) {
      return reject("Named module imports require an owning package graph; "
                    "direct compilation can use a quoted import.");
    }
    const auto local = std::find_if(
        owner->modules.begin(), owner->modules.end(),
        [&](const auto &module) { return module.first == names.front(); });
    const auto dependency = std::find_if(
        owner->dependencies.begin(), owner->dependencies.end(),
        [&](const auto &edge) { return edge.alias == names.front(); });
    if (names.size() == 1 && local != owner->modules.end() &&
        dependency != owner->dependencies.end()) {
      return reject(
          "Module name '" + names.front() +
          "' is ambiguous with a dependency alias; rename the alias.");
    }
    const PackageSourceRoot *target = owner;
    std::string name = names.front();
    if (dependency != owner->dependencies.end()) {
      target = packageByIdentity(dependency->targetIdentity);
      name = names.size() == 1 ? "main" : names[1];
    } else if (names.size() == 2) {
      return reject("Package '" + owner->name +
                    "' has no direct dependency alias '" + names.front() +
                    "'.");
    }
    if (target == nullptr) {
      return reject(
          "Imported package is absent from the resolved package graph.");
    }
    const auto module =
        std::find_if(target->modules.begin(), target->modules.end(),
                     [&](const auto &entry) { return entry.first == name; });
    if (module == target->modules.end()) {
      return reject("Package '" + target->name +
                    "' does not declare public module '" + name +
                    "' in [modules].");
    }
    path = canonicalPath(module->second);
    if (!pathIsWithin(target->packageRoot, path) ||
        path.extension() != ".gti" || packageForPath(path) != target) {
      return reject("Module interface path escapes its owning package or is "
                    "not a .gti file.");
    }
    expectedName = name;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) &&
      !sourceOverrides->contains(path.string())) {
    return reject("Module interface '" + path.string() + "' was not found.");
  }
  const SourceUnitId loaded =
      loadFile(path, false, false, &keyword, standardModule,
               standardModule ? SourceUnitRole::StandardLibrary
                              : SourceUnitRole::Application);
  const SourceUnit *unit = graph.findUnit(loaded);
  if (unit == nullptr || !unit->moduleDeclaration) {
    return reject("An import must name an explicit 'export module name;' "
                  "interface; legacy units still use '#include'.");
  }
  if (expectedName && unit->moduleDeclaration->lexeme != *expectedName) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-I0012", DiagnosticPhase::SourceLoading, keyword,
        "Imported module '" + *expectedName + "' declares itself as '" +
            unit->moduleDeclaration->lexeme + "'.");
    diagnostic.related.push_back({tokenSpan(*unit->moduleDeclaration),
                                  "Interface declaration is here."});
    diagnostics.push_back(std::move(diagnostic));
    return result;
  }
  result.dependency = loaded;
  return result;
}

void SourceLoader::report(const Token &token, std::string message,
                          std::string code) {
  diagnostics.push_back(makeDiagnostic(std::move(code),
                                       DiagnosticPhase::SourceLoading, token,
                                       std::move(message)));
}

} // namespace lang
