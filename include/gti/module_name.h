#pragma once

#include "gti/diagnostic.h"
#include "gti/token.h"
#include <span>
#include <string_view>

namespace lang {

[[nodiscard]] inline bool isModuleNameComponent(std::string_view name) {
  const auto letter = [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           value == '_';
  };
  if (name.empty() || !letter(name.front()) || name.starts_with("__gti_") ||
      keywords.contains(name) || isCppReservedIdentifier(name))
    return false;
  for (char value : name)
    if (!letter(value) && !(value >= '0' && value <= '9'))
      return false;
  return true;
}

[[nodiscard]] inline bool isModuleName(std::string_view name) {
  if (name.empty())
    return false;
  while (true) {
    const auto separator = name.find('.');
    if (!isModuleNameComponent(name.substr(0, separator)))
      return false;
    if (separator == std::string_view::npos)
      return true;
    name.remove_prefix(separator + 1);
  }
}

struct ModuleHeader {
  Token declaration;
  SourceSpan span;
  bool interface = false;
  std::size_t tokenEnd = 0;
};

// Shared by source loading and project discovery; neither invents a second
// interpretation of a module's spelling or leading declaration.
[[nodiscard]] inline std::optional<ModuleHeader>
parseModuleHeader(std::span<const Token> tokens) {
  if (tokens.size() < 3)
    return std::nullopt;
  const bool interface = tokens[0].kind == TokenKind::EXPORT;
  if ((!interface && tokens[0].kind != TokenKind::MODULE) ||
      (interface && tokens[1].kind != TokenKind::MODULE))
    return std::nullopt;
  const std::size_t first = interface ? 2 : 1;
  std::size_t at = first;
  std::string name;
  while (at < tokens.size() && tokens[at].kind == TokenKind::IDENTIFIER) {
    name += tokens[at++].lexeme;
    if (at >= tokens.size() || tokens[at].kind != TokenKind::DOT)
      break;
    name += '.';
    ++at;
  }
  if (!isModuleName(name) || at >= tokens.size() ||
      tokens[at].kind != TokenKind::SEMICOLON)
    return std::nullopt;
  Token declaration = tokens[first];
  declaration.lexeme = std::move(name);
  return ModuleHeader{std::move(declaration),
                      {tokens[first].source, tokens[first].position,
                       tokens[at - 1].position + tokens[at - 1].lexeme.size(),
                       tokens[first].line},
                      interface,
                      at + 1};
}

} // namespace lang
