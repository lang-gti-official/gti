#pragma once

#include "gti/borrow_conditions.h"

#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

namespace lang {

// Derived from the owning phase's parameter binding records. A source binding
// remains grouped; executable argument ordinals refer to concrete leaf lanes.
// This is a structural map, never origin, access, liveness or transfer evidence.
struct ConcreteFormalRoot {
  std::size_t binding = 0;
  SemanticType type = SemanticType::Unknown;
};
struct ConcreteFormalInput {
  std::size_t argument = 0;
  std::size_t binding = 0;
  BorrowPath bindingPrefix;
  SemanticType type = SemanticType::Unknown;
  bool operator==(const ConcreteFormalInput &) const = default;
};

[[nodiscard]] inline std::optional<std::vector<ConcreteFormalInput>>
deriveConcreteFormalInputs(std::span<const ConcreteFormalRoot> roots,
                           std::span<const SemanticType> signature) {
  BorrowInventoryBudget budget;
  std::vector<SemanticType> flat;
  const auto flatten = [&](auto &&self, const SemanticType &type,
                           std::size_t depth) -> bool {
    if (depth >= borrowInventoryDepthLimit || !budget.consume() ||
        type.kind == SemanticType::Unknown)
      return false;
    // Reference<TypePack> is one physical reference parameter. Only an
    // actual concrete pack node expands; referents and nominal children do not.
    if (type.kind != SemanticType::TypePack) {
      flat.push_back(type);
      return true;
    }
    if (!type.concretePack)
      return false;
    for (const auto &element : type.arguments)
      if (!self(self, element, depth + 1))
        return false;
    return true;
  };
  for (const auto &type : signature)
    if (!flatten(flatten, type, 0))
      return std::nullopt;
  std::vector<ConcreteFormalInput> inputs;
  const auto append = [&](auto &&self, std::size_t binding,
                          const SemanticType &type, BorrowPath prefix) -> bool {
    if (prefix.size() >= borrowInventoryDepthLimit || !budget.consume() ||
        type.kind == SemanticType::Unknown)
      return false;
    if (type.kind == SemanticType::TypePack) {
      if (!type.concretePack)
        return false;
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        auto child = prefix;
        child.push_back(constantIndexProjection(index));
        if (!self(self, binding, type.arguments[index], std::move(child)))
          return false;
      }
      return true;
    }
    const auto ordinal = inputs.size();
    if (ordinal >= flat.size() || type != flat[ordinal])
      return false;
    inputs.push_back({.argument = ordinal, .binding = binding,
                      .bindingPrefix = std::move(prefix), .type = type});
    return true;
  };
  std::unordered_set<std::size_t> identities;
  for (const auto &root : roots)
    if (!root.binding || !identities.insert(root.binding).second ||
        !append(append, root.binding, root.type, {}))
      return std::nullopt;
  return inputs.size() == flat.size() ? std::optional(std::move(inputs))
                                      : std::nullopt;
}

} // namespace lang
