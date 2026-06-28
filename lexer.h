#pragma once

#include "Tokens.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

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

    while (!source.eof()) {
      start = current;
      scan();
    }
    // add end of line token
  }

private:
  bool match(const char &expected) {
    if (source.eof())
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
        while (peek() != '\n' && !source.eof())
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
      } else {
        return;
      }
    }
  }

  char advance() { return source.str().at(current++); }

  char peek() {
    if (source.eof())
      return '\0';
    return source.str().at(current++);
  }

  void add_token(TokenKind token) { add_token(token, nullptr); }

  void add_token(TokenKind token, void *tmp) {
    std::string text = source.str().substr(start, current);
    tokens.push_back(token);
  }

  void string() {
    while (peek() != '"' && !source.eof()) {
      if (peek() == '\n')
        line++;
      advance();
    }

    if (source.eof()) {
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
    while (isNum(peek()))
      advance();

    // Look for a fractional part.
    if (peek() == '.' && isNum(peekNext())) {
      // Consume the "."
      advance();

      while (isNum(peek()))
        advance();
    }
    add_token(TokenKind::INT);
  }

  char peekNext() {
    if (current + 1 >= source.str().length())
      return '\0';
    return source.str().at(current + 1);
  }

private:
  std::stringstream source;
  std::vector<TokenKind> tokens;
  int start = 0;
  int current = 0;
  int line = 1;
};
} // namespace lang
