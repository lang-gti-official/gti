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

  if (parameters.size() != bindings.size())
    return std::nullopt;
  if (packType.arguments.empty()) {
    if (std::find(bindings.begin(), bindings.end(), packBinding) !=
            bindings.end() ||
        std::any_of(bindings.begin(), bindings.end(),
                    [](HirBindingId binding) { return binding == 0; }))
      return std::nullopt;
    return parameters.size();
  }

  const auto first = std::find(bindings.begin(), bindings.end(), packBinding);
  if (first == bindings.end() ||
      std::find(first + 1, bindings.end(), packBinding) != bindings.end())
    return std::nullopt;
  const std::size_t prefix = static_cast<std::size_t>(first - bindings.begin());
  if (prefix + packType.arguments.size() != parameters.size() ||
      !std::equal(packType.arguments.begin(), packType.arguments.end(),
                  parameters.begin() + prefix) ||
      std::any_of(bindings.begin(), first,
                  [](HirBindingId binding) { return binding == 0; }) ||
      std::any_of(first + 1, bindings.end(),
                  [](HirBindingId binding) { return binding != 0; }))
    return std::nullopt;
  return prefix;
}

} // namespace lang::native_llvm
