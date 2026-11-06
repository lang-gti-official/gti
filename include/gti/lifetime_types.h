#pragma once

#include "gti/semantic_core.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lang {

using LifetimeBinding = std::pair<Lifetime, Lifetime>;

namespace lifetime_detail {
template <typename Type, typename Visitor>
void children(Type &type, Visitor &&visit) {
  for (auto &argument : type.arguments)
    visit(argument);
  for (auto &argument : type.lambdaEnclosingClassTypes)
    visit(argument);
  for (auto &argument : type.lambdaEnclosingFunctionTypes)
    visit(argument);
}

inline bool sameRegion(Lifetime left, Lifetime right,
                       std::span<const LifetimeBinding> bindings) {
  for (auto binding = bindings.rbegin(); binding != bindings.rend(); ++binding) {
    if (binding->first == left)
      return binding->second == right;
    if (binding->second == right)
      return false;
  }
  return left == right;
}

inline bool equivalent(const SemanticType &left, const SemanticType &right,
                       std::vector<LifetimeBinding> bindings) {
  if (left.quantifiedLifetimes.size() != right.quantifiedLifetimes.size())
    return false;
  for (std::size_t index = 0; index < left.quantifiedLifetimes.size(); ++index) {
    const Lifetime source = Lifetime::parameter(left.quantifiedLifetimes[index]);
    const Lifetime target = Lifetime::parameter(right.quantifiedLifetimes[index]);
    if (!source.specified() || !target.specified() ||
        std::any_of(bindings.begin(), bindings.end(), [&](auto binding) {
          return binding.first == source || binding.second == target;
        }))
      return false;
    bindings.emplace_back(source, target);
  }
  if (!sameRegion(left.referenceLifetime, right.referenceLifetime, bindings) ||
      left.lifetimeArguments.size() != right.lifetimeArguments.size())
    return false;
  for (std::size_t index = 0; index < left.lifetimeArguments.size(); ++index)
    if (!sameRegion(left.lifetimeArguments[index], right.lifetimeArguments[index],
                    bindings))
      return false;

  // Compare every non-region fact as well. Do not accidentally equate distinct
  // access permissions, nominal declarations, value arguments or dispatch.
  SemanticType source = left;
  SemanticType target = right;
  const auto clearChildrenAndRegions = [](SemanticType &type) {
    type.arguments.clear();
    type.lambdaEnclosingClassTypes.clear();
    type.lambdaEnclosingFunctionTypes.clear();
    type.referenceLifetime = {};
    type.lifetimeArguments.clear();
    type.quantifiedLifetimes.clear();
  };
  clearChildrenAndRegions(source);
  clearChildrenAndRegions(target);
  if (source != target)
    return false;
  const auto equivalentChildren = [&](const auto &a, const auto &b) {
    if (a.size() != b.size())
      return false;
    for (std::size_t index = 0; index < a.size(); ++index)
      if (!equivalent(a[index], b[index], bindings))
        return false;
    return true;
  };
  return equivalentChildren(left.arguments, right.arguments) &&
         equivalentChildren(left.lambdaEnclosingClassTypes,
                            right.lambdaEnclosingClassTypes) &&
         equivalentChildren(left.lambdaEnclosingFunctionTypes,
                            right.lambdaEnclosingFunctionTypes);
}

inline bool substitute(SemanticType &type,
                       std::span<const LifetimeBinding> bindings,
                       std::vector<Lifetime> quantified) {
  for (const auto id : type.quantifiedLifetimes) {
    const Lifetime region = Lifetime::parameter(id);
    if (!region.specified() ||
        std::find(quantified.begin(), quantified.end(), region) != quantified.end())
      return false;
    quantified.push_back(region);
  }
  const auto replace = [&](Lifetime &region) {
    if (std::find(quantified.begin(), quantified.end(), region) != quantified.end())
      return true;
    for (const auto &[source, target] : bindings) {
      if (source != region)
        continue;
      if (!target.specified() ||
          std::find(quantified.begin(), quantified.end(), target) != quantified.end())
        return false; // A substitution must never capture a quantified region.
      region = target;
      break;
    }
    return true;
  };
  if (!replace(type.referenceLifetime))
    return false;
  for (auto &region : type.lifetimeArguments)
    if (!replace(region))
      return false;
  bool valid = true;
  children(type, [&](SemanticType &child) {
    valid = substitute(child, bindings, quantified) && valid;
  });
  return valid;
}
} // namespace lifetime_detail

// Free declaration/inference regions only; higher-ranked binders stay scoped
// inside their callable type and are never inferred from a particular call.
inline std::vector<Lifetime> freeTypeLifetimes(const SemanticType &type) {
  std::vector<Lifetime> result;
  const auto walk = [&](auto &&self, const SemanticType &current,
                        std::vector<Lifetime> bound) -> void {
    for (const auto id : current.quantifiedLifetimes)
      bound.push_back(Lifetime::parameter(id));
    const auto add = [&](Lifetime region) {
      if (region.specified() &&
          std::find(bound.begin(), bound.end(), region) == bound.end() &&
          std::find(result.begin(), result.end(), region) == result.end())
        result.push_back(region);
    };
    add(current.referenceLifetime);
    for (const auto region : current.lifetimeArguments)
      add(region);
    lifetime_detail::children(current, [&](const SemanticType &child) {
      self(self, child, bound);
    });
  };
  walk(walk, type, {});
  return result;
}

inline std::optional<SemanticType>
substituteTypeLifetimes(SemanticType type,
                       std::span<const LifetimeBinding> bindings) {
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    if (!bindings[index].first.specified() || !bindings[index].second.specified())
      return std::nullopt;
    for (std::size_t earlier = 0; earlier < index; ++earlier)
      if (bindings[earlier].first == bindings[index].first &&
          bindings[earlier].second != bindings[index].second)
        return std::nullopt;
  }
  if (!lifetime_detail::substitute(type, bindings, {}))
    return std::nullopt;
  return type;
}

inline bool equivalentLifetimeTypes(
    const SemanticType &left, const SemanticType &right,
    std::span<const LifetimeBinding> declarationBinders = {}) {
  std::vector<LifetimeBinding> bindings;
  for (const auto binding : declarationBinders) {
    if (binding.first.kind != Lifetime::Kind::Parameter ||
        binding.second.kind != Lifetime::Kind::Parameter ||
        !binding.first.specified() || !binding.second.specified() ||
        std::any_of(bindings.begin(), bindings.end(), [&](auto previous) {
          return previous.first == binding.first || previous.second == binding.second;
        }))
      return false;
    bindings.push_back(binding);
  }
  return lifetime_detail::equivalent(left, right, std::move(bindings));
}

// Representation-only operation. Semantic conversion, overload selection and
// borrow verification must use the original types and their contracts. This
// helper alone is not evidence that erasing a caller's obligations is sound.
inline SemanticType eraseTypeLifetimes(SemanticType type) {
  type.referenceLifetime = {};
  type.lifetimeArguments.clear();
  type.quantifiedLifetimes.clear();
  lifetime_detail::children(type, [](SemanticType &child) {
    child = eraseTypeLifetimes(std::move(child));
  });
  return type;
}

} // namespace lang
