#include "semantic_analyzer_impl.h"
#include "gti/lifetime_types.h"

namespace lang {

auto SemanticVisitor::Impl::canonicalizeExecutableInstance(
    const InstanceAnalysisKey &key)
    -> std::optional<SemanticExecutableInstantiation> {
  const FunctionInfo *function = nullptr;
  std::optional<FunctionInfo> declarationFunction;
  const ConstructorInfo *constructor = nullptr;
  const DestructorInfo *destructor = nullptr;
  ClassId ownerId = 0;
  EnumId enumId = 0;
  Token site;
  if (key.kind == InstanceAnalysisKind::Function) {
    function = semanticModel.findFunction(key.declaration);
    if (!function && instanceBaseModel)
      function = instanceBaseModel->findFunction(key.declaration);
    if (!function || !function->declaration)
      return std::nullopt;
    if (const auto *declared = catalog.callable(*function->declaration)) {
      declarationFunction = projectCallableDeclaration(*declared);
      function = &*declarationFunction;
    }
    ownerId = function->ownerClass;
    site = function->declaration->name();
  } else if (key.kind == InstanceAnalysisKind::Constructor) {
    for (const auto &owner : classes) {
      if (owner.designatedConstructor && owner.designatedConstructor->id == key.declaration)
        constructor = &*owner.designatedConstructor;
      if (owner.reversedDesignatedConstructor && owner.reversedDesignatedConstructor->id == key.declaration)
        constructor = &*owner.reversedDesignatedConstructor;
      if (owner.copyConstructor && owner.copyConstructor->id == key.declaration)
        constructor = &*owner.copyConstructor;
      if (owner.moveConstructor && owner.moveConstructor->id == key.declaration)
        constructor = &*owner.moveConstructor;
      for (const auto &candidate : owner.constructors)
        if (candidate.id == key.declaration)
          constructor = &candidate;
      if (constructor)
        break;
    }
    if (!constructor || !constructor->owner)
      return std::nullopt;
    ownerId = constructor->owner;
    site = constructor->declaration ? constructor->declaration->name()
                                    : classInfo(ownerId).name;
  } else if (key.kind == InstanceAnalysisKind::EnumLayout) {
    enumId = key.declaration;
    if (!enumId || enumId > enums.size())
      return std::nullopt;
    site = enums[enumId - 1].name;
  } else {
    ownerId = key.declaration;
    if (!ownerId || ownerId > classes.size())
      return std::nullopt;
    const auto &owner = classInfo(ownerId);
    site = owner.name;
    if (key.kind == InstanceAnalysisKind::Destructor && owner.destructor)
      destructor = &*owner.destructor;
  }

  RequestSessionContext::ExecutableLifetimeKey identity{.runtime = key};
  const auto shape = [&](auto &&self, const SemanticType &type,
                         std::vector<GenericParameterId> bound) -> void {
    identity.regionShape.push_back(type.quantifiedLifetimes.size());
    bound.insert(bound.end(), type.quantifiedLifetimes.begin(), type.quantifiedLifetimes.end());
    const auto region = [&](Lifetime value) {
      if (!value.specified()) {
        identity.regionShape.push_back(0);
        return;
      }
      const auto selected = value.kind == Lifetime::Kind::Parameter
          ? std::find(bound.begin(), bound.end(), value.id) : bound.end();
      identity.regionShape.push_back(selected == bound.end()
          ? 1 : 2 + std::size_t(selected - bound.begin()));
    };
    region(type.referenceLifetime);
    identity.regionShape.push_back(type.lifetimeArguments.size());
    for (auto value : type.lifetimeArguments)
      region(value);
    lifetime_detail::children(type, [&](const auto &child) { self(self, child, bound); });
  };
  for (auto &type : identity.runtime.classTypes) {
    shape(shape, type, {});
    type = eraseTypeLifetimes(std::move(type));
  }
  for (auto &type : identity.runtime.types) {
    shape(shape, type, {});
    type = eraseTypeLifetimes(std::move(type));
  }
  auto &registry = *requestSession.executableLifetimes;
  auto [entry, inserted] = registry.identities.try_emplace(identity);
  auto &descriptors = entry->second.parameters;
  constexpr GenericParameterId highHalf =
      std::numeric_limits<GenericParameterId>::max() / 2 + 1;
  bool valid = true;
  std::size_t ordinal = 0;
  const auto fresh = [&]() -> GenericParameterId {
    if (ordinal < descriptors.size())
      return descriptors[ordinal++].id;
    if (!inserted || nextGenericParameterId >= highHalf ||
        registry.nextId < highHalf ||
        semanticModel.findGenericParameter(registry.nextId)) {
      valid = false;
      return 0;
    }
    Token name = site;
    name.lexeme = "_instance" + std::to_string(ordinal + 1);
    name.generated = true;
    descriptors.push_back({.id = registry.nextId--,
                           .name = std::move(name), .lifetime = true,
                           .implicit = true});
    return descriptors[ordinal++].id;
  };

  SemanticExecutableInstantiation result{.classTypes = key.classTypes,
                                         .types = key.types};
  const auto lift = [&](auto &&self, SemanticType &type,
                        std::vector<LifetimeBinding> bound,
                        std::vector<GenericParameterId> &free) -> void {
    for (auto &id : type.quantifiedLifetimes) {
      const auto source = Lifetime::parameter(id);
      id = fresh();
      bound.emplace_back(source, Lifetime::parameter(id));
    }
    const auto region = [&](Lifetime &value) {
      if (!value.specified())
        return;
      const auto selected = std::find_if(bound.rbegin(), bound.rend(),
          [&](auto binding) { return binding.first == value; });
      if (selected != bound.rend()) {
        value = selected->second;
        return;
      }
      const auto universal = Lifetime::parameter(fresh());
      result.lifetimeApplication.bindings.emplace_back(universal, value);
      value = universal;
      free.push_back(universal.id);
    };
    region(type.referenceLifetime);
    for (auto &value : type.lifetimeArguments)
      region(value);
    lifetime_detail::children(type, [&](auto &child) { self(self, child, bound, free); });
  };
  for (auto &type : result.classTypes) {
    result.classLifetimeParameters.emplace_back();
    lift(lift, type, {}, result.classLifetimeParameters.back());
  }
  for (auto &type : result.types) {
    result.functionLifetimeParameters.emplace_back();
    lift(lift, type, {}, result.functionLifetimeParameters.back());
  }
  if (!valid || ordinal != descriptors.size()) {
    report(site, "Too many distinct executable lifetime parameters in this compilation.",
           "GTI-S2126");
    return std::nullopt;
  }
  result.liftedLifetimes = descriptors;
  for (const auto &parameter : descriptors)
    semanticModel.recordGenericParameter(parameter);

  GenericSubstitution substitution;
  if (ownerId) {
    substitution = nominalSubstitution(classInfo(ownerId).genericParameters,
        {.types = result.classTypes, .values = key.classValues});
    result.ownerType = SemanticType::classType(ownerId, result.classTypes, key.classValues);
    for (const auto &parameter : classInfo(ownerId).genericParameters)
      if (parameter.lifetime)
        result.ownerType.lifetimeArguments.push_back(Lifetime::parameter(parameter.id));
  } else if (enumId)
    substitution = nominalSubstitution(enums[enumId - 1].genericParameters,
        {.types = result.classTypes, .values = key.classValues});
  if (function || constructor) {
    const auto &parameters = function ? function->genericParameters : constructor->genericParameters;
    auto local = nominalSubstitution(parameters, {.types = result.types, .values = key.values});
    substitution.types.insert(local.types.begin(), local.types.end());
    substitution.values.insert(local.values.begin(), local.values.end());
    result.parameterTypes = function ? function->parameterTypes : constructor->parameterTypes;
    result.requirements = function ? function->requirements : constructor->requirements;
    result.lifetimeContract = function ? function->lifetimeContract : constructor->lifetimeContract;
    if (constructor && !constructor->declaration && !result.lifetimeContract.complete)
      result.lifetimeContract = classInfo(ownerId).lifetimeContract;
    if (function)
      result.returnType = function->returnType;
  } else if (destructor) {
    result.lifetimeContract = destructor->lifetimeContract;
  } else if (ownerId) {
    result.lifetimeContract = classInfo(ownerId).lifetimeContract;
  } else if (enumId) {
    result.lifetimeContract = enums[enumId - 1].lifetimeContract;
  }
  result.returnType = substituteType(result.returnType, substitution);
  for (auto &parameter : result.parameterTypes)
    parameter = substituteType(parameter, substitution);
  for (auto &requirement : result.requirements)
    requirement = substituteRequirement(requirement, substitution);
  for (auto &bound : result.lifetimeContract.typeBounds)
    bound.type = substituteType(bound.type, substitution);
  for (auto &node : result.lifetimeContract.requirements.nodes)
    node.type = substituteType(node.type, substitution);
  for (const auto &binding : result.lifetimeApplication.bindings)
    result.lifetimeContract.parameters.push_back(binding.first);

  // Reuse the authoritative WF collector after type substitution. Generic
  // arguments themselves also carry formation obligations even when an
  // individual callable parameter does not mention that argument.
  auto inputs = result.parameterTypes;
  inputs.insert(inputs.end(), result.classTypes.begin(), result.classTypes.end());
  inputs.insert(inputs.end(), result.types.begin(), result.types.end());
  auto normalized = normalizeLifetimeSignature(result.returnType, inputs,
      std::move(result.lifetimeContract), result.requirements, [] { return Lifetime{}; });
  result.lifetimeContract = std::move(normalized.contract);
  if (!result.lifetimeContract.valid())
    return std::nullopt;
  return result;
}

auto SemanticVisitor::Impl::universalizeFunctionCandidate(
    CallableCandidate &candidate, std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments,
    std::span<const GenericParameterId> inferredTypeParameters) -> bool {
  if (!candidate.id)
    return true; // An open protocol application has no executable body yet.
  const auto canonical = canonicalizeExecutableInstance(
      {.kind = InstanceAnalysisKind::Function, .declaration = candidate.id,
       .classTypes = candidate.dispatchOwner.arguments,
       .classValues = candidate.dispatchOwner.valueArguments,
       .types = typeArguments, .values = valueArguments});
  if (!canonical)
    return false;
  std::vector<GenericParameterId> inferred;
  std::size_t index = 0;
  for (const auto &parameter : candidate.record().genericParameters) {
    if (parameter.value || parameter.lifetime)
      continue;
    const bool deduced = std::find(inferredTypeParameters.begin(),
        inferredTypeParameters.end(), parameter.id) != inferredTypeParameters.end();
    const auto count = parameter.pack ? canonical->types.size() - index : 1;
    for (std::size_t element = 0; element < count && index < canonical->types.size();
         ++element, ++index)
      if (deduced) {
        const auto &regions = canonical->functionLifetimeParameters[index];
        inferred.insert(inferred.end(), regions.begin(), regions.end());
      }
  }
  for (const auto binding : canonical->lifetimeApplication.bindings)
    if (std::find(inferred.begin(), inferred.end(), binding.first.id) == inferred.end())
      candidate.lifetimeApplication.bindings.push_back(binding);
  candidate.returnType = canonical->returnType;
  std::vector<SemanticType> expanded;
  for (const auto &parameter : canonical->parameterTypes)
    SemanticType::appendExpandedArgument(expanded, parameter);
  std::vector<SemanticType> grouped;
  std::size_t position = 0;
  for (const auto &parameter : candidate.parameterTypes) {
    if (parameter.kind == SemanticType::TypePack && parameter.concretePack) {
      std::vector<SemanticType> prior;
      SemanticType::appendExpandedArgument(prior, parameter);
      if (position + prior.size() > expanded.size())
        return false;
      grouped.push_back(SemanticType::concreteTypePack(parameter.genericParameterId,
          std::vector<SemanticType>(expanded.begin() + position,
                                    expanded.begin() + position + prior.size())));
      position += prior.size();
    } else {
      if (position == expanded.size())
        return false;
      grouped.push_back(expanded[position++]);
    }
  }
  if (position != expanded.size())
    return false;
  candidate.parameterTypes = std::move(grouped);
  candidate.requirements = canonical->requirements;
  candidate.lifetimeContract = canonical->lifetimeContract;
  if (candidate.dispatchOwner.kind == SemanticType::Class)
    candidate.dispatchOwner.arguments = canonical->classTypes;
  typeArguments = canonical->types;
  applyFunctionReturnBorrowSummary(candidate);
  return true;
}

} // namespace lang
