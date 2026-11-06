#pragma once

#include "gti/semantic_model.h"
#include "gti/lifetime_types.h"

#include <span>
#include <unordered_map>

namespace lang {
inline TypeVariance joinTypeVariance(TypeVariance left, TypeVariance right) {
  if (left == TypeVariance::Bivariant)
    return right;
  if (right == TypeVariance::Bivariant || left == right)
    return left;
  return TypeVariance::Invariant;
}
inline TypeVariance composeTypeVariance(TypeVariance outer, TypeVariance inner) {
  if (outer == TypeVariance::Bivariant || inner == TypeVariance::Bivariant)
    return TypeVariance::Bivariant;
  if (outer == TypeVariance::Invariant || inner == TypeVariance::Invariant)
    return TypeVariance::Invariant;
  return outer == inner ? TypeVariance::Covariant : TypeVariance::Contravariant;
}

// Exact specializations retain their primary's public type/value arguments.
// Their source binders still own the selected lifetime slots; neither slot
// inventory may disappear when the specialization has no lexical type binder.
inline std::optional<std::vector<NominalVarianceParameter>> nominalVarianceParameters(
    const NominalVarianceRecipe &recipe,
    std::span<const NominalVarianceRecipe> recipes) {
  if (!recipe.exactSpecializationPrimary)
    return recipe.parameters;
  const NominalVarianceRecipe *primary = nullptr;
  for (const auto &candidate : recipes)
    if (candidate.kind == recipe.kind &&
        candidate.declaration == recipe.exactSpecializationPrimary) {
      if (primary || candidate.exactSpecializationPrimary)
        return std::nullopt;
      primary = &candidate;
    }
  if (!primary)
    return std::nullopt;
  std::vector<NominalVarianceParameter> lifetimes;
  for (const auto &parameter : recipe.parameters) {
    if (!parameter.lifetime)
      return std::nullopt;
    lifetimes.push_back(parameter);
  }
  auto result = primary->parameters;
  std::size_t regions = 0;
  for (auto &parameter : result)
    if (parameter.lifetime) {
      if (regions == lifetimes.size())
        return std::nullopt;
      parameter = lifetimes[regions++];
    }
  return regions == lifetimes.size() ? std::optional{std::move(result)} : std::nullopt;
}

inline NominalVariance expandNominalVarianceRecipe(
    const SemanticType &type, const NominalVarianceRecipe &recipe,
    const NominalVariance &declaration,
    std::span<const NominalVarianceRecipe> recipes = {}) {
  const auto parameters = nominalVarianceParameters(recipe, recipes);
  if (!declaration.complete || !parameters)
    return {};
  NominalVariance result = declaration;
  const bool pack = std::any_of(parameters->begin(), parameters->end(),
                                [](const auto &parameter) { return parameter.pack; });
  if (pack && !result.types.empty() && type.arguments.size() + 1 >= result.types.size()) {
    const auto tail = result.types.back();
    result.types.resize(type.arguments.size(), tail);
  }
  if (result.types.size() != type.arguments.size() ||
      result.lifetimes.size() != type.lifetimeArguments.size())
    return {};
  return result;
}

// Shared monotone fixed point over declaration storage recipes. MIR supplies
// independently checked recipes; semantics supplies its finalized declaration
// inventory. Neither caller contributes a claimed variance to this algorithm.
inline std::optional<std::vector<NominalVariance>> inferNominalVarianceRecipes(
    std::span<const NominalVarianceRecipe> recipes) {
  std::vector<NominalVariance> result(recipes.size());
  std::vector<std::vector<NominalVarianceParameter>> parameters;
  parameters.reserve(recipes.size());
  for (std::size_t index = 0; index < recipes.size(); ++index) {
    const auto &recipe = recipes[index];
    if (!recipe.complete || !recipe.declaration ||
        (recipe.kind != SemanticType::Class && recipe.kind != SemanticType::Enum))
      return std::nullopt;
    for (std::size_t earlier = 0; earlier < index; ++earlier)
      if (recipes[earlier].kind == recipe.kind &&
          recipes[earlier].declaration == recipe.declaration)
        return std::nullopt;
    const auto publicParameters = nominalVarianceParameters(recipe, recipes);
    if (!publicParameters)
      return std::nullopt;
    parameters.push_back(*publicParameters);
    std::vector<GenericParameterId> ids;
    for (const auto &parameter : parameters.back()) {
      if (!parameter.id || std::find(ids.begin(), ids.end(), parameter.id) != ids.end() ||
          (parameter.value && parameter.lifetime) ||
          (parameter.implicit && !parameter.lifetime) ||
          (parameter.pack && (parameter.value || parameter.lifetime)))
        return std::nullopt;
      ids.push_back(parameter.id);
      if (parameter.lifetime)
        result[index].lifetimes.push_back(TypeVariance::Bivariant);
      else if (!parameter.value && recipe.argumentPatterns.empty())
        result[index].types.push_back(recipe.exactSpecializationPrimary
                                          ? TypeVariance::Invariant
                                          : TypeVariance::Bivariant);
    }
    // Pattern binders are projections inside public type arguments, never
    // additional public region slots. Until each pattern position is composed
    // with its stored uses, the complete public argument is invariant.
    if (!recipe.argumentPatterns.empty())
      result[index].types.assign(recipe.argumentPatterns.size(), TypeVariance::Invariant);
    if (!recipe.argumentPatternLifetimes.empty() && recipe.argumentPatterns.empty())
      return std::nullopt;
    std::vector<Lifetime> scope;
    for (const auto &parameter : parameters.back())
      if (parameter.lifetime)
        scope.push_back(Lifetime::parameter(parameter.id));
    for (const auto &parameter : recipe.argumentPatternLifetimes) {
      if (!parameter.id || !parameter.lifetime || !parameter.implicit || parameter.value ||
          parameter.pack || !parameter.typeArgumentPath.empty() ||
          std::find(ids.begin(), ids.end(), parameter.id) != ids.end())
        return std::nullopt;
      ids.push_back(parameter.id);
      scope.push_back(Lifetime::parameter(parameter.id));
    }
    std::vector<Lifetime> used;
    for (const auto &pattern : recipe.argumentPatterns)
      for (const auto region : freeTypeLifetimes(pattern)) {
        if (!region.specified() || (region.kind != Lifetime::Kind::Static &&
            std::find(scope.begin(), scope.end(), region) == scope.end()))
          return std::nullopt;
        used.push_back(region);
      }
    for (const auto &parameter : recipe.argumentPatternLifetimes)
      if (std::find(used.begin(), used.end(), Lifetime::parameter(parameter.id)) == used.end())
        return std::nullopt;
    result[index].complete = true;
  }
  const auto declarationIndex = [&](SemanticType::Kind kind, std::uint64_t id)
      -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < recipes.size(); ++index)
      if (recipes[index].kind == kind && recipes[index].declaration == id)
        return index;
    return std::nullopt;
  };
  const auto nominal = [&](const SemanticType &type) {
    const auto index = declarationIndex(type.kind,
        type.kind == SemanticType::Class ? type.classId : type.enumId);
    return index ? expandNominalVarianceRecipe(type, recipes[*index], result[*index], recipes)
                 : NominalVariance{};
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t index = 0; index < recipes.size(); ++index) {
      const auto &recipe = recipes[index];
      auto next = result[index];
      std::unordered_map<GenericParameterId, TypeVariance *> slots;
      std::size_t types = 0, lifetimes = 0;
      for (const auto &parameter : parameters[index])
        if (parameter.lifetime)
          slots.emplace(parameter.id, &next.lifetimes[lifetimes++]);
        else if (!parameter.value && recipe.argumentPatterns.empty())
          slots.emplace(parameter.id, &next.types[types++]);
      const auto observe = [&](GenericParameterId id, TypeVariance position) {
        if (const auto found = slots.find(id); found != slots.end())
          *found->second = joinTypeVariance(*found->second, position);
      };
      const auto capability = recipe.compilerCapability;
      const bool owned = capability == CompilerCapabilityTypeKind::UniqueOwner ||
          capability == CompilerCapabilityTypeKind::SharedOwner ||
          capability == CompilerCapabilityTypeKind::OptionalStorage ||
          capability == CompilerCapabilityTypeKind::FunctionPointer ||
          capability == CompilerCapabilityTypeKind::TextView ||
          capability == CompilerCapabilityTypeKind::CString;
      const bool opaque = recipe.cOpaqueHandle || recipe.polymorphic ||
          recipe.classKind == ClassKind::Interface || recipe.forward ||
          recipe.invalidLifetime ||
          (capability != CompilerCapabilityTypeKind::None && !owned);
      if (opaque || owned)
        for (const auto &[id, ignored] : slots) {
          (void)ignored;
          observe(id, opaque ? TypeVariance::Invariant : TypeVariance::Covariant);
        }
      const auto visit = [&](auto &&self, const SemanticType &type,
                              TypeVariance position) -> void {
        if (position == TypeVariance::Bivariant)
          return;
        if (type.kind == SemanticType::TypeParameter || type.kind == SemanticType::TypePack)
          observe(type.genericParameterId, position);
        if (type.kind == SemanticType::Reference &&
            type.referenceLifetime.kind == Lifetime::Kind::Parameter)
          observe(type.referenceLifetime.id, position);
        const auto nestedNominal = nominal(type);
        for (std::size_t slot = 0; slot < type.lifetimeArguments.size(); ++slot) {
          const auto region = type.lifetimeArguments[slot];
          if (region.kind == Lifetime::Kind::Parameter)
            observe(region.id, composeTypeVariance(position,
                nestedNominal.complete ? nestedNominal.lifetimes[slot]
                : (type.kind == SemanticType::StringView || type.kind == SemanticType::CString)
                    ? TypeVariance::Covariant
                                                       : TypeVariance::Invariant));
        }
        for (std::size_t slot = 0; slot < type.arguments.size(); ++slot) {
          TypeVariance nested = TypeVariance::Covariant;
          if (nestedNominal.complete)
            nested = nestedNominal.types[slot];
          else if (type.kind == SemanticType::Class || type.kind == SemanticType::Enum ||
                   type.kind == SemanticType::RawPointer ||
                   (type.kind == SemanticType::Reference &&
                    type.referenceAccess == AccessMode::Mutable))
            nested = TypeVariance::Invariant;
          else if (type.kind == SemanticType::FunctionSignature ||
                   type.kind == SemanticType::NativeFunction)
            nested = slot == 0 ? TypeVariance::Covariant : TypeVariance::Contravariant;
          self(self, type.arguments[slot], composeTypeVariance(position, nested));
        }
      };
      for (const auto &stored : recipe.stored)
        visit(visit, stored.type, TypeVariance::Covariant);
      if (next != result[index]) {
        result[index] = std::move(next);
        changed = true;
      }
    }
    for (std::size_t index = 0; index < recipes.size(); ++index) {
      const auto &recipe = recipes[index];
      if (!recipe.exactSpecializationPrimary)
        continue;
      const auto primary = declarationIndex(recipe.kind, recipe.exactSpecializationPrimary);
      if (!primary || result[*primary].lifetimes.size() != result[index].lifetimes.size())
        return std::nullopt;
      for (std::size_t slot = 0; slot < result[index].lifetimes.size(); ++slot) {
        const auto combined = joinTypeVariance(result[index].lifetimes[slot],
                                               result[*primary].lifetimes[slot]);
        changed = changed || combined != result[index].lifetimes[slot] ||
                              combined != result[*primary].lifetimes[slot];
        result[index].lifetimes[slot] = combined;
        result[*primary].lifetimes[slot] = combined;
      }
    }
  }
  return result;
}
} // namespace lang
