#pragma once

#include "gti/lifetime_types.h"
#include "gti/lifetime_requirements.h"

#include <functional>

namespace lang {

// The phase supplies the exact declaration contract instantiated for this
// one nominal application. Child types are visited by the shared derivation.
using CallableNominalFormation = std::function<
    std::optional<LifetimeRequirementFormula>(const SemanticType &)>;

// Fixed closure premises are established at formation. They may mention only
// the closure's environment slots (and static storage), never a local region
// or an invocation binder. The same predicate guards source and IR metadata.
inline bool validLambdaEnvironmentContract(
    const LifetimeSignatureContract &contract,
    std::span<const Lifetime> environment) {
  if (!contract.valid() || contract.receiver.specified() ||
      contract.parameters.size() != environment.size() ||
      !std::equal(contract.parameters.begin(), contract.parameters.end(),
                  environment.begin()))
    return false;
  std::size_t work = 0;
  const auto scopedType = [&](auto &&self, const SemanticType &type,
                               std::vector<GenericParameterId> bound,
                               std::size_t depth) -> bool {
    if (++work > 4096 || depth > 256)
      return false;
    if (!type.quantifiedLifetimes.empty() &&
        type.kind != SemanticType::FunctionSignature && type.kind != SemanticType::NativeFunction &&
        type.kind != SemanticType::FunctionItem && type.kind != SemanticType::Lambda)
      return false;
    for (const auto id : type.quantifiedLifetimes) {
      if (!id || std::find(bound.begin(), bound.end(), id) != bound.end() ||
          std::find(environment.begin(), environment.end(), Lifetime::parameter(id)) != environment.end())
        return false;
      bound.push_back(id);
    }
    bool valid = true;
    lifetime_detail::children(type, [&](const auto &child) {
      valid = self(self, child, bound, depth + 1) && valid;
    });
    return valid;
  };
  const auto scoped = [&](const SemanticType &type) {
    if (!scopedType(scopedType, type, {}, 0))
      return false;
    for (const auto region : freeTypeLifetimes(type))
      if (region != Lifetime::staticLifetime() &&
          std::find(environment.begin(), environment.end(), region) == environment.end())
        return false;
    return true;
  };
  for (const auto &bound : contract.typeBounds)
    if (!scoped(bound.type))
      return false;
  for (const auto &node : contract.requirements.nodes)
    if (node.kind == LifetimeRequirementNode::Kind::Type && !scoped(node.type))
      return false;
  return true;
}

// Forget premises whose regions belong to a different invocation scope. This
// is monotone Boolean projection: forgetting an atom replaces it with true,
// so forgetting either arm of an OR never grants the other arm. Whole fixed
// All/Any subexpressions retain their original meaning and source order.
inline std::optional<LifetimeSignatureContract> projectLambdaEnvironmentContract(
    const LifetimeSignatureContract &enclosing,
    std::span<const LifetimeBinding> fixed,
    std::span<const Lifetime> environment) {
  if (!enclosing.valid())
    return std::nullopt;
  for (std::size_t index = 0; index < fixed.size(); ++index) {
    if (fixed[index].first.kind != Lifetime::Kind::Parameter ||
        fixed[index].second.kind != Lifetime::Kind::Parameter)
      return std::nullopt;
    for (std::size_t previous = 0; previous < index; ++previous)
      if (fixed[previous].first == fixed[index].first || fixed[previous].second == fixed[index].second)
        return std::nullopt;
  }
  LifetimeSignatureContract result{.parameters = {environment.begin(), environment.end()},
      .origin = LifetimeContractOrigin::Declared, .complete = true};
  const auto region = [&](Lifetime value) -> std::optional<Lifetime> {
    if (value == Lifetime::staticLifetime())
      return value;
    for (const auto binding : fixed)
      if (binding.first == value &&
          std::find(environment.begin(), environment.end(), binding.second) != environment.end())
        return binding.second;
    return std::nullopt;
  };
  const auto type = [&](const SemanticType &value) -> std::optional<SemanticType> {
    for (const auto free : freeTypeLifetimes(value))
      if (!region(free))
        return std::nullopt;
    return substituteTypeLifetimes(value, fixed);
  };
  for (const auto bound : enclosing.bounds) {
    const auto longer = region(bound.longer), shorter = region(bound.shorter);
    if (longer && shorter)
      result.bounds.push_back({*longer, *shorter});
  }
  for (const auto &bound : enclosing.typeBounds) {
    const auto target = region(bound.region);
    const auto value = type(bound.type);
    if (target && value)
      result.typeBounds.push_back({*value, *target});
  }
  using Node = LifetimeRequirementNode;
  using Kind = Node::Kind;
  auto &nodes = result.requirements.nodes;
  bool valid = true;
  std::size_t work = 0;
  const auto project = [&](auto &&self, std::size_t index, std::size_t depth) -> std::size_t {
    if (++work > 4096 || depth > 256 || index >= enclosing.requirements.nodes.size()) {
      valid = false;
      return Node::invalidIndex;
    }
    auto node = enclosing.requirements.nodes[index];
    if (node.kind == Kind::All || node.kind == Kind::Any) {
      const auto start = nodes.size();
      const auto left = self(self, node.left, depth + 1);
      const auto right = self(self, node.right, depth + 1);
      if (node.kind == Kind::Any &&
          (left == Node::invalidIndex || right == Node::invalidIndex)) {
        nodes.resize(start);
        return Node::invalidIndex;
      }
      if (left == Node::invalidIndex)
        return right;
      if (right == Node::invalidIndex)
        return left;
      node.left = left;
      node.right = right;
    } else if (node.kind == Kind::Region) {
      const auto longer = region(node.bound.longer), shorter = region(node.bound.shorter);
      if (!longer || !shorter)
        return Node::invalidIndex;
      node.bound = {*longer, *shorter};
    } else if (node.kind == Kind::Type) {
      const auto target = region(node.bound.shorter);
      const auto value = type(node.type);
      if (!target || !value)
        return Node::invalidIndex;
      node.type = *value;
      node.bound.shorter = *target;
    }
    nodes.push_back(std::move(node));
    return nodes.size() - 1;
  };
  if (!enclosing.requirements.nodes.empty())
    result.requirements.root = project(project, enclosing.requirements.root, 0);
  if (!valid || !validLambdaEnvironmentContract(result, environment))
    return std::nullopt;
  return result;
}

// Body assumptions are the conjunction of independently checked formation
// premises and invocation requirements. Keep complete Boolean subexpressions;
// a disjunctive environment never grants both of its alternatives.
inline std::optional<LifetimeSignatureContract> composeLambdaLifetimeContract(
    const LifetimeSignatureContract &environment,
    const LifetimeSignatureContract &invocation) {
  if (!validLambdaEnvironmentContract(environment, environment.parameters) ||
      !invocation.valid())
    return std::nullopt;
  for (const auto region : environment.parameters)
    if (std::find(invocation.parameters.begin(), invocation.parameters.end(), region) ==
        invocation.parameters.end())
      return std::nullopt;
  auto result = invocation;
  result.bounds = environment.bounds;
  result.typeBounds = environment.typeBounds;
  const auto append = [](auto &destination, const auto &source) {
    for (const auto &value : source)
      if (std::find(destination.begin(), destination.end(), value) == destination.end())
        destination.push_back(value);
  };
  append(result.bounds, invocation.bounds);
  append(result.typeBounds, invocation.typeBounds);
  result.requirements = environment.requirements;
  if (!appendLifetimeRequirementFormula(result.requirements, invocation.requirements) ||
      !result.valid())
    return std::nullopt;
  return result;
}

// A namespace function item has no captured lifetime environment. Its exact
// declaration supplies invocation binders; static storage remains a fixed
// signature choice. This builds the scoped type, not a new target contract.
inline std::optional<SemanticType> functionItemLifetimeType(
    FunctionId function, const SemanticType &result,
    std::span<const SemanticType> parameters,
    const LifetimeSignatureContract &contract) {
  if (!function || result == SemanticType::Unknown || !contract.valid() ||
      contract.receiver.specified())
    return std::nullopt;
  auto item = SemanticType::functionItemType(
      function, result, std::vector<SemanticType>(parameters.begin(), parameters.end()));
  for (const auto parameter : contract.parameters) {
    if (parameter.kind != Lifetime::Kind::Parameter || !parameter.id ||
        std::find(item.quantifiedLifetimes.begin(), item.quantifiedLifetimes.end(),
                  parameter.id) != item.quantifiedLifetimes.end())
      return std::nullopt;
    item.quantifiedLifetimes.push_back(parameter.id);
  }
  for (const auto region : freeTypeLifetimes(item))
    if (region.kind != Lifetime::Kind::Static)
      return std::nullopt;
  return item;
}

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
    if (!type.quantifiedLifetimes.empty() && !isLifetimeInvocationSignature(type) &&
        type.kind != SemanticType::Lambda)
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
    if (type.kind == SemanticType::Lambda) {
      const auto formation = nominalFormation ? nominalFormation(type) : std::nullopt;
      // Closure invocation binders stay closed. Only the checked fixed
      // environment is a premise of the enclosing callable's type.
      return formation && appendLifetimeRequirementFormula(contract.requirements, *formation);
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

// Checked erasure preserves an object's physical storage region and access.
// Restoration additionally requires validity of the exact requested referent
// for that region: an opaque input does not imply this premise. Phase adapters
// independently verify the intrinsic identity and its trusted opaque side.
inline std::optional<LifetimeSignatureContract>
referenceErasureLifetimeContract(
    const SemanticType &signature, bool restoring,
    const CallableNominalFormation &nominalFormation = {}) {
  if (signature.kind != SemanticType::FunctionSignature ||
      signature.signatureReceiver != ReceiverMutability::ReadOnly ||
      signature.arguments.size() != 2)
    return std::nullopt;
  const auto &result = signature.arguments[0];
  const auto &input = signature.arguments[1];
  if (result.kind != SemanticType::Reference || result.arguments.size() != 1 ||
      input.kind != SemanticType::Reference || input.arguments.size() != 1 ||
      result.referenceAccess != input.referenceAccess ||
      result.referenceLifetime != input.referenceLifetime ||
      result.referenceLifetime.kind != Lifetime::Kind::Parameter ||
      std::find(signature.quantifiedLifetimes.begin(), signature.quantifiedLifetimes.end(),
                result.referenceLifetime.id) == signature.quantifiedLifetimes.end())
    return std::nullopt;
  auto contract = callableLifetimeContract(signature, nominalFormation);
  if (!contract || !restoring)
    return contract;
  const LifetimeTypeBound validity{result.arguments.front(), result.referenceLifetime};
  if (std::find(contract->typeBounds.begin(), contract->typeBounds.end(), validity) ==
      contract->typeBounds.end())
    contract->typeBounds.push_back(validity);
  LifetimeRequirementFormula required;
  required.nodes.push_back({.kind = LifetimeRequirementNode::Kind::Type,
                            .bound = {{}, validity.region}, .type = validity.type});
  required.root = 0;
  if (!appendLifetimeRequirementFormula(contract->requirements, required))
    return std::nullopt;
  return contract;
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

inline std::optional<LifetimeRequirementFormula> lambdaEnvironmentLifetimeRequirements(
    const SemanticType &canonical, const SemanticType &actual,
    const LifetimeSignatureContract &environment) {
  if (!canonical.hasLambdaShape() || !actual.hasLambdaShape() ||
      !validLambdaEnvironmentContract(environment, canonical.lifetimeArguments))
    return std::nullopt;
  const auto bindings = matchUniversalTypeLifetimes(
      canonical, actual, environment.parameters);
  auto formula = lifetimeContractRequirements(environment);
  if (!bindings || !formula)
    return std::nullopt;
  const auto region = [&](Lifetime value) {
    for (const auto binding : *bindings)
      if (binding.first == value)
        return binding.second;
    return value;
  };
  for (auto &node : formula->nodes) {
    const auto type = substituteTypeLifetimes(node.type, *bindings);
    if (!type)
      return std::nullopt;
    node.type = *type;
    node.bound = {region(node.bound.longer), region(node.bound.shorter)};
  }
  return formula;
}

struct LambdaCallableLifetimeTarget {
  std::vector<LifetimeBinding> bindings;
  LifetimeSignatureContract environment;
};

// Reconcile fixed closure slots with a requested callable's fixed carrier
// slots before proving invocation implication. A fixed slot cannot stand for
// a fresh invocation quantifier, even if its current actual value coincides.
inline std::optional<LambdaCallableLifetimeTarget> lambdaCallableLifetimeTarget(
    const SemanticType &canonical, const SemanticType &actual,
    const LifetimeSignatureContract &invocation,
    const LifetimeSignatureContract &environment,
    const CallableLifetimeSignature &requested) {
  if (!canonical.hasLambdaShape() || !actual.hasLambdaShape() ||
      !validLambdaEnvironmentContract(environment, canonical.lifetimeArguments) ||
      !composeLambdaLifetimeContract(environment, invocation))
    return std::nullopt;
  const auto fixed = matchUniversalTypeLifetimes(canonical, actual, environment.parameters);
  auto bindings = callableLifetimeTargetBindings(*canonical.lambdaReturnType(),
      canonical.lambdaParameterTypes(), invocation, requested.signature);
  if (!fixed || !bindings)
    return std::nullopt;
  std::vector<Lifetime> mentioned = freeTypeLifetimes(*canonical.lambdaReturnType());
  for (const auto &parameter : canonical.lambdaParameterTypes()) {
    const auto regions = freeTypeLifetimes(parameter);
    mentioned.insert(mentioned.end(), regions.begin(), regions.end());
  }
  const auto requestedActual = [&](Lifetime value) {
    for (const auto binding : requested.signatureBindings)
      if (binding.first == value)
        return binding.second;
    return value;
  };
  for (const auto fixedBinding : *fixed) {
    auto mapped = std::find_if(bindings->begin(), bindings->end(), [&](const auto binding) {
      return binding.first == fixedBinding.first;
    });
    if (mapped == bindings->end())
      return std::nullopt;
    if (std::find(mentioned.begin(), mentioned.end(), fixedBinding.first) == mentioned.end()) {
      // An unused fixed slot remains a private proof name. It is never
      // replaced by the default static choice used for unused universals.
      if (std::find(requested.contract.parameters.begin(), requested.contract.parameters.end(),
                    fixedBinding.first) != requested.contract.parameters.end())
        return std::nullopt;
      mapped->second = fixedBinding.first;
    } else {
      if (mapped->second.kind == Lifetime::Kind::Parameter &&
          std::find(requested.signature.quantifiedLifetimes.begin(),
                    requested.signature.quantifiedLifetimes.end(), mapped->second.id) !=
              requested.signature.quantifiedLifetimes.end())
        return std::nullopt;
      if (requestedActual(mapped->second) != fixedBinding.second)
        return std::nullopt;
    }
  }
  auto mappedEnvironment = substituteLifetimeContract(environment, *bindings);
  if (!mappedEnvironment ||
      !validLambdaEnvironmentContract(*mappedEnvironment, mappedEnvironment->parameters))
    return std::nullopt;
  return LambdaCallableLifetimeTarget{std::move(*bindings), std::move(*mappedEnvironment)};
}

} // namespace lang
