#pragma once

#include "gti/token.h"
#include <string_view>

namespace lang {

[[nodiscard]] inline bool isModuleNameComponent(std::string_view name) {
  const auto letter = [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           value == '_';
  };
  if (name.empty() || !letter(name.front()) || keywords.contains(name) ||
      isCppReservedIdentifier(name))
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

} // namespace lang
