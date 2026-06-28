#pragma once

#include <string>
#include <unordered_map>
namespace lang {
enum class TokenKind : uint8_t {
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
  STRING,
  INT,

  // Keywords.
  AND,
  CLASS,
  ELSE,
  FALSE,
  FUN,
  FOR,
  IF,
  NIL,
  OR,
  PRINT,
  RETURN,
  SUPER,
  THIS,
  TRUE,
  VAR,
  WHILE,
  END_OF_FILE,
};

static const std::unordered_map<std::string_view, TokenKind> keywords{
    {"and", TokenKind::AND},       {"class", TokenKind::CLASS},
    {"else", TokenKind::ELSE},     {"false", TokenKind::FALSE},
    {"fun", TokenKind::FUN},       {"for", TokenKind::FOR},
    {"if", TokenKind::IF},         {"nil", TokenKind::NIL},
    {"or", TokenKind::OR},         {"print", TokenKind::PRINT},
    {"return", TokenKind::RETURN}, {"super", TokenKind::SUPER},
    {"this", TokenKind::THIS},     {"true", TokenKind::TRUE},
    {"var", TokenKind::VAR},       {"while", TokenKind::WHILE},
};

struct Token {
  Token() = default;
  Token(TokenKind kind, std::string lexeme, std::size_t position, int line,
        int column)
      : kind(kind), lexeme(lexeme), position(position), line(line),
        column(column) {}
  TokenKind kind;
  std::string lexeme;
  std::size_t position;
  int line;
  int column;
};
} // namespace lang
