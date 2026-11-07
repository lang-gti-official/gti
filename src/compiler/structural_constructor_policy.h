#pragma once

#include "gti/callable_lifetimes.h"
#include "gti/generic_type_patterns.h"

namespace lang::structural_policy_detail {

// These are transient declaration views. Neither a policy's claimed self type
// nor a concrete instance supplies the declaration's open owner identity.
template <class Parameters>
std::vector<GenericParameterIdentity> identities(const Parameters &parameters) {
  std::vector<GenericParameterIdentity> result;
  for (const auto &parameter : parameters)
    result.push_back({.id = parameter.id, .pack = parameter.pack,
                      .value = parameter.value, .lifetime = parameter.lifetime,
                      .implicit = parameter.implicit,
                      .typeArgumentPath = parameter.typeArgumentPath});
  return result;
}

template <class Parameters>
std::optional<SemanticType> openNominalType(
    SemanticType type, const Parameters &parameters,
    std::span<const SemanticType> patterns = {}) {
  std::vector<GenericParameterId> seen;
  for (const auto &parameter : parameters) {
    if (!parameter.id ||
        std::find(seen.begin(), seen.end(), parameter.id) != seen.end() ||
        (parameter.lifetime && (parameter.value || parameter.pack)))
      return std::nullopt;
    seen.push_back(parameter.id);
    if (parameter.lifetime)
      type.lifetimeArguments.push_back(Lifetime::parameter(parameter.id));
    else if (!patterns.empty()) {
      if (parameter.value || parameter.typeArgumentPath.empty())
        return std::nullopt;
    } else if (parameter.value)
      type.valueArguments.push_back(CompileTimeValue::parameter(parameter.id));
    else if (!parameter.typeArgumentPath.empty())
      return std::nullopt;
    else
      type.arguments.push_back(parameter.pack
          ? SemanticType::typePack(parameter.id)
          : SemanticType::typeParameter(parameter.id));
  }
  if (!patterns.empty())
    type.arguments.assign(patterns.begin(), patterns.end());
  return type;
}

template <class Class>
std::optional<SemanticType> openClassType(const Class &owner) {
  if (!owner.id)
    return std::nullopt;
  if (owner.exactSpecializationPrimary)
    return SemanticType::classType(owner.id);
  return openNominalType(SemanticType::classType(owner.id), owner.genericParameters,
                         owner.genericArgumentPatterns);
}

// Instantiate one exact nominal node. Child formation is traversed separately
// by callableLifetimeContract; this never unfolds storage or imports a nested
// callable's invocation premises.
inline std::optional<LifetimeRequirementFormula> nominalRequirements(
    const SemanticType &declaredType, const SemanticType &actualType,
    const LifetimeSignatureContract &contract,
    std::span<const GenericParameterIdentity> parameters,
    std::span<const GenericParameterIdentity> projected = {}) {
  if (!contract.valid() || !contract.requirements.resolved())
    return std::nullopt;
  GenericSubstitution seed;
  std::size_t valueIndex = 0, lifetimeIndex = 0;
  for (const auto &parameter : parameters) {
    if (parameter.lifetime) {
      if (lifetimeIndex == actualType.lifetimeArguments.size())
        return std::nullopt;
      const auto region = actualType.lifetimeArguments[lifetimeIndex++];
      if (!region.declarationRegion() || !seed.lifetimes.emplace(parameter.id, region).second)
        return std::nullopt;
    } else if (parameter.value) {
      if (valueIndex == actualType.valueArguments.size() ||
          !seed.values.emplace(parameter.id, actualType.valueArguments[valueIndex++]).second)
        return std::nullopt;
    }
  }
  if (valueIndex != actualType.valueArguments.size() ||
      lifetimeIndex != actualType.lifetimeArguments.size())
    return std::nullopt;
  GenericSubstitution values;
  values.values = seed.values;
  const auto pattern = substituteSemanticType(declaredType, values);
  const auto matched = deriveGenericArgumentPatternSubstitution(
      std::span(&pattern, 1), std::span(&actualType, 1), parameters, projected, seed);
  if (!matched)
    return std::nullopt;
  auto result = lifetimeContractRequirements(contract);
  if (!result)
    return std::nullopt;
  std::vector<LifetimeBinding> regions;
  for (const auto &[id, region] : matched->lifetimes)
    regions.emplace_back(Lifetime::parameter(id), region);
  const auto region = [&](Lifetime value) {
    const auto found = std::find_if(regions.begin(), regions.end(),
                                   [&](const auto &binding) { return binding.first == value; });
    return found == regions.end() ? value : found->second;
  };
  for (auto &node : result->nodes) {
    const auto type = substituteTypeLifetimes(
        substituteSemanticType(node.type, *matched), regions);
    if (!type || (node.kind == LifetimeRequirementNode::Kind::Type &&
                  *type == SemanticType::Unknown))
      return std::nullopt;
    node.type = *type;
    node.bound = {region(node.bound.longer), region(node.bound.shorter)};
  }
  return result;
}

// Rebuild the retained, entirely lifetime-only requires expression. Appending
// it to the normalized contract prevents stronger written leaves from being
// hidden by an otherwise structural claimed contract. Boolean arms stay whole.
template <class Nodes, class Leaf>
std::optional<LifetimeRequirementFormula> writtenRequirements(
    const Nodes &nodes, std::size_t root, std::size_t leafCount, Leaf leaf) {
  using Node = LifetimeRequirementNode;
  if (nodes.empty())
    return leafCount == 0 && root == ConstraintExpressionNode::invalidIndex
        ? std::optional(LifetimeRequirementFormula{}) : std::nullopt;
  if (nodes.size() > 4096 || leafCount > 4096 || root != nodes.size() - 1)
    return std::nullopt;
  LifetimeRequirementFormula result;
  std::vector<bool> used(leafCount, false);
  for (const auto &source : nodes) {
    if (source.kind == ConstraintExpressionKind::Application) {
      if (source.application >= leafCount ||
          source.left != ConstraintExpressionNode::invalidIndex ||
          source.right != ConstraintExpressionNode::invalidIndex)
        return std::nullopt;
      const auto value = leaf(source.application);
      if (!value)
        return std::nullopt;
      result.nodes.push_back(*value);
      used[source.application] = true;
    } else {
      if ((source.kind != ConstraintExpressionKind::Conjunction &&
           source.kind != ConstraintExpressionKind::Disjunction) ||
          source.application != ConstraintExpressionNode::invalidIndex ||
          source.left >= result.nodes.size() || source.right >= result.nodes.size())
        return std::nullopt;
      result.nodes.push_back({.kind = source.kind == ConstraintExpressionKind::Conjunction
                                         ? Node::Kind::All : Node::Kind::Any,
                              .left = source.left, .right = source.right});
    }
  }
  if (std::find(used.begin(), used.end(), false) != used.end())
    return std::nullopt;
  result.root = root;
  return result;
}

} // namespace lang::structural_policy_detail
