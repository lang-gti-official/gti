#pragma once

#include "gti/diagnostic.h"
#include "gti/token.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

using LexDiagnostic = Diagnostic;

class Lexer {
public:
  Lexer() = default;
  Lexer(Lexer &&) = default;
  Lexer(const Lexer &) = delete;
  Lexer &operator=(Lexer &&) = default;
  Lexer &operator=(const Lexer &) = delete;
  ~Lexer() = default;

  std::vector<Token> consume(const std::filesystem::path &path);
  std::vector<Token> scan(std::string sourceText, std::string sourceName = {});
  // Project discovery reads only a leading module declaration, not its body.
  std::vector<Token> scanModuleHeader(std::string sourceText,
                                      std::string sourceName = {});
  std::vector<Token> scanForCompletion(std::string sourceText,
                                       std::size_t byteOffset,
                                       std::string sourceName = {});

  [[nodiscard]] bool hadError() const;
  [[nodiscard]] const std::vector<LexDiagnostic> &errors() const;
  [[nodiscard]] const std::string &sourceText() const;

private:
  void reset(std::string sourceName = {});
  [[nodiscard]] bool isAtEnd() const;
  bool match(char expected);
  void scanToken();
  char advance();
  [[nodiscard]] char peek() const;
  [[nodiscard]] char peekNext() const;
  void addToken(TokenKind token);
  void addToken(TokenKind kind, Literal literal);
  void lineComment();
  void blockComment();
  void directive();
  void string();
  void character();
  char decodeEscape(char escaped, std::size_t escapeStart,
                    bool characterLiteral);
  [[nodiscard]] static bool isNumber(char value);
  [[nodiscard]] static bool isAlpha(char value);
  [[nodiscard]] static bool isAlphaNumeric(char value);
  void prefixedInteger(int base, std::string_view description);
  void number();
  void identifier();
  [[nodiscard]] int lineAt(std::size_t position) const;
  void report(std::string code, std::string message);
  void report(std::string code, std::string message, std::size_t errorStart,
              std::size_t errorEnd);

  std::string source;
  std::string sourceName;
  std::vector<Token> tokens;
  std::vector<LexDiagnostic> diagnostics;
  // Half-open editor exclusion ranges, including unterminated trivia/literals.
  std::vector<std::pair<std::size_t, std::size_t>> completionExclusions;
  bool collectCompletionExclusions = false;
  std::size_t start = 0;
  std::size_t current = 0;
  int line = 1;
  std::optional<std::string> pendingDocumentationMarkdown;
  int pendingDocumentationLastLine = 0;
};

} // namespace lang
