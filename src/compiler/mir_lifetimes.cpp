#include "mir_lifetimes.h"
#include "mir_verification_queries.h"
#include "gti/lifetime_types.h"
#include "gti/lifetime_requirements.h"
#include "gti/callable_lifetimes.h"
#include "gti/nominal_variance.h"

namespace lang {
namespace {
bool scopedDeclarationType(const SemanticType &type,
                           std::span<const Lifetime> declaration,
                           std::vector<Lifetime> quantified,
                           std::size_t depth);
} // namespace
MirLifetimeApplication
lowerMirLifetimeApplication(const HirLifetimeApplication &source) {
  MirLifetimeApplication result{.bindings = source.bindings,
                                .variables = source.variables,
                                .obligations = source.obligations,
                                .typeObligations = source.typeObligations,
                                .callableSignature = source.callableSignature,
                                .complete = source.complete};
  for (const auto &origin : source.origins)
    result.origins.push_back({.region = origin.region,
                              .key = origin.place,
                              .source = origin.source});
  return result;
}

std::optional<MirLambdaLifetimeView> mirLambdaLifetimeView(
    const MirLambdaInstance &instance, const SemanticType &type) {
  if (type.kind != SemanticType::Lambda ||
      instance.type.kind != SemanticType::Lambda ||
      type.lambdaId != instance.declaration ||
      instance.type.lambdaId != instance.declaration ||
      instance.type.lifetimeArguments != instance.environmentLifetimes ||
      type.lifetimeArguments.size() != instance.environmentLifetimes.size() ||
      !instance.type.hasLambdaShape() ||
      instance.type.lambdaReturnType() == nullptr ||
      *instance.type.lambdaReturnType() != instance.returnType ||
      !std::equal(instance.type.lambdaParameterTypes().begin(),
                  instance.type.lambdaParameterTypes().end(),
                  instance.parameterTypes.begin(), instance.parameterTypes.end()) ||
      !std::equal(instance.type.lambdaCaptureTypes().begin(),
                  instance.type.lambdaCaptureTypes().end(),
                  instance.captureTypes.begin(), instance.captureTypes.end()))
    return std::nullopt;
  for (const auto region : instance.environmentLifetimes)
    if (region.kind != Lifetime::Kind::Parameter ||
        std::find(instance.lifetimeContract.parameters.begin(),
                  instance.lifetimeContract.parameters.end(), region) ==
            instance.lifetimeContract.parameters.end())
      return std::nullopt;
  const auto bindings = matchUniversalTypeLifetimes(
      instance.type, type, instance.environmentLifetimes);
  return bindings ? std::optional(MirLambdaLifetimeView{&instance, *bindings})
                  : std::nullopt;
}

std::optional<std::vector<SemanticType>> mirLambdaCaptureTypes(
    const MirProgram &program, const SemanticType &type) {
  std::optional<std::vector<SemanticType>> result;
  for (const auto &instance : program.lambdaInstances()) {
    const auto view = mirLambdaLifetimeView(instance, type);
    if (!view) continue;
    // Matching already validates every repeated environment occurrence and
    // scoped invocation binder. Keep the caller's alpha-renamed nested types.
    std::vector<SemanticType> captures(type.lambdaCaptureTypes().begin(),
                                       type.lambdaCaptureTypes().end());
    if (result && *result != captures) return std::nullopt;
    result = std::move(captures);
  }
  return result;
}

std::optional<MirClassLifetimeView> mirClassLifetimeView(
    const MirProgram &program, const SemanticType &type) {
  if (type.kind != SemanticType::Class)
    return std::nullopt;
  std::optional<MirClassLifetimeView> selected;
  for (const auto &instance : program.classInstances()) {
    if (instance.declaration != type.classId)
      continue;
    const auto bindings = matchUniversalTypeLifetimes(
        instance.type, type, instance.lifetimeContract.parameters);
    if (!bindings)
      continue;
    if (selected)
      return std::nullopt;
    selected = MirClassLifetimeView{&instance, *bindings};
  }
  return selected;
}

std::optional<MirEnumLifetimeView> mirEnumLifetimeView(
    const MirProgram &program, const SemanticType &type) {
  if (type.kind != SemanticType::Enum)
    return std::nullopt;
  std::optional<MirEnumLifetimeView> selected;
  const auto select = [&](const auto &instance, bool payload) {
    if (instance.declaration != type.enumId)
      return true;
    const auto bindings = matchUniversalTypeLifetimes(
        instance.type, type, instance.lifetimeContract.parameters);
    if (!bindings)
      return true;
    if (selected)
      return false;
    MirEnumLifetimeView view;
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(instance)>, MirPayloadEnum>)
      view.payload = &instance;
    else
      view.integral = &instance;
    (void)payload;
    view.bindings = *bindings;
    selected = std::move(view);
    return true;
  };
  for (const auto &instance : program.integralEnums())
    if (!select(instance, false))
      return std::nullopt;
  for (const auto &instance : program.payloadEnums())
    if (!select(instance, true))
      return std::nullopt;
  return selected;
}

MirVerificationResult verifyMirNominalLifetimeRecipes(const MirProgram &program) {
  MirVerificationResult result;
  const auto reject = [&](std::string message) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module, .message = std::move(message)});
  };
  const auto &recipes = program.nominalVarianceRecipes();
  const auto inferred = inferNominalVarianceRecipes(recipes);
  if (!inferred) {
    reject("MIR nominal variance recipe graph is malformed or incomplete");
    return result;
  }
  const auto findRecipe = [&](SemanticType::Kind kind, std::uint64_t id)
      -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < recipes.size(); ++index)
      if (recipes[index].kind == kind && recipes[index].declaration == id)
        return index;
    return std::nullopt;
  };
  const auto selectorType = [&](auto &&self, SemanticType type, std::size_t depth)
      -> std::optional<SemanticType> {
    if (depth >= 256)
      return std::nullopt;
    if (type.kind == SemanticType::Class)
      if (const auto index = findRecipe(type.kind, type.classId);
          index && recipes[*index].exactSpecializationPrimary) {
        const auto &selected = recipes[*index];
        type.classId = selected.exactSpecializationPrimary;
        if (type.arguments.empty() && type.valueArguments.empty()) {
          type.arguments = selected.exactTypeArguments;
          type.valueArguments = selected.exactValueArguments;
        }
      }
    bool valid = true;
    lifetime_detail::children(type, [&](SemanticType &child) {
      const auto canonical = self(self, child, depth + 1);
      if (!canonical)
        valid = false;
      else
        child = *canonical;
    });
    // Exact declaration selection is region-independent. This erased copy is
    // only a selector key; the selected instance/storage proof retains and
    // verifies the original type's complete regions and obligations.
    return valid ? std::optional{eraseTypeLifetimes(std::move(type))} : std::nullopt;
  };
  const auto selectorMatches = [&](const NominalVarianceRecipe &recipe,
                                    const SemanticType &type) {
    if (recipe.exactTypeArguments.size() != type.arguments.size() ||
        recipe.exactValueArguments != type.valueArguments)
      return false;
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      const auto pattern = selectorType(selectorType, recipe.exactTypeArguments[index], 0);
      const auto actual = selectorType(selectorType, type.arguments[index], 0);
      if (!pattern || !actual || *pattern != *actual)
        return false;
    }
    return true;
  };
  const auto selectedType = [&](auto &&self, SemanticType type) -> std::optional<SemanticType> {
    bool valid = true;
    lifetime_detail::children(type, [&](SemanticType &child) {
      auto selected = self(self, child);
      if (selected)
        child = std::move(*selected);
      else
        valid = false;
    });
    if (!valid)
      return std::nullopt;
    if (type.kind != SemanticType::Class)
      return type;
    const NominalVarianceRecipe *selected = nullptr;
    for (const auto &recipe : recipes)
      if (recipe.exactSpecializationPrimary == type.classId &&
          selectorMatches(recipe, type)) {
        if (selected)
          return std::nullopt;
        selected = &recipe;
      }
    if (selected)
      type.classId = selected->declaration;
    return type;
  };
  const auto substitution = [&](const NominalVarianceRecipe &recipe, const SemanticType &type)
      -> std::optional<GenericSubstitution> {
    GenericSubstitution values;
    std::size_t types = 0, constants = 0, regions = 0;
    // A pattern family can bind only projected regions (for example
    // Fixed<int&<source>>), with no ordinary type parameter to mark it here.
    // Its complete argument shape is checked by the pattern matcher below.
    bool projected = !recipe.argumentPatterns.empty();
    const auto parameters = nominalVarianceParameters(recipe, recipes);
    if (!parameters)
      return std::nullopt;
    for (const auto &parameter : *parameters) {
      if (parameter.lifetime) {
        if (regions >= type.lifetimeArguments.size())
          return std::nullopt;
        values.lifetimes.emplace(parameter.id, type.lifetimeArguments[regions++]);
      } else if (parameter.value) {
        if (constants >= type.valueArguments.size())
          return std::nullopt;
        values.values.emplace(parameter.id, type.valueArguments[constants++]);
      } else if (!parameter.typeArgumentPath.empty()) {
        const auto argument = projectGenericTypeArgument(type.arguments, parameter.typeArgumentPath,
                                                         parameter.id, parameter.pack);
        if (!argument)
          return std::nullopt;
        values.types.emplace(parameter.id, *argument);
        projected = true;
      } else if (parameter.pack) {
        if (types > type.arguments.size())
          return std::nullopt;
        std::vector<SemanticType> tail(type.arguments.begin() + types, type.arguments.end());
        values.types.emplace(parameter.id, SemanticType::concreteTypePack(parameter.id, std::move(tail)));
        types = type.arguments.size();
      } else {
        if (types >= type.arguments.size())
          return std::nullopt;
        values.types.emplace(parameter.id, type.arguments[types++]);
      }
    }
    if (regions != type.lifetimeArguments.size() ||
        constants != type.valueArguments.size() ||
        (!projected && types != type.arguments.size()))
      return std::nullopt;
    if (recipe.exactSpecializationPrimary && !selectorMatches(recipe, type))
      return std::nullopt;
    if (!recipe.argumentPatterns.empty()) {
      std::vector<LifetimeBinding> knownRegions;
      for (const auto &[id, actual] : values.lifetimes)
        knownRegions.emplace_back(Lifetime::parameter(id), actual);
      std::vector<SemanticType> patterns;
      for (const auto &pattern : recipe.argumentPatterns) {
        const auto replaced = substituteTypeLifetimes(
            substituteSemanticType(pattern, values), knownRegions);
        const auto selected = replaced ? selectedType(selectedType, *replaced)
                                       : std::nullopt;
        if (!selected)
          return std::nullopt;
        SemanticType::appendExpandedArgument(patterns, *selected);
      }
      std::vector<Lifetime> projectedRegions;
      for (const auto &parameter : recipe.argumentPatternLifetimes)
        projectedRegions.push_back(Lifetime::parameter(parameter.id));
      const auto bindings = matchUniversalTypeArgumentLifetimes(
          patterns, type.arguments, projectedRegions);
      if (!bindings || bindings->size() != projectedRegions.size())
        return std::nullopt;
      for (const auto &[parameter, actual] : *bindings)
        if (!values.lifetimes.emplace(parameter.id, actual).second)
          return std::nullopt;
    }
    return values;
  };
  const auto instantiate = [&](const SemanticType &type, const GenericSubstitution &values)
      -> std::optional<SemanticType> {
    auto result = substituteSemanticType(type, values);
    std::vector<LifetimeBinding> regions;
    for (const auto &[id, value] : values.lifetimes)
      regions.emplace_back(Lifetime::parameter(id), value);
    auto replaced = substituteTypeLifetimes(std::move(result), regions);
    return replaced ? selectedType(selectedType, std::move(*replaced)) : std::nullopt;
  };
  const auto liftsValid = [](std::span<const GenericParameterIdentity> lifted,
                            const LifetimeSignatureContract &contract,
                            const NominalVarianceRecipe &recipe,
                            const SemanticType &type) {
    if (!scopedDeclarationType(type, contract.parameters, {}, 0))
      return false;
    std::vector<GenericParameterId> quantified;
    const auto collectQuantified = [&](auto &&self, const SemanticType &current) -> void {
      quantified.insert(quantified.end(), current.quantifiedLifetimes.begin(),
                        current.quantifiedLifetimes.end());
      lifetime_detail::children(current, [&](const SemanticType &child) {
        self(self, child);
      });
    };
    collectQuantified(collectQuantified, type);
    std::vector<GenericParameterId> seen;
    for (const auto &parameter : lifted) {
      if (!parameter.id || !parameter.lifetime || !parameter.implicit || parameter.value ||
          parameter.pack || !parameter.typeArgumentPath.empty() ||
          std::find(seen.begin(), seen.end(), parameter.id) != seen.end() ||
          std::any_of(recipe.parameters.begin(), recipe.parameters.end(), [&](const auto &source) {
            return source.id == parameter.id;
          }) || std::any_of(recipe.argumentPatternLifetimes.begin(),
                           recipe.argumentPatternLifetimes.end(), [&](const auto &source) {
            return source.id == parameter.id;
          }))
        return false;
      // Canonicalization records fresh identities for both free argument
      // regions and nested callable binders. The latter remain scoped by the
      // exact type and must never become nominal declaration assumptions.
      const bool declared = std::find(contract.parameters.begin(), contract.parameters.end(),
                                      Lifetime::parameter(parameter.id)) != contract.parameters.end();
      const bool bound = std::find(quantified.begin(), quantified.end(), parameter.id) != quantified.end();
      if (!declared && !bound)
        return false;
      seen.push_back(parameter.id);
    }
    return true;
  };
  const auto required = [](const auto &instance) {
    return instance.variance.complete || !instance.type.lifetimeArguments.empty() ||
           !instance.liftedLifetimes.empty();
  };
  for (const auto &instance : program.classInstances()) {
    if (!required(instance))
      continue; // Legacy region-free MIR carries no variance authority.
    const auto index = findRecipe(SemanticType::Class, instance.declaration);
    if (!index) {
      reject("MIR class variance lacks its exact declaration storage recipe");
      continue;
    }
    const auto &recipe = recipes[*index];
    const auto values = substitution(recipe, instance.type);
    bool valid = values.has_value() &&
                 instance.compilerCapability == recipe.compilerCapability &&
                 instance.kind == recipe.classKind && instance.cOpaqueHandle == recipe.cOpaqueHandle &&
                 instance.polymorphic == recipe.polymorphic && !recipe.forward && !recipe.invalidLifetime &&
                 instance.variance == expandNominalVarianceRecipe(instance.type, recipe, (*inferred)[*index], recipes) &&
                 liftsValid(instance.liftedLifetimes, instance.lifetimeContract, recipe, instance.type);
    std::size_t fields = 0, bases = 0;
    for (const auto &stored : recipe.stored) {
      if (!values) break;
      const auto type = instantiate(stored.type, *values);
      if (stored.kind == NominalStoredTypeKind::Field) {
        valid = type && stored.index == fields && fields < instance.declaredFields.size() &&
                stored.symbol != 0 && stored.symbol == instance.declaredFields[fields].symbol &&
                equivalentLifetimeTypes(*type, instance.declaredFields[fields].type) && valid;
        ++fields;
      } else if (stored.kind == NominalStoredTypeKind::Base) {
        valid = type && stored.index == bases && bases < instance.structuralBases.size() &&
                stored.symbol == 0 && stored.interface == instance.structuralBases[bases].interface &&
                equivalentLifetimeTypes(*type, instance.structuralBases[bases].type) && valid;
        ++bases;
      } else {
        valid = false;
      }
    }
    valid = fields == instance.declaredFields.size() && bases == instance.structuralBases.size() && valid;
    if (!valid)
      reject("MIR class variance or concrete storage disagrees with its symbolic declaration recipe");
  }
  const auto verifyEnum = [&](const auto &instance) {
    if (!required(instance))
      return;
    const auto index = findRecipe(SemanticType::Enum, instance.declaration);
    if (!index) {
      reject("MIR enum variance lacks its exact declaration payload recipe");
      return;
    }
    const auto &recipe = recipes[*index];
    const auto values = substitution(recipe, instance.type);
    bool valid = values.has_value() && !recipe.invalidLifetime &&
                 instance.variance == expandNominalVarianceRecipe(instance.type, recipe, (*inferred)[*index], recipes) &&
                 liftsValid(instance.liftedLifetimes, instance.lifetimeContract, recipe, instance.type);
    std::size_t count = 0;
    if constexpr (std::is_same_v<std::remove_cvref_t<decltype(instance)>, MirPayloadEnum>) {
      for (const auto &variant : instance.variants)
        count += variant.payloadTypes.size();
      for (const auto &stored : recipe.stored) {
        if (!values) break;
        const auto type = instantiate(stored.type, *values);
        const auto variant = std::find_if(instance.variants.begin(), instance.variants.end(),
            [&](const auto &candidate) { return candidate.variantIndex == stored.variant; });
        valid = type && stored.kind == NominalStoredTypeKind::Payload && stored.symbol &&
                variant != instance.variants.end() && stored.index < variant->payloadTypes.size() &&
                stored.index < variant->payloadSymbols.size() &&
                stored.symbol == variant->payloadSymbols[stored.index] &&
                equivalentLifetimeTypes(*type, variant->payloadTypes[stored.index]) && valid;
      }
    }
    valid = count == recipe.stored.size() && valid;
    if (!valid)
      reject("MIR enum variance or payload storage disagrees with its symbolic declaration recipe");
  };
  for (const auto &instance : program.integralEnums())
    verifyEnum(instance);
  for (const auto &instance : program.payloadEnums())
    verifyEnum(instance);
  const auto checkOwner = [&](const auto &instance) {
    if (!checkedMirCallableOwnerType(program, instance.owner, instance.ownerType,
                                     instance.lifetimeContract))
      reject("MIR callable owner type does not match its universal scope and exact layout");
  };
  for (const auto &instance : program.functionInstances())
    checkOwner(instance);
  for (const auto &instance : program.constructorInstances())
    checkOwner(instance);
  for (const auto &instance : program.destructorInstances())
    checkOwner(instance);
  return result;
}

namespace {
std::optional<LifetimeRequirementFormula>
mirNominalFormationRequirements(const MirProgram &program, const SemanticType &type);
} // namespace

const CallableLifetimeSignature *checkedMirCallableLifetimeSignature(
    const MirProgram &program, const MirInstruction &instruction) {
  const bool nativeForming = instruction.kind == MirInstructionKind::Compute &&
                            instruction.operation == MirOperation::NativeCallback;
  const bool nativeInvoke = instruction.kind == MirInstructionKind::Call &&
                            instruction.nativeCalleeType.has_value();
  const bool forming = nativeForming || instruction.intrinsic == IntrinsicKind::FunctionPointerFromItem;
  if ((!forming && !nativeInvoke && instruction.intrinsic != IntrinsicKind::FunctionPointerInvoke) ||
      !instruction.lifetimeApplication.callableSignature)
    return nullptr;
  const SemanticType *signature = nullptr;
  if (nativeForming || nativeInvoke) {
    signature = nativeForming ? &instruction.info.type : &*instruction.nativeCalleeType;
    if (!signature->hasNativeFunctionShape())
      return nullptr;
  } else {
    const SemanticType *carrier = forming ? &instruction.info.type : nullptr;
    if (!forming && !instruction.parameterTypes.empty()) {
      const auto &parameter = instruction.parameterTypes.front();
      if (parameter.kind == SemanticType::Reference && parameter.arguments.size() == 1 &&
          parameter.referenceAccess == AccessMode::ReadOnly &&
          parameter.referenceLifetime == Lifetime{})
        carrier = &parameter.arguments.front();
    }
    if (!carrier || carrier->kind != SemanticType::Class || carrier->arguments.size() != 1)
      return nullptr;
    const auto owner = mirClassLifetimeView(program, *carrier);
    if (!owner || owner->instance->compilerCapability != CompilerCapabilityTypeKind::FunctionPointer)
      return nullptr;
    signature = &carrier->arguments.front();
  }
  const auto &callable = *instruction.lifetimeApplication.callableSignature;
  if (!callableLifetimeCarrierMatches(callable, *signature,
          [&](const SemanticType &type) { return mirNominalFormationRequirements(program, type); }))
    return nullptr;
  if (!forming) {
    const auto &bindings = instruction.lifetimeApplication.bindings;
    for (const auto fixed : callable.signatureBindings)
      if (std::find(bindings.begin(), bindings.end(), fixed) == bindings.end())
        return nullptr;
    for (const auto id : callable.signature.quantifiedLifetimes) {
      const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto binding) {
        return binding.first == Lifetime::parameter(id);
      });
      if (found == bindings.end() || found->second.kind != Lifetime::Kind::Inference)
        return nullptr;
    }
  }
  return &callable;
}

namespace {
template <typename Facts>
bool resolveOrigins(Facts &application, const MirBody &body) {
  for (auto &origin : application.origins) {
    const MirPlace *selected = nullptr;
    if (!origin.key.valid() && origin.source == 0)
      return false;
    for (const auto &place : body.places) {
      const bool matches =
          origin.key.valid() ? place.key && *place.key == origin.key
                             : place.sourceValue == origin.source &&
                                   place.projections.empty() &&
                                   (place.root == MirPlaceRootKind::Temporary ||
                                    place.root == MirPlaceRootKind::Value);
      if (!matches)
        continue;
      // Repeated aliases of a named exact key designate the same source. A
      // temporary requires a unique materialized storage identity instead.
      if (selected && !origin.key.valid() &&
          (selected->root != place.root ||
           selected->temporary != place.temporary ||
           selected->value != place.value || selected->loan != place.loan))
        return false;
      if (!selected || place.id < selected->id)
        selected = &place;
    }
    if (!selected || selected->id == 0 ||
        (origin.place != 0 && origin.place != selected->id))
      return false;
    origin.place = selected->id;
  }
  return true;
}

template <typename HirFacts, typename MirFacts>
bool preservesFacts(const HirFacts &source, const MirFacts &lowered) {
  if (source.variables != lowered.variables ||
      source.obligations != lowered.obligations ||
      source.typeObligations != lowered.typeObligations ||
      source.complete != lowered.complete ||
      source.origins.size() != lowered.origins.size())
    return false;
  for (std::size_t index = 0; index < source.origins.size(); ++index) {
    const auto &a = source.origins[index];
    const auto &b = lowered.origins[index];
    if (a.region != b.region || a.place != b.key || a.source != b.source ||
        b.place == 0)
      return false;
  }
  return true;
}
} // namespace

bool resolveMirLifetimeOrigins(MirLifetimeApplication &application,
                               const MirBody &body) {
  return resolveOrigins(application, body);
}

bool preservesHirLifetimeApplication(const HirLifetimeApplication &source,
                                     const MirLifetimeApplication &lowered) {
  return source.bindings == lowered.bindings &&
         source.callableSignature == lowered.callableSignature &&
         preservesFacts(source, lowered);
}

MirLifetimeBody lowerMirLifetimeBody(const HirLifetimeBody &source) {
  MirLifetimeBody result{.variables = source.variables,
                         .obligations = source.obligations,
                         .typeObligations = source.typeObligations,
                         .complete = source.complete};
  for (const auto &origin : source.origins)
    result.origins.push_back({.region = origin.region,
                              .key = origin.place,
                              .source = origin.source});
  return result;
}

bool resolveMirLifetimeOrigins(MirLifetimeBody &facts, const MirBody &body) {
  return resolveOrigins(facts, body);
}

bool preservesHirLifetimeBody(const HirLifetimeBody &source,
                              const MirLifetimeBody &lowered) {
  return preservesFacts(source, lowered);
}
namespace {
bool containsLifetimeMetadata(const SemanticType &type) {
  if (type.referenceLifetime != Lifetime{} || !type.lifetimeArguments.empty() ||
      !type.quantifiedLifetimes.empty())
    return true;
  bool found = false;
  lifetime_detail::children(type, [&](const SemanticType &child) {
    found = found || containsLifetimeMetadata(child);
  });
  return found;
}

bool scopedDeclarationType(const SemanticType &type,
                           std::span<const Lifetime> declaration,
                           std::vector<Lifetime> quantified = {},
                           std::size_t depth = 0) {
  if (depth >= 256 ||
      (!type.quantifiedLifetimes.empty() &&
       type.kind != SemanticType::FunctionSignature &&
       type.kind != SemanticType::NativeFunction &&
       type.kind != SemanticType::FunctionItem &&
       type.kind != SemanticType::Lambda) ||
      (type.referenceLifetime != Lifetime{} &&
       type.kind != SemanticType::Reference))
    return false;
  for (const auto id : type.quantifiedLifetimes) {
    const auto region = Lifetime::parameter(id);
    if (!region.specified() ||
        std::find(declaration.begin(), declaration.end(), region) !=
            declaration.end() ||
        std::find(quantified.begin(), quantified.end(), region) !=
            quantified.end())
      return false;
    quantified.push_back(region);
  }
  const auto scoped = [&](Lifetime region) {
    return region == Lifetime::staticLifetime() ||
           (region.kind == Lifetime::Kind::Parameter && region.specified() &&
            (std::find(declaration.begin(), declaration.end(), region) !=
                 declaration.end() ||
             std::find(quantified.begin(), quantified.end(), region) !=
                 quantified.end()));
  };
  if (type.referenceLifetime != Lifetime{} && !scoped(type.referenceLifetime))
    return false;
  for (const auto region : type.lifetimeArguments)
    if (!scoped(region))
      return false;
  bool valid = true;
  lifetime_detail::children(type, [&](const SemanticType &child) {
    valid = scopedDeclarationType(child, declaration, quantified, depth + 1) &&
            valid;
  });
  return valid;
}

bool scopedLifetimeContract(const LifetimeSignatureContract &contract) {
  if (contract == LifetimeSignatureContract{})
    return true;
  if (!contract.valid())
    return false;
  for (const auto &bound : contract.typeBounds)
    if (!scopedDeclarationType(bound.type, contract.parameters))
      return false;
  for (const auto &node : contract.requirements.nodes)
    if (node.kind == LifetimeRequirementNode::Kind::Type &&
        !scopedDeclarationType(node.type, contract.parameters))
      return false;
  return true;
}

std::optional<LifetimeRequirementFormula>
mirNominalFormationRequirements(const MirProgram &program, const SemanticType &type) {
  const LifetimeSignatureContract *declared = nullptr;
  std::vector<LifetimeBinding> bindings;
  if (type.kind == SemanticType::Class) {
    const auto view = mirClassLifetimeView(program, type);
    if (!view)
      return std::nullopt;
    declared = &view->instance->lifetimeContract;
    bindings = view->bindings;
  } else if (type.kind == SemanticType::Enum) {
    const auto view = mirEnumLifetimeView(program, type);
    if (!view)
      return std::nullopt;
    declared = view->payload ? &view->payload->lifetimeContract : &view->integral->lifetimeContract;
    bindings = view->bindings;
  } else {
    return std::nullopt;
  }
  // Explicit nominal requirements are sealed declaration authority, preserved
  // from HIR. The exact view instantiates that contract; a storage walk alone
  // cannot recover explicit requirements on phantom parameters. The caller
  // separately proves every resulting obligation under its scoped signature.
  if (!declared->valid() || !scopedLifetimeContract(*declared) ||
      !declared->requirements.resolved())
    return std::nullopt;
  const auto actual = substituteLifetimeContract(*declared, bindings);
  if (!actual || !actual->valid() || !scopedLifetimeContract(*actual))
    return std::nullopt;
  return lifetimeContractRequirements(*actual);
}

template <typename Verify>
MirVerificationResult verifyLifetimeAssumptionCases(
    const MirBody &body, std::size_t owner,
    const LifetimeSignatureContract &declared, Verify verify) {
  const auto reject = [&](std::string message) {
    return MirVerificationResult{{MirVerificationError{
        .bodyKind = body.kind, .owner = owner, .message = std::move(message)}}};
  };
  if (!scopedLifetimeContract(declared))
    return reject("MIR lifetime contract contains a malformed or out-of-scope type region");
  if (!declared.requirements.resolved())
    return reject("MIR callable retains an unresolved semantic lifetime predicate");
  if (declared.requirements.nodes.empty())
    return verify(declared);
  MirVerificationResult result;
  const auto proof = visitLifetimeAssumptionCases(declared, [&](const auto &branch) {
    result = verify(branch);
    return result.valid();
  });
  if (proof.proof != LifetimeFormulaProof::Proven && result.valid())
    return reject(proof.proof == LifetimeFormulaProof::Exhausted
                      ? "MIR lifetime assumption proof exceeded its bounded case budget"
                      : "MIR lifetime assumption formula has no valid proof");
  return result;
}

Lifetime applyLifetime(Lifetime value,
                       std::span<const LifetimeBinding> bindings) {
  for (const auto binding : bindings)
    if (binding.first == value)
      return binding.second;
  return value;
}
} // namespace

std::optional<SemanticType> checkedMirCallableOwnerType(
    const MirProgram &program, std::optional<HirClassInstanceId> owner,
    const SemanticType &type, const LifetimeSignatureContract &contract) {
  if (!owner)
    return type == SemanticType::Unknown ? std::optional{type} : std::nullopt;
  const auto *layout = program.findClassInstance(*owner);
  if (!layout)
    return std::nullopt;
  const auto selected = type == SemanticType::Unknown &&
                                !containsLifetimeMetadata(layout->type)
                            ? layout->type : type;
  if (containsLifetimeMetadata(selected) &&
      (!contract.valid() || !scopedDeclarationType(selected, contract.parameters)))
    return std::nullopt;
  const auto view = mirClassLifetimeView(program, selected);
  return view && view->instance == layout ? std::optional{selected} : std::nullopt;
}

std::optional<SemanticType> instantiateMirCallableOwnerType(
    const MirProgram &program, const MirFunctionInstance &function,
    const MirLifetimeApplication &application) {
  const auto owner = checkedMirCallableOwnerType(
      program, function.owner, function.ownerType, function.lifetimeContract);
  const auto signature = owner ? instantiateMirLifetimeSignature(
      function.lifetimeContract, *owner, function.parameterTypes, application) : std::nullopt;
  return signature ? std::optional{signature->returnType} : std::nullopt;
}

std::optional<MirInstantiatedLifetimeSignature>
instantiateMirLifetimeSignature(const LifetimeSignatureContract &contract,
                                const SemanticType &returnType,
                                std::span<const SemanticType> parameterTypes,
                                const MirLifetimeApplication &application) {
  const bool regionSignature =
      containsLifetimeMetadata(returnType) ||
      std::any_of(parameterTypes.begin(), parameterTypes.end(),
                  containsLifetimeMetadata);
  const bool contractRequirements =
      !contract.parameters.empty() || !contract.bounds.empty() ||
      !contract.typeBounds.empty() || !contract.requirements.nodes.empty() ||
      contract.receiver != Lifetime{};
  const bool applicationRequirements =
      !application.bindings.empty() || !application.variables.empty() ||
      !application.obligations.empty() ||
      !application.typeObligations.empty() || !application.origins.empty();
  if (!regionSignature && !contractRequirements && !applicationRequirements) {
    if (contract != LifetimeSignatureContract{} && !contract.valid())
      return std::nullopt;
    // Legacy region-free MIR has no lifetime substitution to prove. Preserve
    // its exact signature; this cannot erase a region or an incomplete bound.
    return MirInstantiatedLifetimeSignature{
        returnType, std::vector<SemanticType>(parameterTypes.begin(),
                                              parameterTypes.end())};
  }
  if (!contract.valid() || !contract.requirements.resolved() || !application.complete ||
      application.bindings.size() != contract.parameters.size() ||
      !scopedDeclarationType(returnType, contract.parameters))
    return std::nullopt;
  for (const auto &type : parameterTypes)
    if (!scopedDeclarationType(type, contract.parameters))
      return std::nullopt;
  for (const auto &bound : contract.typeBounds)
    if (!scopedDeclarationType(bound.type, contract.parameters))
      return std::nullopt;
  for (const auto &node : contract.requirements.nodes)
    if (node.kind == LifetimeRequirementNode::Kind::Type &&
        !scopedDeclarationType(node.type, contract.parameters))
      return std::nullopt;
  for (std::size_t index = 0; index < application.variables.size(); ++index) {
    const auto variable = application.variables[index];
    if (variable.kind != Lifetime::Kind::Inference || !variable.specified() ||
        std::find(application.variables.begin(),
                  application.variables.begin() + index,
                  variable) != application.variables.begin() + index)
      return std::nullopt;
  }
  for (std::size_t index = 0; index < application.bindings.size(); ++index) {
    const auto [parameter, actual] = application.bindings[index];
    if (std::find(contract.parameters.begin(), contract.parameters.end(),
                  parameter) == contract.parameters.end() ||
        !actual.specified() ||
        std::any_of(
            application.bindings.begin(), application.bindings.begin() + index,
            [&](auto previous) { return previous.first == parameter; }) ||
        (actual.kind == Lifetime::Kind::Inference &&
         std::find(application.variables.begin(), application.variables.end(),
                   actual) == application.variables.end()))
      return std::nullopt;
  }
  for (const auto bound : contract.bounds) {
    const LifetimeBound required{
        applyLifetime(bound.longer, application.bindings),
        applyLifetime(bound.shorter, application.bindings)};
    if (std::find(application.obligations.begin(),
                  application.obligations.end(),
                  required) == application.obligations.end())
      return std::nullopt;
  }
  for (const auto &bound : contract.typeBounds) {
    auto type = substituteTypeLifetimes(bound.type, application.bindings);
    if (!type)
      return std::nullopt;
    const LifetimeTypeBound required{
        std::move(*type), applyLifetime(bound.region, application.bindings)};
    if (std::find(application.typeObligations.begin(),
                  application.typeObligations.end(),
                  required) == application.typeObligations.end())
      return std::nullopt;
  }
  auto result = substituteTypeLifetimes(returnType, application.bindings);
  if (!result)
    return std::nullopt;
  MirInstantiatedLifetimeSignature signature{.returnType = std::move(*result)};
  for (const auto &parameter : parameterTypes) {
    auto type = substituteTypeLifetimes(parameter, application.bindings);
    if (!type)
      return std::nullopt;
    signature.parameterTypes.push_back(std::move(*type));
  }
  return signature;
}
namespace {
std::optional<LifetimeSignatureContract>
expandedLifetimeAssumptions(const MirProgram &program,
                            const LifetimeSignatureContract &source) {
  if (!scopedLifetimeContract(source))
    return std::nullopt;
  LifetimeSignatureContract result = source;
  const auto addType = [&](const SemanticType &type, Lifetime region) {
    const LifetimeTypeBound bound{type, region};
    if (std::find(result.typeBounds.begin(), result.typeBounds.end(), bound) ==
        result.typeBounds.end())
      result.typeBounds.push_back(bound);
  };
  const auto addRegion = [&](Lifetime longer, Lifetime shorter) {
    const LifetimeBound bound{longer, shorter};
    if (!longer.declarationRegion() || !shorter.declarationRegion())
      return false;
    if (std::find(result.bounds.begin(), result.bounds.end(), bound) ==
        result.bounds.end())
      result.bounds.push_back(bound);
    return true;
  };
  for (std::size_t index = 0; index < result.typeBounds.size(); ++index) {
    if (index >= 4096)
      return std::nullopt;
    // Appending a field may reallocate the vector: retain this exact value.
    const auto bound = result.typeBounds[index];
    const auto &type = bound.type;
    if (type.kind == SemanticType::Unknown)
      return std::nullopt;
    if (type.kind == SemanticType::TypeParameter ||
        (type.kind == SemanticType::TypePack && !type.concretePack))
      continue; // Opaque types retain an exact bound, never their arguments'.
    if (type.kind == SemanticType::Class) {
      const auto owner = mirClassLifetimeView(program, type);
      if (!owner)
        return std::nullopt;
      for (const auto &field : owner->instance->declaredFields) {
        const auto instantiated = owner->instantiate(field.type);
        if (!instantiated)
          return std::nullopt;
        addType(*instantiated, bound.region);
      }
      for (const auto &base : owner->instance->structuralBases)
        if (!base.interface) {
          const auto instantiated = owner->instantiate(base.type);
          if (!instantiated)
            return std::nullopt;
          addType(*instantiated, bound.region);
        }
      continue;
    }
    if (type.kind == SemanticType::Enum) {
      const auto enumeration = mirEnumLifetimeView(program, type);
      if (!enumeration)
        return std::nullopt;
      if (enumeration->payload)
        for (const auto &variant : enumeration->payload->variants)
          for (const auto &field : variant.payloadTypes) {
            const auto instantiated = enumeration->instantiate(field);
            if (!instantiated)
              return std::nullopt;
            addType(*instantiated, bound.region);
          }
      continue;
    }
    if (type.kind == SemanticType::Lambda) {
      const auto captures = mirLambdaCaptureTypes(program, type);
      if (!captures) return std::nullopt;
      for (const auto &capture : *captures) addType(capture, bound.region);
      continue;
    }
    if (type.kind == SemanticType::FunctionSignature ||
        type.kind == SemanticType::FunctionItem) {
      for (const auto region : freeTypeLifetimes(type))
        if (!addRegion(region, bound.region))
          return std::nullopt;
      continue;
    }
    if (type.kind == SemanticType::Reference &&
        !addRegion(type.referenceLifetime, bound.region))
      return std::nullopt;
    for (const auto region : type.lifetimeArguments)
      if (!addRegion(region, bound.region))
        return std::nullopt;
    lifetime_detail::children(
        type, [&](const SemanticType &child) { addType(child, bound.region); });
  }
  return result;
}

bool mirTypeOutlives(const MirProgram &program, const SemanticType &type,
                     Lifetime region, const LifetimeSignatureContract &contract,
                     const LifetimeInferenceConstraints &graph,
                     std::vector<SemanticType> &pending) {
  for (const auto &known : contract.typeBounds)
    if (equivalentLifetimeTypes(type, known.type) &&
        graph.proves(known.region, region))
      return true;
  if (type.kind == SemanticType::Unknown ||
      type.kind == SemanticType::TypeParameter ||
      (type.kind == SemanticType::TypePack && !type.concretePack))
    return false;
  if (std::find(pending.begin(), pending.end(), type) != pending.end())
    return true;
  if (pending.size() >= 256)
    return false;
  pending.push_back(type);
  struct Pop {
    std::vector<SemanticType> &types;
    ~Pop() { types.pop_back(); }
  } pop{pending};
  if (type.kind == SemanticType::Reference &&
      !graph.proves(type.referenceLifetime, region))
    return false;
  if (type.kind != SemanticType::Class && type.kind != SemanticType::Enum &&
      type.kind != SemanticType::Lambda)
    for (const auto lifetime : type.lifetimeArguments)
      if (!graph.proves(lifetime, region))
        return false;
  if ((type.kind == SemanticType::StringView || type.kind == SemanticType::CString) &&
      type.lifetimeArguments.size() != 1)
    return false;
  if (type.kind == SemanticType::Class) {
    const auto instance = mirClassLifetimeView(program, type);
    if (!instance)
      return false;
    for (const auto &field : instance->instance->declaredFields) {
      const auto instantiated = instance->instantiate(field.type);
      if (!instantiated || !mirTypeOutlives(program, *instantiated, region, contract, graph, pending))
        return false;
    }
    for (const auto &base : instance->instance->structuralBases)
      if (!base.interface) {
        const auto instantiated = instance->instantiate(base.type);
        if (!instantiated || !mirTypeOutlives(program, *instantiated, region, contract, graph, pending))
          return false;
      }
    return true;
  }
  if (type.kind == SemanticType::Enum) {
    const auto enumeration = mirEnumLifetimeView(program, type);
    if (!enumeration)
      return false;
    if (enumeration->payload)
      for (const auto &variant : enumeration->payload->variants)
        for (const auto &field : variant.payloadTypes) {
          const auto instantiated = enumeration->instantiate(field);
          if (!instantiated || !mirTypeOutlives(program, *instantiated, region, contract, graph, pending))
            return false;
        }
    return true;
  }
  if (type.kind == SemanticType::Lambda) {
    const auto captures = mirLambdaCaptureTypes(program, type);
    if (!captures) return false;
    for (const auto &capture : *captures)
      if (!mirTypeOutlives(program, capture, region, contract, graph, pending))
        return false;
    return true;
  }
  if ((type.kind == SemanticType::FunctionSignature ||
       type.kind == SemanticType::NativeFunction ||
       type.kind == SemanticType::FunctionItem) &&
      !type.quantifiedLifetimes.empty()) {
    for (const auto lifetime : freeTypeLifetimes(type))
      if (!graph.proves(lifetime, region))
        return false;
    return true;
  }
  bool valid = true;
  lifetime_detail::children(type, [&](const SemanticType &child) {
    valid = mirTypeOutlives(program, child, region, contract, graph, pending) &&
            valid;
  });
  return valid;
}
bool satisfiesLifetimeRequirements(
    const MirProgram &program, const LifetimeSignatureContract &target,
    std::span<const LifetimeBinding> bindings,
    const LifetimeSignatureContract &assumptions,
    const LifetimeInferenceConstraints &graph) {
  if (!scopedLifetimeContract(target) || !target.requirements.resolved())
    return false;
  const auto proof = evaluateLifetimeRequirements(target.requirements, [&](const auto &node) {
    if (node.kind == LifetimeRequirementNode::Kind::Region)
      return graph.proves(applyLifetime(node.bound.longer, bindings),
                          applyLifetime(node.bound.shorter, bindings));
    const auto type = substituteTypeLifetimes(node.type, bindings);
    std::vector<SemanticType> pending;
    return type && mirTypeOutlives(program, *type,
        applyLifetime(node.bound.shorter, bindings), assumptions, graph, pending);
  });
  return proof.value_or(false);
}

bool provesNominalLifetimeFormation(
    const MirProgram &program, const SemanticType &type,
    const LifetimeSignatureContract &assumptions,
    const LifetimeInferenceConstraints &graph, std::size_t depth = 0) {
  if (depth >= 256 || type.kind == SemanticType::Unknown)
    return false;
  const LifetimeSignatureContract *contract = nullptr;
  std::vector<LifetimeBinding> bindings;
  if (type.kind == SemanticType::Class) {
    const auto view = mirClassLifetimeView(program, type);
    if (!view)
      return false;
    contract = &view->instance->lifetimeContract;
    bindings = view->bindings;
  } else if (type.kind == SemanticType::Enum) {
    const auto view = mirEnumLifetimeView(program, type);
    if (!view)
      return false;
    contract = view->payload ? &view->payload->lifetimeContract : &view->integral->lifetimeContract;
    bindings = view->bindings;
  }
  if (contract && *contract != LifetimeSignatureContract{}) {
    if (!contract->valid() || !scopedLifetimeContract(*contract) ||
        !contract->requirements.resolved())
      return false;
    for (const auto parameter : contract->parameters)
      if (std::none_of(bindings.begin(), bindings.end(),
                       [&](const auto &binding) { return binding.first == parameter; }))
        return false;
    for (const auto bound : contract->bounds)
      if (!graph.proves(applyLifetime(bound.longer, bindings),
                        applyLifetime(bound.shorter, bindings)))
        return false;
    for (const auto &bound : contract->typeBounds) {
      const auto actual = substituteTypeLifetimes(bound.type, bindings);
      std::vector<SemanticType> pending;
      if (!actual || !mirTypeOutlives(program, *actual,
              applyLifetime(bound.region, bindings), assumptions, graph, pending))
        return false;
    }
    if (!satisfiesLifetimeRequirements(program, *contract, bindings, assumptions, graph))
      return false;
  } else if (contract && containsLifetimeMetadata(type)) {
    return false;
  }
  // Callable binders belong to their separately verified signature contract.
  // Do not import their nominal premises into an enclosing value's body scope.
  if (type.kind == SemanticType::FunctionSignature || type.kind == SemanticType::FunctionItem ||
      type.kind == SemanticType::Lambda)
    return true;
  bool valid = true;
  lifetime_detail::children(type, [&](const SemanticType &child) {
    valid = provesNominalLifetimeFormation(program, child, assumptions, graph, depth + 1) && valid;
  });
  return valid;
}

LifetimeSignatureContract withLocalStorageLifetimeFacts(
    const MirBody &body, LifetimeSignatureContract assumptions) {
  const auto add = [&](const SemanticType &type, Lifetime region) {
    const LifetimeTypeBound bound{type, region};
    if (std::find(assumptions.typeBounds.begin(), assumptions.typeBounds.end(), bound) ==
        assumptions.typeBounds.end())
      assumptions.typeBounds.push_back(bound);
  };
  // Use only the independently resolved exact origin place. This is a
  // body-local premise, not a declaration assumption or a promise that an
  // opaque type outlives any caller-chosen region. Availability and lifetime
  // verification remain responsible for every executable use of this place.
  for (const auto &origin : body.lifetimeBody.origins) {
    const auto *place = body.findPlace(origin.place);
    if (origin.region.kind != Lifetime::Kind::Local || !place ||
        place->foreignStorage || place->type.kind == SemanticType::Unknown)
      continue;
    add(place->type, origin.region);
    if (place->type.kind == SemanticType::Reference && place->type.arguments.size() == 1)
      add(place->type.arguments.front(), origin.region);
  }
  return assumptions;
}

} // namespace

static MirVerificationResult
verifyMirLifetimeBodyFactsForCase(const MirProgram &program, const MirBody &body,
                           const LifetimeSignatureContract &declaredContract,
                           std::size_t owner) {
  MirVerificationResult result;
  const auto reject = [&](std::string message, MirBlockId block = 0,
                          MirInstructionId instruction = 0) {
    result.errors.push_back({.bodyKind = body.kind,
                             .owner = owner,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };
  if (!declaredContract.requirements.resolved()) {
    reject("MIR callable retains an unresolved semantic lifetime predicate");
    return result;
  }
  const auto expanded = expandedLifetimeAssumptions(program, declaredContract);
  if (!expanded) {
    reject("MIR declaration type-outlives assumptions have no exact structural "
           "inventory");
    return result;
  }
  const auto &contract = *expanded;
  const auto &facts = body.lifetimeBody;
  if (facts == MirLifetimeBody{}) {
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (!instruction.lifetimeApplication.bindings.empty() ||
            !instruction.lifetimeApplication.variables.empty() ||
            !instruction.lifetimeApplication.obligations.empty() ||
            !instruction.lifetimeApplication.typeObligations.empty() ||
            !instruction.lifetimeApplication.origins.empty())
          reject("MIR lifetime application has no complete body inventory",
                 block.id, instruction.id);
    return result;
  }
  if (!facts.complete ||
      (contract != LifetimeSignatureContract{} && !contract.valid())) {
    reject("MIR lifetime body has an incomplete declaration or body contract");
    return result;
  }
  auto resolved = facts;
  if (!resolveMirLifetimeOrigins(resolved, body) || resolved != facts) {
    reject(
        "MIR lifetime body origin does not designate its exact mapped place");
    return result;
  }
  for (std::size_t index = 0; index < facts.variables.size(); ++index) {
    const auto variable = facts.variables[index];
    if (variable.kind != Lifetime::Kind::Inference || !variable.specified() ||
        std::find(facts.variables.begin(), facts.variables.begin() + index,
                  variable) != facts.variables.begin() + index)
      reject(
          "MIR lifetime body has an invalid or repeated inference allocation");
  }
  for (std::size_t index = 0; index < facts.origins.size(); ++index) {
    const auto &origin = facts.origins[index];
    if (origin.region.kind != Lifetime::Kind::Local ||
        !origin.region.specified() ||
        (origin.key.valid() && origin.key.domain != body.placeDomain) ||
        std::any_of(facts.origins.begin(), facts.origins.begin() + index,
                    [&](const auto &previous) {
                      return previous.region == origin.region;
                    }))
      reject("MIR local lifetime has no unique body-owned origin witness");
  }
  const auto known = [&](Lifetime region) {
    if (!region.specified())
      return false;
    switch (region.kind) {
    case Lifetime::Kind::Static:
      return true;
    case Lifetime::Kind::Parameter:
      return std::find(contract.parameters.begin(), contract.parameters.end(),
                       region) != contract.parameters.end();
    case Lifetime::Kind::Inference:
      return std::find(facts.variables.begin(), facts.variables.end(),
                       region) != facts.variables.end();
    case Lifetime::Kind::Local:
      return std::any_of(
          facts.origins.begin(), facts.origins.end(),
          [&](const auto &origin) { return origin.region == region; });
    case Lifetime::Kind::Unspecified:
      return false;
    }
    return false;
  };
  const auto checkTypeRegions = [&](const SemanticType &type) {
    for (const auto region : freeTypeLifetimes(type))
      if (!known(region))
        reject(
            "MIR body type contains an unallocated or out-of-scope lifetime");
  };
  checkTypeRegions(body.returnType);
  for (const auto &place : body.places)
    checkTypeRegions(place.type);
  for (const auto &value : body.values)
    checkTypeRegions(value.info.type);
  LifetimeBounds assumptions;
  for (const auto bound : contract.bounds)
    if (!assumptions.assume(bound.longer, bound.shorter))
      reject("MIR declaration lifetime assumptions are malformed");
  LifetimeInferenceConstraints graph(assumptions);
  for (const auto bound : facts.obligations)
    if (!known(bound.longer) || !known(bound.shorter) ||
        !graph.require(bound.longer, bound.shorter))
      reject("MIR body lifetime obligation uses an unallocated or out-of-scope "
             "region");
  for (const auto &bound : facts.typeObligations) {
    if (!known(bound.region))
      reject("MIR type-outlives obligation uses an unallocated or out-of-scope "
             "region");
    for (const auto region : freeTypeLifetimes(bound.type))
      if (!known(region))
        reject("MIR type-outlives obligation contains an out-of-scope type "
               "region");
  }
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto &application = instruction.lifetimeApplication;
      if (application == MirLifetimeApplication{})
        continue;
      if (!application.complete ||
          (instruction.kind != MirInstructionKind::Call &&
           instruction.kind != MirInstructionKind::Construct &&
           instruction.kind != MirInstructionKind::InitializeBase &&
           !(instruction.kind == MirInstructionKind::Compute &&
             instruction.operation == MirOperation::NativeCallback))) {
        reject("MIR instruction carries an incomplete or misplaced lifetime "
               "application",
               block.id, instruction.id);
        continue;
      }
      if (application.callableSignature)
        for (const auto fixed : application.callableSignature->signatureBindings)
          if (!known(fixed.second))
            reject("MIR callable carrier fixes an unallocated or out-of-scope lifetime", block.id, instruction.id);
      for (const auto binding : application.bindings)
        if (!known(binding.second))
          reject(
              "MIR lifetime binding uses an unallocated or out-of-scope region",
              block.id, instruction.id);
      for (const auto variable : application.variables)
        if (variable.kind != Lifetime::Kind::Inference || !known(variable))
          reject("MIR call inference allocation is missing from the body "
                 "inventory",
                 block.id, instruction.id);
      for (const auto bound : application.obligations)
        if (std::find(facts.obligations.begin(), facts.obligations.end(),
                      bound) == facts.obligations.end())
          reject("MIR call lifetime obligation is missing from the body graph",
                 block.id, instruction.id);
      for (const auto &bound : application.typeObligations)
        if (std::find(facts.typeObligations.begin(),
                      facts.typeObligations.end(),
                      bound) == facts.typeObligations.end())
          reject("MIR call type obligation is missing from the body graph",
                 block.id, instruction.id);
      for (const auto &origin : application.origins)
        if (std::find(facts.origins.begin(), facts.origins.end(), origin) ==
            facts.origins.end())
          reject(
              "MIR call lifetime origin differs from the body's exact witness",
              block.id, instruction.id);
    }
  if (!result.valid())
    return result;
  if (graph.failure()) {
    reject("MIR body requires an outlives relation not proved by its "
           "declaration contract");
    return result;
  }
  const auto localAssumptions = withLocalStorageLifetimeFacts(body, contract);
  for (const auto &bound : facts.typeObligations) {
    std::vector<SemanticType> pending;
    if (!mirTypeOutlives(program, bound.type, bound.region, localAssumptions, graph,
                         pending))
      reject("MIR body cannot prove an exact type-outlives obligation");
  }
  return result;
}
bool mirDeclaredLifetimeRelationship(
    const MirProgram &program, const LifetimeSignatureContract &contract,
    Lifetime source, Lifetime target) {
  if (!scopedLifetimeContract(contract) || !contract.requirements.resolved())
    return false;
  const auto prove = [&](const auto &branch) {
    const auto expanded = expandedLifetimeAssumptions(program, branch);
    if (!expanded)
      return false;
    LifetimeBounds bounds;
    for (const auto bound : expanded->bounds)
      if (!bounds.assume(bound.longer, bound.shorter))
        return false;
    return bounds.proves(source, target);
  };
  return contract.requirements.nodes.empty()
             ? prove(contract)
             : visitLifetimeAssumptionCases(contract, prove).proof == LifetimeFormulaProof::Proven;
}

MirVerificationResult verifyMirLifetimeBodyFacts(
    const MirProgram &program, const MirBody &body,
    const LifetimeSignatureContract &contract, std::size_t owner) {
  return verifyLifetimeAssumptionCases(body, owner, contract, [&](const auto &branch) {
    return verifyMirLifetimeBodyFactsForCase(program, body, branch, owner);
  });
}

namespace {
const LifetimeSignatureContract &bodyLifetimeContract(const MirProgram &program,
                                                      const MirBody &body,
                                                      std::size_t owner) {
  if (body.kind == MirBodyKind::Function)
    if (const auto *instance = program.findFunctionInstance(owner))
      return instance->lifetimeContract;
  if (body.kind == MirBodyKind::Constructor)
    if (const auto *instance = program.findConstructorInstance(owner))
      return instance->lifetimeContract;
  if (body.kind == MirBodyKind::Destructor)
    if (const auto *instance = program.findDestructorInstance(owner))
      return instance->lifetimeContract;
  if (body.kind == MirBodyKind::Lambda)
    if (const auto *instance = program.findLambda(owner))
      return instance->lifetimeContract;
  static const LifetimeSignatureContract absent;
  return absent;
}

const LifetimeSignatureContract *appliedTargetContract(
    const MirProgram &program, const MirInstruction &instruction) {
  if (instruction.functionTarget) {
    const auto *target = program.findFunctionInstance(*instruction.functionTarget);
    return target ? &target->lifetimeContract : nullptr;
  }
  if (instruction.constructorTarget) {
    const auto *target = program.findConstructorInstance(*instruction.constructorTarget);
    return target ? &target->lifetimeContract : nullptr;
  }
  if (instruction.lambdaTarget) {
    const auto *target = program.findLambda(*instruction.lambdaTarget);
    return target ? &target->lifetimeContract : nullptr;
  }
  const auto *callable = checkedMirCallableLifetimeSignature(program, instruction);
  return callable ? &callable->contract : nullptr;
}

std::optional<MirInstantiatedLifetimeSignature>
appliedTargetSignature(const MirProgram &program,
                       const MirInstruction &instruction) {
  if (instruction.functionTarget) {
    const auto *target =
        program.findFunctionInstance(*instruction.functionTarget);
    if (!target)
      return std::nullopt;
    return instantiateMirLifetimeSignature(
        target->lifetimeContract, target->returnType, target->parameterTypes,
        instruction.lifetimeApplication);
  }
  if (instruction.constructorTarget) {
    const auto *target =
        program.findConstructorInstance(*instruction.constructorTarget);
    const auto *owner =
        target ? program.findClassInstance(target->owner) : nullptr;
    if (!owner)
      return std::nullopt;
    const auto ownerType = checkedMirCallableOwnerType(
        program, target->owner, target->ownerType, target->lifetimeContract);
    if (!ownerType)
      return std::nullopt;
    return instantiateMirLifetimeSignature(target->lifetimeContract,
                                           *ownerType, target->parameterTypes,
                                           instruction.lifetimeApplication);
  }
  if (instruction.lambdaTarget) {
    const auto *target = program.findLambda(*instruction.lambdaTarget);
    if (!target)
      return std::nullopt;
    return instantiateMirLifetimeSignature(
        target->lifetimeContract, target->returnType, target->parameterTypes,
        instruction.lifetimeApplication);
  }
  if (instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke || instruction.nativeCalleeType) {
    const auto *callable = checkedMirCallableLifetimeSignature(program, instruction);
    if (!callable)
      return std::nullopt;
    auto result = instantiateMirLifetimeSignature(
        callable->contract, callable->signature.arguments.front(),
        std::span(callable->signature.arguments).subspan(1),
        instruction.lifetimeApplication);
    if (result && instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke)
      result->parameterTypes.insert(result->parameterTypes.begin(),
                                    instruction.parameterTypes.front());
    return result;
  }
  return std::nullopt;
}

class MirLifetimeRelations {
public:
  MirLifetimeRelations(const MirProgram &program, const MirBody &body,
                       const LifetimeSignatureContract &contract,
                       const LifetimeInferenceConstraints &graph)
      : program(program), body(body), contract(contract), graph(graph) {}

  const MirInstruction *definition(MirValueId id) const {
    const auto *value = body.findValue(id);
    const auto *block =
        value ? body.findBlock(value->definitionBlock) : nullptr;
    if (!block)
      return nullptr;
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [&](const auto &instruction) {
                       return instruction.id == value->definition;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  }

  const MirOperand *sourceOperand(const MirOperand &operand,
                                  HirValueId &source) const {
    const MirOperand *current = &operand;
    for (std::size_t depth = 0; depth < 64; ++depth) {
      const auto *stage = current->kind == MirOperandKind::Value
                              ? definition(current->value)
                              : nullptr;
      if (!stage || stage->kind != MirInstructionKind::CallInput ||
          stage->info.type.kind == SemanticType::Reference)
        return current;
      if (stage->tombstone || stage->operands.size() != 1)
        return nullptr;
      source = stage->hirValue;
      current = &stage->operands.front();
    }
    return nullptr;
  }

  std::optional<SemanticType> actualType(const MirOperand &operand) const {
    // Every input stage is independently proved below. Its declared result
    // is the next operation's type boundary, not a route back to an older
    // source region. Other operands are reconstructed from physical facts.
    if (operand.kind == MirOperandKind::Value)
      if (const auto *stage = definition(operand.value);
          stage && stage->kind == MirInstructionKind::CallInput)
        return stage->tombstone ? std::nullopt : std::optional{stage->info.type};
    const auto *source = &operand;
    if (source->place)
      if (const auto *place = body.findPlace(source->place))
        return place->type;
    if (source->kind == MirOperandKind::Value)
      if (const auto *value = body.findValue(source->value))
        return value->info.type;
    if (source->kind == MirOperandKind::Loan)
      if (const auto *loan = body.findLoan(source->loan)) {
        if (const auto *place = body.findPlace(loan->source))
          return place->type;
      }
    return source->type;
  }

  bool borrowedOperand(const MirOperand &operand, Lifetime target,
                       HirValueId source = 0) const {
    if (!target.specified())
      return false;
    const auto *actual = sourceOperand(operand, source);
    if (!actual)
      return false;
    if (actual->kind == MirOperandKind::Value)
      if (const auto *stage = definition(actual->value);
          stage && stage->kind == MirInstructionKind::CallInput &&
          stage->info.type.kind == SemanticType::Reference)
        return !stage->tombstone &&
               graph.proves(stage->info.type.referenceLifetime, target);
    // A checked expression's own declared/instantiated reference region takes
    // precedence over older supplier places; a shortened alias cannot regain
    // a longer region merely because its physical owner survives longer.
    if (source) {
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.hirValue == source &&
              (instruction.kind == MirInstructionKind::Call ||
               instruction.kind == MirInstructionKind::Construct)) {
            const auto signature = appliedTargetSignature(program, instruction);
            if (signature &&
                signature->returnType.kind == SemanticType::Reference)
              return graph.proves(signature->returnType.referenceLifetime,
                                  target);
          }
      std::optional<Lifetime> selected;
      for (const auto &place : body.places)
        if (place.sourceValue == source) {
          const auto known = declaredPlaceRegion(place);
          if (known.specified()) {
            if (selected && *selected != known)
              return false;
            selected = known;
          }
        }
      if (selected)
        return graph.proves(*selected, target);
    }
    // A selected stored reference may use its carrier's abstract entry place
    // for origin bookkeeping. That place's outer storage region does not name
    // the selected external referent. Replay the exact typed extraction first.
    if (actual->kind == MirOperandKind::Loan)
      if (const auto *loan = body.findLoan(actual->loan);
          loan && loan->referenceSelection && loan->referenceSelection->kind ==
              ReferenceSelectionKind::StoredExternalReference)
        return loanProves(actual->loan, target, 0);
    if (actual->place)
      if (const auto *place = body.findPlace(actual->place)) {
        const auto known = declaredPlaceRegion(*place);
        if (known.specified())
          return graph.proves(known, target);
        if (originPlaceProves(*place, target))
          return true;
      }
    if (actual->kind == MirOperandKind::Loan)
      return loanProves(actual->loan, target, 0);
    return false;
  }

  bool types(const SemanticType &actual, const SemanticType &expected,
             int variance = 1, std::size_t depth = 0,
             bool exactShape = false) const {
    if (depth >= 256)
      return false;
    if (actual == expected)
      return true;
    if (!actual.quantifiedLifetimes.empty() ||
        !expected.quantifiedLifetimes.empty())
      return equivalentLifetimeTypes(actual, expected);
    if (actual.kind != expected.kind)
      return !exactShape && freeTypeLifetimes(actual).empty() &&
             freeTypeLifetimes(expected).empty();
    if (actual.kind == SemanticType::Class &&
        actual.classId != expected.classId) {
      std::vector<SemanticType> matches;
      std::vector<SemanticType> visited;
      const auto project = [&](auto &&self, const SemanticType &type) -> void {
        if (visited.size() >= 256 ||
            std::find(visited.begin(), visited.end(), type) != visited.end())
          return;
        visited.push_back(type);
        if (type.classId == expected.classId) {
          if (std::find(matches.begin(), matches.end(), type) == matches.end())
            matches.push_back(type);
          return;
        }
        if (const auto instance = mirClassLifetimeView(program, type))
          for (const auto &base : instance->instance->structuralBases)
            if (const auto projected = instance->instantiate(base.type))
              self(self, *projected);
      };
      project(project, actual);
      return matches.size() == 1 &&
             types(matches.front(), expected, variance, depth + 1, exactShape);
    }
    // Lifetime covariance never changes nominal/value identity, array
    // extent, dispatch or access authority. Compare the non-region shell at
    // every level before checking its independently proved child relations.
    auto sourceShell = actual;
    auto targetShell = expected;
    const auto shell = [](SemanticType &type) {
      type.arguments.clear();
      type.lambdaEnclosingClassTypes.clear();
      type.lambdaEnclosingFunctionTypes.clear();
      type.referenceLifetime = {};
      type.lifetimeArguments.clear();
      type.quantifiedLifetimes.clear();
    };
    shell(sourceShell);
    shell(targetShell);
    if (variance > 0 && actual.kind == SemanticType::Reference &&
        actual.referenceAccess == AccessMode::Mutable &&
        expected.referenceAccess == AccessMode::ReadOnly)
      sourceShell.referenceAccess = AccessMode::ReadOnly;
    if (sourceShell != targetShell)
      return false;
    const auto region = [&](Lifetime source, Lifetime target, int mode) {
      if (source == Lifetime{} && target == Lifetime{})
        return true;
      return (mode < 0 ? graph.proves(target, source)
                       : graph.proves(source, target)) &&
             (mode != 0 || graph.proves(target, source));
    };
    if (actual.kind == SemanticType::Reference &&
        !region(actual.referenceLifetime, expected.referenceLifetime, variance))
      return false;
    if (actual.lifetimeArguments.size() != expected.lifetimeArguments.size() ||
        actual.arguments.size() != expected.arguments.size())
      return false;
    const NominalVariance *nominal = nullptr;
    if (expected.kind == SemanticType::Class) {
      if (const auto *owner = nominalClass(expected))
        nominal = &owner->variance;
    } else if (expected.kind == SemanticType::Enum) {
      if (const auto enumeration = mirEnumLifetimeView(program, expected))
        nominal = enumeration->payload ? &enumeration->payload->variance
                                       : &enumeration->integral->variance;
    }
    if ((expected.kind == SemanticType::Class ||
         expected.kind == SemanticType::Enum) &&
        (!nominal || !nominal->complete ||
         nominal->types.size() != expected.arguments.size() ||
         nominal->lifetimes.size() != expected.lifetimeArguments.size()))
      return false;
    const auto mode = [&](TypeVariance inner) {
      return inner == TypeVariance::Invariant       ? 0
             : inner == TypeVariance::Contravariant ? -variance
                                                    : variance;
    };
    for (std::size_t index = 0; index < actual.lifetimeArguments.size();
         ++index) {
      const auto direction = nominal ? nominal->lifetimes[index]
                             : (expected.kind == SemanticType::StringView || expected.kind == SemanticType::CString)
                                 ? TypeVariance::Covariant
                                 : TypeVariance::Invariant;
      if (direction != TypeVariance::Bivariant &&
          !region(actual.lifetimeArguments[index],
                  expected.lifetimeArguments[index], mode(direction)))
        return false;
    }
    for (std::size_t index = 0; index < actual.arguments.size(); ++index) {
      TypeVariance direction = TypeVariance::Covariant;
      if (nominal)
        direction = nominal->types[index];
      else if ((expected.kind == SemanticType::Reference &&
                expected.referenceAccess == AccessMode::Mutable) ||
               expected.kind == SemanticType::RawPointer)
        direction = TypeVariance::Invariant;
      else if (expected.kind == SemanticType::FunctionSignature && index != 0)
        direction = TypeVariance::Contravariant;
      if (direction != TypeVariance::Bivariant &&
          !types(actual.arguments[index], expected.arguments[index],
                 mode(direction), depth + 1, exactShape))
        return false;
    }
    const auto enclosing = [&](const auto &source, const auto &target) {
      if (source.size() != target.size())
        return false;
      for (std::size_t index = 0; index < source.size(); ++index)
        if (!types(source[index], target[index], 0, depth + 1, true))
          return false;
      return true;
    };
    return enclosing(actual.lambdaEnclosingClassTypes, expected.lambdaEnclosingClassTypes) &&
           enclosing(actual.lambdaEnclosingFunctionTypes, expected.lambdaEnclosingFunctionTypes);
  }

private:
  const MirClassInstance *nominalClass(const SemanticType &type) const {
    const auto view = mirClassLifetimeView(program, type);
    return view ? view->instance : nullptr;
  }

  Lifetime declaredPlaceRegion(const MirPlace &place,
                               std::size_t depth = 0) const {
    if (depth >= 256)
      return {};
    const MirPlace *root = &place;
    if (!place.projections.empty()) {
      const auto found = std::find_if(
          body.places.begin(), body.places.end(), [&](const auto &candidate) {
            return candidate.projections.empty() &&
                   candidate.root == place.root &&
                   candidate.binding == place.binding &&
                   candidate.symbol == place.symbol &&
                   candidate.capture == place.capture &&
                   candidate.temporary == place.temporary &&
                   candidate.value == place.value &&
                   candidate.loan == place.loan;
          });
      if (found == body.places.end())
        return {};
      root = &*found;
    }
    SemanticType type = root->type;
    Lifetime region;
    if (root->root == MirPlaceRootKind::This)
      region = contract.receiver;
    else if (root->root == MirPlaceRootKind::Symbol && root->symbol &&
             !program.findThreadLocalStorage(root->symbol))
      region = Lifetime::staticLifetime();
    else if (root->root == MirPlaceRootKind::Value) {
      // BorrowValue materializes a reference to physical payload storage.
      // Its lifetime is the checked carrier's lifetime, not the payload
      // value type's (possibly opaque) stored-reference inventory. Replay the
      // exact extraction and selector; an arbitrary Value-root place is not
      // evidence that the carrier supplies the borrow.
      const MirReferenceSelection *selected = nullptr;
      for (const auto &loan : body.loans)
        if (loan.referenceSelection &&
            loan.referenceSelection->kind == ReferenceSelectionKind::PhysicalPayload &&
            loan.referenceSelection->value == root->value) {
          const auto &candidate = *loan.referenceSelection;
          const auto *source = mir_detail::physicalPayloadSelectionSource(
              program, body, candidate);
          if (!source || source->id != root->id ||
              (selected && *selected != candidate))
            return {};
          selected = &candidate;
        }
      if (selected) {
        const auto *carrier = body.findPlace(selected->carrier);
        if (!carrier)
          return {};
        region = declaredPlaceRegion(*carrier, depth + 1);
        if (!region.specified())
          for (const auto &origin : body.lifetimeBody.origins)
            if (origin.place == carrier->id ||
                (carrier->key && origin.key.valid() && *carrier->key == origin.key)) {
              if (region.specified() && region != origin.region)
                return {};
              region = origin.region;
            }
      }
    }
    const auto dereference = [&] {
      if (type.kind != SemanticType::Reference || type.arguments.size() != 1)
        return false;
      region = type.referenceLifetime;
      type = SemanticType(type.arguments.front());
      return true;
    };
    // A direct reference carrier denotes its declared referent in source.
    if (place.projections.empty())
      (void)dereference();
    for (const auto &projection : place.projections) {
      if (projection.kind == MirProjectionKind::Dereference) {
        if (!dereference())
          return {};
      } else if (projection.kind == MirProjectionKind::Field) {
        std::optional<SemanticType> fieldType;
        if (const auto owner = mirClassLifetimeView(program, type)) {
          for (const auto &field : owner->instance->declaredFields) {
            if (field.symbol != projection.field)
              continue;
            if (fieldType)
              return {};
            fieldType = owner->instantiate(field.type);
            if (!fieldType)
              return {};
          }
        } else if (const auto owner = mirEnumLifetimeView(program, type);
                   owner && owner->payload) {
          // A stored reference crosses from the carrier's storage into its
          // declared external region. The stable payload symbol selects one
          // exact field; variant activity and origin/loan authority are checked
          // independently, never granted by this structural type lookup.
          for (const auto &variant : owner->payload->variants) {
            if (variant.payloadSymbols.size() != variant.payloadTypes.size())
              return {};
            for (std::size_t index = 0; index < variant.payloadSymbols.size(); ++index) {
              if (variant.payloadSymbols[index] != projection.field)
                continue;
              if (fieldType)
                return {};
              fieldType = owner->instantiate(variant.payloadTypes[index]);
              if (!fieldType)
                return {};
            }
          }
        }
        if (!fieldType)
          return {};
        type = *fieldType;
      } else if (projection.kind == MirProjectionKind::Index &&
                 type.kind == SemanticType::Array &&
                 type.arguments.size() == 1) {
        type = SemanticType(type.arguments.front());
      } else if (projection.kind == MirProjectionKind::PackElement &&
                 type.kind == SemanticType::TypePack && type.concretePack &&
                 projection.constantIndex &&
                 *projection.constantIndex < type.arguments.size()) {
        type = type.arguments[*projection.constantIndex];
      } else if (projection.kind == MirProjectionKind::Base) {
        const auto owner = mirClassLifetimeView(program, type);
        if (!owner)
          return {};
        const auto base = std::find_if(owner->instance->structuralBases.begin(),
                                      owner->instance->structuralBases.end(),
                                      [&](const auto &candidate) {
                                        return candidate.instance == projection.selection;
                                      });
        if (base == owner->instance->structuralBases.end())
          return {};
        const auto instantiated = owner->instantiate(base->type);
        if (!instantiated)
          return {};
        type = *instantiated;
      } else {
        return {};
      }
    }
    (void)dereference();
    return region;
  }

  bool originPlaceProves(const MirPlace &place, Lifetime target) const {
    for (const auto &origin : body.lifetimeBody.origins)
      if ((origin.place == place.id ||
           (place.key && origin.key.valid() && *place.key == origin.key)) &&
          graph.proves(origin.region, target))
        return true;
    return false;
  }

  bool loanProves(MirLoanId id, Lifetime target, std::size_t depth) const {
    if (depth >= 256)
      return false;
    const auto *loan = body.findLoan(id);
    if (!loan)
      return false;
    if (loan->referenceSelection && loan->referenceSelection->kind ==
            ReferenceSelectionKind::StoredExternalReference) {
      const auto reference = mir_detail::storedPayloadSelectionReference(
          program, body, *loan->referenceSelection);
      return reference && graph.proves(reference->referenceLifetime, target);
    }
    if (loan->producedBy)
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.hirValue == loan->producedBy &&
              instruction.kind == MirInstructionKind::Call) {
            const auto signature = appliedTargetSignature(program, instruction);
            if (signature &&
                signature->returnType.kind == SemanticType::Reference)
              return graph.proves(signature->returnType.referenceLifetime,
                                  target);
          }
    const auto sourceProves = [&](MirPlaceId id) {
      const auto *place = body.findPlace(id);
      if (!place)
        return false;
      const auto region = declaredPlaceRegion(*place);
      return region.specified() ? graph.proves(region, target)
                                : originPlaceProves(*place, target);
    };
    bool proved = loan->source   ? sourceProves(loan->source)
                  : loan->parent ? loanProves(loan->parent, target, depth + 1)
                                 : false;
    for (const auto &dependency : loan->additionalDependencies)
      proved = (dependency.source ? sourceProves(dependency.source)
                : dependency.parent
                    ? loanProves(dependency.parent, target, depth + 1)
                    : false) &&
               proved;
    return proved;
  }
  const MirProgram &program;
  const MirBody &body;
  const LifetimeSignatureContract &contract;
  const LifetimeInferenceConstraints &graph;
};
} // namespace

static bool mirLifetimeTypeConversionForCase(
    const MirProgram &program, const MirBody &body, std::size_t owner,
    const SemanticType &actual, const SemanticType &expected,
    const LifetimeSignatureContract &declared, bool allowDerived) {
  if (!verifyMirLifetimeBodyFactsForCase(program, body, declared, owner).valid())
    return false;
  const auto expanded = expandedLifetimeAssumptions(program, declared);
  if (!expanded)
    return false;
  const auto known = [&](Lifetime region) {
    return region == Lifetime::staticLifetime() ||
           std::find(expanded->parameters.begin(), expanded->parameters.end(), region) != expanded->parameters.end() ||
           std::find(body.lifetimeBody.variables.begin(), body.lifetimeBody.variables.end(), region) != body.lifetimeBody.variables.end() ||
           std::any_of(body.lifetimeBody.origins.begin(), body.lifetimeBody.origins.end(),
                       [&](const auto &origin) { return origin.region == region; });
  };
  for (const auto &type : {actual, expected})
    for (const auto region : freeTypeLifetimes(type))
      if (!known(region))
        return false;
  LifetimeBounds assumptions;
  for (const auto bound : expanded->bounds)
    if (!assumptions.assume(bound.longer, bound.shorter))
      return false;
  LifetimeInferenceConstraints graph(assumptions);
  for (const auto bound : body.lifetimeBody.obligations)
    if (!graph.require(bound.longer, bound.shorter))
      return false;
  return !graph.failure() && MirLifetimeRelations(program, body, *expanded, graph)
                              .types(actual, expected, 1, 0, allowDerived);
}

bool mirLifetimeReferenceConversion(
    const MirProgram &program, const MirBody &body, std::size_t owner,
    const SemanticType &actual, const SemanticType &expected) {
  if (actual.kind != SemanticType::Reference || expected.kind != SemanticType::Reference ||
      actual.arguments.size() != 1 || expected.arguments.size() != 1)
    return false;
  const auto &declared = bodyLifetimeContract(program, body, owner);
  return verifyLifetimeAssumptionCases(body, owner, declared, [&](const auto &branch) {
    if (mirLifetimeTypeConversionForCase(program, body, owner, actual, expected, branch, true))
      return MirVerificationResult{};
    return MirVerificationResult{{MirVerificationError{
        .bodyKind = body.kind, .owner = owner,
        .message = "MIR reference conversion is not proved in every lifetime assumption case"}}};
  }).valid();
}

bool mirLifetimeValueConversion(
    const MirProgram &program, const MirBody &body, std::size_t owner,
    const SemanticType &actual, const SemanticType &expected) {
  if (actual.kind == SemanticType::Reference || expected.kind == SemanticType::Reference)
    return false;
  const auto &declared = bodyLifetimeContract(program, body, owner);
  return verifyLifetimeAssumptionCases(body, owner, declared, [&](const auto &branch) {
    if (mirLifetimeTypeConversionForCase(program, body, owner, actual, expected, branch, false))
      return MirVerificationResult{};
    return MirVerificationResult{{MirVerificationError{
        .bodyKind = body.kind, .owner = owner,
        .message = "MIR value conversion is not proved in every lifetime assumption case"}}};
  }).valid();
}

static MirVerificationResult verifyMirLifetimeApplicationsForCase(
    const MirProgram &program, const MirBody &body, std::size_t owner,
    const LifetimeSignatureContract &declaredContract) {
  auto result =
      verifyMirLifetimeBodyFactsForCase(program, body, declaredContract, owner);
  if (!result.valid())
    return result;
  const auto expanded = expandedLifetimeAssumptions(program, declaredContract);
  if (!expanded)
    return result; // The body-fact check above already rejects this case.
  LifetimeBounds assumptions;
  for (const auto bound : expanded->bounds)
    if (!assumptions.assume(bound.longer, bound.shorter))
      return {{MirVerificationError{
          .bodyKind = body.kind,
          .owner = owner,
          .message = "MIR lifetime assumptions are malformed"}}};
  LifetimeInferenceConstraints graph(assumptions);
  for (const auto bound : body.lifetimeBody.obligations)
    (void)graph.require(bound.longer, bound.shorter);
  const MirLifetimeRelations relations(program, body, *expanded, graph);
  const auto localAssumptions = withLocalStorageLifetimeFacts(body, *expanded);
  const auto reject = [&](std::string message, MirBlockId block,
                          MirInstructionId instruction = 0) {
    result.errors.push_back({.bodyKind = body.kind,
                             .owner = owner,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };
  for (const auto &block : body.blocks) {
    for (const auto &instruction : block.instructions) {
      if (instruction.tombstone)
        continue;
      if (instruction.kind == MirInstructionKind::CallInput) {
        if (instruction.operands.size() != 1) {
          reject("MIR lifetime input stage has no unique source", block.id, instruction.id);
          continue;
        }
        const auto &input = instruction.operands.front();
        const auto *previous = input.kind == MirOperandKind::Value
            ? relations.definition(input.value) : nullptr;
        const bool previousStage = previous && previous->kind == MirInstructionKind::CallInput;
        // A prior stage is checked by this same exhaustive pass. Its converted
        // result cannot regain an older region by looking through it again.
        const auto actual = previousStage
            ? (previous->tombstone ? std::nullopt : std::optional{previous->info.type})
            : relations.actualType(input);
        const auto &expected = instruction.info.type;
        if (!actual) {
          reject("MIR lifetime input stage has no original source type", block.id, instruction.id);
          continue;
        }
        if (containsLifetimeMetadata(*actual) || containsLifetimeMetadata(expected)) {
          bool proved = false;
          if (expected.kind == SemanticType::Reference) {
            const auto &referent = actual->kind == SemanticType::Reference && actual->arguments.size() == 1
                ? actual->arguments.front() : *actual;
            const bool regionProved = previousStage && actual->kind == SemanticType::Reference
                ? graph.proves(actual->referenceLifetime, expected.referenceLifetime)
                : relations.borrowedOperand(input, expected.referenceLifetime, instruction.hirValue);
            proved = expected.arguments.size() == 1 && regionProved &&
                relations.types(referent, expected.arguments.front(),
                                expected.referenceAccess == AccessMode::Mutable ? 0 : 1);
          } else if (expected.kind == SemanticType::CString &&
                     actual->kind == SemanticType::NullPtr) {
            // Null has no borrowed storage. The existing scalar conversion
            // checks its exact source; static validity covers any stored slot.
            proved = expected.lifetimeArguments.size() == 1 &&
                graph.proves(Lifetime::staticLifetime(), expected.lifetimeArguments.front());
          } else if (expected.kind == SemanticType::CString &&
                     actual->kind == SemanticType::StringView) {
            proved = actual->lifetimeArguments.size() == 1 && expected.lifetimeArguments.size() == 1 &&
                graph.proves(actual->lifetimeArguments.front(), expected.lifetimeArguments.front());
          } else {
            auto source = *actual;
            if (source.kind == SemanticType::RawPointer && expected.kind == SemanticType::RawPointer &&
                source.pointerAccess == AccessMode::Mutable && expected.pointerAccess == AccessMode::ReadOnly)
              source.pointerAccess = AccessMode::ReadOnly;
            const int variance = instruction.callInputKind == HirCallInputKind::MutableBorrow ||
                                 instruction.callInputKind == HirCallInputKind::MutableTemporaryBorrow ? 0 : 1;
            proved = relations.types(source, expected, variance, 0, true);
          }
          if (!proved)
            reject("MIR call-input stage does not prove its original source lifetime conversion",
                   block.id, instruction.id);
        }
        continue;
      }
      if (instruction.kind == MirInstructionKind::Compute &&
          instruction.operation == MirOperation::NativeCallback) {
        const auto *adapter = instruction.nativeCallbackAdapter
            ? program.findNativeCallbackAdapter(*instruction.nativeCallbackAdapter) : nullptr;
        const auto *target = adapter ? program.findFunctionInstance(adapter->target) : nullptr;
        const auto *callable = checkedMirCallableLifetimeSignature(program, instruction);
        const auto &application = instruction.lifetimeApplication;
        if (!adapter || !target || !callable || adapter->type != instruction.info.type ||
            !application.complete || !application.bindings.empty() || !application.variables.empty() ||
            !application.obligations.empty() || !application.typeObligations.empty() ||
            !application.origins.empty() ||
            !mirCallableLifetimeTargetMatches(program, *callable, target->returnType,
                                               target->parameterTypes, target->lifetimeContract))
          reject("MIR native callback formation does not prove its exact target lifetime contract",
                 block.id, instruction.id);
        continue;
      }
      if (instruction.kind == MirInstructionKind::Compute &&
          instruction.operation == MirOperation::PayloadConstruct) {
        const auto view = mirEnumLifetimeView(program, instruction.info.type);
        const auto variant = view && instruction.enumVariant
            ? view->instantiateVariant(*instruction.enumVariant) : std::nullopt;
        if (!view || !view->payload || !variant ||
            instruction.enumOwner != view->payload->declaration ||
            variant->payloadTypes.size() != instruction.operands.size() ||
            instruction.lifetimeApplication != MirLifetimeApplication{}) {
          reject("MIR payload lifetime proof lacks its exact instantiated variant",
                 block.id, instruction.id);
          continue;
        }
        if (!provesNominalLifetimeFormation(program, instruction.info.type, localAssumptions, graph))
          reject("MIR payload construction does not prove its nominal lifetime formation",
                 block.id, instruction.id);
        for (std::size_t index = 0; index < variant->payloadTypes.size(); ++index) {
          const auto &expected = variant->payloadTypes[index];
          const auto actual = relations.actualType(instruction.operands[index]);
          if (!actual) {
            reject("MIR payload lifetime argument has no exact source type", block.id, instruction.id);
            continue;
          }
          if (expected.kind == SemanticType::Reference && expected.referenceLifetime.specified()) {
            const auto &referent = actual->kind == SemanticType::Reference && actual->arguments.size() == 1
                ? actual->arguments.front() : *actual;
            if (expected.arguments.size() != 1 ||
                !relations.borrowedOperand(instruction.operands[index], expected.referenceLifetime) ||
                !relations.types(referent, expected.arguments.front(),
                    expected.referenceAccess == AccessMode::Mutable ? 0 : 1))
              reject("MIR reference payload argument does not prove its stored region",
                     block.id, instruction.id);
          } else if ((containsLifetimeMetadata(*actual) || containsLifetimeMetadata(expected)) &&
                     !relations.types(*actual, expected)) {
            reject("MIR payload value does not prove its stored lifetime conversion",
                   block.id, instruction.id);
          }
        }
        continue;
      }
      if (instruction.kind == MirInstructionKind::Call ||
          instruction.kind == MirInstructionKind::Construct ||
          instruction.kind == MirInstructionKind::InitializeBase) {
        const bool hasTarget = instruction.functionTarget ||
                               instruction.constructorTarget ||
                               instruction.lambdaTarget || instruction.nativeCalleeType ||
                               instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke;
        if (!hasTarget) {
          const auto &application = instruction.lifetimeApplication;
          if (!application.bindings.empty() || !application.variables.empty() ||
              !application.obligations.empty() ||
              !application.typeObligations.empty() ||
              !application.origins.empty())
            reject("MIR lifetime application has no independently checkable "
                   "target contract",
                   block.id, instruction.id);
          continue;
        }
        const auto signature = appliedTargetSignature(program, instruction);
        if (!signature || (instruction.intrinsic != IntrinsicKind::FunctionPointerInvoke &&
                           !instruction.nativeCalleeType && instruction.lifetimeApplication.callableSignature)) {
          reject("MIR call lifetime substitution does not cover its exact "
                 "target contract",
                 block.id, instruction.id);
          continue;
        }
        const auto &application = instruction.lifetimeApplication;
        const auto *targetContract = appliedTargetContract(program, instruction);
        if (!targetContract || !satisfiesLifetimeRequirements(
                program, *targetContract, application.bindings, localAssumptions, graph)) {
          reject("MIR call does not satisfy its exact target lifetime requirement formula",
                 block.id, instruction.id);
          continue;
        }
        const bool requiresRegions =
            containsLifetimeMetadata(signature->returnType) ||
            std::any_of(signature->parameterTypes.begin(),
                        signature->parameterTypes.end(),
                        containsLifetimeMetadata) ||
            !application.bindings.empty() || !application.variables.empty() ||
            !application.obligations.empty() ||
            !application.typeObligations.empty() ||
            !application.origins.empty();
        if (!requiresRegions)
          continue;
        std::vector<SemanticType> parameters;
        for (const auto &parameter : signature->parameterTypes)
          SemanticType::appendExpandedArgument(parameters, parameter);
        std::vector<SemanticType> selected;
        for (const auto &parameter : instruction.parameterTypes)
          SemanticType::appendExpandedArgument(selected, parameter);
        if (parameters != selected ||
            selected.size() != instruction.operands.size()) {
          reject("MIR lifetime-instantiated parameter types differ from the "
                 "exact call inputs",
                 block.id, instruction.id);
          continue;
        }
        for (std::size_t index = 0; index < selected.size(); ++index) {
          // The trusted pointer carrier is borrowed only for invocation. Its
          // exact signature and storage borrow have independent checks; it is
          // not one of the signature's potential result suppliers.
          if (index == 0 && instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke)
            continue;
          const auto &expected = selected[index];
          const auto actual = relations.actualType(instruction.operands[index]);
          if (!actual) {
            reject("MIR call lifetime argument has no exact source type",
                   block.id, instruction.id);
            continue;
          }
          if (expected.kind == SemanticType::Reference &&
              expected.referenceLifetime.specified()) {
            const auto &referent = actual->kind == SemanticType::Reference &&
                                           actual->arguments.size() == 1
                                       ? actual->arguments.front()
                                       : *actual;
            if (expected.arguments.size() != 1 ||
                !relations.borrowedOperand(instruction.operands[index],
                                           expected.referenceLifetime) ||
                !relations.types(
                    referent, expected.arguments.front(),
                    expected.referenceAccess == AccessMode::Mutable ? 0 : 1))
              reject("MIR borrowed argument does not prove its required region "
                     "relationship",
                     block.id, instruction.id);
          } else if ((containsLifetimeMetadata(*actual) ||
                      containsLifetimeMetadata(expected)) &&
                     !relations.types(*actual, expected)) {
            reject("MIR argument type does not prove its required region "
                   "relationship",
                   block.id, instruction.id);
          }
        }
        if (instruction.functionTarget) {
          const auto *target =
              program.findFunctionInstance(*instruction.functionTarget);
          if (target && target->lifetimeContract.receiver.specified() &&
              (!instruction.receiver ||
               !relations.borrowedOperand(
                   *instruction.receiver,
                   applyLifetime(target->lifetimeContract.receiver,
                                 instruction.lifetimeApplication.bindings))))
            reject(
                "MIR receiver does not prove its required region relationship",
                block.id, instruction.id);
          if (target && target->owner && !target->staticMember && instruction.receiver) {
            const auto expectedOwner = instantiateMirCallableOwnerType(
                program, *target, instruction.lifetimeApplication);
            auto actualOwner = relations.actualType(*instruction.receiver);
            if (actualOwner && actualOwner->kind == SemanticType::Reference &&
                actualOwner->arguments.size() == 1)
              actualOwner = SemanticType(actualOwner->arguments.front());
            if (actualOwner && instruction.unsafeOperation == UnsafeOperationKind::RawMember &&
                actualOwner->kind == SemanticType::RawPointer && actualOwner->arguments.size() == 1)
              actualOwner = SemanticType(actualOwner->arguments.front());
            if (!actualOwner || !expectedOwner ||
                ((containsLifetimeMetadata(*actualOwner) || containsLifetimeMetadata(*expectedOwner)) &&
                 !relations.types(*actualOwner, *expectedOwner,
                                  target->receiverMutability == ReceiverMutability::Mutable ? 0 : 1,
                                  0, true)))
              reject("MIR receiver type does not prove its nested lifetime relationships",
                     block.id, instruction.id);
          }
        }
        const auto &returned = signature->returnType;
        if (instruction.kind != MirInstructionKind::InitializeBase &&
            (returned.kind == SemanticType::Reference
                 ? returned.arguments.size() != 1 ||
                       instruction.info.type != returned.arguments.front() ||
                       instruction.info.category != ValueCategory::Place ||
                       instruction.info.access != returned.referenceAccess
                 : instruction.info.type != returned))
          reject("MIR result differs from its lifetime-instantiated signature",
                 block.id, instruction.id);
      }
      if ((instruction.kind == MirInstructionKind::Initialize ||
           instruction.kind == MirInstructionKind::Assign) &&
          instruction.destination && instruction.operands.size() == 1) {
        const auto *destination = body.findPlace(*instruction.destination);
        if (!destination || !containsLifetimeMetadata(destination->type))
          continue;
        const auto &expected = destination->type;
        if (expected.kind == SemanticType::Reference &&
            expected.referenceLifetime.specified()) {
          if (!relations.borrowedOperand(instruction.operands.front(),
                                         expected.referenceLifetime,
                                         instruction.hirValue))
            reject("MIR reference initialization does not prove its declared "
                   "region",
                   block.id, instruction.id);
        } else if (const auto actual =
                       relations.actualType(instruction.operands.front());
                   !actual || !relations.types(*actual, expected)) {
          reject("MIR stored value does not prove its destination lifetime "
                 "contract",
                 block.id, instruction.id);
        }
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Return &&
        block.terminator.value && containsLifetimeMetadata(body.returnType)) {
      if (body.returnType.kind == SemanticType::Reference) {
        if (!relations.borrowedOperand(*block.terminator.value,
                                       body.returnType.referenceLifetime,
                                       block.terminator.hirValue))
          reject("MIR returned reference does not prove its declared result "
                 "region",
                 block.id);
      } else if (const auto actual =
                     relations.actualType(*block.terminator.value);
                 !actual || !relations.types(*actual, body.returnType)) {
        reject(
            "MIR returned value does not prove its declared lifetime contract",
            block.id);
      }
    }
  }
  return result;
}

MirVerificationResult verifyMirLifetimeApplications(
    const MirProgram &program, const MirBody &body, std::size_t owner) {
  const auto &declared = bodyLifetimeContract(program, body, owner);
  return verifyLifetimeAssumptionCases(body, owner, declared, [&](const auto &branch) {
    return verifyMirLifetimeApplicationsForCase(program, body, owner, branch);
  });
}

namespace {
std::vector<SemanticType>
canonicalLifetimeParameters(std::span<const SemanticType> types) {
  std::vector<SemanticType> result;
  for (const auto &type : types)
    SemanticType::appendExpandedArgument(result, type);
  return result;
}
bool appliedSignatureMatches(
    const MirInstruction &instruction,
    const MirInstantiatedLifetimeSignature &signature) {
  if (canonicalLifetimeParameters(instruction.parameterTypes) !=
      canonicalLifetimeParameters(signature.parameterTypes))
    return false;
  const auto &result = signature.returnType;
  return result.kind == SemanticType::Reference
             ? result.arguments.size() == 1 &&
                   instruction.info.type == result.arguments.front() &&
                   instruction.info.category == ValueCategory::Place &&
                   instruction.info.access == result.referenceAccess
             : instruction.info.type == result;
}
} // namespace

bool mirLifetimeParameterTypesMatch(std::span<const SemanticType> selected,
                                    std::span<const SemanticType> declared,
                                    const LifetimeSignatureContract &contract,
                                    const MirLifetimeApplication &application) {
  const auto signature = instantiateMirLifetimeSignature(
      contract, SemanticType::Void, declared, application);
  return signature &&
         canonicalLifetimeParameters(selected) ==
             canonicalLifetimeParameters(signature->parameterTypes);
}

bool mirLifetimeCallSignatureMatches(const MirInstruction &instruction,
                                     const MirFunctionInstance &target) {
  const auto signature = instantiateMirLifetimeSignature(
      target.lifetimeContract, target.returnType, target.parameterTypes,
      instruction.lifetimeApplication);
  return signature && appliedSignatureMatches(instruction, *signature);
}
bool mirLifetimeCallSignatureMatches(const MirInstruction &instruction,
                                     const MirLambdaInstance &target) {
  const auto signature = instantiateMirLifetimeSignature(
      target.lifetimeContract, target.returnType, target.parameterTypes,
      instruction.lifetimeApplication);
  return signature && appliedSignatureMatches(instruction, *signature);
}
std::optional<FunctionBorrowDependencySummary> mirCallableLifetimeBorrowSummary(
    const MirProgram &program, const MirInstruction &instruction) {
  const auto *callable = checkedMirCallableLifetimeSignature(program, instruction);
  if (!callable || instruction.intrinsic != IntrinsicKind::FunctionPointerInvoke ||
      callable->signature.arguments.empty())
    return std::nullopt;
  const auto &result = callable->signature.arguments.front();
  if (result.kind != SemanticType::Reference || result.arguments.size() != 1)
    return std::nullopt;
  LifetimeBounds bounds;
  for (const auto bound : callable->contract.bounds)
    if (!bounds.assume(bound.longer, bound.shorter))
      return std::nullopt;
  FunctionBorrowDependencySummary summary;
  std::vector<SemanticType> parameters;
  for (const auto &parameter : std::span(callable->signature.arguments).subspan(1))
    SemanticType::appendExpandedArgument(parameters, parameter);
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto &parameter = parameters[index];
    if (parameter.kind == SemanticType::Reference && parameter.arguments.size() == 1 &&
        bounds.proves(parameter.referenceLifetime, result.referenceLifetime) &&
        (result.referenceAccess == AccessMode::ReadOnly ||
         parameter.referenceAccess == AccessMode::Mutable))
      summary.dependencies.push_back({.origin = BorrowOriginKind::Argument,
          .parameter = index + 1, .access = result.referenceAccess});
  }
  return summary;
}

bool mirCallableLifetimeTargetMatches(
    const MirProgram &program, const CallableLifetimeSignature &callable,
    const SemanticType &returnType, std::span<const SemanticType> parameters,
    const LifetimeSignatureContract &contract) {
  const auto bindings = callableLifetimeTargetBindings(
      returnType, parameters, contract, callable.signature);
  if (!bindings)
    return false;
  return visitLifetimeAssumptionCases(callable.contract, [&](const auto &branch) {
    const auto assumptions = expandedLifetimeAssumptions(program, branch);
    if (!assumptions)
      return false;
    LifetimeBounds bounds;
    for (const auto edge : assumptions->bounds)
      if (!bounds.assume(edge.longer, edge.shorter))
        return false;
    LifetimeInferenceConstraints graph(bounds);
    for (const auto edge : contract.bounds)
      if (!graph.proves(applyLifetime(edge.longer, *bindings), applyLifetime(edge.shorter, *bindings)))
        return false;
    for (const auto &bound : contract.typeBounds) {
      const auto type = substituteTypeLifetimes(bound.type, *bindings);
      std::vector<SemanticType> pending;
      if (!type || !mirTypeOutlives(program, *type, applyLifetime(bound.region, *bindings),
                                   *assumptions, graph, pending))
        return false;
    }
    return satisfiesLifetimeRequirements(program, contract, *bindings, *assumptions, graph);
  }).proof == LifetimeFormulaProof::Proven;
}

bool mirNativeCallbackLifetimeMatches(const MirProgram &program,
                                      const MirNativeCallbackAdapter &adapter) {
  const auto *target = program.findFunctionInstance(adapter.target);
  if (!target || !adapter.type.hasNativeFunctionShape())
    return false;
  std::vector<GenericParameterId> occupied;
  for (const auto parameter : target->lifetimeContract.parameters)
    occupied.push_back(parameter.id);
  std::size_t work = 0;
  const auto inventory = [&](auto &&self, const SemanticType &type, std::size_t depth) -> bool {
    if (++work > 4096 || depth >= 256)
      return false;
    occupied.insert(occupied.end(), type.quantifiedLifetimes.begin(), type.quantifiedLifetimes.end());
    if (type.referenceLifetime.kind == Lifetime::Kind::Parameter)
      occupied.push_back(type.referenceLifetime.id);
    for (const auto region : type.lifetimeArguments)
      if (region.kind == Lifetime::Kind::Parameter)
        occupied.push_back(region.id);
    bool valid = true;
    lifetime_detail::children(type, [&](const auto &child) { valid = valid && self(self, child, depth + 1); });
    return valid;
  };
  if (!inventory(inventory, adapter.type, 0))
    return false;
  GenericParameterId next = 1;
  const auto callable = makeCallableLifetimeSignature(adapter.type,
      [&](const SemanticType &type) { return mirNominalFormationRequirements(program, type); },
      [&]() {
        while (next && std::find(occupied.begin(), occupied.end(), next) != occupied.end())
          ++next;
        if (!next)
          return Lifetime{};
        const auto id = next++;
        occupied.push_back(id);
        return Lifetime::parameter(id);
      });
  return callable && mirCallableLifetimeTargetMatches(program, *callable,
      target->returnType, target->parameterTypes, target->lifetimeContract);
}

std::optional<MirInstantiatedLifetimeSignature>
instantiateMirLifetimeTarget(const MirProgram &program,
                             const MirInstruction &instruction) {
  return appliedTargetSignature(program, instruction);
}
} // namespace lang
