#include "parameter_packs.h"

#include "gti/backend.h"

#include <algorithm>

namespace lang::native_llvm {

std::vector<SemanticType>
flattenConcreteParameters(std::span<const SemanticType> parameters) {
  std::vector<SemanticType> flattened;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto &parameter = parameters[index];
    if (parameter.kind != SemanticType::TypePack) {
      flattened.push_back(parameter);
      continue;
    }
    if (!parameter.concretePack || index + 1 != parameters.size() ||
        std::any_of(parameter.arguments.begin(), parameter.arguments.end(),
                    [](const SemanticType &element) {
                      return element.kind == SemanticType::TypePack;
                    }))
      throw BackendUnsupportedError(
          "LLVM GTI ABI requires an exact trailing concrete parameter pack");
    flattened.insert(flattened.end(), parameter.arguments.begin(),
                     parameter.arguments.end());
  }
  return flattened;
}

std::optional<std::size_t>
concretePackPrefix(std::span<const SemanticType> parameters,
                   std::span<const HirBindingId> bindings,
                   HirBindingId packBinding, const SemanticType &packType) {
  if (!packBinding || packType.kind != SemanticType::TypePack ||
      !packType.concretePack ||
      std::any_of(packType.arguments.begin(), packType.arguments.end(),
                  [](const SemanticType &element) {
                    return element.kind == SemanticType::TypePack;
                  }))
    return std::nullopt;

  const auto sourcePack = std::find_if(
      parameters.begin(), parameters.end(), [](const SemanticType &parameter) {
        return parameter.kind == SemanticType::TypePack;
      });
  if (sourcePack != parameters.end()) {
    const std::size_t prefix =
        static_cast<std::size_t>(sourcePack - parameters.begin());
    if (prefix + 1 != parameters.size() || *sourcePack != packType ||
        bindings.size() != parameters.size() ||
        bindings.back() != packBinding ||
        std::any_of(bindings.begin(), bindings.end() - 1,
                    [](HirBindingId binding) { return binding == 0; }))
      return std::nullopt;
    return prefix;
  }

  // Some instantiated MIR owners retain the source binding list while their
  // parameter types have already been expanded into concrete ABI lanes. The
  // trailing source pack binding then has no one-to-one type entry (and an
  // empty pack has no lane at all). Prove that representation before accepting
  // it so body lowering and admission share the same exact mapping.
  if (bindings.size() <= parameters.size() + 1 && !bindings.empty() &&
      bindings.back() == packBinding) {
    const std::size_t prefix = bindings.size() - 1;
    if (prefix + packType.arguments.size() == parameters.size() &&
        std::equal(packType.arguments.begin(), packType.arguments.end(),
                   parameters.begin() + prefix) &&
        std::none_of(bindings.begin(), bindings.end() - 1,
                     [](HirBindingId binding) { return binding == 0; }))
      return prefix;
  }

  return std::nullopt;
}

std::optional<ConcreteParameterPlan>
planConcreteParameters(std::span<const SemanticType> parameters,
                       std::span<const HirBindingId> bindings,
                       const MirBody &body) {
  const auto flattened = flattenConcreteParameters(parameters);
  const MirPlace *packRoot = nullptr;
  for (const auto &place : body.places) {
    if (place.root != MirPlaceRootKind::Binding || !place.projections.empty() ||
        place.type.kind != SemanticType::TypePack)
      continue;
    if (!place.binding || !place.type.concretePack ||
        !place.initiallyAvailable || !place.key)
      return std::nullopt;
    if (packRoot &&
        (packRoot->binding != place.binding || packRoot->type != place.type ||
         packRoot->symbol != place.symbol || packRoot->key != place.key))
      return std::nullopt;
    packRoot = &place;
  }

  ConcreteParameterPlan result;
  if (!packRoot) {
    if (bindings.size() != flattened.size() ||
        std::any_of(bindings.begin(), bindings.end(),
                    [](HirBindingId binding) { return binding == 0; }))
      return std::nullopt;
    for (std::size_t index = 0; index < bindings.size(); ++index)
      result.lanes.push_back(
          {.binding = bindings[index], .type = flattened[index]});
    return result;
  }

  const auto prefixPlan = concretePackPrefix(parameters, bindings,
                                             packRoot->binding, packRoot->type);
  if (!prefixPlan)
    return std::nullopt;
  const std::size_t prefix = *prefixPlan;
  if (flattened.size() != prefix + packRoot->type.arguments.size() ||
      !std::equal(packRoot->type.arguments.begin(),
                  packRoot->type.arguments.end(), flattened.begin() + prefix))
    return std::nullopt;
  for (std::size_t index = 0; index < prefix; ++index) {
    if (!bindings[index])
      return std::nullopt;
    result.lanes.push_back(
        {.binding = bindings[index], .type = flattened[index]});
  }
  result.packs.push_back(
      {.binding = packRoot->binding, .type = packRoot->type});
  for (const auto &place : body.places) {
    if (place.root != MirPlaceRootKind::Binding ||
        place.binding != packRoot->binding || place.projections.empty() ||
        place.projections.front().kind != MirProjectionKind::PackElement)
      continue;
    if (!place.projections.front().constantIndex ||
        *place.projections.front().constantIndex >=
            packRoot->type.arguments.size())
      return std::nullopt;
  }
  for (std::size_t element = 0; element < packRoot->type.arguments.size();
       ++element) {
    bool used = false;
    std::optional<PlaceKey> exactIdentity;
    PlaceKey expectedIdentity = *packRoot->key;
    expectedIdentity.projections.push_back(
        {.kind = PlaceProjectionKind::ConstantIndex, .index = element});
    for (const auto &place : body.places) {
      if (place.root != MirPlaceRootKind::Binding ||
          place.binding != packRoot->binding || place.projections.empty() ||
          place.projections.front().kind != MirProjectionKind::PackElement ||
          place.projections.front().constantIndex != element)
        continue;
      used = true;
      if (place.projections.size() != 1)
        continue;
      if (place.type != packRoot->type.arguments[element] || !place.key ||
          *place.key != expectedIdentity)
        return std::nullopt;
      if (exactIdentity && *exactIdentity != *place.key)
        return std::nullopt;
      exactIdentity = place.key;
    }
    if (used && !exactIdentity)
      return std::nullopt;
    result.lanes.push_back({.binding = packRoot->binding,
                            .type = packRoot->type.arguments[element],
                            .packElement = element,
                            .materialized = used});
  }
  return result;
}

} // namespace lang::native_llvm
