#pragma once

#include "gti/diagnostic.h"
#include "gti/token.h"
#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
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

  std::vector<Token> consume(const std::filesystem::path &path) {
    std::ifstream file(path);

    if (!file) {
      reset(path.string());
      report("GTI-L0001", "Failed to open file: " + path.string(), 0, 0);
      add_token(TokenKind::END_OF_FILE);
      return std::move(tokens);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return scan(buffer.str(), path.string());
  }

  std::vector<Token> scan(std::string sourceText,
                          std::string sourceName = {}) {
    reset(std::move(sourceName));
    source = std::move(sourceText);

    while (!isAtEnd()) {
      start = current;
      scan();
    }

    add_token(TokenKind::END_OF_FILE);
    return std::move(tokens);
  }

  [[nodiscard]] bool hadError() const { return !diagnostics.empty(); }

  [[nodiscard]] const std::vector<LexDiagnostic> &errors() const {
    return diagnostics;
  }

  [[nodiscard]] const std::string &sourceText() const { return source; }

private:
  void reset(std::string sourceName = {}) {
    source.clear();
    this->sourceName = std::move(sourceName);
    tokens.clear();
    diagnostics.clear();
    start = 0;
    current = 0;
    line = 1;
  }

  bool isAtEnd() { return current >= source.length(); }

  bool match(const char &expected) {
    if (isAtEnd())
      return false;
    if (source.at(current) != expected)
      return false;

    current++;
    return true;
  }

  void scan() {
    const char c = advance();

    switch (c) {
    case ' ':
    case '\r':
    case '\t':
      // Ignore whitespace.
      break;

    case '\n':
      line++;
      break;
    case '@':
      add_token(TokenKind::AT);
      break;
    case '&':
      add_token(TokenKind::AMPERSAND);
      break;
    case '^':
      add_token(TokenKind::CARET);
      break;
    case '#':
      directive();
      break;
    case '(':
      add_token(TokenKind::LEFT_PAREN);
      break;
    case ')':
      add_token(TokenKind::RIGHT_PAREN);
      break;
    case '{':
      add_token(TokenKind::LEFT_BRACE);
      break;
    case '}':
      add_token(TokenKind::RIGHT_BRACE);
      break;
    case '[':
      add_token(TokenKind::LEFT_BRACKET);
      break;
    case ']':
      add_token(TokenKind::RIGHT_BRACKET);
      break;
    case ',':
      add_token(TokenKind::COMMA);
      break;
    case '.':
      add_token(TokenKind::DOT);
      break;
    case '-':
      add_token(match('>')   ? TokenKind::ARROW
                : match('-') ? TokenKind::MINUS_MINUS
                : match('=') ? TokenKind::MINUS_EQUAL
                             : TokenKind::MINUS);
      break;
    case '%':
      add_token(TokenKind::PERCENT);
      break;
    case '|':
      add_token(TokenKind::PIPE);
      break;
    case '+':
      add_token(match('+')   ? TokenKind::PLUS_PLUS
                : match('=') ? TokenKind::PLUS_EQUAL
                             : TokenKind::PLUS);
      break;
    case ';':
      add_token(TokenKind::SEMICOLON);
      break;
    case ':':
      if (match(':')) {
        add_token(TokenKind::SCOPE);
      } else {
        add_token(TokenKind::COLON);
      }
      break;
    case '*':
      add_token(TokenKind::STAR);
      break;
    case '~':
      add_token(TokenKind::TILDE);
      break;
    case '/':
      if (match('/')) {
        // A comment goes until the end of the line.
        while (peek() != '\n' && !isAtEnd())
          advance();
      } else {
        add_token(TokenKind::SLASH);
      }
      break;
    case '!':
      add_token(match('=') ? TokenKind::BANG_EQUAL : TokenKind::BANG);
      break;
    case '=':
      add_token(match('=') ? TokenKind::EQUAL_EQUAL : TokenKind::EQUAL);
      break;
    case '<':
      add_token(match('=') ? TokenKind::LESS_EQUAL : TokenKind::LESS);
      break;
    case '>':
      add_token(match('=') ? TokenKind::GREATER_EQUAL : TokenKind::GREATER);
      break;
    case '"':
      string();
      break;
    default:
      if (isNum(c)) {
        number();
      } else if (isAlpha(c)) {
        identifier();
      } else {
        report("GTI-L0002", std::string("Unexpected character '") + c + "'.");
      }
    }
  }

  char advance() { return source.at(current++); }

  char peek() {
    if (isAtEnd())
      return '\0';
    return source.at(current);
  }

  void add_token(TokenKind token) { add_token(token, std::monostate{}); }

  void add_token(TokenKind kind, Literal literal) {
    std::string text = source.substr(start, current - start);
    tokens.emplace_back(kind, text, std::move(literal), start, line,
                        sourceName);
  }

  void directive() {
    while (isAlphaNumeric(peek())) {
      advance();
    }

    const std::string text = source.substr(start, current - start);
    if (text == "#if") {
      add_token(TokenKind::HASH_IF);
    } else if (text == "#elif") {
      add_token(TokenKind::HASH_ELIF);
    } else if (text == "#else") {
      add_token(TokenKind::HASH_ELSE);
    } else if (text == "#endif") {
      add_token(TokenKind::HASH_ENDIF);
    } else {
      report("GTI-L0003", "Unknown compile-time directive '" + text + "'.");
    }
  }

  void string() {
    while (peek() != '"' && !isAtEnd()) {
      if (peek() == '\\') {
        advance();
        if (!isAtEnd()) {
          if (peek() == '\n') {
            ++line;
          }
          advance();
        }
        continue;
      }
      if (peek() == '\n') {
        ++line;
      }

      advance();
    }

    if (isAtEnd()) {
      report("GTI-L0004", "Unterminated string.", start,
             std::min(start + 1, source.size()));
      return;
    }

    advance(); // closing quote

    const std::string_view encoded(source.data() + start + 1,
                                   current - start - 2);
    std::string value;
    value.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      if (encoded[index] != '\\' || index + 1 >= encoded.size()) {
        value += encoded[index];
        continue;
      }

      const char escaped = encoded[++index];
      switch (escaped) {
      case '\\':
        value += '\\';
        break;
      case '"':
        value += '"';
        break;
      case 'n':
        value += '\n';
        break;
      case 'r':
        value += '\r';
        break;
      case 't':
        value += '\t';
        break;
      case '0':
        value += '\0';
        break;
      default:
        const std::size_t escapeStart = start + index;
        report("GTI-L0005",
               std::string("Unknown escape sequence '\\") + escaped + "'.",
               escapeStart, std::min(escapeStart + 2, source.size()));
        value += escaped;
      }
    }
    add_token(TokenKind::STRING_LITERAL, value);
  }

  bool isNum(const char c) { return c >= '0' && c <= '9'; }

  void number() {
    while (isNum(peek())) {
      advance();
    }

    // Look for a fractional part.
    if (peek() == '.' && isNum(peekNext())) {
      // Consume the "."
      advance();

      while (isNum(peek())) {
        advance();
      }

      std::string text = source.substr(start, current - start);
      try {
        add_token(TokenKind::FLOAT_LITERAL, std::stod(text));
      } catch (const std::exception &) {
        report("GTI-L0006", "Invalid floating-point literal.");
        add_token(TokenKind::FLOAT_LITERAL, 0.0);
      }
    } else {
      std::string text = source.substr(start, current - start);
      std::uint64_t value = 0;
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size()) {
        report("GTI-L0007", "Invalid integer literal.");
        add_token(TokenKind::INT_LITERAL, std::uint64_t{0});
      } else {
        add_token(TokenKind::INT_LITERAL, value);
      }
    }
  }

  char peekNext() {
    if (current + 1 >= source.length())
      return '\0';
    return source.at(current + 1);
  }

  bool isAlpha(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  bool isAlphaNumeric(const char c) { return isAlpha(c) || isNum(c); }

  void identifier() {
    while (isAlphaNumeric(peek())) {
      advance();
    }
    std::string text = source.substr(start, current - start);
    if (text.rfind("__gti_", 0) == 0) {
      report("GTI-L0008",
             "Identifiers beginning with '__gti_' are reserved for "
             "compiler-generated names.");
    }
    if (auto type = keywords.find(text); type != keywords.end()) {
      add_token(type->second);
    } else {
      add_token(TokenKind::IDENTIFIER);
    }
  }

  [[nodiscard]] int lineAt(std::size_t position) const {
    int result = 1;
    const std::size_t limit = std::min(position, source.size());
    for (std::size_t index = 0; index < limit; ++index) {
      if (source[index] == '\n') {
        ++result;
      }
    }
    return result;
  }

  void report(std::string code, std::string message) {
    report(std::move(code), std::move(message), start, current);
  }

  void report(std::string code, std::string message, std::size_t errorStart,
              std::size_t errorEnd) {
    diagnostics.push_back(makeDiagnostic(
        std::move(code), DiagnosticPhase::Lexing,
        SourceSpan{sourceName, errorStart, errorEnd, lineAt(errorStart)},
        std::move(message)));
  }

private:
  std::string source;
  std::string sourceName;
  std::vector<Token> tokens;
  std::vector<LexDiagnostic> diagnostics;
  std::size_t start = 0;
  std::size_t current = 0;
  int line = 1;
};
} // namespace lang
