#pragma once

#include "gti/binary_float.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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
  HASH_DEFINE,
  HASH_UNDEF,
  HASH_IFDEF,
  HASH_IFNDEF,

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
  CPP_RESERVED,
  DEFAULT,
  DELETE,
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
  UNION,
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
  DOUBLE,
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
  return kind >= TokenKind::HASH_IF && kind <= TokenKind::HASH_IFNDEF;
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
  // The loader resolves each flag reference in source order before parsing.
  // Target comparisons and boolean composition remain semantic authority.
  std::optional<bool> configurationFlagDefined;
  bool configurationFlagReference = false;
};

extern const std::unordered_map<std::string_view, TokenKind> keywords;

// GTI emits C++ directly, so a C++ core keyword cannot safely name a GTI
// declaration. Spellings that are already GTI keywords live in `keywords`;
// this list owns the remaining C++20/C++23 core keywords.
inline constexpr std::string_view cppReservedIdentifiers[]{
    "alignas",     "and_eq",     "asm",
    "bitand",      "bitor",      "catch",
    "char8_t",     "char16_t",   "char32_t",
    "compl",       "const_cast", "consteval",
    "constinit",   "co_await",   "co_return",
    "co_yield",    "decltype",   "dynamic_cast",
    "explicit",    "export",     "friend",
    "goto",        "inline",     "long",
    "mutable",     "new",        "noexcept",
    "not",         "not_eq",     "or_eq",
    "protected",   "register",   "reinterpret_cast",
    "short",       "signed",     "static_assert",
    "static_cast", "template",   "thread_local",
    "throw",       "try",        "typedef",
    "typeid",      "typename",   "unsigned",
    "volatile",    "wchar_t",    "xor",
    "xor_eq",
};

inline constexpr bool isCppReservedIdentifier(std::string_view spelling) {
  for (const std::string_view reserved : cppReservedIdentifiers) {
    if (reserved == spelling) {
      return true;
    }
  }
  return false;
}

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
  case TokenKind::HASH_DEFINE:
    return "HASH_DEFINE";
  case TokenKind::HASH_UNDEF:
    return "HASH_UNDEF";
  case TokenKind::HASH_IFDEF:
    return "HASH_IFDEF";
  case TokenKind::HASH_IFNDEF:
    return "HASH_IFNDEF";

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
  case TokenKind::CPP_RESERVED:
    return "CPP_RESERVED";
  case TokenKind::DEFAULT:
    return "DEFAULT";
  case TokenKind::DELETE:
    return "DELETE";
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
  case TokenKind::UNION:
    return "UNION";
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
  case TokenKind::DOUBLE:
    return "DOUBLE";
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
