#pragma once

#include "gti/semantic_model.h"

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

inline NominalVariance expandNominalVarianceRecipe(
    const SemanticType &type, const NominalVarianceRecipe &recipe,
    const NominalVariance &declaration) {
  if (!declaration.complete)
    return {};
  NominalVariance result = declaration;
  const bool pack = std::any_of(recipe.parameters.begin(), recipe.parameters.end(),
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
  for (std::size_t index = 0; index < recipes.size(); ++index) {
    const auto &recipe = recipes[index];
    if (!recipe.complete || !recipe.declaration ||
        (recipe.kind != SemanticType::Class && recipe.kind != SemanticType::Enum))
      return std::nullopt;
    for (std::size_t earlier = 0; earlier < index; ++earlier)
      if (recipes[earlier].kind == recipe.kind &&
          recipes[earlier].declaration == recipe.declaration)
        return std::nullopt;
    std::vector<GenericParameterId> ids;
    for (const auto &parameter : recipe.parameters) {
      if (!parameter.id || std::find(ids.begin(), ids.end(), parameter.id) != ids.end() ||
          (parameter.value && parameter.lifetime) ||
          (parameter.implicit && !parameter.lifetime) ||
          (parameter.pack && (parameter.value || parameter.lifetime)))
        return std::nullopt;
      ids.push_back(parameter.id);
      if (parameter.lifetime)
        result[index].lifetimes.push_back(TypeVariance::Bivariant);
      else if (!parameter.value)
        result[index].types.push_back(TypeVariance::Bivariant);
    }
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
    return index ? expandNominalVarianceRecipe(type, recipes[*index], result[*index])
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
      for (const auto &parameter : recipe.parameters)
        if (parameter.lifetime)
          slots.emplace(parameter.id, &next.lifetimes[lifetimes++]);
        else if (!parameter.value)
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
