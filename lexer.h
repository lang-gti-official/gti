#pragma once

#include "Tokens.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace lang {

class Lexer {
public:
  Lexer() = default;
  Lexer(Lexer &&) = default;
  Lexer(const Lexer &) = delete;
  Lexer &operator=(Lexer &&) = default;
  Lexer &operator=(const Lexer &) = delete;
  ~Lexer() = default;

  void consume(const std::filesystem::path &path) {
    std::ifstream file(path);

    if (!file) {
      std::cerr << "Failed to open file: " << path << '\n';
      return;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    source = buffer.str();

    while (!isAtEnd()) {
      start = current;
      scan();
    }

    for (const auto &token : tokens) {
      std::cout << to_string(token.kind) << " \"" << token.lexeme << "\""
                << " line=" << token.line << " pos=" << token.position << '\n';
    }
    add_token(TokenKind::END_OF_FILE);
  }

private:
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
    case ',':
      add_token(TokenKind::COMMA);
      break;
    case '.':
      add_token(TokenKind::DOT);
      break;
    case '-':
      add_token(TokenKind::MINUS);
      break;
    case '+':
      add_token(TokenKind::PLUS);
      break;
    case ';':
      add_token(TokenKind::SEMICOLON);
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
        std::cout << "unknown text" << std::endl;
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
    tokens.emplace_back(kind, text, std::move(literal), start, line);
  }

  void string() {
    while (peek() != '"' && !isAtEnd()) {
      if (peek() == '\n') {
        ++line;
      }

      advance();
    }

    if (isAtEnd()) {
      std::cerr << "Unterminated string on line " << line << '\n';
      return;
    }

    advance(); // closing quote

    std::string value = source.substr(start + 1, current - start - 2);
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
      add_token(TokenKind::FLOAT_LITERAL, std::stod(text));
    } else {
      std::string text = source.substr(start, current - start);
      add_token(TokenKind::INT_LITERAL, std::stoi(text));
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
      add_token(type->second, 0);
    } else {
      add_token(TokenKind::IDENTIFIER, 0);
    }
  }

private:
  std::string source;
  std::vector<Token> tokens;
  int start = 0;
  int current = 0;
  int line = 1;
};
} // namespace lang
