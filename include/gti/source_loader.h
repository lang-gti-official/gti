#pragma once

#include "gti/diagnostic.h"
#include "gti/lexer.h"
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
  std::vector<Token>
  load(const std::filesystem::path &entryPath,
       std::optional<std::string> entrySource = std::nullopt,
       const std::vector<std::filesystem::path> &preludePaths = {}) {
    diagnostics.clear();
    states.clear();
    sourceManager.clear();
    this->entrySource = std::move(entrySource);
    entrySourceConsumed = false;
    entryEof = Token{};

    std::vector<Token> tokens;
    const std::filesystem::path canonicalEntry = canonicalPath(entryPath);
    for (const std::filesystem::path &preludePath : preludePaths) {
      loadFile(canonicalPath(preludePath), false, nullptr, tokens);
    }
    loadFile(canonicalEntry, true, nullptr, tokens);

    if (entryEof.kind != TokenKind::END_OF_FILE) {
      entryEof = Token(TokenKind::END_OF_FILE, "", std::monostate{}, 0, 1,
                       canonicalEntry.string());
    }
    tokens.push_back(std::move(entryEof));
    return tokens;
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

  void loadFile(const std::filesystem::path &path, bool isEntry,
                const Token *includeToken, std::vector<Token> &output) {
    const std::string key = path.string();
    if (const auto state = states.find(key); state != states.end()) {
      if (state->second == LoadState::Visiting && includeToken != nullptr) {
        report(*includeToken, "Include cycle detected for '" + key + "'.",
               "GTI-I0001");
      }
      return;
    }
    states.emplace(key, LoadState::Visiting);

    Lexer lexer;
    std::vector<Token> fileTokens;
    if (isEntry && entrySource && !entrySourceConsumed) {
      entrySourceConsumed = true;
      fileTokens = lexer.scan(*entrySource, key);
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
      states[key] = LoadState::Loaded;
      return;
    }

    int braceDepth = 0;
    int conditionalDepth = 0;
    Token outerConditional;
    for (std::size_t index = 0; index < fileTokens.size(); ++index) {
      Token &token = fileTokens[index];
      if (token.kind == TokenKind::END_OF_FILE) {
        if (isEntry) {
          entryEof = token;
        }
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
        index = resolveInclude(fileTokens, index, path, braceDepth,
                               conditionalDepth, output);
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

    states[key] = LoadState::Loaded;
  }

  std::size_t resolveInclude(std::vector<Token> &tokens, std::size_t index,
                             const std::filesystem::path &includingFile,
                             int braceDepth, int conditionalDepth,
                             std::vector<Token> &output) {
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
      return directiveEnd;
    }
    if (conditionalDepth != 0) {
      report(includeToken,
             "Include directives cannot appear inside '#if' blocks.",
             "GTI-I0004");
      return directiveEnd;
    }
    if (!hasPath) {
      report(includeToken, "Expect a quoted .gti path after 'include'.",
             "GTI-I0005");
      return directiveEnd;
    }

    const Token &pathToken = tokens[index + 1];
    const auto *pathText = std::get_if<std::string>(&pathToken.literal);
    if (pathText == nullptr || pathText->empty()) {
      report(pathToken, "Include path cannot be empty.", "GTI-I0006");
      return directiveEnd;
    }

    const std::filesystem::path requestedPath(*pathText);
    if (requestedPath.is_absolute()) {
      report(pathToken, "Include path must be relative to the including file.",
             "GTI-I0006");
      return directiveEnd;
    }
    if (requestedPath.extension() != ".gti") {
      report(pathToken, "Included source file must use the .gti extension.",
             "GTI-I0006");
      return directiveEnd;
    }

    const std::filesystem::path resolved =
        canonicalPath(includingFile.parent_path() / requestedPath);
    loadFile(resolved, false, &includeToken, output);
    return directiveEnd;
  }

  void report(const Token &token, std::string message,
              std::string code = "GTI-I0000") {
    diagnostics.push_back(makeDiagnostic(std::move(code),
                                         DiagnosticPhase::SourceLoading, token,
                                         std::move(message)));
  }

  std::vector<SourceDiagnostic> diagnostics;
  SourceManager sourceManager;
  std::unordered_map<std::string, LoadState> states;
  std::optional<std::string> entrySource;
  bool entrySourceConsumed = false;
  Token entryEof;
};

} // namespace lang
