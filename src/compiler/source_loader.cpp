#include "gti/source_loader.h"

#include "configuration_directive_processor.h"
#include "loaded_lexical_unit.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string_view>
#include <type_traits>
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
    ConfigurationFlags configurationFlags) {
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

  for (const SourceUnit &unit : graph.sourceUnits()) {
    if (unit.prelude) {
      continue;
    }
    for (const SourceUnitId prelude : preludes) {
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
          "GTI-S2075", DiagnosticPhase::Semantics, token,
          "Configuration flag '" + token.lexeme +
              "' is referenced but defined nowhere in this compilation.");
      diagnostic.severity = DiagnosticSeverity::Warning;
      diagnostic.hints.emplace_back(
          "Check the spelling or define the flag in source, with '-D " +
          token.lexeme + "', or in '[build].defines'.");
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

SourceUnitId
SourceLoader::loadFile(const std::filesystem::path &path, bool isEntry,
                       bool isPrelude, const Token *includeToken,
                       std::optional<std::string> standardLibraryName,
                       SourceUnitRole role) {
  const std::string key = path.string();
  if (const auto state = states.find(key); state != states.end()) {
    if (state->second.state == LoadState::Visiting && includeToken != nullptr) {
      report(*includeToken, "Include cycle detected for '" + key + "'.",
             "GTI-I0001");
    }
    if (SourceUnit *unit = graph.findUnit(state->second.unit)) {
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
    }
    if (isEntry) {
      graph.entry = state->second.unit;
    }
    return state->second.unit;
  }
  std::optional<std::string> packageIdentity;
  std::optional<std::string> packageRelativePath;
  if (role == SourceUnitRole::Application) {
    if (const PackageSourceRoot *package = packageForPath(path)) {
      packageIdentity = package->identity;
      packageRelativePath =
          path.lexically_relative(package->packageRoot).generic_string();
    }
  }
  const SourceUnitId unitId = graph.addUnit(
      path, isEntry, isPrelude, std::move(standardLibraryName), role,
      std::move(packageIdentity), std::move(packageRelativePath));
  states.emplace(key, FileState{.state = LoadState::Visiting, .unit = unitId});
  std::optional<std::string_view> exactSource;
  std::optional<std::size_t> unitCompletionOffset;
  if (isEntry && entrySource && !entrySourceConsumed) {
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

  const bool requiresDirectiveProcessing =
      std::any_of(fileTokens.begin(), fileTokens.end(), [](const Token &token) {
        return isDirectiveToken(token.kind) ||
               (token.kind == TokenKind::IDENTIFIER &&
                token.lexeme == "include");
      });
  if (!requiresDirectiveProcessing) {
    SourceUnit *loadedUnit = graph.findUnit(unitId);
    loadedUnit->syntaxDigest = loadedSourceDigest;
    loadedUnit->tokens = std::move(fileTokens);
    states[key].state = LoadState::Loaded;
    return unitId;
  }

  std::vector<Token> output;
  int braceDepth = 0;
  std::size_t includeOccurrence = 0;
  ConfigurationDirectiveProcessor directiveProcessor(
      fileTokens, targetInfo, activeFlags, declaredFlags, diagnostics);
  for (std::size_t index = 0; index < fileTokens.size(); ++index) {
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

    if (token.kind == TokenKind::HASH_INCLUDE) {
      const std::size_t occurrence = includeOccurrence++;
      const ResolvedInclude include =
          resolveInclude(fileTokens, index, path, braceDepth,
                         directiveProcessor.conditionalDepth());
      index = include.directiveEnd;
      if (include.dependency != 0) {
        graph.addDependency({.source = unitId,
                             .target = include.dependency,
                             .kind = include.kind,
                             .directive = tokenSpan(token),
                             .includeSpelling = include.includeSpelling,
                             .includeOccurrence = occurrence});
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
  loadedUnit->syntaxDigest = syntaxDigest(output);
  loadedUnit->tokens = std::move(output);
  loadedUnit->inactiveSpans = directiveProcessor.takeInactiveSpans();
  loadedUnit->configurationTokens =
      directiveProcessor.takeConfigurationTokens();
  states[key].state = LoadState::Loaded;
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

void SourceLoader::report(const Token &token, std::string message,
                          std::string code) {
  diagnostics.push_back(makeDiagnostic(std::move(code),
                                       DiagnosticPhase::SourceLoading, token,
                                       std::move(message)));
}

} // namespace lang
