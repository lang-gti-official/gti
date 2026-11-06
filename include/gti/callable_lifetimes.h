#pragma once

#include "gti/lifetime_types.h"
#include "gti/lifetime_requirements.h"

#include <functional>

namespace lang {

// The phase supplies the exact declaration contract instantiated for this
// one nominal application. Child types are visited by the shared derivation.
using CallableNominalFormation = std::function<
    std::optional<LifetimeRequirementFormula>(const SemanticType &)>;

// Both callable kinds own invocation binders. NativeFunction additionally
// denotes a nullable ABI pointer; its boundary/unsafe rules remain separate.
inline bool isLifetimeInvocationSignature(const SemanticType &type) {
  return type.kind == SemanticType::FunctionSignature ||
         type.kind == SemanticType::NativeFunction;
}

// Derive the interface of one invocation from an already normalized callable
// type. Outer quantified names become invocation universals; nested callable
// binders keep their own scope. No body facts or client spelling participates.
namespace callable_lifetime_detail {
inline std::optional<LifetimeSignatureContract>
deriveContract(const SemanticType &signature,
               const CallableNominalFormation &nominalFormation, std::size_t &work) {
  if (!isLifetimeInvocationSignature(signature) || signature.arguments.empty())
    return std::nullopt;
  // Receiver qualifiers describe general callable interfaces. The consumers
  // that form or invoke an ABI/invocation pointer own their narrower policy.
  switch (signature.signatureReceiver) {
  case ReceiverMutability::ReadOnly:
    break;
  case ReceiverMutability::Mutable:
  case ReceiverMutability::Consuming:
    if (signature.kind == SemanticType::NativeFunction)
      return std::nullopt;
    break;
  default:
    return std::nullopt;
  }
  LifetimeSignatureContract contract;
  contract.origin = LifetimeContractOrigin::Declared;
  const auto add = [&](Lifetime region) {
    if (region.kind == Lifetime::Kind::Parameter &&
        std::find(contract.parameters.begin(), contract.parameters.end(), region) ==
            contract.parameters.end())
      contract.parameters.push_back(region);
  };
  for (const auto id : signature.quantifiedLifetimes) {
    const auto region = Lifetime::parameter(id);
    if (!region.specified() ||
        std::find(contract.parameters.begin(), contract.parameters.end(), region) !=
            contract.parameters.end())
      return std::nullopt;
    add(region);
  }
  const auto scoped = [&](auto &&self, const SemanticType &type,
                          std::vector<Lifetime> bound) -> bool {
    if (++work > 4096)
      return false;
    if (!type.quantifiedLifetimes.empty() && !isLifetimeInvocationSignature(type))
      return false;
    for (const auto id : type.quantifiedLifetimes) {
      const auto region = Lifetime::parameter(id);
      if (!region.specified() || std::find(bound.begin(), bound.end(), region) != bound.end())
        return false;
      bound.push_back(region);
    }
    if ((type.kind == SemanticType::Reference &&
         (!type.referenceLifetime.declarationRegion() || type.arguments.size() != 1)) ||
        (type.kind != SemanticType::Reference && type.referenceLifetime != Lifetime{}) ||
        ((type.kind == SemanticType::StringView || type.kind == SemanticType::CString) &&
         type.lifetimeArguments.size() != 1))
      return false;
    for (const auto region : type.lifetimeArguments)
      if (!region.declarationRegion())
        return false;
    bool valid = true;
    lifetime_detail::children(type, [&](const auto &child) {
      valid = self(self, child, bound) && valid;
    });
    return valid;
  };
  if (!scoped(scoped, signature, {}))
    return std::nullopt;
  for (std::size_t index = 1; index < signature.arguments.size(); ++index)
    for (const auto region : freeTypeLifetimes(signature.arguments[index]))
      add(region);
  for (const auto region : freeTypeLifetimes(signature.arguments.front()))
    add(region);
  const auto implied = [&](auto &&self, const SemanticType &type, bool input) -> bool {
    if (++work > 4096)
      return false;
    if (isLifetimeInvocationSignature(type)) {
      // Validate this callable's own formation contract. Its universally
      // quantified premises do not become assumptions of the enclosing one.
      return deriveContract(type, nominalFormation, work).has_value();
    }
    if (!type.quantifiedLifetimes.empty())
      return false;
    if (input && type.kind == SemanticType::Reference) {
      LifetimeTypeBound bound{type.arguments.front(), type.referenceLifetime};
      if (std::find(contract.typeBounds.begin(), contract.typeBounds.end(), bound) ==
          contract.typeBounds.end())
        contract.typeBounds.push_back(std::move(bound));
    }
    if (type.kind == SemanticType::Class || type.kind == SemanticType::Enum) {
      const auto formation = nominalFormation ? nominalFormation(type) : std::nullopt;
      if (!formation || !appendLifetimeRequirementFormula(contract.requirements, *formation))
        return false;
    }
    bool valid = true;
    lifetime_detail::children(type, [&](const auto &child) {
      valid = valid && self(self, child, input);
    });
    return valid;
  };
  for (std::size_t index = 1; index < signature.arguments.size(); ++index)
    if (!implied(implied, signature.arguments[index], true))
      return std::nullopt;
  if (!implied(implied, signature.arguments.front(), false))
    return std::nullopt;
  const auto declared = [&](Lifetime region) {
    return region == Lifetime::staticLifetime() ||
           std::find(contract.parameters.begin(), contract.parameters.end(), region) !=
               contract.parameters.end();
  };
  for (const auto &node : contract.requirements.nodes)
    if (node.kind == LifetimeRequirementNode::Kind::Type)
      for (const auto region : freeTypeLifetimes(node.type))
        if (!declared(region))
          return std::nullopt;
  contract.complete = true;
  return contract.valid() ? std::optional{std::move(contract)} : std::nullopt;
}
} // namespace callable_lifetime_detail

inline std::optional<LifetimeSignatureContract>
callableLifetimeContract(const SemanticType &signature,
                         const CallableNominalFormation &nominalFormation = {}) {
  std::size_t work = 0;
  return callable_lifetime_detail::deriveContract(signature, nominalFormation, work);
}

// Fixed carrier choices are distinct from invocation quantifiers. The phase
// supplies fresh declaration identities when publishing the result; a query
// may use temporary fresh names provided the returned metadata does not escape.
template <class Fresh>
inline std::optional<CallableLifetimeSignature> makeCallableLifetimeSignature(
    const SemanticType &signature, const CallableNominalFormation &nominalFormation,
    Fresh fresh) {
  std::vector<Lifetime> occupied;
  std::size_t work = 0;
  const auto inventory = [&](auto &&self, const SemanticType &type) -> bool {
    if (++work > 4096)
      return false;
    for (const auto id : type.quantifiedLifetimes)
      occupied.push_back(Lifetime::parameter(id));
    if (type.referenceLifetime.specified())
      occupied.push_back(type.referenceLifetime);
    occupied.insert(occupied.end(), type.lifetimeArguments.begin(), type.lifetimeArguments.end());
    bool valid = true;
    lifetime_detail::children(type, [&](const auto &child) { valid = valid && self(self, child); });
    return valid;
  };
  if (!inventory(inventory, signature))
    return std::nullopt;
  CallableLifetimeSignature result;
  std::vector<LifetimeBinding> lift;
  for (const auto region : freeTypeLifetimes(signature)) {
    if (region == Lifetime::staticLifetime())
      continue;
    const auto universal = fresh();
    if (!universal.declarationRegion() || universal.kind != Lifetime::Kind::Parameter ||
        std::find(occupied.begin(), occupied.end(), universal) != occupied.end())
      return std::nullopt;
    occupied.push_back(universal);
    lift.emplace_back(region, universal);
    result.signatureBindings.emplace_back(universal, region);
  }
  const auto universal = substituteTypeLifetimes(signature, lift);
  const auto contract = universal ? callableLifetimeContract(*universal, nominalFormation)
                                  : std::nullopt;
  if (!contract)
    return std::nullopt;
  result.signature = *universal;
  result.contract = *contract;
  return result;
}

// Every free slot is fixed by the carrier, even if two fixed regions happen
// to be equal. Quantified slots must never be supplied by this substitution.
inline bool callableLifetimeCarrierMatches(
    const CallableLifetimeSignature &callable, const SemanticType &carrier,
    const CallableNominalFormation &nominalFormation = {}) {
  const auto derived = callableLifetimeContract(callable.signature, nominalFormation);
  if (!derived || *derived != callable.contract)
    return false;
  const auto free = freeTypeLifetimes(callable.signature);
  std::vector<Lifetime> fixed;
  for (const auto region : free)
    if (region.kind == Lifetime::Kind::Parameter)
      fixed.push_back(region);
  if (fixed.size() != callable.signatureBindings.size())
    return false;
  for (std::size_t index = 0; index < callable.signatureBindings.size(); ++index) {
    const auto binding = callable.signatureBindings[index];
    if (std::find(fixed.begin(), fixed.end(), binding.first) == fixed.end() ||
        !binding.second.specified())
      return false;
    for (std::size_t previous = 0; previous < index; ++previous)
      if (callable.signatureBindings[previous].first == binding.first)
        return false;
  }
  const auto reconstructed =
      substituteTypeLifetimes(callable.signature, callable.signatureBindings);
  return reconstructed && equivalentLifetimeTypes(*reconstructed, carrier);
}

// Match all typed positions, deriving only substitutions for the target's
// universal names. Every other fact (nominal identity, access, value argument,
// nested quantifier scope) is compared exactly. This is a signature proof,
// not representation erasure or an ordinary conversion.
inline std::optional<std::vector<LifetimeBinding>> callableLifetimeTargetBindings(
    const SemanticType &targetResult, std::span<const SemanticType> targetParameters,
    const LifetimeSignatureContract &targetContract,
    const SemanticType &requestedSignature) {
  if (!targetContract.valid() ||
      !isLifetimeInvocationSignature(requestedSignature) ||
      requestedSignature.arguments.size() != targetParameters.size() + 1)
    return std::nullopt;
  std::vector<LifetimeBinding> bindings;
  const auto regionMatches = [&](Lifetime target, Lifetime requested,
                                  std::span<const LifetimeBinding> scoped) {
    for (const auto binding : scoped)
      if (binding.first == target)
        return binding.second == requested;
    if (std::find(targetContract.parameters.begin(), targetContract.parameters.end(), target) ==
        targetContract.parameters.end())
      return target == requested;
    if (!requested.declarationRegion())
      return false;
    for (const auto binding : bindings)
      if (binding.first == target)
        return binding.second == requested;
    bindings.emplace_back(target, requested);
    return true;
  };
  const auto match = [&](auto &&self, const SemanticType &target,
                         const SemanticType &requested,
                         std::vector<LifetimeBinding> scoped) -> bool {
    if (target.quantifiedLifetimes.size() != requested.quantifiedLifetimes.size() ||
        target.lifetimeArguments.size() != requested.lifetimeArguments.size())
      return false;
    for (std::size_t index = 0; index < target.quantifiedLifetimes.size(); ++index)
      scoped.emplace_back(Lifetime::parameter(target.quantifiedLifetimes[index]),
                          Lifetime::parameter(requested.quantifiedLifetimes[index]));
    if (!regionMatches(target.referenceLifetime, requested.referenceLifetime, scoped))
      return false;
    for (std::size_t index = 0; index < target.lifetimeArguments.size(); ++index)
      if (!regionMatches(target.lifetimeArguments[index], requested.lifetimeArguments[index], scoped))
        return false;
    auto left = target;
    auto right = requested;
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
    const auto childrenMatch = [&](const auto &a, const auto &b) {
      if (a.size() != b.size())
        return false;
      for (std::size_t index = 0; index < a.size(); ++index)
        if (!self(self, a[index], b[index], scoped))
          return false;
      return true;
    };
    return childrenMatch(target.arguments, requested.arguments) &&
           childrenMatch(target.lambdaEnclosingClassTypes, requested.lambdaEnclosingClassTypes) &&
           childrenMatch(target.lambdaEnclosingFunctionTypes, requested.lambdaEnclosingFunctionTypes);
  };
  if (!match(match, targetResult, requestedSignature.arguments.front(), {}))
    return std::nullopt;
  for (std::size_t index = 0; index < targetParameters.size(); ++index)
    if (!match(match, targetParameters[index], requestedSignature.arguments[index + 1], {}))
      return std::nullopt;
  for (const auto parameter : targetContract.parameters)
    if (std::none_of(bindings.begin(), bindings.end(),
                     [&](const auto binding) { return binding.first == parameter; }))
      bindings.emplace_back(parameter, Lifetime::staticLifetime());
  return bindings;
}

} // namespace lang
