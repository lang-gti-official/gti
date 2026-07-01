#pragma once

#include "Tokens.h"
#include <initializer_list>
namespace lang {
class Parser {
public:
  Parser() = default;
  Parser(Parser &&) = default;
  Parser(const Parser &) = default;
  Parser &operator=(Parser &&) = default;
  Parser &operator=(const Parser &) = default;
  ~Parser() = default;

private:
  bool match(std::initializer_list<TokenKind> tokens) {
    for (const auto &token : tokens) {
      if (check(token)) {
        advance();
        return true;
      }
    }
    return false;
  }

  bool check(TokenKind kind) {
    if (isAtEnd()) {
      return false;
    }
    return peek().kind == kind;
  }

  bool isAtEnd() { return peek().kind == TokenKind::END_OF_FILE; }

  Token peek() { return tokens.at(current); }

  Token advance() {
    if (!isAtEnd()) {
      current++;
    }
    return previous();
  }

  Token previous() { return tokens.at(current - 1); }

private:
  std::vector<Token> tokens;
  std::size_t current = 0;
};
} // namespace lang
