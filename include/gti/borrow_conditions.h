#pragma once

#include "gti/lifetime_types.h"
#include "gti/semantic_model.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lang {

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
        if (from.enumeration.kind != SemanticType::Enum ||
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
