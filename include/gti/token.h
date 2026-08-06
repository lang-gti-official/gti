#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace lang {

enum class TokenKind : std::uint8_t {
  // Single-character tokens.
  AMPERSAND,
  AT,
  CARET,
  LEFT_PAREN,
  RIGHT_PAREN,
  LEFT_BRACE,
  RIGHT_BRACE,
  LEFT_BRACKET,
  RIGHT_BRACKET,
  COLON,
  COMMA,
  DOT,
  MINUS,
  PERCENT,
  PIPE,
  PLUS,
  SEMICOLON,
  SLASH,
  STAR,
  TILDE,

  // One or two character tokens.
  BANG,
  BANG_EQUAL,
  EQUAL,
  EQUAL_EQUAL,
  GREATER,
  GREATER_EQUAL,
  LESS,
  LESS_EQUAL,
  MINUS_MINUS,
  MINUS_EQUAL,
  PLUS_PLUS,
  PLUS_EQUAL,
  SCOPE,

  // Parser-combined operators. Angle tokens remain separate for generics.
  SHIFT_LEFT,
  SHIFT_RIGHT,

  // Compile-time directives.
  HASH_IF,
  HASH_ELIF,
  HASH_ELSE,
  HASH_ENDIF,

  // Literals.
  IDENTIFIER,
  STRING_LITERAL,
  INT_LITERAL,
  FLOAT_LITERAL,

  // Keywords.
  AND,
  BREAK,
  CLASS,
  CONTINUE,
  ELSE,
  FALSE,
  FOR,
  IF,
  INCLUDE,
  MUT,
  NAMESPACE,
  OR,
  PRIVATE,
  PUBLIC,
  RETURN,
  STRUCT,
  TRUE,
  WHILE,

  // Type keywords.
  INT,
  INT8,
  INT16,
  INT32,
  INT64,
  UINT,
  UINT8,
  UINT16,
  UINT32,
  UINT64,
  FLOAT,
  BOOL,
  STRING_TYPE,
  EXPECTED,
  VOID,

  // Special keywords.
  SELF,
  NULLPTR,
  UNEXPECTED,

  END_OF_FILE,
};

using Literal =
    std::variant<std::monostate, std::nullptr_t, std::uint64_t, double,
                 std::string, bool>;

struct Token {
  Token() = default;

  Token(TokenKind kind, std::string lexeme, Literal literal,
        std::size_t position, int line, std::string source = {})
      : kind(kind), lexeme(std::move(lexeme)), literal(std::move(literal)),
        position(position), line(line), source(std::move(source)) {}

  TokenKind kind{};
  std::string lexeme;
  Literal literal{};
  std::size_t position{};
  int line{};
  std::string source;
};

inline const std::unordered_map<std::string_view, TokenKind> keywords{
    {"and", TokenKind::AND},
    {"break", TokenKind::BREAK},
    {"class", TokenKind::CLASS},
    {"continue", TokenKind::CONTINUE},
    {"else", TokenKind::ELSE},
    {"false", TokenKind::FALSE},
    {"for", TokenKind::FOR},
    {"if", TokenKind::IF},
    {"include", TokenKind::INCLUDE},
    {"mut", TokenKind::MUT},
    {"namespace", TokenKind::NAMESPACE},
    {"or", TokenKind::OR},
    {"private", TokenKind::PRIVATE},
    {"public", TokenKind::PUBLIC},
    {"return", TokenKind::RETURN},
    {"struct", TokenKind::STRUCT},
    {"true", TokenKind::TRUE},
    {"while", TokenKind::WHILE},

    {"int", TokenKind::INT},
    {"int8", TokenKind::INT8},
    {"int16", TokenKind::INT16},
    {"int32", TokenKind::INT32},
    {"int64", TokenKind::INT64},
    {"float", TokenKind::FLOAT},
    {"uint", TokenKind::UINT},
    {"uint8", TokenKind::UINT8},
    {"uint16", TokenKind::UINT16},
    {"uint32", TokenKind::UINT32},
    {"uint64", TokenKind::UINT64},
    {"bool", TokenKind::BOOL},
    {"string", TokenKind::STRING_TYPE},
    {"expected", TokenKind::EXPECTED},
    {"void", TokenKind::VOID},

    {"self", TokenKind::SELF},
    {"nullptr", TokenKind::NULLPTR},
    {"unexpected", TokenKind::UNEXPECTED},
};

inline constexpr std::string_view to_string(TokenKind kind) {
  switch (kind) {
  case TokenKind::AMPERSAND:
    return "AMPERSAND";
  case TokenKind::AT:
    return "AT";
  case TokenKind::CARET:
    return "CARET";
  case TokenKind::LEFT_PAREN:
    return "LEFT_PAREN";
  case TokenKind::RIGHT_PAREN:
    return "RIGHT_PAREN";
  case TokenKind::LEFT_BRACE:
    return "LEFT_BRACE";
  case TokenKind::RIGHT_BRACE:
    return "RIGHT_BRACE";
  case TokenKind::LEFT_BRACKET:
    return "LEFT_BRACKET";
  case TokenKind::RIGHT_BRACKET:
    return "RIGHT_BRACKET";
  case TokenKind::COLON:
    return "COLON";
  case TokenKind::COMMA:
    return "COMMA";
  case TokenKind::DOT:
    return "DOT";
  case TokenKind::MINUS:
    return "MINUS";
  case TokenKind::PERCENT:
    return "PERCENT";
  case TokenKind::PIPE:
    return "PIPE";
  case TokenKind::PLUS:
    return "PLUS";
  case TokenKind::SEMICOLON:
    return "SEMICOLON";
  case TokenKind::SLASH:
    return "SLASH";
  case TokenKind::STAR:
    return "STAR";
  case TokenKind::TILDE:
    return "TILDE";

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
  case TokenKind::MINUS_MINUS:
    return "MINUS_MINUS";
  case TokenKind::MINUS_EQUAL:
    return "MINUS_EQUAL";
  case TokenKind::PLUS_PLUS:
    return "PLUS_PLUS";
  case TokenKind::PLUS_EQUAL:
    return "PLUS_EQUAL";
  case TokenKind::SCOPE:
    return "SCOPE";
  case TokenKind::SHIFT_LEFT:
    return "SHIFT_LEFT";
  case TokenKind::SHIFT_RIGHT:
    return "SHIFT_RIGHT";

  case TokenKind::HASH_IF:
    return "HASH_IF";
  case TokenKind::HASH_ELIF:
    return "HASH_ELIF";
  case TokenKind::HASH_ELSE:
    return "HASH_ELSE";
  case TokenKind::HASH_ENDIF:
    return "HASH_ENDIF";

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
  case TokenKind::BREAK:
    return "BREAK";
  case TokenKind::CLASS:
    return "CLASS";
  case TokenKind::CONTINUE:
    return "CONTINUE";
  case TokenKind::ELSE:
    return "ELSE";
  case TokenKind::FALSE:
    return "FALSE";
  case TokenKind::FOR:
    return "FOR";
  case TokenKind::IF:
    return "IF";
  case TokenKind::INCLUDE:
    return "INCLUDE";
  case TokenKind::MUT:
    return "MUT";
  case TokenKind::NAMESPACE:
    return "NAMESPACE";
  case TokenKind::OR:
    return "OR";
  case TokenKind::PRIVATE:
    return "PRIVATE";
  case TokenKind::PUBLIC:
    return "PUBLIC";
  case TokenKind::RETURN:
    return "RETURN";
  case TokenKind::STRUCT:
    return "STRUCT";
  case TokenKind::TRUE:
    return "TRUE";
  case TokenKind::WHILE:
    return "WHILE";

  case TokenKind::INT:
    return "INT";
  case TokenKind::INT8:
    return "INT8";
  case TokenKind::INT16:
    return "INT16";
  case TokenKind::INT32:
    return "INT32";
  case TokenKind::INT64:
    return "INT64";
  case TokenKind::UINT:
    return "UINT";
  case TokenKind::UINT8:
    return "UINT8";
  case TokenKind::UINT16:
    return "UINT16";
  case TokenKind::UINT32:
    return "UINT32";
  case TokenKind::UINT64:
    return "UINT64";
  case TokenKind::FLOAT:
    return "FLOAT";
  case TokenKind::BOOL:
    return "BOOL";
  case TokenKind::STRING_TYPE:
    return "STRING_TYPE";
  case TokenKind::EXPECTED:
    return "EXPECTED";
  case TokenKind::VOID:
    return "VOID";

  case TokenKind::SELF:
    return "SELF";
  case TokenKind::NULLPTR:
    return "NULLPTR";
  case TokenKind::UNEXPECTED:
    return "UNEXPECTED";

  case TokenKind::END_OF_FILE:
    return "END_OF_FILE";
  }

  return "UNKNOWN";
}

inline std::ostream &operator<<(std::ostream &os, TokenKind kind) {
  return os << to_string(kind);
}

} // namespace lang
