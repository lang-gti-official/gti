#pragma once

#include "gti/binary_float.h"

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
  QUESTION,
  SEMICOLON,
  SLASH,
  STAR,
  TILDE,

  // One or two character tokens.
  ARROW,
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
  PERCENT_EQUAL,
  PLUS_PLUS,
  PLUS_EQUAL,
  SLASH_EQUAL,
  STAR_EQUAL,
  AMPERSAND_EQUAL,
  CARET_EQUAL,
  PIPE_EQUAL,
  SCOPE,

  // Three-character tokens.
  ELLIPSIS,
  SHIFT_LEFT_EQUAL,
  SHIFT_RIGHT_EQUAL,

  // Parser-combined operators. Angle tokens remain separate for generics.
  SHIFT_LEFT,
  SHIFT_RIGHT,

  // Compile-time directives.
  HASH_IF,
  HASH_ELIF,
  HASH_ELSE,
  HASH_ENDIF,
  HASH_ERROR,
  HASH_INCLUDE,

  // Literals.
  IDENTIFIER,
  STRING_LITERAL,
  CHARACTER_LITERAL,
  INT_LITERAL,
  FLOAT_LITERAL,

  // Keywords. AND and OR also represent their symbolic aliases.
  AND,
  ALIGNOF,
  BREAK,
  CASE,
  CLASS,
  CONCEPT,
  CONST,
  CONSTEXPR,
  CONTINUE,
  DEFAULT,
  DO,
  ELSE,
  ENUM,
  EXTERN,
  FALSE,
  FOR,
  IF,
  INTERFACE,
  MUT,
  NAMESPACE,
  OPERATOR,
  OR,
  OVERRIDE,
  PRIVATE,
  PUBLIC,
  REQUIRES,
  RETURN,
  STATIC,
  STRUCT,
  SIZEOF,
  SWITCH,
  TRUE,
  UNSAFE,
  USING,
  VIRTUAL,
  WHILE,

  // Type keywords.
  AUTO,
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
  CHAR,
  EXPECTED,
  NULLPTR_TYPE,
  VOID,

  // Special keywords.
  THIS,
  NULLPTR,
  UNEXPECTED,

  END_OF_FILE,
};

inline constexpr bool isOperatorToken(TokenKind kind) {
  using enum TokenKind;
  switch (kind) {
  case ALIGNOF:
  case AMPERSAND:
  case ARROW:
  case AT:
  case CARET:
  case LEFT_BRACKET:
  case RIGHT_BRACKET:
  case AND:
  case OR:
  case MINUS:
  case PERCENT:
  case PIPE:
  case PLUS:
  case QUESTION:
  case SLASH:
  case STAR:
  case TILDE:
  case BANG:
  case BANG_EQUAL:
  case EQUAL:
  case EQUAL_EQUAL:
  case GREATER:
  case GREATER_EQUAL:
  case LESS:
  case LESS_EQUAL:
  case MINUS_MINUS:
  case MINUS_EQUAL:
  case PERCENT_EQUAL:
  case PLUS_PLUS:
  case PLUS_EQUAL:
  case SLASH_EQUAL:
  case STAR_EQUAL:
  case AMPERSAND_EQUAL:
  case CARET_EQUAL:
  case PIPE_EQUAL:
  case ELLIPSIS:
  case SHIFT_LEFT:
  case SHIFT_LEFT_EQUAL:
  case SHIFT_RIGHT:
  case SHIFT_RIGHT_EQUAL:
  case SIZEOF:
  case COLON:
    return true;
  default:
    return false;
  }
}

inline constexpr bool isDirectiveToken(TokenKind kind) {
  return kind >= TokenKind::HASH_IF && kind <= TokenKind::HASH_INCLUDE;
}

inline constexpr bool isKeywordToken(TokenKind kind) {
  const bool ordinaryKeyword =
      kind >= TokenKind::AND && kind <= TokenKind::WHILE &&
      kind != TokenKind::AND && kind != TokenKind::ALIGNOF &&
      kind != TokenKind::OR && kind != TokenKind::SIZEOF;
  const bool specialKeyword =
      kind >= TokenKind::THIS && kind <= TokenKind::UNEXPECTED;
  return ordinaryKeyword || specialKeyword;
}

inline constexpr bool isTypeKeywordToken(TokenKind kind) {
  return kind >= TokenKind::AUTO && kind <= TokenKind::VOID;
}

struct CharacterLiteral {
  std::uint8_t value = 0;

  friend bool operator==(CharacterLiteral, CharacterLiteral) = default;
};

using Literal = std::variant<std::monostate, std::nullptr_t, std::uint64_t,
                             BinaryFloat, CharacterLiteral, std::string, bool>;

struct Token {
  Token() = default;

  Token(TokenKind kind, std::string lexeme, Literal literal,
        std::size_t position, int line, std::string source = {},
        bool completion = false, bool generated = false)
      : kind(kind), lexeme(std::move(lexeme)), literal(std::move(literal)),
        position(position), line(line), source(std::move(source)),
        completion(completion), generated(generated) {}

  TokenKind kind{};
  std::string lexeme;
  Literal literal{};
  std::size_t position{};
  int line{};
  std::string source;
  bool completion = false;
  bool generated = false;
};

inline const std::unordered_map<std::string_view, TokenKind> keywords{
    {"and", TokenKind::AND},
    {"alignof", TokenKind::ALIGNOF},
    {"break", TokenKind::BREAK},
    {"case", TokenKind::CASE},
    {"class", TokenKind::CLASS},
    {"concept", TokenKind::CONCEPT},
    {"const", TokenKind::CONST},
    {"constexpr", TokenKind::CONSTEXPR},
    {"continue", TokenKind::CONTINUE},
    {"default", TokenKind::DEFAULT},
    {"do", TokenKind::DO},
    {"else", TokenKind::ELSE},
    {"enum", TokenKind::ENUM},
    {"extern", TokenKind::EXTERN},
    {"false", TokenKind::FALSE},
    {"for", TokenKind::FOR},
    {"if", TokenKind::IF},
    {"interface", TokenKind::INTERFACE},
    {"mut", TokenKind::MUT},
    {"namespace", TokenKind::NAMESPACE},
    {"operator", TokenKind::OPERATOR},
    {"or", TokenKind::OR},
    {"override", TokenKind::OVERRIDE},
    {"private", TokenKind::PRIVATE},
    {"public", TokenKind::PUBLIC},
    {"requires", TokenKind::REQUIRES},
    {"return", TokenKind::RETURN},
    {"static", TokenKind::STATIC},
    {"struct", TokenKind::STRUCT},
    {"sizeof", TokenKind::SIZEOF},
    {"switch", TokenKind::SWITCH},
    {"true", TokenKind::TRUE},
    {"unsafe", TokenKind::UNSAFE},
    {"using", TokenKind::USING},
    {"virtual", TokenKind::VIRTUAL},
    {"while", TokenKind::WHILE},

    {"auto", TokenKind::AUTO},
    {"int", TokenKind::INT},
    {"int8_t", TokenKind::INT8},
    {"int16_t", TokenKind::INT16},
    {"int32_t", TokenKind::INT32},
    {"int64_t", TokenKind::INT64},
    {"int8", TokenKind::INT8},
    {"int16", TokenKind::INT16},
    {"int32", TokenKind::INT32},
    {"int64", TokenKind::INT64},
    {"float", TokenKind::FLOAT},
    {"uint", TokenKind::UINT},
    {"uint8_t", TokenKind::UINT8},
    {"uint16_t", TokenKind::UINT16},
    {"uint32_t", TokenKind::UINT32},
    {"uint64_t", TokenKind::UINT64},
    {"uint8", TokenKind::UINT8},
    {"uint16", TokenKind::UINT16},
    {"uint32", TokenKind::UINT32},
    {"uint64", TokenKind::UINT64},
    {"bool", TokenKind::BOOL},
    {"char", TokenKind::CHAR},
    {"expected", TokenKind::EXPECTED},
    {"nullptr_t", TokenKind::NULLPTR_TYPE},
    {"void", TokenKind::VOID},

    {"this", TokenKind::THIS},
    {"nullptr", TokenKind::NULLPTR},
    {"unexpected", TokenKind::UNEXPECTED},
};

inline constexpr std::string_view to_string(TokenKind kind) {
  switch (kind) {
  case TokenKind::AMPERSAND:
    return "AMPERSAND";
  case TokenKind::ARROW:
    return "ARROW";
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
  case TokenKind::QUESTION:
    return "QUESTION";
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
  case TokenKind::PERCENT_EQUAL:
    return "PERCENT_EQUAL";
  case TokenKind::PLUS_PLUS:
    return "PLUS_PLUS";
  case TokenKind::PLUS_EQUAL:
    return "PLUS_EQUAL";
  case TokenKind::SLASH_EQUAL:
    return "SLASH_EQUAL";
  case TokenKind::STAR_EQUAL:
    return "STAR_EQUAL";
  case TokenKind::AMPERSAND_EQUAL:
    return "AMPERSAND_EQUAL";
  case TokenKind::CARET_EQUAL:
    return "CARET_EQUAL";
  case TokenKind::PIPE_EQUAL:
    return "PIPE_EQUAL";
  case TokenKind::SCOPE:
    return "SCOPE";
  case TokenKind::ELLIPSIS:
    return "ELLIPSIS";
  case TokenKind::SHIFT_LEFT_EQUAL:
    return "SHIFT_LEFT_EQUAL";
  case TokenKind::SHIFT_RIGHT_EQUAL:
    return "SHIFT_RIGHT_EQUAL";
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
  case TokenKind::HASH_ERROR:
    return "HASH_ERROR";
  case TokenKind::HASH_INCLUDE:
    return "HASH_INCLUDE";

  case TokenKind::IDENTIFIER:
    return "IDENTIFIER";
  case TokenKind::STRING_LITERAL:
    return "STRING_LITERAL";
  case TokenKind::CHARACTER_LITERAL:
    return "CHARACTER_LITERAL";
  case TokenKind::INT_LITERAL:
    return "INT_LITERAL";
  case TokenKind::FLOAT_LITERAL:
    return "FLOAT_LITERAL";

  case TokenKind::AND:
    return "AND";
  case TokenKind::ALIGNOF:
    return "ALIGNOF";
  case TokenKind::BREAK:
    return "BREAK";
  case TokenKind::CASE:
    return "CASE";
  case TokenKind::CLASS:
    return "CLASS";
  case TokenKind::CONCEPT:
    return "CONCEPT";
  case TokenKind::CONST:
    return "CONST";
  case TokenKind::CONSTEXPR:
    return "CONSTEXPR";
  case TokenKind::CONTINUE:
    return "CONTINUE";
  case TokenKind::DEFAULT:
    return "DEFAULT";
  case TokenKind::DO:
    return "DO";
  case TokenKind::ELSE:
    return "ELSE";
  case TokenKind::ENUM:
    return "ENUM";
  case TokenKind::EXTERN:
    return "EXTERN";
  case TokenKind::FALSE:
    return "FALSE";
  case TokenKind::FOR:
    return "FOR";
  case TokenKind::IF:
    return "IF";
  case TokenKind::INTERFACE:
    return "INTERFACE";
  case TokenKind::MUT:
    return "MUT";
  case TokenKind::NAMESPACE:
    return "NAMESPACE";
  case TokenKind::OPERATOR:
    return "OPERATOR";
  case TokenKind::OR:
    return "OR";
  case TokenKind::OVERRIDE:
    return "OVERRIDE";
  case TokenKind::PRIVATE:
    return "PRIVATE";
  case TokenKind::PUBLIC:
    return "PUBLIC";
  case TokenKind::REQUIRES:
    return "REQUIRES";
  case TokenKind::RETURN:
    return "RETURN";
  case TokenKind::STATIC:
    return "STATIC";
  case TokenKind::STRUCT:
    return "STRUCT";
  case TokenKind::SIZEOF:
    return "SIZEOF";
  case TokenKind::SWITCH:
    return "SWITCH";
  case TokenKind::TRUE:
    return "TRUE";
  case TokenKind::UNSAFE:
    return "UNSAFE";
  case TokenKind::USING:
    return "USING";
  case TokenKind::VIRTUAL:
    return "VIRTUAL";
  case TokenKind::WHILE:
    return "WHILE";

  case TokenKind::AUTO:
    return "AUTO";
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
  case TokenKind::CHAR:
    return "CHAR";
  case TokenKind::EXPECTED:
    return "EXPECTED";
  case TokenKind::NULLPTR_TYPE:
    return "NULLPTR_TYPE";
  case TokenKind::VOID:
    return "VOID";

  case TokenKind::THIS:
    return "THIS";
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
