#pragma once

#include "gti/diagnostic.h"
#include "gti/lexer.h"
#include "gti/source_graph.h"
#include "gti/token.h"

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
  SourceGraph load(const std::filesystem::path &entryPath,
                   std::optional<std::string> entrySource = std::nullopt,
                   const std::vector<std::filesystem::path> &preludePaths = {},
                   const std::unordered_map<std::string, std::string>
                       &sourceOverrides = {}) {
    diagnostics.clear();
    states.clear();
    graph.clear();
    sourceManager.clear();
    this->entrySource = std::move(entrySource);
    this->sourceOverrides = &sourceOverrides;
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

  SourceUnitId loadFile(const std::filesystem::path &path, bool isEntry,
                        bool isPrelude, const Token *includeToken) {
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
      }
      if (isEntry) {
        graph.entry = state->second.unit;
      }
      return state->second.unit;
    }
    const SourceUnitId unitId = graph.addUnit(path, isEntry, isPrelude);
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
        const auto [directiveEnd, dependency] = resolveInclude(
            fileTokens, index, path, braceDepth, conditionalDepth);
        index = directiveEnd;
        if (dependency != 0) {
          graph.addDependency({.source = unitId,
                               .target = dependency,
                               .kind = SourceDependencyKind::Include,
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

  std::pair<std::size_t, SourceUnitId>
  resolveInclude(std::vector<Token> &tokens, std::size_t index,
                 const std::filesystem::path &includingFile, int braceDepth,
                 int conditionalDepth) {
    const Token includeToken = tokens[index];
    const bool hasPath = index + 1 < tokens.size() &&
                         tokens[index + 1].kind == TokenKind::STRING_LITERAL;
    std::size_t directiveEnd = hasPath ? index + 1 : index;
    if (hasPath && index + 2 < tokens.size() &&
        tokens[index + 2].kind == TokenKind::SEMICOLON) {
      directiveEnd = index + 2;
    }

    if (braceDepth != 0) {
      report(includeToken, "Include directives are only allowed at top level.",
             "GTI-I0004");
      return {directiveEnd, 0};
    }
    if (conditionalDepth != 0) {
      report(includeToken,
             "Include directives cannot appear inside '#if' blocks.",
             "GTI-I0004");
      return {directiveEnd, 0};
    }
    if (!hasPath) {
      report(includeToken, "Expect a quoted .gti path after 'include'.",
             "GTI-I0005");
      return {directiveEnd, 0};
    }

    const Token &pathToken = tokens[index + 1];
    const auto *pathText = std::get_if<std::string>(&pathToken.literal);
    if (pathText == nullptr || pathText->empty()) {
      report(pathToken, "Include path cannot be empty.", "GTI-I0006");
      return {directiveEnd, 0};
    }

    const std::filesystem::path requestedPath(*pathText);
    if (requestedPath.is_absolute()) {
      report(pathToken, "Include path must be relative to the including file.",
             "GTI-I0006");
      return {directiveEnd, 0};
    }
    if (requestedPath.extension() != ".gti") {
      report(pathToken, "Included source file must use the .gti extension.",
             "GTI-I0006");
      return {directiveEnd, 0};
    }

    const std::filesystem::path resolved =
        canonicalPath(includingFile.parent_path() / requestedPath);
    return {directiveEnd, loadFile(resolved, false, false, &includeToken)};
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
  bool entrySourceConsumed = false;
};

} // namespace lang
