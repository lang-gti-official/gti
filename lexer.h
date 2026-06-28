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
  Lexer(const Lexer &) = default;
  Lexer &operator=(Lexer &&) = default;
  Lexer &operator=(const Lexer &) = default;
  ~Lexer() = default;

  void consume(const std::filesystem::path &path) {
    std::ifstream file("test_lang.cpp");
    source << file.rdbuf();

    while (!isAtEnd()) {
      start = current;
      scan();
    }

    for (const auto &token : tokens) {
      std::cout << static_cast<int>(token) << std::endl;
    }
    // add end of line token
  }

private:
  bool isAtEnd() { return current >= source.str().length(); }

  bool match(const char &expected) {
    if (isAtEnd())
      return false;
    if (source.str().at(current) != expected)
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

  char advance() { return source.str().at(current++); }

  char peek() {
    if (isAtEnd())
      return '\0';
    return source.str().at(current++);
  }

  void add_token(TokenKind token) { add_token(token, nullptr); }

  template <typename T> void add_token(TokenKind token, T value) {
    std::string text = source.str().substr(start, current);
    tokens.push_back(token);
  }

  void string() {
    while (peek() != '"' && !isAtEnd()) {
      if (peek() == '\n')
        line++;
      advance();
    }

    if (isAtEnd()) {
      return;
    }

    // The closing ".
    advance();

    // Trim the surrounding quotes.
    std::string value = source.str().substr(start + 1, current - 1);

    add_token(TokenKind::STRING, nullptr);
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
      add_token(TokenKind::FLOAT,
                std::stod(source.str().substr(start, current)));
    } else {
      add_token(TokenKind::INT);
    }
  }

  char peekNext() {
    if (current + 1 >= source.str().length())
      return '\0';
    return source.str().at(current + 1);
  }

  bool isAlpha(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  }

  bool isAlphaNumeric(const char c) { return isAlpha(c) || isNum(c); }

  void identifier() {
    while (isAlphaNumeric(peek())) {
      advance();
    }
    std::string text = source.str().substr(start, current);
    if (auto type = keywords.find(text); type != keywords.end()) {
      add_token(type->second);
    } else {
      add_token(TokenKind::IDENTIFIER);
    }
  }

private:
  std::stringstream source;
  std::vector<TokenKind> tokens;
  int start = 0;
  int current = 0;
  int line = 1;
};
} // namespace lang
