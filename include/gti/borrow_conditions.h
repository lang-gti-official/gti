#pragma once

#include "gti/lifetime_types.h"
#include "gti/semantic_model.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lang {

inline constexpr std::size_t borrowInventoryWorkLimit = 4096;
inline constexpr std::size_t borrowInventoryDepthLimit = 256;

// One budget belongs to the entire query, never each array or recursive arm.
// Exhaustion is unknown evidence and must not become a truncated inventory.
struct BorrowInventoryBudget {
  std::size_t remaining = borrowInventoryWorkLimit;
  [[nodiscard]] bool consume(std::size_t amount = 1) {
    if (amount > remaining)
      return false;
    remaining -= amount;
    return true;
  }
};

// Prove only the additional inline-storage case hidden by a Base path: two
// validated sibling base/field subobjects of the same exact root. The caller
// supplies an authoritative inline projector that rejects unions, interfaces,
// enum payloads, indirect storage and mismatched owner/ordinal/type identities.
// A reference root denotes its referent, but no later reference crossing can
// turn different physical slots into evidence about their external referents.
// False is unknown/overlap, never evidence that a malformed path is valid.
template <typename InlineProjector>
[[nodiscard]] bool inlineBorrowPathsDisjoint(
    SemanticType root, std::span<const PlaceProjection> left,
    std::span<const PlaceProjection> right, InlineProjector &&project) {
  if (left.size() > borrowInventoryDepthLimit ||
      right.size() > borrowInventoryDepthLimit ||
      !isWellFormedBorrowPath(left) || !isWellFormedBorrowPath(right))
    return false;
  if (root.kind == SemanticType::Reference) {
    if (root.arguments.size() != 1)
      return false;
    root = SemanticType(root.arguments.front());
  }
  SemanticType leftType = root;
  SemanticType rightType = root;
  bool diverged = false;
  const auto inlineParent = [](const SemanticType &type) {
    return type.kind == SemanticType::Class ||
           type.kind == SemanticType::Array ||
           (type.kind == SemanticType::TypePack && type.concretePack);
  };
  for (std::size_t index = 0; index < std::max(left.size(), right.size()); ++index) {
    const bool hasLeft = index < left.size();
    const bool hasRight = index < right.size();
    if (!diverged && (!hasLeft || !hasRight))
      return false; // One path contains the other.
    if (!diverged && left[index] != right[index]) {
      const auto &lhs = left[index];
      const auto &rhs = right[index];
      const auto sibling = [](const PlaceProjection &step) {
        return step.kind == PlaceProjectionKind::Base ||
               step.kind == PlaceProjectionKind::Field;
      };
      if (leftType != rightType || leftType.kind != SemanticType::Class ||
          !sibling(lhs) || !sibling(rhs) ||
          (lhs.kind != PlaceProjectionKind::Base &&
           rhs.kind != PlaceProjectionKind::Base))
        return false;
      diverged = true;
    }
    const auto advance = [&](SemanticType &type, const PlaceProjection &step) {
      if (!inlineParent(type))
        return false;
      const auto child = project(type, step);
      if (!child || child->kind == SemanticType::Unknown ||
          child->kind == SemanticType::Reference)
        return false;
      type = *child;
      return true;
    };
    if ((hasLeft && !advance(leftType, left[index])) ||
        (hasRight && !advance(rightType, right[index])))
      return false;
  }
  return diverged;
}

// The inventory is an authoritative enumeration of typed enum subobjects,
// including alternatives with no borrowed payload. A selected subset may
// narrow each domain, but cannot omit an independently stored sibling or an
// active nested domain. Missing evidence never proves an alternative empty.
[[nodiscard]] inline bool hasCompleteBorrowAlternativeCoverage(
    std::span<const BorrowAlternativePath> inventory,
    std::span<const BorrowAlternativePath> selected) {
  if (inventory.empty())
    return selected.empty();
  if (selected.empty())
    return false;
  const auto contains = [](const auto &all, const auto &required) {
    return std::all_of(required.begin(), required.end(), [&](const auto &condition) {
      return std::find(all.begin(), all.end(), condition) != all.end();
    });
  };
  for (const auto &path : selected)
    if (path.conditions.empty() ||
        std::find(inventory.begin(), inventory.end(), path) == inventory.end())
      return false;
  for (const auto &path : inventory) {
    if (path.conditions.empty())
      return false;
    for (std::size_t depth = 0; depth < path.conditions.size(); ++depth) {
      const auto ancestors = std::span(path.conditions).first(depth);
      const bool active = ancestors.empty() ||
          std::any_of(selected.begin(), selected.end(), [&](const auto &alternative) {
            return contains(alternative.conditions, ancestors);
          });
      if (!active)
        continue;
      const auto &domain = path.conditions[depth];
      if (!std::any_of(selected.begin(), selected.end(), [&](const auto &alternative) {
            return contains(alternative.conditions, ancestors) &&
                std::any_of(alternative.conditions.begin(), alternative.conditions.end(),
                            [&](const auto &condition) {
                  return condition.subobject == domain.subobject &&
                         condition.enumeration == domain.enumeration;
                });
          }))
        return false;
    }
  }
  return true;
}

// Transient structural transport between complete typed alternative inventories.
// Each caller must first prove the source-to-target conversion. This helper
// establishes no origin, permission, region relationship or feasible variant.
class BorrowConditionTransport final {
public:
  [[nodiscard]] static std::optional<BorrowConditionTransport>
  between(std::span<const BorrowAlternativePath> source,
          std::span<const BorrowAlternativePath> target) {
    if (source.size() != target.size())
      return std::nullopt;
    const auto samePath = [](const BorrowAlternativePath &left,
                             const BorrowAlternativePath &right) {
      if (left.conditions.empty() || left.conditions.size() != right.conditions.size())
        return false;
      for (std::size_t index = 0; index < left.conditions.size(); ++index) {
        const auto &from = left.conditions[index];
        const auto &to = right.conditions[index];
        if (!isWellFormedBorrowPath(from.subobject) || !isWellFormedBorrowPath(to.subobject) ||
            from.enumeration.kind != SemanticType::Enum ||
            to.enumeration.kind != SemanticType::Enum ||
            from.subobject != to.subobject || from.variant != to.variant ||
            !sameLifetimeConversionShape(from.enumeration, to.enumeration))
          return false;
      }
      return true;
    };
    BorrowConditionTransport result;
    std::vector<bool> used(target.size());
    for (const auto &path : source) {
      std::optional<std::size_t> selected;
      for (std::size_t index = 0; index < target.size(); ++index)
        if (samePath(path, target[index])) {
          if (selected)
            return std::nullopt;
          selected = index;
        }
      if (!selected || used[*selected])
        return std::nullopt;
      used[*selected] = true;
      for (std::size_t index = 0; index < path.conditions.size(); ++index) {
        const auto &from = path.conditions[index];
        const auto &to = target[*selected].conditions[index];
        const auto existing = std::find_if(result.conditions_.begin(), result.conditions_.end(),
            [&](const auto &pair) { return pair.first == from || pair.second == to; });
        if (existing != result.conditions_.end()) {
          if (existing->first != from || existing->second != to)
            return std::nullopt;
        } else {
          result.conditions_.emplace_back(from, to);
        }
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<std::vector<BorrowDependencyCondition>>
  apply(std::span<const BorrowDependencyCondition> source) const {
    std::vector<BorrowDependencyCondition> result;
    for (const auto &condition : source) {
      const auto found = std::find_if(conditions_.begin(), conditions_.end(),
          [&](const auto &pair) { return pair.first == condition; });
      if (found == conditions_.end())
        return std::nullopt;
      result.push_back(found->second);
    }
    return result;
  }

private:
  BorrowConditionTransport() = default;
  std::vector<std::pair<BorrowDependencyCondition, BorrowDependencyCondition>> conditions_;
};

// Preserve the exact active subset and every dependency's origin/access/path.
// The two phase-owned complete inventories must come from the original and
// converted types, never from this producer's possibly restricted alternatives.
template <typename DependencySet>
[[nodiscard]] std::optional<DependencySet>
transportBorrowDependencyConditions(const DependencySet &source,
                                    const BorrowConditionTransport &transport) {
  auto result = source;
  for (auto &dependency : result.dependencies) {
    auto conditions = transport.apply(dependency.conditions);
    if (!conditions)
      return std::nullopt;
    dependency.conditions = std::move(*conditions);
  }
  for (auto &alternative : result.possibleAlternatives) {
    auto conditions = transport.apply(alternative.conditions);
    if (!conditions)
      return std::nullopt;
    alternative.conditions = std::move(*conditions);
  }
  return result;
}

// A caller validates the exact canonical signature/application first. Bound
// callable regions remain scoped through capture-avoiding type substitution.
[[nodiscard]] inline std::optional<FunctionBorrowDependencySummary>
instantiateBorrowSummaryLifetimes(const FunctionBorrowDependencySummary &source,
                                 std::span<const LifetimeBinding> bindings) {
  auto result = source;
  const auto substitute = [&](auto &conditions) {
    for (auto &condition : conditions) {
      auto actual = substituteTypeLifetimes(condition.enumeration, bindings);
      if (!actual)
        return false;
      condition.enumeration = std::move(*actual);
    }
    return true;
  };
  for (auto &dependency : result.dependencies)
    if (!substitute(dependency.sourceConditions) || !substitute(dependency.resultConditions))
      return std::nullopt;
  for (auto &alternative : result.possibleAlternatives)
    if (!substitute(alternative.conditions))
      return std::nullopt;
  return result;
}

} // namespace lang
