#pragma once

#include "gti/token.h"
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

struct LexDiagnostic {
  std::string source;
  int line;
  std::size_t position;
  std::string message;
};

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
      report("Failed to open file: " + path.string());
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
      add_token(match('-')   ? TokenKind::MINUS_MINUS
                : match('=') ? TokenKind::MINUS_EQUAL
                             : TokenKind::MINUS);
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
        report("Unexpected ':'. Use '::' for scope resolution.");
      }
      break;
    case '*':
      add_token(TokenKind::STAR);
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
        report(std::string("Unexpected character '") + c + "'.");
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
      report("Unterminated string.");
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
        report(std::string("Unknown escape sequence '\\") + escaped + "'.");
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
        report("Invalid floating-point literal.");
        add_token(TokenKind::FLOAT_LITERAL, 0.0);
      }
    } else {
      std::string text = source.substr(start, current - start);
      try {
        add_token(TokenKind::INT_LITERAL, std::stoi(text));
      } catch (const std::exception &) {
        report("Invalid integer literal.");
        add_token(TokenKind::INT_LITERAL, 0);
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
    if (auto type = keywords.find(text); type != keywords.end()) {
      add_token(type->second);
    } else {
      add_token(TokenKind::IDENTIFIER);
    }
  }

  void report(std::string_view message) {
    diagnostics.push_back({sourceName, line, static_cast<std::size_t>(start),
                           std::string(message)});
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
