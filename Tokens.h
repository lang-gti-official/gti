#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace lang {

enum class TokenKind : std::uint8_t {
  // Single-character tokens.
  LEFT_PAREN,
  RIGHT_PAREN,
  LEFT_BRACE,
  RIGHT_BRACE,
  COMMA,
  DOT,
  MINUS,
  PLUS,
  SEMICOLON,
  SLASH,
  STAR,

  // One or two character tokens.
  BANG,
  BANG_EQUAL,
  EQUAL,
  EQUAL_EQUAL,
  GREATER,
  GREATER_EQUAL,
  LESS,
  LESS_EQUAL,

  // Literals.
  IDENTIFIER,
  STRING_LITERAL,
  INT_LITERAL,
  FLOAT_LITERAL,

  // Keywords.
  AND,
  CLASS,
  ELSE,
  FALSE,
  FOR,
  IF,
  OR,
  PRINT,
  RETURN,
  TRUE,
  WHILE,

  // Type keywords.
  INT,
  FLOAT,
  BOOL,

  // Special keywords.
  SELF,
  NULLPTR,

  END_OF_FILE,
};

using Literal = std::variant<std::monostate, int, double, std::string, bool>;

struct Token {
  Token() = default;

  Token(TokenKind kind, std::string lexeme, Literal literal,
        std::size_t position, int line)
      : kind(kind), lexeme(std::move(lexeme)), literal(std::move(literal)),
        position(position), line(line) {}

  TokenKind kind{};
  std::string lexeme;
  Literal literal{};
  std::size_t position{};
  int line{};
};

inline const std::unordered_map<std::string_view, TokenKind> keywords{
    {"and", TokenKind::AND},       {"class", TokenKind::CLASS},
    {"else", TokenKind::ELSE},     {"false", TokenKind::FALSE},
    {"for", TokenKind::FOR},       {"if", TokenKind::IF},
    {"or", TokenKind::OR},         {"print", TokenKind::PRINT},
    {"return", TokenKind::RETURN}, {"true", TokenKind::TRUE},
    {"while", TokenKind::WHILE},

    {"int", TokenKind::INT},       {"float", TokenKind::FLOAT},
    {"bool", TokenKind::BOOL},

    {"self", TokenKind::SELF},     {"nullptr", TokenKind::NULLPTR},
};

inline constexpr std::string_view to_string(TokenKind kind) {
  switch (kind) {
  case TokenKind::LEFT_PAREN:
    return "LEFT_PAREN";
  case TokenKind::RIGHT_PAREN:
    return "RIGHT_PAREN";
  case TokenKind::LEFT_BRACE:
    return "LEFT_BRACE";
  case TokenKind::RIGHT_BRACE:
    return "RIGHT_BRACE";
  case TokenKind::COMMA:
    return "COMMA";
  case TokenKind::DOT:
    return "DOT";
  case TokenKind::MINUS:
    return "MINUS";
  case TokenKind::PLUS:
    return "PLUS";
  case TokenKind::SEMICOLON:
    return "SEMICOLON";
  case TokenKind::SLASH:
    return "SLASH";
  case TokenKind::STAR:
    return "STAR";

  case TokenKind::BANG:
    return "BANG";
  case TokenKind::BANG_EQUAL:
    return "BANG_EQUAL";
  case TokenKind::EQUAL:
    return "EQUAL";
  case TokenKind::EQUAL_EQUAL:
    return "EQUAL_EQUAL";
  case TokenKind::GREATER:
    return "GREATER";
  case TokenKind::GREATER_EQUAL:
    return "GREATER_EQUAL";
  case TokenKind::LESS:
    return "LESS";
  case TokenKind::LESS_EQUAL:
    return "LESS_EQUAL";

  case TokenKind::IDENTIFIER:
    return "IDENTIFIER";
  case TokenKind::STRING_LITERAL:
    return "STRING_LITERAL";
  case TokenKind::INT_LITERAL:
    return "INT_LITERAL";
  case TokenKind::FLOAT_LITERAL:
    return "FLOAT_LITERAL";

  case TokenKind::AND:
    return "AND";
  case TokenKind::CLASS:
    return "CLASS";
  case TokenKind::ELSE:
    return "ELSE";
  case TokenKind::FALSE:
    return "FALSE";
  case TokenKind::FOR:
    return "FOR";
  case TokenKind::IF:
    return "IF";
  case TokenKind::OR:
    return "OR";
  case TokenKind::PRINT:
    return "PRINT";
  case TokenKind::RETURN:
    return "RETURN";
  case TokenKind::TRUE:
    return "TRUE";
  case TokenKind::WHILE:
    return "WHILE";

  case TokenKind::INT:
    return "INT";
  case TokenKind::FLOAT:
    return "FLOAT";
  case TokenKind::BOOL:
    return "BOOL";

  case TokenKind::SELF:
    return "SELF";
  case TokenKind::NULLPTR:
    return "NULLPTR";

  case TokenKind::END_OF_FILE:
    return "END_OF_FILE";
  }

  return "UNKNOWN";
}

inline std::ostream &operator<<(std::ostream &os, TokenKind kind) {
  return os << to_string(kind);
}

} // namespace lang
