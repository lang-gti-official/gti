#pragma once

#include "gti/token.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

// Configuration flags are deliberately valueless. They select source branches
// before semantic analysis; they are never substituted into the token stream.
using ConfigurationFlags = std::vector<std::string>;

inline bool isConfigurationFlagName(std::string_view name) {
  const auto alphabetic = [](char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= 'a' && character <= 'z') || character == '_';
  };
  const auto digit = [](char character) {
    return character >= '0' && character <= '9';
  };
  return !name.empty() && alphabetic(name.front()) &&
         std::all_of(name.begin() + 1, name.end(),
                     [&](char character) {
                       return alphabetic(character) || digit(character);
                     }) &&
         !keywords.contains(name) && !isCppReservedIdentifier(name);
}

inline void normalizeConfigurationFlags(ConfigurationFlags &flags) {
  std::sort(flags.begin(), flags.end());
  flags.erase(std::unique(flags.begin(), flags.end()), flags.end());
}

} // namespace lang
