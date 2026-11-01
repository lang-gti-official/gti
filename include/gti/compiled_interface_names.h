#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace lang {

// The same bounded name grammar is used by manifest selection and metadata
// projection. Namespace qualification is distinct from nested class ownership.
[[nodiscard]] inline bool isCompiledInterfaceName(std::string_view name) {
  if (name.empty() || name.size() > 1024)
    return false;
  const auto letter = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  };
  for (unsigned depth = 0; depth < 64; ++depth) {
    const auto separator = name.find("::");
    const auto component = name.substr(0, separator);
    if (component.empty() || !letter(component.front()))
      return false;
    for (char c : component)
      if (!letter(c) && !(c >= '0' && c <= '9'))
        return false;
    if (separator == name.npos)
      return true;
    name.remove_prefix(separator + 2);
  }
  return false;
}

[[nodiscard]] inline std::string_view
compiledInterfaceBaseName(std::string_view name) {
  const auto separator = name.rfind("::");
  return separator == name.npos ? name : name.substr(separator + 2);
}

[[nodiscard]] inline std::string
compiledInterfaceAliasName(std::string_view name, std::string_view prefix) {
  if (prefix.empty())
    return std::string(name);
  if (!isCompiledInterfaceName(prefix) || prefix.find(':') != prefix.npos)
    throw std::invalid_argument("invalid compiled interface alias prefix");
  constexpr char hex[] = "0123456789abcdef";
  std::string result(prefix);
  for (unsigned char byte : name) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

} // namespace lang
