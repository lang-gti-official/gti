#pragma once

#include "gti/lifetime_types.h"
#include "gti/semantic_model.h"

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace lang {

enum class GenericPatternMatch { Structure, Complete };

// Match an already selected type/value recipe. Structural selection deliberately
// ignores region relationships; Complete is a separate formation obligation and
// cannot select a different nominal family after a failed relationship.
// Value expressions in patterns must already have their ordinary substitution.
[[nodiscard]] inline std::optional<GenericSubstitution>
deriveGenericArgumentPatternSubstitution(
    std::span<const SemanticType> patterns,
    std::span<const SemanticType> actual,
    std::span<const GenericParameterIdentity> parameters,
    std::span<const GenericParameterIdentity> projectedLifetimes,
    GenericSubstitution seed = {},
    GenericPatternMatch mode = GenericPatternMatch::Complete) {
  if (patterns.size() != actual.size())
    return std::nullopt;
  const auto parameter = [&](GenericParameterId id) -> const GenericParameterIdentity * {
    const auto found = std::find_if(parameters.begin(), parameters.end(),
        [&](const auto &candidate) { return candidate.id == id; });
    return found == parameters.end() ? nullptr : &*found;
  };
  const auto projected = [&](GenericParameterId id) {
    return std::any_of(projectedLifetimes.begin(), projectedLifetimes.end(),
        [&](const auto &candidate) { return candidate.id == id && candidate.lifetime; });
  };
  // Independently projected entries are not scope evidence. Re-derive every
  // pattern participant together, retaining unrelated outer substitutions.
  const auto discardProjection = [&](auto &&self, const SemanticType &type) -> void {
    if ((type.kind == SemanticType::TypeParameter || type.kind == SemanticType::TypePack) &&
        parameter(type.genericParameterId))
      seed.types.erase(type.genericParameterId);
    lifetime_detail::children(type, [&](const auto &child) { self(self, child); });
  };
  for (const auto &pattern : patterns)
    discardProjection(discardProjection, pattern);
  std::erase_if(seed.callableScopes, [&](const auto &scope) {
    if (scope.argumentPath.empty() || scope.argumentPath.front() >= patterns.size())
      return false;
    const auto *pattern = &patterns[scope.argumentPath.front()];
    for (const auto index : std::span(scope.argumentPath).subspan(1)) {
      if (index >= pattern->arguments.size())
        return false;
      pattern = &pattern->arguments[index];
    }
    return *pattern == scope.pattern;
  });
  for (const auto &parameter : projectedLifetimes)
    seed.lifetimes.erase(parameter.id);
  std::vector<std::size_t> scopes;
  std::size_t work = 0;
  const auto scopedActual = [&](Lifetime region) {
    return region.kind == Lifetime::Kind::Parameter &&
        std::any_of(scopes.begin(), scopes.end(), [&](auto index) {
          const auto &bound = seed.callableScopes[index].signature.quantifiedLifetimes;
          return std::find(bound.begin(), bound.end(), region.id) != bound.end();
        });
  };
  const auto bindRegion = [&](Lifetime from, Lifetime to) {
    if (mode == GenericPatternMatch::Structure)
      return true;
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
      for (const auto binding : seed.callableScopes[*scope].regionBindings)
        if (binding.first == from)
          return binding.second == to;
    const bool projection = from.kind == Lifetime::Kind::Parameter && projected(from.id);
    const bool hole = from.kind == Lifetime::Kind::Unspecified ||
                      from.kind == Lifetime::Kind::Inference;
    if (!scopes.empty() && (projection || hole)) {
      if (!to.specified())
        return hole;
      // Unspecified slots have no reusable declaration identity. A concrete
      // reconstruction must normalize them from its complete input signature.
      if (from.specified())
        seed.callableScopes[scopes.back()].regionBindings.emplace_back(from, to);
      return true;
    }
    if (projection) {
      if (!to.specified() || scopedActual(to))
        return false;
      const auto [entry, inserted] = seed.lifetimes.emplace(from.id, to);
      return inserted || entry->second == to;
    }
    if (from.kind == Lifetime::Kind::Parameter)
      if (const auto found = seed.lifetimes.find(from.id); found != seed.lifetimes.end())
        return found->second == to;
    return from == to;
  };
  const auto bindType = [&](GenericParameterId id, SemanticType type) {
    const auto regions = freeTypeLifetimes(type);
    std::optional<std::size_t> scope;
    if (mode == GenericPatternMatch::Complete)
      for (auto current = scopes.rbegin(); current != scopes.rend(); ++current) {
        const auto &bound = seed.callableScopes[*current].signature.quantifiedLifetimes;
        if (std::any_of(regions.begin(), regions.end(), [&](Lifetime region) {
              return region.kind == Lifetime::Kind::Parameter &&
                  std::find(bound.begin(), bound.end(), region.id) != bound.end();
            })) {
          scope = *current;
          break;
        }
      }
    const auto equal = [&](const SemanticType &previous) {
      return mode == GenericPatternMatch::Structure
          ? eraseTypeLifetimes(previous) == eraseTypeLifetimes(type)
          : equivalentLifetimeTypes(previous, type);
    };
    if (const auto found = seed.types.find(id); found != seed.types.end())
      return !scope && equal(found->second);
    for (const auto &captured : seed.callableScopes)
      if (const auto found = captured.capturedTypes.find(id);
          found != captured.capturedTypes.end())
        return scope && equal(found->second);
    if (scope)
      seed.callableScopes[*scope].capturedTypes.emplace(id, std::move(type));
    else
      seed.types.emplace(id, std::move(type));
    return true;
  };
  const auto match = [&](auto &&self, const SemanticType &pattern,
                         const SemanticType &value,
                         std::vector<std::size_t> path, std::size_t depth) -> bool {
    if (++work > 65536 || depth >= 256)
      return false;
    if ((pattern.kind == SemanticType::TypeParameter ||
         (pattern.kind == SemanticType::TypePack && !pattern.concretePack))) {
      const auto *binder = parameter(pattern.genericParameterId);
      if (binder && !binder->value && !binder->lifetime)
        return bindType(binder->id, value);
    }
    const auto shape = [](SemanticType type) {
      type.referenceLifetime = {};
      type.lifetimeArguments.clear();
      type.quantifiedLifetimes.clear();
      type.arguments.clear();
      type.lambdaEnclosingClassTypes.clear();
      type.lambdaEnclosingFunctionTypes.clear();
      return type;
    };
    if (shape(pattern) != shape(value) ||
        pattern.lifetimeArguments.size() != value.lifetimeArguments.size())
      return false;
    const bool callable = pattern.kind == SemanticType::FunctionSignature ||
                          pattern.kind == SemanticType::NativeFunction;
    const auto scopeCount = scopes.size();
    if (callable && mode == GenericPatternMatch::Complete) {
      const auto uniqueBinders = [](const auto &binders) {
        for (std::size_t index = 0; index < binders.size(); ++index)
          if (!binders[index] ||
              std::find(binders.begin(), binders.begin() + index, binders[index]) !=
                  binders.begin() + index)
            return false;
        return true;
      };
      if (!uniqueBinders(pattern.quantifiedLifetimes) ||
          !uniqueBinders(value.quantifiedLifetimes) ||
          std::any_of(value.quantifiedLifetimes.begin(), value.quantifiedLifetimes.end(),
                      [&](auto id) { return scopedActual(Lifetime::parameter(id)); }))
        return false;
      GenericCallableScope scope{.argumentPath = path, .pattern = pattern, .signature = value};
      if (!pattern.quantifiedLifetimes.empty()) {
        if (pattern.quantifiedLifetimes.size() != value.quantifiedLifetimes.size())
          return false;
        for (std::size_t index = 0; index < pattern.quantifiedLifetimes.size(); ++index)
          scope.regionBindings.emplace_back(
              Lifetime::parameter(pattern.quantifiedLifetimes[index]),
              Lifetime::parameter(value.quantifiedLifetimes[index]));
      }
      seed.callableScopes.push_back(std::move(scope));
      scopes.push_back(seed.callableScopes.size() - 1);
    } else if (!callable && pattern.quantifiedLifetimes != value.quantifiedLifetimes) {
      // Other scoped runtime types use their established exact type matching;
      // they are not declaration-pattern binding sites.
      if (!equivalentLifetimeTypes(pattern, value))
        return false;
    }
    const auto restoreScopes = [&] { scopes.resize(scopeCount); };
    if (!bindRegion(pattern.referenceLifetime, value.referenceLifetime)) {
      restoreScopes();
      return false;
    }
    for (std::size_t index = 0; index < pattern.lifetimeArguments.size(); ++index)
      if (!bindRegion(pattern.lifetimeArguments[index], value.lifetimeArguments[index])) {
        restoreScopes();
        return false;
      }
    const auto sequence = [&](const auto &left, const auto &right,
                              bool argumentPath) {
      std::size_t index = 0;
      for (; index < left.size(); ++index) {
        const auto &part = left[index];
        const auto *binder = part.kind == SemanticType::TypePack && !part.concretePack
            ? parameter(part.genericParameterId) : nullptr;
        if (binder && binder->pack && index + 1 == left.size()) {
          if (index > right.size())
            return false;
          const auto tail = std::span(right).subspan(index);
          const auto pack = tail.size() == 1 && tail.front().kind == SemanticType::TypePack &&
                                    !tail.front().concretePack
              ? tail.front()
              : SemanticType::concreteTypePack(binder->id, {tail.begin(), tail.end()});
          return bindType(binder->id, pack);
        }
        if (index >= right.size())
          return false;
        auto child = path;
        if (argumentPath)
          child.push_back(index);
        if (!self(self, part, right[index], std::move(child), depth + 1))
          return false;
      }
      return index == right.size();
    };
    const bool valid = sequence(pattern.arguments, value.arguments, true) &&
        sequence(pattern.lambdaEnclosingClassTypes, value.lambdaEnclosingClassTypes, false) &&
        sequence(pattern.lambdaEnclosingFunctionTypes, value.lambdaEnclosingFunctionTypes, false);
    restoreScopes();
    return valid;
  };
  for (std::size_t index = 0; index < patterns.size(); ++index)
    if (!match(match, patterns[index], actual[index], {index}, 0))
      return std::nullopt;
  return seed;
}

// Reconstruct projected invocation types without exposing their bound regions
// as ordinary nominal arguments. The callbacks retain the existing value and
// exact-specialization authorities; this routine only adds lexical scope to
// their usual structural substitution. A member may open a captured scope by
// supplying fresh universal imports, but a stored callable always closes its
// own captured invocation binders instead.
template <class SubstituteValue, class ResolveType>
[[nodiscard]] std::optional<SemanticType> substituteScopedGenericType(
    const SemanticType &type, const GenericSubstitution &substitution,
    const SubstituteValue &substituteValue, const ResolveType &resolveType) {
  std::vector<std::size_t> active;
  std::size_t work = 0;
  const auto callable = [](const SemanticType &type) {
    return type.kind == SemanticType::FunctionSignature ||
           type.kind == SemanticType::NativeFunction;
  };
  const auto scopeBinds = [&](std::size_t scope, Lifetime region) {
    const auto &binders = substitution.callableScopes[scope].signature.quantifiedLifetimes;
    return region.kind == Lifetime::Kind::Parameter &&
        std::find(binders.begin(), binders.end(), region.id) != binders.end();
  };
  const auto isActive = [&](std::size_t scope) {
    return std::find(active.begin(), active.end(), scope) != active.end();
  };
  const auto import = [&](const SemanticType &value, std::size_t scope)
      -> std::optional<SemanticType> {
    const auto &capture = substitution.callableScopes[scope];
    std::vector<LifetimeBinding> bindings;
    for (const auto region : freeTypeLifetimes(value)) {
      if (!scopeBinds(scope, region))
        continue;
      const auto found = capture.imports.find(region.id);
      if (found == capture.imports.end() || !found->second.specified())
        return std::nullopt;
      bindings.emplace_back(region, found->second);
    }
    return substituteTypeLifetimes(value, bindings);
  };
  const auto scopedRegion = [&](Lifetime region) -> std::optional<Lifetime> {
    std::optional<Lifetime> result;
    for (std::size_t index = 0; index < substitution.callableScopes.size(); ++index) {
      const auto &scope = substitution.callableScopes[index];
      for (const auto &[from, actual] : scope.regionBindings) {
        if (!from.specified() || from != region)
          continue;
        auto value = actual;
        if (scopeBinds(index, actual) && !isActive(index)) {
          const auto found = scope.imports.find(actual.id);
          if (found == scope.imports.end() || !found->second.specified())
            return std::nullopt;
          value = found->second;
        }
        if (result && *result != value)
          return std::nullopt;
        result = value;
      }
    }
    return result.value_or(region);
  };
  // Only direct participants select a callable's scope. An inner signature
  // does not implicitly bind the surrounding signature or its object.
  const auto participants = [&](const SemanticType &type) {
    std::vector<GenericParameterId> types;
    std::vector<Lifetime> regions;
    const auto collect = [&](auto &&self, const SemanticType &part, bool root) -> void {
      if (!root && callable(part))
        return;
      if (part.kind == SemanticType::TypeParameter ||
          (part.kind == SemanticType::TypePack && !part.concretePack))
        types.push_back(part.genericParameterId);
      if (part.referenceLifetime.specified())
        regions.push_back(part.referenceLifetime);
      regions.insert(regions.end(), part.lifetimeArguments.begin(), part.lifetimeArguments.end());
      lifetime_detail::children(part, [&](const auto &child) { self(self, child, false); });
    };
    collect(collect, type, true);
    return std::pair(std::move(types), std::move(regions));
  };
  const auto substitute = [&](auto &&self, const SemanticType &input,
                              std::size_t depth) -> std::optional<SemanticType> {
    if (++work > 65536 || depth >= 256)
      return std::nullopt;
    if (input.kind == SemanticType::TypeParameter ||
        (input.kind == SemanticType::TypePack && !input.concretePack)) {
      std::optional<SemanticType> captured;
      for (std::size_t index = 0; index < substitution.callableScopes.size(); ++index) {
        const auto &scope = substitution.callableScopes[index];
        const auto found = scope.capturedTypes.find(input.genericParameterId);
        if (found == scope.capturedTypes.end())
          continue;
        auto value = isActive(index) ? std::optional(found->second) : import(found->second, index);
        if (!value || (captured && !equivalentLifetimeTypes(*captured, *value)))
          return std::nullopt;
        captured = std::move(value);
      }
      if (captured)
        return resolveType(std::move(*captured));
      const auto found = substitution.types.find(input.genericParameterId);
      if (found == substitution.types.end())
        return input;
      // An ordinary projection has no authority to carry one of the newly
      // closed invocation binders. Such a value must have a scoped capture.
      for (const auto region : freeTypeLifetimes(found->second))
        for (const auto scope : active)
          if (scopeBinds(scope, region))
            return std::nullopt;
      return resolveType(found->second);
    }
    const auto savedActive = active.size();
    struct Restore {
      std::vector<std::size_t> &active;
      std::size_t size;
      ~Restore() { active.resize(size); }
    } restore{active, savedActive};
    SemanticType result = input;
    if (callable(input)) {
      const auto [typeUses, regionUses] = participants(input);
      for (std::size_t index = 0; index < substitution.callableScopes.size(); ++index) {
        if (isActive(index))
          continue;
        const auto &scope = substitution.callableScopes[index];
        const auto [patternTypes, patternRegions] = participants(scope.pattern);
        const bool relevantType = std::any_of(typeUses.begin(), typeUses.end(), [&](auto id) {
          return std::find(patternTypes.begin(), patternTypes.end(), id) != patternTypes.end();
        });
        const bool relevantRegion = std::any_of(regionUses.begin(), regionUses.end(), [&](auto region) {
          return std::any_of(scope.regionBindings.begin(), scope.regionBindings.end(),
                             [&](const auto &binding) { return binding.first == region; });
        });
        if (!relevantType && !relevantRegion)
          continue;
        // Two independent captured scopes must not acquire a coincidental
        // equality from reused numeric identities.
        for (const auto id : scope.signature.quantifiedLifetimes)
          for (const auto outer : active)
            if (scopeBinds(outer, Lifetime::parameter(id)))
              return std::nullopt;
        active.push_back(index);
        for (auto &id : result.quantifiedLifetimes)
          for (const auto &[from, to] : scope.regionBindings)
            if (from == Lifetime::parameter(id)) {
              if (to.kind != Lifetime::Kind::Parameter)
                return std::nullopt;
              id = to.id;
              break;
            }
        for (const auto id : scope.signature.quantifiedLifetimes)
          if (std::find(result.quantifiedLifetimes.begin(), result.quantifiedLifetimes.end(), id) ==
              result.quantifiedLifetimes.end())
            result.quantifiedLifetimes.push_back(id);
      }
    }
    const auto reference = scopedRegion(result.referenceLifetime);
    if (!reference)
      return std::nullopt;
    result.referenceLifetime = *reference;
    for (auto &region : result.lifetimeArguments) {
      const auto value = scopedRegion(region);
      if (!value)
        return std::nullopt;
      region = *value;
    }
    const auto substituteTypes = [&](auto &types) {
      for (auto &argument : types) {
        auto value = self(self, argument, depth + 1);
        if (!value)
          return false;
        argument = std::move(*value);
      }
      return true;
    };
    if (!substituteTypes(result.arguments) ||
        !substituteTypes(result.lambdaEnclosingClassTypes) ||
        !substituteTypes(result.lambdaEnclosingFunctionTypes))
      return std::nullopt;
    if (result.kind == SemanticType::Class || result.kind == SemanticType::Enum || callable(result)) {
      std::vector<SemanticType> expanded;
      for (auto &argument : result.arguments)
        SemanticType::appendExpandedArgument(expanded, std::move(argument));
      result.arguments = std::move(expanded);
    }
    const auto substituteValues = [&](auto &values) {
      for (auto &value : values)
        value = substituteValue(value);
    };
    substituteValues(result.valueArguments);
    substituteValues(result.lambdaEnclosingClassValues);
    substituteValues(result.lambdaEnclosingFunctionValues);
    if (result.arrayLengthParameterId) {
      const auto value = substituteValue(CompileTimeValue::parameter(result.arrayLengthParameterId));
      if (value.kind == CompileTimeValue::UInt64) {
        result.arrayLength = value.value;
        result.arrayLengthParameterId = 0;
      } else if (value.kind == CompileTimeValue::Parameter) {
        result.arrayLengthParameterId = value.parameterId;
      }
    }
    return resolveType(std::move(result));
  };
  return substitute(substitute, type, 0);
}

} // namespace lang
