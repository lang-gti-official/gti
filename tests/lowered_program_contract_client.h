#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace lang {
class LoweredProgram;
}

namespace gti_test {

inline constexpr std::size_t loweredDeclarationKindCount = 14;
inline constexpr std::size_t loweredGeneratedItemKindCount = 7;

struct LoweredProgramInventory {
  std::size_t bodies = 0;
  std::size_t declarations = 0;
  std::size_t symbols = 0;
  std::size_t classInstances = 0;
  std::size_t functionInstances = 0;
  std::size_t constructorInstances = 0;
  std::size_t destructorInstances = 0;
  std::size_t lambdaInstances = 0;
  std::size_t generatedItems = 0;
  std::array<std::size_t, loweredDeclarationKindCount> declarationKinds{};
  std::array<std::size_t, loweredGeneratedItemKindCount> generatedItemKinds{};
  std::string deterministicText;
};

[[nodiscard]] LoweredProgramInventory
inspectLoweredProgram(const lang::LoweredProgram &program);

} // namespace gti_test
