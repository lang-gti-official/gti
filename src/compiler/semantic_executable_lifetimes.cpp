#include "semantic_analyzer_impl.h"
#include "gti/lifetime_types.h"
#include "gti/generic_type_patterns.h"
#include "gti/callable_lifetimes.h"

namespace lang {
namespace {
void appendExecutableRegionShape(const SemanticType &type,
                                 std::vector<std::size_t> &result,
                                 std::vector<GenericParameterId> bound = {}) {
  result.push_back(type.quantifiedLifetimes.size());
  bound.insert(bound.end(), type.quantifiedLifetimes.begin(), type.quantifiedLifetimes.end());
  const auto region = [&](Lifetime value) {
    if (!value.specified()) {
      result.push_back(0);
      return;
    }
    const auto selected = value.kind == Lifetime::Kind::Parameter
        ? std::find(bound.begin(), bound.end(), value.id) : bound.end();
    result.push_back(selected == bound.end() ? 1 : 2 + std::size_t(selected - bound.begin()));
  };
  region(type.referenceLifetime);
  result.push_back(type.lifetimeArguments.size());
  for (const auto value : type.lifetimeArguments)
    region(value);
  lifetime_detail::children(type, [&](const auto &child) { appendExecutableRegionShape(child, result, bound); });
}

bool hasFreeExecutableLifetime(const SemanticExecutableInstantiation &instance,
                               Lifetime region) {
  const auto typeUses = [&](const SemanticType &type) {
    // Failed scoped substitution is unknown, never evidence that the captured
    // region disappeared. A finite typed walk must prove this role resolved.
    std::size_t work = 0;
    const auto unknown = [&](auto &&self, const SemanticType &value, std::size_t depth) -> bool {
      if (++work > 4096 || depth >= 256 || value == SemanticType::Unknown)
        return true;
      bool unresolved = false;
      lifetime_detail::children(value, [&](const auto &child) {
        unresolved = unresolved || self(self, child, depth + 1);
      });
      return unresolved;
    };
    if (unknown(unknown, type, 0))
      return true;
    const auto free = freeTypeLifetimes(type);
    return std::find(free.begin(), free.end(), region) != free.end();
  };
  const auto typesUse = [&](const auto &types) {
    return std::any_of(types.begin(), types.end(), typeUses);
  };
  const auto boundUses = [&](LifetimeBound bound) {
    return bound.longer == region || bound.shorter == region;
  };
  const auto typeBoundUses = [&](const LifetimeTypeBound &bound) {
    return bound.region == region || typeUses(bound.type);
  };
  const auto &contract = instance.lifetimeContract;
  if (typeUses(instance.ownerType) || typeUses(instance.returnType) ||
      typesUse(instance.classTypes) || typesUse(instance.types) ||
      typesUse(instance.parameterTypes) || contract.receiver == region ||
      std::any_of(contract.bounds.begin(), contract.bounds.end(), boundUses) ||
      std::any_of(contract.typeBounds.begin(), contract.typeBounds.end(), typeBoundUses) ||
      std::any_of(contract.requirements.nodes.begin(), contract.requirements.nodes.end(),
                  [&](const auto &node) {
                    return boundUses(node.bound) ||
                        (node.kind == LifetimeRequirementNode::Kind::Type && typeUses(node.type));
                  }))
    return true;
  for (const auto &requirement : instance.requirements)
    if (typesUse(requirement.arguments) ||
        std::find(requirement.lifetimeArguments.begin(), requirement.lifetimeArguments.end(), region) !=
            requirement.lifetimeArguments.end() ||
        (requirement.lifetimeBound && boundUses(*requirement.lifetimeBound)) ||
        (requirement.outlivesType && typeUses(*requirement.outlivesType)))
      return true;
  const auto &application = instance.lifetimeApplication;
  return std::any_of(application.bindings.begin(), application.bindings.end(),
                    [&](const auto &binding) { return binding.first == region || binding.second == region; }) ||
      std::find(application.variables.begin(), application.variables.end(), region) != application.variables.end() ||
      std::any_of(application.obligations.begin(), application.obligations.end(), boundUses) ||
      std::any_of(application.typeObligations.begin(), application.typeObligations.end(), typeBoundUses) ||
      std::any_of(application.origins.begin(), application.origins.end(),
                  [&](const auto &origin) { return origin.region == region; }) ||
      application.callableSignature.has_value();
}
}

auto SemanticVisitor::Impl::importMemberCallableScopes(
    GenericSubstitution &substitution, InstanceAnalysisKind kind,
    std::size_t declaration, const SemanticType &owner, const Token &site,
    bool lambdaInvocation) const -> bool {
  if (!substitution.valid)
    return false;
  if (substitution.callableScopes.empty())
    return true;
  RequestSessionContext::ExecutableLifetimeKey key{
      .runtime = {.kind = kind, .declaration = declaration,
                  .classTypes = owner.arguments, .classValues = owner.valueArguments},
      .regionShape = {lambdaInvocation ? 1U : 0U}};
  for (auto &type : key.runtime.classTypes) {
    appendExecutableRegionShape(type, key.regionShape);
    type = eraseTypeLifetimes(std::move(type));
  }
  auto &registry = *requestSession.executableLifetimes;
  auto [entry, inserted] = registry.memberScopes.try_emplace(std::move(key));
  auto &descriptors = entry->second.parameters;
  constexpr GenericParameterId highHalf = std::numeric_limits<GenericParameterId>::max() / 2 + 1;
  std::size_t ordinal = 0;
  for (auto &scope : substitution.callableScopes) {
    const auto previousImports = std::move(scope.imports);
    scope.imports.clear();
    for (const auto parameter : scope.signature.quantifiedLifetimes) {
      if (ordinal == descriptors.size()) {
        if (!inserted || nextGenericParameterId >= highHalf || registry.nextId < highHalf ||
            semanticModel.findGenericParameter(registry.nextId)) {
          substitution.valid = false;
          return false;
        }
        auto name = site;
        name.lexeme = "_invocation" + std::to_string(ordinal + 1);
        name.generated = true;
        descriptors.push_back({.id = registry.nextId--, .name = std::move(name),
                               .lifetime = true, .implicit = true});
      }
      const auto &descriptor = descriptors[ordinal++];
      semanticModel.recordGenericParameter(descriptor);
      scope.imports.emplace(parameter, Lifetime::parameter(descriptor.id));
    }
    for (const auto &[from, to] : scope.regionBindings) {
      if (from.kind != Lifetime::Kind::Parameter)
        continue;
      const auto imported = to.kind == Lifetime::Kind::Parameter ? scope.imports.find(to.id)
                                                               : scope.imports.end();
      const auto value = imported == scope.imports.end() ? to : imported->second;
      const auto [existing, fresh] = substitution.lifetimes.emplace(from.id, value);
      if (!fresh && existing->second != value) {
        const auto previous = to.kind == Lifetime::Kind::Parameter ? previousImports.find(to.id)
                                                                 : previousImports.end();
        if (!lambdaInvocation || previous == previousImports.end() || existing->second != previous->second) {
          substitution.valid = false;
          return false;
        }
        existing->second = value;
      }
    }
  }
  substitution.valid = ordinal == descriptors.size();
  return substitution.valid;
}

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

  if (ownerId && classInfo(ownerId).exactSpecializationPrimary) {
    const auto &owner = classInfo(ownerId);
    const auto actual = canonicalExactSpecializationType(SemanticType::classType(
        owner.exactSpecializationPrimary, key.classTypes, key.classValues));
    const auto expected = canonicalExactSpecializationType(SemanticType::classType(
        owner.exactSpecializationPrimary, owner.exactTypeArguments,
        owner.exactValueArguments));
    // Exact selection is checked before lifting. Region spelling cannot
    // select a declaration, but the complete runtime argument shape must.
    if (eraseTypeLifetimes(actual) != eraseTypeLifetimes(expected))
      return std::nullopt;
  }

  const ClassInfo *patternOwner = ownerId ? &publicClassInfo(ownerId) : nullptr;
  const std::span<const SemanticType> ownerPatterns = patternOwner
      ? std::span<const SemanticType>(patternOwner->genericArgumentPatterns)
      : std::span<const SemanticType>{};
  if (!ownerPatterns.empty() && !deriveGenericArgumentPatternSubstitution(
          ownerPatterns, key.classTypes,
          genericParameterIdentities(patternOwner->genericParameters),
          genericParameterIdentities(patternOwner->genericArgumentPatternLifetimes),
          {}, GenericPatternMatch::Structure))
    return std::nullopt;
  const auto fixedPatternRegion = [&](Lifetime region) {
    return region.kind == Lifetime::Kind::Static ||
        (patternOwner && region.kind == Lifetime::Kind::Parameter &&
         std::any_of(patternOwner->genericParameters.begin(),
                     patternOwner->genericParameters.end(), [&](const auto &parameter) {
           return parameter.lifetime && parameter.id == region.id;
         }));
  };

  RequestSessionContext::ExecutableLifetimeKey identity{.runtime = key};
  for (auto &type : identity.runtime.classTypes) {
    appendExecutableRegionShape(type, identity.regionShape);
    type = eraseTypeLifetimes(std::move(type));
  }
  for (auto &type : identity.runtime.types) {
    appendExecutableRegionShape(type, identity.regionShape);
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
  bool patternValid = true;
  const auto lift = [&](auto &&self, SemanticType &type,
                        std::vector<LifetimeBinding> bound,
                        std::vector<GenericParameterId> &free,
                        const SemanticType *pattern) -> void {
    if (type.kind == SemanticType::FunctionItem) {
      const auto *target = semanticModel.findFunction(type.functionId);
      if (!target && instanceBaseModel)
        target = instanceBaseModel->findFunction(type.functionId);
      const auto declared = target
          ? functionItemLifetimeType(target->id, target->returnType,
                                     target->parameterTypes, target->lifetimeContract)
          : std::nullopt;
      if (!declared || !equivalentLifetimeTypes(*declared, type)) {
        valid = false;
        return;
      }
      // Item identity fixes its namespace signature. Only the declaration's
      // quantified invocation slots are renamed below; static is not an
      // independently inferred environment parameter of the enclosing call.
      bound.emplace_back(Lifetime::staticLifetime(), Lifetime::staticLifetime());
    }
    // A projected type is an opaque argument: equal actual regions inside it
    // do not establish a declaration-level relationship with the owner.
    if (pattern && (pattern->kind == SemanticType::TypeParameter ||
                    pattern->kind == SemanticType::TypePack))
      pattern = nullptr;
    if (type.hasLambdaShape()) {
      const auto *lambda = semanticModel.findLambda(type.lambdaId);
      if (!lambda && instanceBaseModel)
        lambda = instanceBaseModel->findLambda(type.lambdaId);
      const auto environment = lambda && lambda->type.hasLambdaShape()
          ? matchUniversalTypeLifetimes(lambda->type, type, lambda->environmentLifetimes)
          : std::nullopt;
      if (!environment || environment->size() != lambda->environmentLifetimes.size()) {
        valid = false;
        return;
      }
      std::vector<LifetimeBinding> liftedEnvironment;
      for (const auto parameter : lambda->environmentLifetimes) {
        const auto actual = std::find_if(environment->begin(), environment->end(),
            [&](const auto binding) { return binding.first == parameter; });
        if (actual == environment->end()) {
          valid = false;
          return;
        }
        const auto enclosing = std::find_if(bound.rbegin(), bound.rend(),
            [&](const auto binding) { return binding.first == actual->second; });
        Lifetime universal;
        if (enclosing != bound.rend()) {
          universal = enclosing->second;
        } else {
          universal = Lifetime::parameter(fresh());
          result.lifetimeApplication.bindings.emplace_back(universal, actual->second);
          free.push_back(universal.id);
        }
        liftedEnvironment.emplace_back(parameter, universal);
        // The declaration recipe repeats this same environment slot in its
        // signature and capture fields. Preserve that declared correlation,
        // independently of equality between separate actual capture regions.
        bound.emplace_back(universal, universal);
      }
      const auto canonical = substituteTypeLifetimes(lambda->type, liftedEnvironment);
      if (!canonical) {
        valid = false;
        return;
      }
      type = *canonical;
    }
    for (auto &id : type.quantifiedLifetimes) {
      const auto source = Lifetime::parameter(id);
      id = fresh();
      bound.emplace_back(source, Lifetime::parameter(id));
    }
    const auto region = [&](Lifetime &value, Lifetime declared) {
      if (fixedPatternRegion(declared)) {
        // The pattern itself fixes this occurrence to a nominal parameter or
        // static storage. Preserve that identity instead of lifting it into an
        // independent executable slot. An invocation binder cannot supply it.
        if (!value.specified() ||
            std::any_of(bound.begin(), bound.end(), [&](const auto binding) {
              return binding.first == value;
            })) {
          patternValid = false;
          return;
        }
        if (declared.kind == Lifetime::Kind::Static) {
          patternValid = patternValid && value == declared;
        } else {
          auto &bindings = result.lifetimeApplication.bindings;
          const auto previous = std::find_if(bindings.begin(), bindings.end(),
              [&](const auto binding) { return binding.first == declared; });
          if (previous == bindings.end())
            bindings.emplace_back(declared, value);
          else if (previous->second != value)
            patternValid = false;
        }
        value = declared;
        return;
      }
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
    region(type.referenceLifetime, pattern ? pattern->referenceLifetime : Lifetime{});
    for (std::size_t index = 0; index < type.lifetimeArguments.size(); ++index)
      region(type.lifetimeArguments[index],
             pattern ? pattern->lifetimeArguments[index] : Lifetime{});
    const auto children = [&](auto &values, const auto *patterns) {
      for (std::size_t index = 0; index < values.size(); ++index)
        self(self, values[index], bound, free,
             patterns && index < patterns->size() ? &(*patterns)[index] : nullptr);
    };
    children(type.arguments, pattern ? &pattern->arguments : nullptr);
    children(type.lambdaEnclosingClassTypes,
             pattern ? &pattern->lambdaEnclosingClassTypes : nullptr);
    children(type.lambdaEnclosingFunctionTypes,
             pattern ? &pattern->lambdaEnclosingFunctionTypes : nullptr);
  };
  for (std::size_t index = 0; index < result.classTypes.size(); ++index) {
    result.classLifetimeParameters.emplace_back();
    lift(lift, result.classTypes[index], {}, result.classLifetimeParameters.back(),
         ownerPatterns.empty() ? nullptr : &ownerPatterns[index]);
  }
  for (auto &type : result.types) {
    result.functionLifetimeParameters.emplace_back();
    lift(lift, type, {}, result.functionLifetimeParameters.back(), nullptr);
  }
  if (!patternValid)
    return std::nullopt;
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
    result.ownerType = SemanticType::classType(ownerId, result.classTypes, key.classValues);
    for (const auto &parameter : classInfo(ownerId).genericParameters)
      if (parameter.lifetime)
        result.ownerType.lifetimeArguments.push_back(Lifetime::parameter(parameter.id));
    substitution = classSubstitution(result.ownerType);
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
  if (!substitution.valid)
    return std::nullopt;
  // Structural default construction has no member invocation signature or
  // source body. Its nested callable scopes stay closed, just as in layout;
  // opening them would invent construction arguments for invocation binders.
  const bool memberInvocation = function || destructor ||
      (constructor && !requestsStructuralDefaultConstruction(*constructor));
  if (ownerId && memberInvocation) {
    if (!importMemberCallableScopes(substitution, key.kind, key.declaration, result.ownerType, site))
      return std::nullopt;
    for (const auto &scope : substitution.callableScopes)
      for (const auto parameter : scope.signature.quantifiedLifetimes) {
        const auto imported = scope.imports.at(parameter);
        if (std::find(result.lifetimeContract.parameters.begin(), result.lifetimeContract.parameters.end(), imported) ==
            result.lifetimeContract.parameters.end())
          result.lifetimeContract.parameters.push_back(imported);
        const auto *descriptor = semanticModel.findGenericParameter(imported.id);
        if (!descriptor)
          return std::nullopt;
        result.liftedLifetimes.push_back(*descriptor);
      }
  }
  result.callableScopes = substitution.callableScopes;
  result.returnType = substituteType(result.returnType, substitution);
  for (auto &parameter : result.parameterTypes)
    parameter = substituteType(parameter, substitution);
  for (auto &requirement : result.requirements)
    requirement = substituteRequirement(requirement, substitution);
  for (auto &bound : result.lifetimeContract.typeBounds)
    bound.type = substituteType(bound.type, substitution);
  for (auto &node : result.lifetimeContract.requirements.nodes)
    node.type = substituteType(node.type, substitution);
  std::vector<LifetimeBinding> projectedRegions;
  if (ownerId)
    for (const auto &parameter : publicClassInfo(ownerId).genericArgumentPatternLifetimes) {
      const auto selected = substitution.lifetimes.find(parameter.id);
      if (selected == substitution.lifetimes.end()) {
        const bool scoped = std::any_of(substitution.callableScopes.begin(), substitution.callableScopes.end(),
            [&](const auto &scope) {
              return std::any_of(scope.regionBindings.begin(), scope.regionBindings.end(),
                  [&](const auto &binding) { return binding.first == Lifetime::parameter(parameter.id); });
            });
        if (scoped && !memberInvocation) {
          if (constructor) {
            const auto region = Lifetime::parameter(parameter.id);
            // A structural default keeps the actual callable scope closed.
            // Its declaration may still list this projected name as a formal,
            // but it is not a construction invocation parameter. Remove only
            // that unused formal; a free use or existing application fact
            // requires proof and must never be silently erased.
            if (hasFreeExecutableLifetime(result, region))
              return std::nullopt;
            auto &parameters = result.lifetimeContract.parameters;
            parameters.erase(std::remove(parameters.begin(), parameters.end(), region), parameters.end());
          }
          continue;
        }
        return std::nullopt;
      }
      projectedRegions.emplace_back(Lifetime::parameter(parameter.id), selected->second);
    }
  const auto projectedContract = substituteLifetimeContract(
      std::move(result.lifetimeContract), projectedRegions);
  if (!projectedContract)
    return std::nullopt;
  result.lifetimeContract = *projectedContract;
  for (const auto &binding : result.lifetimeApplication.bindings)
    if (std::find(result.lifetimeContract.parameters.begin(),
                  result.lifetimeContract.parameters.end(), binding.first) ==
        result.lifetimeContract.parameters.end())
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
  if (function && function->declaration)
    addScopedCallableResultBound(*function->declaration, result.returnType,
                                result.parameterTypes, substitution,
                                result.lifetimeContract);
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
