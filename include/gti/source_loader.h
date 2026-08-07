#pragma once

#include "gti/diagnostic.h"
#include "gti/lexer.h"
#include "gti/source_graph.h"
#include "gti/token.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lang {

using SourceDiagnostic = Diagnostic;

class SourceLoader {
public:
  SourceGraph
  load(const std::filesystem::path &entryPath,
       std::optional<std::string> entrySource = std::nullopt,
       const std::vector<std::filesystem::path> &preludePaths = {},
       const std::unordered_map<std::string, std::string> &sourceOverrides = {},
       const std::vector<std::filesystem::path> &standardLibraryRoots = {}) {
    diagnostics.clear();
    states.clear();
    graph.clear();
    sourceManager.clear();
    this->entrySource = std::move(entrySource);
    this->sourceOverrides = &sourceOverrides;
    this->standardLibraryRoots.clear();
    this->standardLibraryRoots.reserve(standardLibraryRoots.size());
    for (const std::filesystem::path &root : standardLibraryRoots) {
      this->standardLibraryRoots.emplace_back(canonicalPath(root));
    }
    entrySourceConsumed = false;
    std::vector<SourceUnitId> preludes;
    const std::filesystem::path canonicalEntry = canonicalPath(entryPath);
    for (const std::filesystem::path &preludePath : preludePaths) {
      preludes.push_back(
          loadFile(canonicalPath(preludePath), false, true, nullptr));
    }
    const SourceUnitId entry = loadFile(canonicalEntry, true, false, nullptr);

    for (const SourceUnit &unit : graph.sourceUnits()) {
      if (unit.prelude) {
        continue;
      }
      for (const SourceUnitId prelude : preludes) {
        if (unit.id != prelude &&
            !graph.hasDirectDependency(unit.id, prelude) &&
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
    return std::move(graph);
  }

  [[nodiscard]] bool hadError() const { return !diagnostics.empty(); }

  [[nodiscard]] const std::vector<SourceDiagnostic> &errors() const {
    return diagnostics;
  }

  [[nodiscard]] const SourceManager &sources() const { return sourceManager; }

private:
  enum class LoadState {
    Visiting,
    Loaded,
  };

  struct FileState {
    LoadState state = LoadState::Visiting;
    SourceUnitId unit = 0;
  };

  struct ResolvedInclude {
    std::size_t directiveEnd = 0;
    SourceUnitId dependency = 0;
    SourceDependencyKind kind = SourceDependencyKind::Include;
  };

  static std::filesystem::path
  canonicalPath(const std::filesystem::path &path) {
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

  static bool isStandardLibraryPathSegment(const Token &token) {
    if (token.lexeme.empty()) {
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
    return validStart(token.lexeme.front()) &&
           std::all_of(token.lexeme.begin() + 1, token.lexeme.end(), validPart);
  }

  SourceUnitId
  loadFile(const std::filesystem::path &path, bool isEntry, bool isPrelude,
           const Token *includeToken,
           std::optional<std::string> standardLibraryName = std::nullopt) {
    const std::string key = path.string();
    if (const auto state = states.find(key); state != states.end()) {
      if (state->second.state == LoadState::Visiting &&
          includeToken != nullptr) {
        report(*includeToken, "Include cycle detected for '" + key + "'.",
               "GTI-I0001");
      }
      if (SourceUnit *unit = graph.findUnit(state->second.unit)) {
        unit->entry = unit->entry || isEntry;
        unit->prelude = unit->prelude || isPrelude;
        if (standardLibraryName) {
          unit->standardLibraryName = std::move(standardLibraryName);
        }
      }
      if (isEntry) {
        graph.entry = state->second.unit;
      }
      return state->second.unit;
    }
    const SourceUnitId unitId =
        graph.addUnit(path, isEntry, isPrelude, std::move(standardLibraryName));
    states.emplace(key,
                   FileState{.state = LoadState::Visiting, .unit = unitId});

    Lexer lexer;
    std::vector<Token> fileTokens;
    if (isEntry && entrySource && !entrySourceConsumed) {
      entrySourceConsumed = true;
      fileTokens = lexer.scan(*entrySource, key);
    } else if (const auto override = sourceOverrides->find(key);
               override != sourceOverrides->end()) {
      fileTokens = lexer.scan(override->second, key);
    } else {
      fileTokens = lexer.consume(path);
    }
    sourceManager.set(key, lexer.sourceText());

    for (const LexDiagnostic &diagnostic : lexer.errors()) {
      Diagnostic forwarded = diagnostic;
      if (includeToken != nullptr) {
        forwarded.related.push_back(
            {tokenSpan(*includeToken), "Included from here."});
      }
      diagnostics.emplace_back(std::move(forwarded));
    }
    if (lexer.hadError()) {
      states[key].state = LoadState::Loaded;
      return unitId;
    }

    std::vector<Token> output;
    int braceDepth = 0;
    int conditionalDepth = 0;
    Token outerConditional;
    for (std::size_t index = 0; index < fileTokens.size(); ++index) {
      Token &token = fileTokens[index];
      if (token.kind == TokenKind::END_OF_FILE) {
        output.push_back(std::move(token));
        continue;
      }

      if (token.kind == TokenKind::HASH_IF) {
        if (conditionalDepth == 0) {
          outerConditional = token;
        }
        ++conditionalDepth;
      } else if (token.kind == TokenKind::HASH_ENDIF) {
        if (conditionalDepth == 0) {
          report(token, "Unexpected '#endif' without a matching '#if'.",
                 "GTI-I0002");
        } else {
          --conditionalDepth;
        }
      } else if ((token.kind == TokenKind::HASH_ELIF ||
                  token.kind == TokenKind::HASH_ELSE) &&
                 conditionalDepth == 0) {
        report(token,
               "Unexpected '" + token.lexeme + "' without a matching '#if'.",
               "GTI-I0002");
      }

      if (token.kind == TokenKind::INCLUDE) {
        const ResolvedInclude include = resolveInclude(
            fileTokens, index, path, braceDepth, conditionalDepth);
        index = include.directiveEnd;
        if (include.dependency != 0) {
          graph.addDependency({.source = unitId,
                               .target = include.dependency,
                               .kind = include.kind,
                               .directive = tokenSpan(token)});
        }
        continue;
      }

      if (token.kind == TokenKind::LEFT_BRACE) {
        ++braceDepth;
      } else if (token.kind == TokenKind::RIGHT_BRACE && braceDepth > 0) {
        --braceDepth;
      }
      output.push_back(std::move(token));
    }

    if (conditionalDepth != 0) {
      report(outerConditional,
             "Unterminated compile-time conditional. Expect '#endif'.",
             "GTI-I0003");
    }

    if (output.empty() || output.back().kind != TokenKind::END_OF_FILE) {
      output.emplace_back(TokenKind::END_OF_FILE, "", std::monostate{}, 0, 1,
                          key);
    }
    graph.findUnit(unitId)->tokens = std::move(output);
    states[key].state = LoadState::Loaded;
    return unitId;
  }

  ResolvedInclude resolveInclude(std::vector<Token> &tokens, std::size_t index,
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
             "Expect a quoted .gti path or <std/name> after 'include'.",
             "GTI-I0005");
      return {.directiveEnd = directiveEnd};
    }

    if (hasStandardPath) {
      return resolveStandardLibraryInclude(tokens, index, includeToken);
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
    return {.directiveEnd = directiveEnd,
            .dependency = loadFile(resolved, false, false, &includeToken)};
  }

  ResolvedInclude resolveStandardLibraryInclude(std::vector<Token> &tokens,
                                                std::size_t index,
                                                const Token &includeToken) {
    std::size_t current = index + 2;
    std::vector<std::string> segments;
    while (current < tokens.size() &&
           isStandardLibraryPathSegment(tokens[current])) {
      segments.emplace_back(tokens[current].lexeme);
      ++current;
      if (current >= tokens.size() ||
          tokens[current].kind != TokenKind::SLASH) {
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

    if (!closed || segments.size() < 2 || segments.front() != "std") {
      report(includeToken,
             "Standard-library includes use syntax such as "
             "'include <std/array>'.",
             "GTI-I0007");
      return {.directiveEnd = directiveEnd,
              .kind = SourceDependencyKind::StandardLibrary};
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
        return {.directiveEnd = directiveEnd,
                .dependency = loadFile(candidate, false, false, &includeToken,
                                       importName),
                .kind = SourceDependencyKind::StandardLibrary};
      }
    }

    report(includeToken,
           "Standard-library unit '<" + importName + ">' was not found.",
           "GTI-I0007");
    return {.directiveEnd = directiveEnd,
            .kind = SourceDependencyKind::StandardLibrary};
  }

  void report(const Token &token, std::string message,
              std::string code = "GTI-I0000") {
    diagnostics.push_back(makeDiagnostic(std::move(code),
                                         DiagnosticPhase::SourceLoading, token,
                                         std::move(message)));
  }

  std::vector<SourceDiagnostic> diagnostics;
  SourceGraph graph;
  SourceManager sourceManager;
  std::unordered_map<std::string, FileState> states;
  std::optional<std::string> entrySource;
  const std::unordered_map<std::string, std::string> *sourceOverrides = nullptr;
  std::vector<std::filesystem::path> standardLibraryRoots;
  bool entrySourceConsumed = false;
};

} // namespace lang
