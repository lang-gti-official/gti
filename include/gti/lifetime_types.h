#pragma once

#include "gti/semantic_core.h"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lang {

// Shared declaration policy; each phase supplies its own checked outlives
// query and typed storage inventory.
template <class Proves>
[[nodiscard]] bool lifetimeBorrowSupplierPermitted(
    Lifetime source, AccessMode sourceAccess, Lifetime result,
    AccessMode resultAccess, Proves &&proves) {
  return source.declarationRegion() && result.declarationRegion() &&
         (resultAccess == AccessMode::ReadOnly || sourceAccess == AccessMode::Mutable) &&
         proves(source, result);
}

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
                       std::vector<LifetimeBinding> bindings,
                       bool allowFreeRegionVariation = false, std::size_t depth = 0) {
  if (allowFreeRegionVariation && (depth >= 256 ||
      left.kind == SemanticType::Unknown || right.kind == SemanticType::Unknown))
    return false;
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
  const auto matchesRegion = [&](Lifetime source, Lifetime target) {
    if (!allowFreeRegionVariation)
      return sameRegion(source, target, bindings);
    // Invocation binders must retain their exact scoped relationships. Only
    // free, fully specified regions can vary at this structural boundary.
    for (auto binding = bindings.rbegin(); binding != bindings.rend(); ++binding)
      if (binding->first == source || binding->second == target)
        return binding->first == source && binding->second == target;
    return (source == Lifetime{} && target == Lifetime{}) ||
           (source.specified() && target.specified());
  };
  if (!matchesRegion(left.referenceLifetime, right.referenceLifetime) ||
      left.lifetimeArguments.size() != right.lifetimeArguments.size())
    return false;
  for (std::size_t index = 0; index < left.lifetimeArguments.size(); ++index)
    if (!matchesRegion(left.lifetimeArguments[index], right.lifetimeArguments[index]))
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
      if (!equivalent(a[index], b[index], bindings, allowFreeRegionVariation, depth + 1))
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

// Structural admission only: every non-region fact and bound invocation
// relationship remains exact, while free region choices may differ. This is
// never evidence of a valid conversion. The owning semantic/MIR proof must
// independently establish variance and outlives from original source facts.
inline bool sameLifetimeConversionShape(const SemanticType &left,
                                        const SemanticType &right) {
  return lifetime_detail::equivalent(left, right, {}, true);
}

namespace lifetime_detail {
inline std::optional<std::vector<LifetimeBinding>> matchTypeLifetimeSlots(
    const SemanticType &pattern, const SemanticType &actual,
    std::span<const Lifetime> universals, Lifetime::Kind slotKind) {
  for (std::size_t index = 0; index < universals.size(); ++index)
    if (universals[index].kind != slotKind ||
        !universals[index].specified() ||
        std::find(universals.begin(), universals.begin() + index, universals[index]) !=
            universals.begin() + index)
      return std::nullopt;
  std::vector<LifetimeBinding> result;
  const auto match = [&](auto &&self, const SemanticType &source,
                         const SemanticType &target,
                         std::vector<LifetimeBinding> quantified,
                         std::size_t depth) -> bool {
    if (depth >= 256 || source.quantifiedLifetimes.size() != target.quantifiedLifetimes.size() ||
        source.lifetimeArguments.size() != target.lifetimeArguments.size())
      return false;
    for (std::size_t index = 0; index < source.quantifiedLifetimes.size(); ++index) {
      const auto from = Lifetime::parameter(source.quantifiedLifetimes[index]);
      const auto to = Lifetime::parameter(target.quantifiedLifetimes[index]);
      if (!from.specified() || !to.specified() ||
          std::find(universals.begin(), universals.end(), from) != universals.end() ||
          std::any_of(quantified.begin(), quantified.end(), [&](const auto binding) {
            return binding.first == from || binding.second == to;
          }) || std::any_of(result.begin(), result.end(), [&](const auto binding) {
            return binding.second == to;
          }))
        return false;
      quantified.emplace_back(from, to);
    }
    const auto region = [&](Lifetime from, Lifetime to) {
      for (const auto binding : quantified) {
        if (binding.first == from)
          return binding.second == to;
        if (binding.second == to)
          return false;
      }
      if (std::find(universals.begin(), universals.end(), from) == universals.end())
        return from == to;
      if (!to.specified())
        return false;
      for (const auto binding : result)
        if (binding.first == from)
          return binding.second == to;
      result.emplace_back(from, to);
      return true;
    };
    if (!region(source.referenceLifetime, target.referenceLifetime))
      return false;
    for (std::size_t index = 0; index < source.lifetimeArguments.size(); ++index)
      if (!region(source.lifetimeArguments[index], target.lifetimeArguments[index]))
        return false;
    auto left = source;
    auto right = target;
    const auto shell = [](SemanticType &type) {
      type.arguments.clear();
      type.lambdaEnclosingClassTypes.clear();
      type.lambdaEnclosingFunctionTypes.clear();
      type.referenceLifetime = {};
      type.lifetimeArguments.clear();
      type.quantifiedLifetimes.clear();
    };
    shell(left);
    shell(right);
    if (left != right)
      return false;
    const auto children = [&](const auto &a, const auto &b) {
      if (a.size() != b.size())
        return false;
      for (std::size_t index = 0; index < a.size(); ++index)
        if (!self(self, a[index], b[index], quantified, depth + 1))
          return false;
      return true;
    };
    return children(source.arguments, target.arguments) &&
           children(source.lambdaEnclosingClassTypes, target.lambdaEnclosingClassTypes) &&
           children(source.lambdaEnclosingFunctionTypes, target.lambdaEnclosingFunctionTypes);
  };
  if (!match(match, pattern, actual, {}, 0))
    return std::nullopt;
  return result;
}

} // namespace lifetime_detail

// Exact structural matching for a universal declaration view. This only
// identifies a substitution; callers must separately prove all its bounds.
inline std::optional<std::vector<LifetimeBinding>> matchUniversalTypeLifetimes(
    const SemanticType &pattern, const SemanticType &actual,
    std::span<const Lifetime> universals) {
  return lifetime_detail::matchTypeLifetimeSlots(pattern, actual, universals,
                                                Lifetime::Kind::Parameter);
}

// Contextual syntax inference changes only the explicitly inventoried holes.
// Named/free regions, type/value arguments, access and quantified scopes remain
// exact; this does not establish an outlives conversion or invent an origin.
inline std::optional<std::vector<LifetimeBinding>> matchInferredTypeLifetimes(
    const SemanticType &pattern, const SemanticType &actual,
    std::span<const Lifetime> inferences) {
  return lifetime_detail::matchTypeLifetimeSlots(pattern, actual, inferences,
                                                Lifetime::Kind::Inference);
}

// A generic argument recipe is one structural scope: repeated region slots
// across separate arguments must receive the same actual lifetime.
inline std::optional<std::vector<LifetimeBinding>> matchUniversalTypeArgumentLifetimes(
    std::span<const SemanticType> patterns, std::span<const SemanticType> actual,
    std::span<const Lifetime> universals) {
  return matchUniversalTypeLifetimes(
      SemanticType::concreteTypePack(0, {patterns.begin(), patterns.end()}),
      SemanticType::concreteTypePack(0, {actual.begin(), actual.end()}), universals);
}

// Rename declaration universals after structural type-argument projection.
// Actual local/inference choices belong to an application, never a published
// declaration contract. Nested callable binders remain capture-avoiding.
inline std::optional<LifetimeSignatureContract> substituteLifetimeContract(
    LifetimeSignatureContract contract, std::span<const LifetimeBinding> bindings) {
  const auto region = [&](Lifetime value) {
    for (const auto binding : bindings)
      if (binding.first == value)
        return binding.second;
    return value;
  };
  std::vector<Lifetime> parameters;
  for (const auto parameter : contract.parameters) {
    const auto mapped = region(parameter);
    if (mapped == Lifetime::staticLifetime())
      continue;
    if (mapped.kind != Lifetime::Kind::Parameter || !mapped.specified())
      return std::nullopt;
    if (std::find(parameters.begin(), parameters.end(), mapped) == parameters.end())
      parameters.push_back(mapped);
  }
  contract.parameters = std::move(parameters);
  contract.receiver = region(contract.receiver);
  for (auto &bound : contract.bounds) {
    bound.longer = region(bound.longer);
    bound.shorter = region(bound.shorter);
  }
  for (auto &bound : contract.typeBounds) {
    const auto type = substituteTypeLifetimes(bound.type, bindings);
    if (!type)
      return std::nullopt;
    bound.type = *type;
    bound.region = region(bound.region);
  }
  for (auto &node : contract.requirements.nodes) {
    const auto type = substituteTypeLifetimes(node.type, bindings);
    if (!type)
      return std::nullopt;
    node.type = *type;
    node.bound.longer = region(node.bound.longer);
    node.bound.shorter = region(node.bound.shorter);
  }
  return contract;
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
