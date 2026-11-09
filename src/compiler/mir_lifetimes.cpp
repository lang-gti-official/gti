#include "mir_lifetimes.h"
#include "mir_verification_queries.h"
#include "gti/lifetime_types.h"
#include "gti/lifetime_requirements.h"
#include "gti/callable_lifetimes.h"
#include "gti/nominal_variance.h"
#include "gti/generic_type_patterns.h"

namespace lang {
namespace {
// An owner instance with no lifted regions and no region parameters binds no
// regions of its type arguments (an exact specialization): a receiver of the
// same erased shape is that owner whatever regions its arguments spell.
[[nodiscard]] bool regionFreeOwnerMatches(const MirProgram &program,
                                                 const SemanticType &receiver,
                                                 const SemanticType &owner) {
  if (receiver.kind != SemanticType::Class || owner.kind != SemanticType::Class ||
      eraseTypeLifetimes(receiver) != eraseTypeLifetimes(owner))
    return false;
  for (const auto &instance : program.classInstances())
    if (instance.type == owner)
      return instance.liftedLifetimes.empty() &&
             instance.lifetimeContract.parameters.empty() &&
             owner.lifetimeArguments.empty();
  return false;
}

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
      !validLambdaEnvironmentContract(instance.environmentContract, instance.environmentLifetimes) ||
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
      // Re-derive every projected type and region under its complete lexical
      // callable scope. Raw R/Args projections alone would expose invocation
      // binders as free storage types before the recipe could close them.
      GenericSubstitution patternValues;
      patternValues.values = values.values;
      std::vector<SemanticType> patterns;
      for (const auto &pattern : recipe.argumentPatterns) {
        const auto replaced = substituteTypeLifetimes(
            substituteSemanticType(pattern, patternValues), knownRegions);
        const auto selected = replaced ? selectedType(selectedType, *replaced)
                                       : std::nullopt;
        if (!selected)
          return std::nullopt;
        patterns.push_back(*selected);
      }
      const auto matched = deriveGenericArgumentPatternSubstitution(
          patterns, type.arguments, *parameters,
          recipe.argumentPatternLifetimes, std::move(values));
      if (!matched)
        return std::nullopt;
      values = *matched;
    }
    return values;
  };
  const auto instantiate = [&](const SemanticType &type, const GenericSubstitution &values)
      -> std::optional<SemanticType> {
    auto result = substituteSemanticType(type, values);
    if (result.kind == SemanticType::Unknown)
      return std::nullopt;
    // Scoped reconstruction applies its own captures/imports. Ordinary free
    // lifetime substitutions still run once, simultaneously, after that step.
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

bool referenceErasureIntrinsic(IntrinsicKind intrinsic) {
  return intrinsic == IntrinsicKind::EraseReference ||
         intrinsic == IntrinsicKind::EraseReferenceMut ||
         intrinsic == IntrinsicKind::RestoreReference ||
         intrinsic == IntrinsicKind::RestoreReferenceMut;
}
} // namespace

const CallableLifetimeSignature *checkedMirCallableLifetimeSignature(
    const MirProgram &program, const MirInstruction &instruction) {
  if (referenceErasureIntrinsic(instruction.intrinsic)) {
    const bool restoring = instruction.intrinsic == IntrinsicKind::RestoreReference ||
                           instruction.intrinsic == IntrinsicKind::RestoreReferenceMut;
    const auto access = instruction.intrinsic == IntrinsicKind::EraseReferenceMut ||
                                instruction.intrinsic == IntrinsicKind::RestoreReferenceMut
                            ? AccessMode::Mutable : AccessMode::ReadOnly;
    const auto &application = instruction.lifetimeApplication;
    if (instruction.kind != MirInstructionKind::Call || instruction.functionTarget ||
        instruction.constructorTarget || instruction.lambdaTarget || instruction.nativeCalleeType ||
        instruction.receiver || !application.complete || !application.callableSignature)
      return nullptr;
    const auto &callable = *application.callableSignature;
    const auto derived = referenceErasureLifetimeContract(callable.signature, restoring,
        [&](const SemanticType &type) { return mirNominalFormationRequirements(program, type); });
    if (!derived || callable.contract != *derived || !callable.signatureBindings.empty() ||
        callable.signature.arguments.front().referenceAccess != access ||
        callable.contract.parameters.size() != callable.signature.quantifiedLifetimes.size())
      return nullptr;
    for (std::size_t index = 0; index < callable.contract.parameters.size(); ++index)
      if (callable.contract.parameters[index] !=
          Lifetime::parameter(callable.signature.quantifiedLifetimes[index]))
        return nullptr;
    const auto &opaque = callable.signature.arguments[restoring ? 1 : 0].arguments.front();
    if (opaque.kind != SemanticType::Class || !opaque.arguments.empty() ||
        !opaque.valueArguments.empty() || !opaque.lifetimeArguments.empty())
      return nullptr;
    const auto owner = mirClassLifetimeView(program, opaque);
    if (!owner || owner->instance->compilerCapability != CompilerCapabilityTypeKind::ErasedObject)
      return nullptr;
    // The operation and exact capability determine this rule. The attached
    // signature is only a witness: it cannot invent requirements, a stronger
    // result region, receiver facts or authority from an opaque input.
    return &callable;
  }
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
    // General callable type formation can preserve mutable/consuming
    // signatures. This invocation capability still lends a read-only carrier;
    // its operation policy is distinct from signature well-formedness.
    if (signature->kind != SemanticType::FunctionSignature ||
        signature->signatureReceiver != ReceiverMutability::ReadOnly)
      return nullptr;
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
// A projection-free receiver key names the whole receiver object.
[[nodiscard]] bool wholeReceiverKey(const PlaceKey &key) {
  return key.valid() && key.receiver && key.root == 0 && key.projections.empty();
}

// Lowered `This` places carry no separate semantic key, yet a projection-free
// `This` root designates exactly the receiver object, so every such alias is
// that one exact place. Lambda bodies root their environment at `This` and
// never use the receiver key form, so they are excluded.
[[nodiscard]] bool keylessWholeReceiverPlace(const MirBody &body,
                                             const MirPlace &place) {
  return body.kind != MirBodyKind::Lambda && !place.key &&
         place.root == MirPlaceRootKind::This && place.projections.empty() &&
         place.capture == 0;
}

// A field, base or element of the receiver reached without any dereference is
// inline storage of the receiver object and lives exactly as long as it, so
// the whole-receiver origin also proves its validity. A dereference reaches an
// external referent whose region is unrelated and never qualifies.
[[nodiscard]] bool inlineReceiverSubobject(const MirBody &body,
                                           const MirPlace &place) {
  return body.kind != MirBodyKind::Lambda &&
         place.root == MirPlaceRootKind::This && place.capture == 0 &&
         std::all_of(place.projections.begin(), place.projections.end(),
                     [](const MirPlaceProjection &projection) {
                       return projection.kind == MirProjectionKind::Field ||
                              projection.kind == MirProjectionKind::Base ||
                              projection.kind == MirProjectionKind::Index;
                     });
}

template <typename Facts>
bool resolveOrigins(Facts &application, const MirBody &body) {
  for (auto &origin : application.origins) {
    const MirPlace *selected = nullptr;
    if (!origin.key.valid() && origin.source == 0)
      return false;
    const bool wholeReceiver = wholeReceiverKey(origin.key);
    for (const auto &place : body.places) {
      const bool matches =
          origin.key.valid()
              ? (place.key ? *place.key == origin.key
                           : wholeReceiver && keylessWholeReceiverPlace(body, place))
              : place.sourceValue == origin.source &&
                                   place.projections.empty() &&
                                   (place.root == MirPlaceRootKind::Temporary ||
                                    place.root == MirPlaceRootKind::Value ||
                                    // A constant's static storage (an
                                    // enumerator) evaluated for this source.
                                    (place.root == MirPlaceRootKind::Symbol &&
                                     !place.key));
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
    // A keyed origin may end in a pointee step that has no MIR projection on
    // a class (an overloaded pointee accessor or a compiler-owned storage
    // capability's element). Its owner is the exact evaluated class place.
    if (!selected && origin.key.valid() && !origin.key.projections.empty() &&
        origin.key.projections.back().kind == PlaceProjectionKind::Dereference) {
      auto prefix = origin.key;
      while (!prefix.projections.empty() &&
             prefix.projections.back().kind == PlaceProjectionKind::Dereference)
        prefix.projections.pop_back();
      for (const auto &place : body.places)
        if (place.key && *place.key == prefix &&
            place.type.kind == SemanticType::Class) {
          if (selected && selected->id != place.id &&
              !(selected->root == place.root && selected->binding == place.binding &&
                selected->projections == place.projections)) {
            selected = nullptr;
            break;
          }
          if (!selected || place.id < selected->id)
            selected = &place;
        }
    }
    // A keyed origin may name its owner through a retained alias (a range
    // element's iterator) that lowers to no place of its own. Its exact
    // source expression still designates one lowered place: use it.
    if (!selected && origin.key.valid() && origin.source != 0 &&
        origin.key.domain == body.placeDomain) {
      bool ambiguous = false;
      for (const auto &place : body.places) {
        // Never cross place domains: equal identities elsewhere are no evidence.
        if (place.sourceValue != origin.source || place.id == 0 ||
            (place.key && place.key->domain != origin.key.domain))
          continue;
        if (selected && !(selected->key && place.key && *selected->key == *place.key))
          ambiguous = true;
        if (!selected || place.id < selected->id)
          selected = &place;
      }
      if (ambiguous)
        selected = nullptr;
    }
    if (!selected || selected->id == 0 ||
        (origin.place != 0 && origin.place != selected->id)) {
      if (std::getenv("GTI_TRACE_MIR_ORIGIN")) { // TEMP-TRACE
        std::fprintf(stderr, "[origin] keyValid=%d source=%llu selected=%llu originPlace=%llu root=%llu receiver=%d proj=%zu first=%d/%llu domainMatchesBody=%d\n", // TEMP-TRACE
                     origin.key.valid() ? 1 : 0, (unsigned long long)origin.source, // TEMP-TRACE
                     (unsigned long long)(selected ? selected->id : 0), (unsigned long long)origin.place, // TEMP-TRACE
                     (unsigned long long)origin.key.root, origin.key.receiver ? 1 : 0, origin.key.projections.size(), // TEMP-TRACE
                     origin.key.projections.empty() ? -1 : static_cast<int>(origin.key.projections.front().kind), // TEMP-TRACE
                     origin.key.projections.empty() ? 0ull : (unsigned long long)origin.key.projections.front().field, // TEMP-TRACE
                     origin.key.domain == body.placeDomain ? 1 : 0); // TEMP-TRACE
        for (const auto &place : body.places) // TEMP-TRACE
          if ((place.root == MirPlaceRootKind::This && place.projections.empty()) || // TEMP-TRACE
              (origin.source && place.sourceValue == origin.source) || // TEMP-TRACE
              (origin.key.valid() && place.key && place.key->root == origin.key.root)) // TEMP-TRACE
          { std::string kp; if (place.key) for (const auto &pp : place.key->projections) kp += std::to_string(static_cast<int>(pp.kind)) + "/" + std::to_string(pp.field) + "/i" + std::to_string(pp.index) + "/s" + std::to_string(pp.selection) + "/b" + std::to_string(pp.baseOwner) + " "; if (place.key) kp += " dom=" + std::to_string(place.key->domain.snapshot) + ":" + std::to_string(place.key->domain.body) + ":" + std::to_string(place.key->domain.revision) + " odom=" + std::to_string(origin.key.domain.snapshot) + ":" + std::to_string(origin.key.domain.body) + ":" + std::to_string(origin.key.domain.revision) + " oproj=" + (origin.key.projections.empty() ? std::string("-") : std::to_string(origin.key.projections.front().selection) + "/b" + std::to_string(origin.key.projections.front().baseOwner) + "/i" + std::to_string(origin.key.projections.front().index)); // TEMP-TRACE
            std::fprintf(stderr, "[origin]   place=%llu root=%d projections=%zu hasKey=%d keyEq=%d keyproj=[%s] keyrecv=%d keyroot=%llu\n", // TEMP-TRACE
                         (unsigned long long)place.id, static_cast<int>(place.root), place.projections.size(), // TEMP-TRACE
                         place.key ? 1 : 0, place.key && *place.key == origin.key ? 1 : 0, kp.c_str(), place.key ? place.key->receiver : 0, place.key ? (unsigned long long)place.key->root : 0ull); } // TEMP-TRACE
      } // TEMP-TRACE
      return false;
    }
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
// A GTI invocation pointer's first operand lends its code carrier only for
// this invocation. The callable contract describes the later signature
// operands; the carrier's shell is neither a quantified input nor a result
// supplier. Prove that exact operation/endpoint before omitting an outer
// region relationship. Ordinary place, read permission and loan replay still
// validate the input independently.
bool immediatePointerCarrierInput(const MirProgram &program, const MirBody &body,
                                  const MirInstruction &stage,
                                  const SemanticType &actual) {
  const auto &expected = stage.info.type;
  if (!stage.result || !stage.callSite || stage.callInputRole != MirCallInputRole::Argument ||
      stage.callInputIndex != 0 || stage.operands.size() != 1 ||
      (stage.callInputKind != HirCallInputKind::ReadBorrow &&
       stage.callInputKind != HirCallInputKind::ReadTemporaryBorrow) ||
      expected.kind != SemanticType::Reference || expected.arguments.size() != 1 ||
      expected.referenceAccess != AccessMode::ReadOnly || expected.referenceLifetime.specified())
    return false;
  const auto &referent = actual.kind == SemanticType::Reference && actual.arguments.size() == 1
      ? actual.arguments.front() : actual;
  if (!equivalentLifetimeTypes(referent, expected.arguments.front()))
    return false;
  const auto &uses = body.usesOf(*stage.result);
  if (uses.size() != 1 || uses.front().kind != MirValueUseKind::InstructionOperand ||
      uses.front().operandIndex != 0)
    return false;
  const auto *block = body.findBlock(uses.front().block);
  if (!block)
    return false;
  const auto found = std::find_if(block->instructions.begin(), block->instructions.end(),
      [&](const auto &instruction) { return instruction.id == uses.front().instruction; });
  if (found == block->instructions.end())
    return false;
  const auto &call = *found;
  return !call.tombstone && call.kind == MirInstructionKind::Call &&
      call.intrinsic == IntrinsicKind::FunctionPointerInvoke && call.hirValue == stage.callSite &&
      !call.functionTarget && !call.constructorTarget && !call.lambdaTarget && !call.nativeCalleeType &&
      !call.receiver && !call.parameterTypes.empty() && !call.operands.empty() &&
      call.parameterTypes.front() == expected &&
      call.operands.front().kind == MirOperandKind::Value &&
      call.operands.front().value == *stage.result && call.operands.front().type == expected &&
      !(call.borrowOrigin == BorrowOriginKind::Argument && call.borrowArgument == 0) &&
      std::find(call.borrowArguments.begin(), call.borrowArguments.end(), 0) == call.borrowArguments.end() &&
      std::none_of(call.borrowFields.begin(), call.borrowFields.end(), [](const auto &field) {
        return field.origin == BorrowOriginKind::Argument && field.parameter == 0;
      }) && checkedMirCallableLifetimeSignature(program, call);
}

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
    // A binder may shadow a declaration region (a scoped callable result
    // projects an item's binder into the enclosing contract); inside the
    // quantified type the binder is the region named. Re-binding an
    // already quantified name is still malformed.
    if (!region.specified() ||
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
  if (type.kind == SemanticType::Lambda) {
    std::optional<LifetimeRequirementFormula> result;
    for (const auto &instance : program.lambdaInstances()) {
      if (!mirLambdaLifetimeView(instance, type))
        continue;
      const auto actual = lambdaEnvironmentLifetimeRequirements(
          instance.type, type, instance.environmentContract);
      if (!actual || (result && *result != *actual))
        return std::nullopt;
      result = *actual;
    }
    return result;
  }
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
  if (!scopedLifetimeContract(declared)) {
    if (std::getenv("GTI_TRACE_SCOPED")) { // TEMP-TRACE
      const auto dump = [](auto &&self, const SemanticType &t, int d) -> void { // TEMP-TRACE
        std::fprintf(stderr, "%*s kind=%d class=%u refLt=%d:%llu q=%zu lts=", d*2, "", (int)t.kind, (unsigned)t.classId, (int)t.referenceLifetime.kind, (unsigned long long)t.referenceLifetime.id, t.quantifiedLifetimes.size()); for (auto q : t.quantifiedLifetimes) std::fprintf(stderr, "[q%llu]", (unsigned long long)q); // TEMP-TRACE
        for (auto r : t.lifetimeArguments) std::fprintf(stderr, "%d:%u,", (int)r.kind, (unsigned)r.id); // TEMP-TRACE
        std::fprintf(stderr, "\n"); // TEMP-TRACE
        lifetime_detail::children(t, [&](const SemanticType &c) { self(self, c, d+1); }); // TEMP-TRACE
      }; // TEMP-TRACE
      std::fprintf(stderr, "[scoped] owner=%zu params=", owner); // TEMP-TRACE
      for (auto r : declared.parameters) std::fprintf(stderr, "%d:%llu,", (int)r.kind, (unsigned long long)r.id); // TEMP-TRACE
      std::fprintf(stderr, "\n"); // TEMP-TRACE
      for (const auto &b : declared.typeBounds) { std::fprintf(stderr, "[scoped] bound region=%d:%llu ok=%d\n", (int)b.region.kind, (unsigned long long)b.region.id, scopedDeclarationType(b.type, declared.parameters)); dump(dump, b.type, 1); } // TEMP-TRACE
      for (const auto &n : declared.requirements.nodes) if (n.kind == LifetimeRequirementNode::Kind::Type) { std::fprintf(stderr, "[scoped] node ok=%d\n", scopedDeclarationType(n.type, declared.parameters)); dump(dump, n.type, 1); } // TEMP-TRACE
    } // TEMP-TRACE
    return reject("MIR lifetime contract contains a malformed or out-of-scope type region");
  }
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
      function.lifetimeContract, *owner, function.parameterTypes, application,
      &function.ownerType) : std::nullopt;
  return signature ? std::optional{signature->returnType} : std::nullopt;
}

std::optional<MirInstantiatedLifetimeSignature>
instantiateMirLifetimeSignature(const LifetimeSignatureContract &contract,
                                const SemanticType &returnType,
                                std::span<const SemanticType> parameterTypes,
                                const MirLifetimeApplication &application,
                                const SemanticType *receiverOwner) {
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
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
    // Legacy region-free MIR has no lifetime substitution to prove. Preserve
    // its exact signature; this cannot erase a region or an incomplete bound.
    return MirInstantiatedLifetimeSignature{
        returnType, std::vector<SemanticType>(parameterTypes.begin(),
                                              parameterTypes.end())};
  }
  if (!contract.valid() || !contract.requirements.resolved() || !application.complete ||
      application.bindings.size() != contract.parameters.size() ||
      !scopedDeclarationType(returnType, contract.parameters))
    { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
  for (const auto &type : parameterTypes)
    if (!scopedDeclarationType(type, contract.parameters))
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
  for (const auto &bound : contract.typeBounds)
    if (!scopedDeclarationType(bound.type, contract.parameters))
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
  for (const auto &node : contract.requirements.nodes)
    if (node.kind == LifetimeRequirementNode::Kind::Type &&
        !scopedDeclarationType(node.type, contract.parameters))
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
  for (std::size_t index = 0; index < application.variables.size(); ++index) {
    const auto variable = application.variables[index];
    if (variable.kind != Lifetime::Kind::Inference || !variable.specified() ||
        std::find(application.variables.begin(),
                  application.variables.begin() + index,
                  variable) != application.variables.begin() + index)
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
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
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
  }
  for (const auto bound : contract.bounds) {
    const LifetimeBound required{
        applyLifetime(bound.longer, application.bindings),
        applyLifetime(bound.shorter, application.bindings)};
    if (std::find(application.obligations.begin(),
                  application.obligations.end(),
                  required) == application.obligations.end())
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
  }
  for (const auto &bound : contract.typeBounds) {
    // The call's actual argument discharges a bound implied by a reference
    // parameter; semantics records no separate obligation for it.
    if (typeBoundImpliedByReferenceParameter(bound, parameterTypes) ||
        (receiverOwner && typeBoundImpliedByReceiver(bound, contract, *receiverOwner)))
      continue;
    auto type = substituteTypeLifetimes(bound.type, application.bindings);
    if (!type)
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
    const LifetimeTypeBound required{
        std::move(*type), applyLifetime(bound.region, application.bindings)};
    if (std::find(application.typeObligations.begin(),
                  application.typeObligations.end(),
                  required) == application.typeObligations.end())
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) { std::fprintf(stderr, "[ims] fail at %d required kind=%d class=%llu lambda=%llu regions=%zu ref=%d:%llu region=%d:%llu\n", __LINE__, static_cast<int>(required.type.kind), (unsigned long long)required.type.classId, (unsigned long long)required.type.lambdaId, required.type.lifetimeArguments.size(), static_cast<int>(required.type.referenceLifetime.kind), (unsigned long long)required.type.referenceLifetime.id, static_cast<int>(required.region.kind), (unsigned long long)required.region.id); for (const auto &have : application.typeObligations) { std::fprintf(stderr, "[ims]   have kind=%d class=%llu lambda=%llu regions=%zu region=%d:%llu eqErased=%d\n", static_cast<int>(have.type.kind), (unsigned long long)have.type.classId, (unsigned long long)have.type.lambdaId, have.type.lifetimeArguments.size(), static_cast<int>(have.region.kind), (unsigned long long)have.region.id, eraseTypeLifetimes(have.type) == eraseTypeLifetimes(required.type)); for (auto r : have.type.lifetimeArguments) std::fprintf(stderr, "[ims]     arg %d:%llu\n", static_cast<int>(r.kind), (unsigned long long)r.id); } for (auto r : required.type.lifetimeArguments) std::fprintf(stderr, "[ims]   req arg %d:%llu\n", static_cast<int>(r.kind), (unsigned long long)r.id); for (const auto &a : required.type.arguments) std::fprintf(stderr, "[ims]   req targ kind=%d gp=%llu\n", static_cast<int>(a.kind), (unsigned long long)a.genericParameterId); for (const auto &have : application.typeObligations) for (const auto &a : have.type.arguments) std::fprintf(stderr, "[ims]   have targ kind=%d gp=%llu\n", static_cast<int>(a.kind), (unsigned long long)a.genericParameterId); if (receiverOwner) { std::fprintf(stderr, "[ims]   owner class=%llu\n", (unsigned long long)receiverOwner->classId); for (const auto &a : receiverOwner->arguments) std::fprintf(stderr, "[ims]   owner targ kind=%d\n", static_cast<int>(a.kind)); } std::fprintf(stderr, "[ims]   contract receiver=%d:%llu bound.region=%d:%llu\n", static_cast<int>(contract.receiver.kind), (unsigned long long)contract.receiver.id, static_cast<int>(bound.region.kind), (unsigned long long)bound.region.id); } return std::nullopt; } // TEMP-TRACE
  }
  auto result = substituteTypeLifetimes(returnType, application.bindings);
  if (!result)
    { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
  MirInstantiatedLifetimeSignature signature{.returnType = std::move(*result)};
  for (const auto &parameter : parameterTypes) {
    auto type = substituteTypeLifetimes(parameter, application.bindings);
    if (!type)
      { if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[ims] fail at %d\n", __LINE__); return std::nullopt; } // TEMP-TRACE
    signature.parameterTypes.push_back(std::move(*type));
  }
  return signature;
}
namespace {
// An opaque address has no checked pointee storage dependency. Keep structural
// and scoped type validation separate from that outlives fact: unknown types
// and malformed pointer shells cannot acquire proof by avoiding recursion.
bool rawPointerStorageShape(const SemanticType &type) {
  if (type.kind != SemanticType::RawPointer)
    return false;
  const auto known = [&](auto &&self, const SemanticType &current,
                         std::size_t depth) -> bool {
    if (depth >= 256 || current.kind == SemanticType::Unknown)
      return false;
    if (current.kind == SemanticType::RawPointer &&
        (current.arguments.size() != 1 || !current.lifetimeArguments.empty() ||
         !current.quantifiedLifetimes.empty() || current.referenceLifetime != Lifetime{}))
      return false;
    bool valid = true;
    lifetime_detail::children(current, [&](const SemanticType &child) {
      valid = self(self, child, depth + 1) && valid;
    });
    return valid;
  };
  return known(known, type, 0);
}
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
    if (type.kind == SemanticType::RawPointer) {
      if (!rawPointerStorageShape(type))
        return std::nullopt;
      continue; // T*:a is never a premise about T or its checked references.
    }
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
  if (type.kind == SemanticType::RawPointer)
    return rawPointerStorageShape(type);
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
  if (type.kind == SemanticType::Lambda) {
    const auto formation = mirNominalFormationRequirements(program, type);
    return formation && evaluateLifetimeRequirements(*formation, [&](const auto &node) {
      if (node.kind == LifetimeRequirementNode::Kind::Region)
        return graph.proves(node.bound.longer, node.bound.shorter);
      std::vector<SemanticType> pending;
      return mirTypeOutlives(program, node.type, node.bound.shorter, assumptions, graph, pending);
    }).value_or(false);
  }
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
    const MirProgram &program, const MirBody &body,
    LifetimeSignatureContract assumptions) {
  const auto add = [&](const SemanticType &type, Lifetime region) {
    const LifetimeTypeBound bound{type, region};
    if (std::find(assumptions.typeBounds.begin(), assumptions.typeBounds.end(), bound) ==
        assumptions.typeBounds.end())
      assumptions.typeBounds.push_back(bound);
  };
  std::vector<LifetimeTypeBound> roots;
  // Preserve every independently mapped direct premise before spending any
  // budget on optional containment inference. Many unrelated local origins
  // cannot invalidate a fact already established by their exact storage.
  for (const auto &origin : body.lifetimeBody.origins) {
    const auto *place = body.findPlace(origin.place);
    if (origin.region.kind != Lifetime::Kind::Local || !place ||
        place->foreignStorage || place->type.kind == SemanticType::Unknown)
      continue;
    add(place->type, origin.region);
    if (place->type.kind == SemanticType::Class)
      roots.push_back({place->type, origin.region});
    // A checked reference origin already designates its referent, unlike an
    // arbitrary reference field encountered inside that referent's storage.
    if (place->type.kind == SemanticType::Reference && place->type.arguments.size() == 1) {
      const auto &referent = place->type.arguments.front();
      add(referent, origin.region);
      if (referent.kind == SemanticType::Class)
        roots.push_back({referent, origin.region});
    }
  }
  std::vector<LifetimeTypeBound> visited;
  std::size_t remaining = 4096;
  const auto expand = [&](const auto &self, const SemanticType &type,
                          Lifetime region, std::size_t depth) -> void {
    // These are additional proof facts, not a required complete inventory.
    // A missing view or exhausted budget adds no premise and never erases a
    // direct fact; an obligation needing an unavailable fact still fails.
    if (type.kind != SemanticType::Class || remaining == 0 || depth >= 256)
      return;
    const LifetimeTypeBound bound{type, region};
    if (std::find(visited.begin(), visited.end(), bound) != visited.end())
      return;
    --remaining;
    visited.push_back(bound);
    const auto view = mirClassLifetimeView(program, type);
    if (!view || view->instance->kind == ClassKind::Union ||
        view->instance->kind == ClassKind::Interface)
      return;
    const auto child = [&](const SemanticType &declared) {
      if (remaining == 0) return;
      --remaining;
      const auto actual = view->instantiate(declared);
      if (!actual || actual->kind == SemanticType::Unknown) return;
      add(*actual, region);
      self(self, *actual, region, depth + 1);
    };
    // Only actual inline fields and state bases are physical subobjects.
    // References, raw pointers and callable values stop this traversal;
    // nominal arguments are never an inventory of stored values.
    for (const auto &field : view->instance->declaredFields) {
      if (remaining == 0) return;
      child(field.type);
    }
    for (const auto &base : view->instance->structuralBases) {
      if (remaining == 0) return;
      if (!base.interface) child(base.type);
    }
  };
  for (const auto &root : roots) {
    if (remaining == 0) break;
    expand(expand, root.type, root.region, 0);
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
    {
      if (std::getenv("GTI_TRACE_MIR_ORIGIN")) { // TEMP-TRACE
        std::fprintf(stderr, "[origin-unique] index=%zu region=%d:%llu keyValid=%d keyDomain=%d place=%llu source=%llu\n", index, static_cast<int>(origin.region.kind), (unsigned long long)origin.region.id, origin.key.valid(), origin.key.valid() && origin.key.domain == body.placeDomain, (unsigned long long)origin.place, (unsigned long long)origin.source); // TEMP-TRACE
        for (const auto &other : facts.origins) std::fprintf(stderr, "[origin-unique]   region=%d:%llu place=%llu source=%llu root=%llu\n", static_cast<int>(other.region.kind), (unsigned long long)other.region.id, (unsigned long long)other.place, (unsigned long long)other.source, (unsigned long long)other.key.root); // TEMP-TRACE
      } // TEMP-TRACE
      reject("MIR local lifetime has no unique body-owned origin witness");
    }
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
      if (!known(region)) {
        if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
          { std::string ps; for (const auto p : contract.parameters) ps += " " + std::to_string(p.id); // TEMP-TRACE
          std::fprintf(stderr, "[scope] region=%d:%llu typeKind=%d class=%llu params=[%s] vars=%zu ret=%d\n", static_cast<int>(region.kind), (unsigned long long)region.id, static_cast<int>(type.kind), (unsigned long long)type.classId, ps.c_str(), facts.variables.size(), &type == &body.returnType); } // TEMP-TRACE
        reject(
            "MIR body type contains an unallocated or out-of-scope lifetime");
      }
  };
  checkTypeRegions(body.returnType);
  for (const auto &place : body.places)
    checkTypeRegions(place.type);
  // A published program constant is static data: its literal-backed text and
  // passive fields hold no body-local borrow, so its regions are closed.
  const auto publishedConstant = [&](const MirValue &value) {
    for (const auto &block : body.blocks)
      for (const auto &instruction : block.instructions)
        if (instruction.id == value.definition)
          return instruction.programConstantSubstitution;
    return false;
  };
  for (const auto &value : body.values) {
    if (publishedConstant(value))
      continue;
    if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
      for (const auto region : freeTypeLifetimes(value.info.type)) // TEMP-TRACE
        if (!known(region)) { // TEMP-TRACE
          const auto *def = [&]() -> const MirInstruction * { for (const auto &b : body.blocks) for (const auto &i : b.instructions) if (i.id == value.definition) return &i; return nullptr; }(); // TEMP-TRACE
          std::fprintf(stderr, "[scope-value] v%llu hir=%llu def kind=%d fn=%d callInputRole=%d intrinsic=%d\n", (unsigned long long)value.id, (unsigned long long)value.sourceValue, def ? static_cast<int>(def->kind) : -1, def && def->functionTarget ? 1 : 0, def && def->callInputRole ? static_cast<int>(*def->callInputRole) : -1, def ? static_cast<int>(def->intrinsic) : -1); // TEMP-TRACE
        } // TEMP-TRACE
    checkTypeRegions(value.info.type);
  }
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
             (instruction.operation == MirOperation::NativeCallback ||
              instruction.operation == MirOperation::Closure)))) {
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
          { if (std::getenv("GTI_TRACE_SCOPED")) { std::fprintf(stderr, "[missing-ob] inst=%llu %d:%llu >= %d:%llu\n", (unsigned long long)instruction.id, (int)bound.longer.kind, (unsigned long long)bound.longer.id, (int)bound.shorter.kind, (unsigned long long)bound.shorter.id); for (const auto &o : application.origins) std::fprintf(stderr, "[missing-ob]  app origin %d:%llu src=%llu\n", (int)o.region.kind, (unsigned long long)o.region.id, (unsigned long long)o.source); for (const auto &o : facts.origins) std::fprintf(stderr, "[missing-ob]  body origin %d:%llu src=%llu\n", (int)o.region.kind, (unsigned long long)o.region.id, (unsigned long long)o.source); for (const auto &b : facts.obligations) std::fprintf(stderr, "[missing-ob]  body %d:%llu >= %d:%llu\n", (int)b.longer.kind, (unsigned long long)b.longer.id, (int)b.shorter.kind, (unsigned long long)b.shorter.id); } // TEMP-TRACE
          reject("MIR call lifetime obligation is missing from the body graph",
                 block.id, instruction.id); }
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
  const auto localAssumptions = withLocalStorageLifetimeFacts(program, body, contract);
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
std::optional<LifetimeSignatureContract> bodyLifetimeContract(const MirProgram &program,
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
  if (body.kind == MirBodyKind::Lambda) {
    const auto *instance = program.findLambda(owner);
    return instance && mirLambdaLifetimeView(*instance, instance->type)
        ? composeLambdaLifetimeContract(instance->environmentContract, instance->lifetimeContract)
        : std::nullopt;
  }
  return LifetimeSignatureContract{};
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
        instruction.lifetimeApplication, target->owner ? &target->ownerType : nullptr);
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
  if (instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke || instruction.nativeCalleeType ||
      referenceErasureIntrinsic(instruction.intrinsic)) {
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
    if (operand.kind == MirOperandKind::Value)
      if (const auto *stage = definition(operand.value);
          stage && stage->kind == MirInstructionKind::CallInput &&
          stage->callInputRole == MirCallInputRole::Receiver &&
          stage->preparedParameterDrop) {
        // An owning receiver stage materializes new storage. Its borrow is
        // bounded by that exact prepared destination, not the pre-transfer
        // operand. Ordered-input and lifecycle verification independently
        // prove source transfer, unique consumption and cleanup of this drop.
        const auto *place = stage->destination
                                ? body.findPlace(*stage->destination) : nullptr;
        const auto *drop = body.findDropObligation(*stage->preparedParameterDrop);
        if (stage->tombstone || !stage->result || *stage->result != operand.value ||
            stage->hirValue == 0 || stage->callSite == 0 ||
            stage->operands.size() != 1 || stage->info.type != operand.type ||
            stage->info.type.kind == SemanticType::Reference ||
            (stage->callInputKind != HirCallInputKind::MoveValue &&
             stage->callInputKind != HirCallInputKind::CopyValue) ||
            !place || place->root != MirPlaceRootKind::Temporary ||
            place->temporary == 0 || !place->projections.empty() ||
            place->sourceValue != stage->hirValue || place->type != stage->info.type ||
            place->foreignStorage || place->initiallyAvailable || !drop ||
            drop->kind != MirDropObligationKind::PreparedParameter ||
            drop->place != place->id || drop->dropType.type != place->type ||
            drop->initiallyActive ||
            std::count_if(stage->lifecycle.begin(), stage->lifecycle.end(),
                [&](const MirLifecycleEvent &event) {
                  return (event.kind == MirLifecycleEventKind::Initialize ||
                          event.kind == MirLifecycleEventKind::Reparent) &&
                         event.target == drop->id && !event.conditional &&
                         !event.failureCleanup;
                }) != 1)
          return false;
        return std::any_of(body.lifetimeBody.origins.begin(), body.lifetimeBody.origins.end(),
            [&](const MirLifetimeOrigin &origin) {
              return origin.place == place->id && origin.source == stage->hirValue &&
                     graph.proves(origin.region, target);
            });
      }
    // A consuming receiver moved or copied directly (no prepared temporary)
    // is witnessed by the origin semantics gave that exact staged value; its
    // region must still cover the required receiver region.
    if (operand.kind == MirOperandKind::Value)
      if (const auto *stage = definition(operand.value);
          stage && !stage->tombstone && stage->kind == MirInstructionKind::CallInput &&
          stage->callInputRole == MirCallInputRole::Receiver &&
          !stage->preparedParameterDrop && stage->result == operand.value &&
          stage->operands.size() == 1 && stage->hirValue != 0 &&
          stage->info.type.kind != SemanticType::Reference &&
          (stage->callInputKind == HirCallInputKind::MoveValue ||
           stage->callInputKind == HirCallInputKind::CopyValue)) {
        const auto *input = stage->operands.front().kind == MirOperandKind::Value
            ? definition(stage->operands.front().value) : nullptr;
        if (std::any_of(body.lifetimeBody.origins.begin(), body.lifetimeBody.origins.end(),
                [&](const MirLifetimeOrigin &origin) {
                  return origin.source != 0 &&
                         (origin.source == stage->hirValue ||
                          (input && origin.source == input->hirValue)) &&
                         graph.proves(origin.region, target);
                }))
          return true;
      }
    const auto *actual = sourceOperand(operand, source);
    if (std::getenv("GTI_TRACE_MIR_ORIGIN")) { // TEMP-TRACE
      const auto *ap = actual && actual->place ? body.findPlace(actual->place) : nullptr; // TEMP-TRACE
      std::fprintf(stderr, "[borrowedOperand] source=%llu target=%d:%llu operandKind=%d actual=%d actualKind=%d place=p%llu root=%d loan=%llu\n", (unsigned long long)source, static_cast<int>(target.kind), (unsigned long long)target.id, static_cast<int>(operand.kind), actual != nullptr, actual ? static_cast<int>(actual->kind) : -1, actual ? (unsigned long long)actual->place : 0ull, ap ? static_cast<int>(ap->root) : -1, actual ? (unsigned long long)actual->loan : 0ull); // TEMP-TRACE
    } // TEMP-TRACE
    if (!actual)
      return false;
    // A receiver stage typed as its object (a borrowed-place receiver of a
    // method) lends exactly the borrowed place; that place's own checked
    // region or origin is the proof. Owning (prepared) receivers are above.
    if (actual->kind == MirOperandKind::Value)
      if (const auto *stage = definition(actual->value);
          stage && !stage->tombstone && stage->kind == MirInstructionKind::CallInput &&
          stage->callInputRole == MirCallInputRole::Receiver &&
          stage->info.type.kind != SemanticType::Reference &&
          !stage->preparedParameterDrop && stage->operands.size() == 1 &&
          (stage->operands.front().kind == MirOperandKind::BorrowRead ||
           stage->operands.front().kind == MirOperandKind::BorrowWrite) &&
          stage->operands.front().place)
        if (const auto *place = body.findPlace(stage->operands.front().place)) {
          const auto known = declaredPlaceRegion(*place);
          if (known.specified() && graph.proves(known, target))
            return true;
          if (!known.specified() &&
              (originPlaceProves(*place, target) || loanSubobjectProves(*place, target)))
            return true;
          return aliasReferentProves(*place, target);
        }
    if (actual->kind == MirOperandKind::Value)
      if (const auto *stage = definition(actual->value);
          stage && stage->kind == MirInstructionKind::CallInput &&
          stage->info.type.kind == SemanticType::Reference) {
        if (std::getenv("GTI_TRACE_MIR_ORIGIN")) { // TEMP-TRACE
          const auto *op = stage->operands.empty() ? nullptr : &stage->operands.front(); // TEMP-TRACE
          const auto *pl = op && op->place ? body.findPlace(op->place) : nullptr; // TEMP-TRACE
          std::fprintf(stderr, "[recv]   stage role=%d lifetime=%d:%llu ops=%zu opKind=%d opPlace=%llu root=%d proj=%zu\n", // TEMP-TRACE
                       stage->callInputRole ? static_cast<int>(*stage->callInputRole) : -1, static_cast<int>(stage->info.type.referenceLifetime.kind), // TEMP-TRACE
                       (unsigned long long)stage->info.type.referenceLifetime.id, stage->operands.size(), // TEMP-TRACE
                       op ? static_cast<int>(op->kind) : -1, op ? (unsigned long long)op->place : 0ull, // TEMP-TRACE
                       pl ? static_cast<int>(pl->root) : -1, pl ? pl->projections.size() : 0); // TEMP-TRACE
        } // TEMP-TRACE
        if (stage->tombstone)
          return false;
        if (graph.proves(stage->info.type.referenceLifetime, target))
          return true;
        // A receiver stage that directly borrows a place lends exactly that
        // place for this call. Its own region may be a fresh placeholder;
        // the place's checked origin then supplies the validity proof. This
        // never applies to an argument stage or a checked expression region.
        if (stage->callInputRole == MirCallInputRole::Receiver &&
            stage->operands.size() == 1) {
          const auto &input = stage->operands.front();
          if ((input.kind == MirOperandKind::BorrowRead ||
               input.kind == MirOperandKind::BorrowWrite) && input.place)
            if (const auto *place = body.findPlace(input.place))
              return originPlaceProves(*place, target);
        }
        return false;
      }
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
            if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
              std::fprintf(stderr, "[borrowedOperand]  source call sig=%d ret=%d region=%d:%llu bindings=%zu\n", signature.has_value(), signature ? static_cast<int>(signature->returnType.kind) : -1, signature ? static_cast<int>(signature->returnType.referenceLifetime.kind) : -1, signature ? (unsigned long long)signature->returnType.referenceLifetime.id : 0ull, instruction.lifetimeApplication.bindings.size()); // TEMP-TRACE
            if (signature &&
                signature->returnType.kind == SemanticType::Reference)
              return graph.proves(signature->returnType.referenceLifetime,
                                  target);
          }
      std::optional<Lifetime> selected;
      for (const auto &place : body.places)
        if (place.sourceValue == source &&
            (!actual->place || place.id == actual->place)) {
          // One expression can also supply deeper projections with different
          // external regions. An addressed operand selects its exact place;
          // those other projections cannot replace or contradict that view.
          const auto known = declaredPlaceRegion(place);
          if (known.specified()) {
            if (selected && *selected != known)
              return false;
            selected = known;
          }
        }
      if (selected) {
        if (graph.proves(*selected, target))
          return true;
        const auto *place = actual->place ? body.findPlace(actual->place) : nullptr;
        return place && aliasReferentProves(*place, target);
      }
    }
    // A selected stored reference may use its carrier's abstract entry place
    // for origin bookkeeping. That place's outer storage region does not name
    // the selected external referent. Replay the exact typed extraction first.
    if (actual->kind == MirOperandKind::Loan)
      if (const auto *loan = body.findLoan(actual->loan);
          loan && loan->referenceSelection && loan->referenceSelection->kind ==
              ReferenceSelectionKind::StoredExternalReference)
        return loanProves(actual->loan, target, 0);
    if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
      std::fprintf(stderr, "[borrowedOperand]  reached place block place=%llu\n", (unsigned long long)actual->place); // TEMP-TRACE
    if (actual->place)
      if (const auto *place = body.findPlace(actual->place)) {
        const auto known = declaredPlaceRegion(*place);
        if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
          std::fprintf(stderr, "[borrowedOperand]  place p%llu known=%d:%llu origin=%d sub=%d\n", (unsigned long long)place->id, static_cast<int>(known.kind), (unsigned long long)known.id, originPlaceProves(*place, target), loanSubobjectProves(*place, target)); // TEMP-TRACE
        if (known.specified())
          return graph.proves(known, target) || aliasReferentProves(*place, target);
        if (originPlaceProves(*place, target))
          return true;
        if (loanSubobjectProves(*place, target))
          return true;
        if (aliasReferentProves(*place, target))
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
    if (depth == 0 && actual == SemanticType::NullPtr &&
        expected.kind == SemanticType::RawPointer)
      // Null initializes the already-scoped pointer type without supplying
      // pointee storage or any region relationship. This is only a whole
      // value conversion; nested pointer/type arguments remain invariant.
      return variance > 0 && rawPointerStorageShape(expected) &&
             isImplicitScalarInitializationConversion(expected, actual);
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
        // Detach the selected child before replacing its containing type.
        // Assigning directly from the first child can overwrite that child's
        // region while the parent's argument vector is being assigned.
        type = SemanticType(type.arguments[*projection.constantIndex]);
      } else if (projection.kind == MirProjectionKind::Base) {
        const auto owner = mirClassLifetimeView(program, type);
        if (!owner)
          return {};
        if (!projection.constantIndex || projection.field || projection.index ||
            *projection.constantIndex >= owner->instance->structuralBases.size())
          return {};
        const auto &base = owner->instance->structuralBases[*projection.constantIndex];
        if (base.interface || base.instance != projection.selection)
          return {};
        const auto instantiated = owner->instantiate(base.type);
        const auto selected = instantiated ? mirClassLifetimeView(program, *instantiated) : std::nullopt;
        if (!selected || selected->instance->id != projection.selection)
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
    const bool receiverAlias = inlineReceiverSubobject(body, place);
    for (const auto &origin : body.lifetimeBody.origins) {
      // Distinct MIR places with one exact key name the same storage.
      const auto *originPlace = origin.place ? body.findPlace(origin.place) : nullptr;
      const bool sameKey = place.key && originPlace && originPlace->key &&
                           *originPlace->key == *place.key;
      if ((origin.place == place.id || sameKey ||
           (place.key && origin.key.valid() && *place.key == origin.key) ||
           (receiverAlias && wholeReceiverKey(origin.key))) &&
          graph.proves(origin.region, target))
        return true;
    }
    return false;
  }

  // The exact referent of a reference binding that carries one body-local
  // loan (possibly a reborrow) is that loan's borrowed place. Semantics may check a desugared call
  // (a range loop's begin/end over its hidden range alias) against that
  // place's own origin; the loan's liveness is verified separately.
  bool aliasReferentProves(const MirPlace &place, Lifetime target) const {
    if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
      std::fprintf(stderr, "[alias] enter place=p%llu root=%d binding=%llu proj=%zu kind0=%d\n", (unsigned long long)place.id, static_cast<int>(place.root), (unsigned long long)place.binding, place.projections.size(), place.projections.empty() ? -1 : static_cast<int>(place.projections.front().kind)); // TEMP-TRACE
    // The referent itself, or an inline field/element subobject of it, which
    // lives exactly as long as that referent.
    if (place.root != MirPlaceRootKind::Binding || !place.binding ||
        place.projections.empty() ||
        place.projections.front().kind != MirProjectionKind::Dereference ||
        !std::all_of(place.projections.begin() + 1, place.projections.end(),
                     [](const auto &projection) {
                       return projection.kind == MirProjectionKind::Field ||
                              projection.kind == MirProjectionKind::Index;
                     }))
      return false;
    const MirLoan *carried = nullptr;
    for (const auto &loan : body.loans)
      if (std::find(loan.carriers.begin(), loan.carriers.end(), place.binding) !=
          loan.carriers.end()) {
        if (carried)
          return false;
        carried = &loan;
      }
    if (std::getenv("GTI_TRACE_MIR_ORIGIN") && carried) // TEMP-TRACE
      std::fprintf(stderr, "[alias] carried loan=%llu kind=%d escapes=%d entry=%d parent=%llu extra=%zu source=p%llu fields=%zu\n", (unsigned long long)carried->id, static_cast<int>(carried->kind), carried->escapes, carried->entry, (unsigned long long)carried->parent, carried->additionalDependencies.size(), (unsigned long long)carried->source, carried->designatedFields.size()); // TEMP-TRACE
    // A binding bound to a call's reference result (a range element from
    // `*it`) designates that result's referent: the call loan's own proof.
    if (carried && carried->kind == MirLoanKind::CallResult && !carried->escapes &&
        !carried->entry)
      return loanProves(carried->id, target, 0);
    if (!carried || carried->kind != MirLoanKind::Local || carried->escapes ||
        carried->entry || !carried->designatedFields.empty() ||
        !carried->additionalDependencies.empty())
      return false;
    const auto *source = body.findPlace(carried->source);
    if (source) {
      // The borrowed place's own declared region (a stored reference field's
      // referent) or exact origin is the proof, exactly as for that place.
      const auto known = declaredPlaceRegion(*source);
      if (known.specified())
        return graph.proves(known, target);
    }
    if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
      std::fprintf(stderr, "[alias] place=p%llu loan=%llu source=p%llu proves=%d target=%d:%llu\n", (unsigned long long)place.id, (unsigned long long)carried->id, (unsigned long long)carried->source, source && originPlaceProves(*source, target), static_cast<int>(target.kind), (unsigned long long)target.id); // TEMP-TRACE
    if (source && originPlaceProves(*source, target))
      return true;
    // A local reborrow's referent lies within its parent loan's referent, so
    // the parent's proven region also covers this alias (a range binding
    // reborrowing a reference formal's entry).
    return carried->parent != 0 && loanProves(carried->parent, target, 0);
  }

  // A loan's referent, and any inline field chain over it, lives in exactly
  // that loan's region; a dereference, index or payload selection needs its
  // own proof.
  bool loanSubobjectProves(const MirPlace &place, Lifetime target,
                           std::size_t depth = 0) const {
    return place.root == MirPlaceRootKind::Loan && place.loan &&
           std::all_of(place.projections.begin(), place.projections.end(),
                       [](const auto &part) { return part.kind == MirProjectionKind::Field; }) &&
           loanProves(place.loan, target, depth + 1);
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
    if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
      std::fprintf(stderr, "[loanProves] loan=%llu kind=%d source=p%llu parent=%llu target=%d:%llu depth=%zu\n", (unsigned long long)id, static_cast<int>(loan->kind), (unsigned long long)loan->source, (unsigned long long)loan->parent, static_cast<int>(target.kind), (unsigned long long)target.id, depth); // TEMP-TRACE
    bool signatureChecked = false;
    if (loan->producedBy)
      for (const auto &block : body.blocks)
        for (const auto &instruction : block.instructions)
          if (instruction.hirValue == loan->producedBy &&
              instruction.kind == MirInstructionKind::Call) {
            const auto signature = appliedTargetSignature(program, instruction);
            if (signature &&
                signature->returnType.kind == SemanticType::Reference) {
              if (graph.proves(signature->returnType.referenceLifetime, target))
                return true;
              signatureChecked = true;
            }
          }
    // A call result's referent is derived from its borrowed source (the
    // summarized receiver or argument place). When the body's facts relate
    // that source, rather than the call's own result slot, to the target,
    // the source's checked region is the proof, as for any other loan.
    if (signatureChecked && loan->kind != MirLoanKind::CallResult)
      return false;
    const auto sourceProves = [&](MirPlaceId id) {
      const auto *place = body.findPlace(id);
      if (!place)
        return false;
      const auto region = declaredPlaceRegion(*place);
      return region.specified() ? graph.proves(region, target)
                                : originPlaceProves(*place, target) ||
                                      loanSubobjectProves(*place, target, depth);
    };
    if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
      std::fprintf(stderr, "[loanProves]  loan=%llu signatureChecked=%d sourceProves=%d\n", (unsigned long long)id, signatureChecked, loan->source ? sourceProves(loan->source) : -1); // TEMP-TRACE
    bool proved = loan->source   ? sourceProves(loan->source)
                  : loan->parent ? loanProves(loan->parent, target, depth + 1)
                                 : false;
    for (const auto &dependency : loan->additionalDependencies)
      proved = (dependency.source ? sourceProves(dependency.source)
                : dependency.parent
                    ? loanProves(dependency.parent, target, depth + 1)
                    : false) &&
               proved;
    // A call result whose referent lives in a retained stored group (an
    // iterator's element in its range's storage) is also valid wherever that
    // group's own source is; semantics witnesses that retained place.
    // The same holds when the result's only other suppliers are that group's
    // own stored edge, the path a whole-receiver result forwards through it.
    if (!proved && loan->kind == MirLoanKind::CallResult && loan->storedDependency &&
        std::all_of(loan->additionalDependencies.begin(),
                    loan->additionalDependencies.end(),
                    [&](const MirLoanDependency &dependency) {
                      const auto *group = body.findLoan(loan->storedDependency);
                      return group && dependency.parent == group->id &&
                             dependency.source == group->source;
                    }))
      proved = loanProves(loan->storedDependency, target, depth + 1);
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
  const auto declared = bodyLifetimeContract(program, body, owner);
  if (!declared)
    return false;
  return verifyLifetimeAssumptionCases(body, owner, *declared, [&](const auto &branch) {
    if (mirLifetimeTypeConversionForCase(program, body, owner, actual, expected, branch, true))
      return MirVerificationResult{};
    return MirVerificationResult{{MirVerificationError{
        .bodyKind = body.kind, .owner = owner,
        .message = "MIR reference conversion is not proved in every lifetime assumption case"}}};
  }).valid();
}

namespace {
// A call may denote its returned reference by the CallResult loan it produces
// while the instruction itself carries the referent value. An overloaded
// subscript store does this: the stored value is committed through that loan
// after the call. The reference shape is then that exact loan's access, never
// an assumption from the instruction's value category.
bool publishesCallResultLoan(const MirBody &body, const MirInstruction &instruction,
                             AccessMode access) {
  if (instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.hirValue == 0)
    return false;
  std::size_t published = 0;
  for (const auto &loan : body.loans)
    if (loan.kind == MirLoanKind::CallResult && loan.producedBy == instruction.hirValue) {
      if (loan.access != access)
        return false;
      ++published;
    }
  return published == 1;
}
} // namespace

bool mirLifetimeValueConversion(
    const MirProgram &program, const MirBody &body, std::size_t owner,
    const SemanticType &actual, const SemanticType &expected) {
  if (actual.kind == SemanticType::Reference || expected.kind == SemanticType::Reference)
    return false;
  const auto declared = bodyLifetimeContract(program, body, owner);
  if (!declared)
    return false;
  return verifyLifetimeAssumptionCases(body, owner, *declared, [&](const auto &branch) {
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
  const auto localAssumptions = withLocalStorageLifetimeFacts(program, body, *expanded);
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
            if (std::getenv("GTI_TRACE_MIR_ORIGIN")) std::fprintf(stderr, "[cin-parts] region=%d types=%d referentLts=%zu expectedLts=%zu r0=%d:%llu e0=%d:%llu\n", regionProved, expected.arguments.size() == 1 && relations.types(referent, expected.arguments.front(), expected.referenceAccess == AccessMode::Mutable ? 0 : 1), referent.lifetimeArguments.size(), expected.arguments.empty() ? 0 : expected.arguments.front().lifetimeArguments.size(), referent.lifetimeArguments.empty() ? -1 : (int)referent.lifetimeArguments[0].kind, referent.lifetimeArguments.empty() ? 0ull : (unsigned long long)referent.lifetimeArguments[0].id, expected.arguments.empty() || expected.arguments.front().lifetimeArguments.empty() ? -1 : (int)expected.arguments.front().lifetimeArguments[0].kind, expected.arguments.empty() || expected.arguments.front().lifetimeArguments.empty() ? 0ull : (unsigned long long)expected.arguments.front().lifetimeArguments[0].id); // TEMP-TRACE
            proved = immediatePointerCarrierInput(program, body, instruction, *actual) ||
                (expected.arguments.size() == 1 && regionProved &&
                relations.types(referent, expected.arguments.front(),
                                expected.referenceAccess == AccessMode::Mutable ? 0 : 1));
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
          if (!proved && std::getenv("GTI_TRACE_MIR_ORIGIN")) { // TEMP-TRACE
            const auto *pl = input.place ? body.findPlace(input.place) : nullptr; // TEMP-TRACE
            for (const auto &o : body.lifetimeBody.origins) { const auto *op = body.findPlace(o.place); std::fprintf(stderr, "[cin-or] region=%d:%llu place=p%llu root=%d binding=%llu proj=%zu source=%llu keyroot=%llu\n", static_cast<int>(o.region.kind), (unsigned long long)o.region.id, (unsigned long long)o.place, op ? static_cast<int>(op->root) : -1, op ? (unsigned long long)op->binding : 0ull, op ? op->projections.size() : 0, (unsigned long long)o.source, op && op->key ? (unsigned long long)op->key->root : 0ull); } // TEMP-TRACE
            for (const auto &edge : body.lifetimeBody.obligations) std::fprintf(stderr, "[cin-ob] %d:%llu >= %d:%llu\n", static_cast<int>(edge.longer.kind), (unsigned long long)edge.longer.id, static_cast<int>(edge.shorter.kind), (unsigned long long)edge.shorter.id); // TEMP-TRACE
            for (const auto &b : body.blocks) for (const auto &i : b.instructions) if (i.loan == 3 && i.kind == MirInstructionKind::Call) { const auto sig = appliedTargetSignature(program, i); std::fprintf(stderr, "[cin-sig] loan3 producer inst=%llu ret=%d:%llu\n", (unsigned long long)i.id, sig ? static_cast<int>(sig->returnType.referenceLifetime.kind) : -1, sig ? (unsigned long long)sig->returnType.referenceLifetime.id : 0ull); } // TEMP-TRACE
            std::fprintf(stderr, "[cin] inst=%llu prevStage=%d actualKind=%d actualRef=%d:%llu expectedKind=%d expectedRef=%d:%llu inputKind=%d place=p%llu root=%d binding=%llu proj=%zu callInputKind=%d\n", (unsigned long long)instruction.id, previousStage, static_cast<int>(actual->kind), static_cast<int>(actual->referenceLifetime.kind), (unsigned long long)actual->referenceLifetime.id, static_cast<int>(expected.kind), static_cast<int>(expected.referenceLifetime.kind), (unsigned long long)expected.referenceLifetime.id, static_cast<int>(input.kind), (unsigned long long)input.place, pl ? static_cast<int>(pl->root) : -1, pl ? (unsigned long long)pl->binding : 0ull, pl ? pl->projections.size() : 0, static_cast<int>(instruction.callInputKind)); // TEMP-TRACE
          } // TEMP-TRACE
          if (!proved)
            reject("MIR call-input stage does not prove its original source lifetime conversion",
                   block.id, instruction.id);
        }
        continue;
      }
      if (instruction.kind == MirInstructionKind::Compute &&
          instruction.operation == MirOperation::Closure) {
        const auto *target = instruction.lambdaTarget
            ? program.findLambda(*instruction.lambdaTarget) : nullptr;
        const auto view = target ? mirLambdaLifetimeView(*target, instruction.info.type) : std::nullopt;
        const auto &application = instruction.lifetimeApplication;
        const auto signature = view ? instantiateMirLifetimeSignature(
            target->environmentContract, target->type, {}, application) : std::nullopt;
        if (!view || !signature || !application.complete || application.callableSignature ||
            !instruction.lifetimeArguments.empty() ||
            !equivalentLifetimeTypes(signature->returnType, instruction.info.type) ||
            !satisfiesLifetimeRequirements(program, target->environmentContract,
                                          application.bindings, localAssumptions, graph) ||
            !provesNominalLifetimeFormation(program, instruction.info.type, localAssumptions, graph))
          reject("MIR closure formation does not prove its exact fixed environment contract",
                 block.id, instruction.id);
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
                               instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke ||
                               referenceErasureIntrinsic(instruction.intrinsic);
        if (!hasTarget &&
            (instruction.intrinsic == IntrinsicKind::TextViewFromAddress ||
             instruction.intrinsic == IntrinsicKind::CStringFromAddress)) {
          // The operation determines this contract: the produced text is valid
          // only while its witness owner stays borrowed. Structural checks
          // prove that the result loan borrows exactly that witness; here the
          // witness borrow must independently outlive the text's own region.
          // The attached application cannot add a stronger result region.
          const std::size_t witness =
              instruction.intrinsic == IntrinsicKind::TextViewFromAddress ? 2 : 1;
          const auto &text = instruction.info.type;
          const Lifetime region = text.lifetimeArguments.size() == 1
                                      ? text.lifetimeArguments.front() : Lifetime{};
          if (instruction.kind != MirInstructionKind::Call ||
              instruction.operands.size() != witness + 1 ||
              (region.specified() &&
               !relations.borrowedOperand(instruction.operands[witness], region,
                                          instruction.hirValue)))
            reject("MIR text address result does not prove its witness owner region",
                   block.id, instruction.id);
          continue;
        }
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
                           !referenceErasureIntrinsic(instruction.intrinsic) &&
                           !instruction.nativeCalleeType && instruction.lifetimeApplication.callableSignature)) {
          if (std::getenv("GTI_TRACE_MIR_ORIGIN")) // TEMP-TRACE
            std::fprintf(stderr, "[cover] inst=%llu signature=%d callableSignature=%d lambda=%d fn=%d\n", (unsigned long long)instruction.id, signature.has_value(), instruction.lifetimeApplication.callableSignature.has_value(), instruction.lambdaTarget.has_value(), instruction.functionTarget.has_value()); // TEMP-TRACE
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
        // In-place construction carries its destination before the selected
        // constructor's arguments. It returns void; allocation returns an
        // owner. Both still instantiate the same constructor object contract.
        std::size_t argumentOffset = 0;
        const SemanticType *constructedType = nullptr;
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.constructorTarget) {
          if (instruction.intrinsic == IntrinsicKind::ObjectConstruct ||
              instruction.intrinsic == IntrinsicKind::OptionalStorageConstruct) {
            const auto storageKind = instruction.intrinsic == IntrinsicKind::ObjectConstruct
                ? SemanticType::RawPointer : SemanticType::Class;
            if (instruction.parameterTypes.empty() ||
                instruction.parameterTypes.front().kind != storageKind ||
                instruction.parameterTypes.front().arguments.size() != 1) {
              reject("MIR in-place lifetime application lacks its exact destination type",
                     block.id, instruction.id);
              continue;
            }
            argumentOffset = 1;
            constructedType = &instruction.parameterTypes.front().arguments.front();
          } else if (instruction.objectAllocation != 0 &&
                     instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
                     instruction.info.type.kind == SemanticType::UniqueOwner &&
                     instruction.info.type.arguments.size() == 1) {
            constructedType = &instruction.info.type.arguments.front();
          }
        }
        std::vector<SemanticType> parameters;
        for (const auto &parameter : signature->parameterTypes)
          SemanticType::appendExpandedArgument(parameters, parameter);
        std::vector<SemanticType> selected;
        for (const auto &parameter : std::span(instruction.parameterTypes).subspan(argumentOffset))
          SemanticType::appendExpandedArgument(selected, parameter);
        if (parameters != selected ||
            selected.size() + argumentOffset != instruction.operands.size()) {
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
          const auto actual = relations.actualType(instruction.operands[index + argumentOffset]);
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
                !relations.borrowedOperand(instruction.operands[index + argumentOffset],
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
                                 instruction.lifetimeApplication.bindings)))) {
            if (std::getenv("GTI_TRACE_MIR_ORIGIN")) { // TEMP-TRACE
              const auto required = applyLifetime(target->lifetimeContract.receiver, // TEMP-TRACE
                                                  instruction.lifetimeApplication.bindings); // TEMP-TRACE
              std::fprintf(stderr, "[recv] body=%d inst=%llu required=%d:%llu hasReceiver=%d\n", static_cast<int>(body.kind), // TEMP-TRACE
                           (unsigned long long)instruction.id, static_cast<int>(required.kind), (unsigned long long)required.id, // TEMP-TRACE
                           instruction.receiver ? 1 : 0); // TEMP-TRACE
              if (instruction.receiver) { // TEMP-TRACE
                const auto &op = *instruction.receiver; // TEMP-TRACE
                const auto *pl = op.place ? body.findPlace(op.place) : nullptr; // TEMP-TRACE
                const auto *ln = op.kind == MirOperandKind::Loan ? body.findLoan(op.loan) : nullptr; // TEMP-TRACE
                std::fprintf(stderr, "[recv]   operand kind=%d place=%llu root=%d proj=%zu key=%d loan=%llu loanKind=%d loanPlace=%llu\n", // TEMP-TRACE
                             static_cast<int>(op.kind), (unsigned long long)op.place, pl ? static_cast<int>(pl->root) : -1, // TEMP-TRACE
                             pl ? pl->projections.size() : 0, pl && pl->key ? 1 : 0, (unsigned long long)op.loan, // TEMP-TRACE
                             ln ? static_cast<int>(ln->kind) : -1, ln ? (unsigned long long)ln->source : 0ull); // TEMP-TRACE
                if (op.kind == MirOperandKind::Value) if (const auto *d = relations.definition(op.value)) std::fprintf(stderr, "[recv]   stage kind=%d prepared=%llu dest=%llu tomb=%d op0value=%llu\n", static_cast<int>(d->callInputKind), (unsigned long long)d->preparedParameterDrop.value_or(0), (unsigned long long)d->destination.value_or(0), d->tombstone, d->operands.empty() ? 0ull : (unsigned long long)d->operands.front().value); // TEMP-TRACE
                if (op.kind == MirOperandKind::Value) if (const auto *d = relations.definition(op.value)) { // TEMP-TRACE
                  const auto *dop = d->operands.empty() ? nullptr : &d->operands.front(); // TEMP-TRACE
                  std::fprintf(stderr, "[recv]   value def kind=%d intrinsic=%d fn=%d result-type=%d:%llu ops=%zu op0kind=%d op0place=%llu op0loan=%llu lifetime=%d:%llu\n", static_cast<int>(d->kind), static_cast<int>(d->intrinsic), d->functionTarget ? 1 : 0, static_cast<int>(d->info.type.kind), (unsigned long long)d->info.type.classId, d->operands.size(), dop ? static_cast<int>(dop->kind) : -1, dop ? (unsigned long long)dop->place : 0ull, dop ? (unsigned long long)dop->loan : 0ull, static_cast<int>(d->info.type.referenceLifetime.kind), (unsigned long long)d->info.type.referenceLifetime.id); } // TEMP-TRACE
                if (op.kind == MirOperandKind::Value) if (const auto *d = relations.definition(op.value); d && !d->operands.empty() && d->operands.front().place) { // TEMP-TRACE
                  const auto *sp = body.findPlace(d->operands.front().place); // TEMP-TRACE
                  const auto *sl = sp && sp->root == MirPlaceRootKind::Loan ? body.findLoan(sp->loan) : nullptr; // TEMP-TRACE
                  const auto *src = sl ? body.findPlace(sl->source) : nullptr; // TEMP-TRACE
                  std::fprintf(stderr, "[recv]   stage place root=%d loan=%llu loanKind=%d loanSource=p%llu srcRoot=%d srcBinding=%llu srcKey=%d producedBy=%llu parent=%llu\n", sp ? static_cast<int>(sp->root) : -1, sp ? (unsigned long long)sp->loan : 0ull, sl ? static_cast<int>(sl->kind) : -1, sl ? (unsigned long long)sl->source : 0ull, src ? static_cast<int>(src->root) : -1, src ? (unsigned long long)src->binding : 0ull, src && src->key ? 1 : 0, sl ? (unsigned long long)sl->producedBy : 0ull, sl ? (unsigned long long)sl->parent : 0ull); // TEMP-TRACE
                } // TEMP-TRACE
                if (ln) if (const auto *lp = body.findPlace(ln->source)) // TEMP-TRACE
                  std::fprintf(stderr, "[recv]   loan place root=%d proj=%zu key=%d\n", static_cast<int>(lp->root), lp->projections.size(), lp->key ? 1 : 0); // TEMP-TRACE
              } // TEMP-TRACE
              for (const auto &pl : body.places) if (pl.id <= 8) std::fprintf(stderr, "[recv]   place p%llu root=%d binding=%llu symbol=%llu temp=%llu value=%llu loan=%llu proj=%zu key=%d keyroot=%llu type=%d\n", (unsigned long long)pl.id, static_cast<int>(pl.root), (unsigned long long)pl.binding, (unsigned long long)pl.symbol, (unsigned long long)pl.temporary, (unsigned long long)pl.value, (unsigned long long)pl.loan, pl.projections.size(), pl.key.has_value(), pl.key ? (unsigned long long)pl.key->root : 0ull, static_cast<int>(pl.type.kind)); // TEMP-TRACE
              for (const auto &origin : body.lifetimeBody.origins) // TEMP-TRACE
                std::fprintf(stderr, "[recv]   origin place=%llu receiverKey=%d region=%d:%llu\n", (unsigned long long)origin.place, // TEMP-TRACE
                             origin.key.receiver ? 1 : 0, static_cast<int>(origin.region.kind), (unsigned long long)origin.region.id); // TEMP-TRACE
              for (const auto &edge : body.lifetimeBody.obligations) // TEMP-TRACE
                std::fprintf(stderr, "[recv]   obligation %d:%llu >= %d:%llu\n", static_cast<int>(edge.longer.kind), (unsigned long long)edge.longer.id, // TEMP-TRACE
                             static_cast<int>(edge.shorter.kind), (unsigned long long)edge.shorter.id); // TEMP-TRACE
            } // TEMP-TRACE
            reject(
                "MIR receiver does not prove its required region relationship",
                block.id, instruction.id);
          }
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
                 !regionFreeOwnerMatches(program, *actualOwner, *expectedOwner) &&
                 !relations.types(*actualOwner, *expectedOwner,
                                  target->receiverMutability == ReceiverMutability::Mutable ? 0 : 1,
                                  0, true)))
              reject("MIR receiver type does not prove its nested lifetime relationships",
                     block.id, instruction.id);
          }
        }
        const auto &returned = signature->returnType;
        if (constructedType) {
          if (!equivalentLifetimeTypes(*constructedType, returned))
            reject("MIR construction destination differs from its lifetime-instantiated owner",
                   block.id, instruction.id);
        } else if (instruction.kind != MirInstructionKind::InitializeBase &&
            (returned.kind == SemanticType::Reference
                 ? returned.arguments.size() != 1 ||
                       !equivalentLifetimeTypes(instruction.info.type, returned.arguments.front()) ||
                       !(instruction.info.category == ValueCategory::Place
                             ? instruction.info.access == returned.referenceAccess
                             : publishesCallResultLoan(body, instruction,
                                                       returned.referenceAccess))
                 : !equivalentLifetimeTypes(instruction.info.type, returned))) {
          if (std::getenv("GTI_TRACE_MIR_ORIGIN")) { // TEMP-TRACE
            const auto dump = [](const auto &self, const SemanticType &type) -> std::string { // TEMP-TRACE
              std::string out = "k" + std::to_string(static_cast<int>(type.kind)); // TEMP-TRACE
              if (type.classId) out += "#c" + std::to_string(type.classId); // TEMP-TRACE
              if (type.kind == SemanticType::Reference) // TEMP-TRACE
                out += "&a" + std::to_string(static_cast<int>(type.referenceAccess)) + "<" + // TEMP-TRACE
                       std::to_string(static_cast<int>(type.referenceLifetime.kind)) + ":" + // TEMP-TRACE
                       std::to_string(type.referenceLifetime.id) + ">"; // TEMP-TRACE
              for (const auto &l : type.lifetimeArguments) // TEMP-TRACE
                out += "{" + std::to_string(static_cast<int>(l.kind)) + ":" + std::to_string(l.id) + "}"; // TEMP-TRACE
              if (!type.arguments.empty()) { out += "("; for (const auto &a : type.arguments) out += self(self, a) + ","; out += ")"; } // TEMP-TRACE
              return out; // TEMP-TRACE
            }; // TEMP-TRACE
            std::fprintf(stderr, "[sig] inst=%llu kind=%d result=%d operands=%zu actual=%s cat=%d acc=%d returned=%s\n", // TEMP-TRACE
                         (unsigned long long)instruction.id, static_cast<int>(instruction.kind), instruction.result ? 1 : 0, instruction.operands.size(), dump(dump, instruction.info.type).c_str(), // TEMP-TRACE
                         static_cast<int>(instruction.info.category), static_cast<int>(instruction.info.access), // TEMP-TRACE
                         dump(dump, returned).c_str()); // TEMP-TRACE
            for (const auto &loan : body.loans) // TEMP-TRACE
              if (loan.producedBy == instruction.hirValue) // TEMP-TRACE
                std::fprintf(stderr, "[sig]   loan=%llu kind=%d access=%d hirValue=%llu\n", (unsigned long long)loan.id, // TEMP-TRACE
                             static_cast<int>(loan.kind), static_cast<int>(loan.access), (unsigned long long)instruction.hirValue); // TEMP-TRACE
          } // TEMP-TRACE
          reject("MIR result differs from its lifetime-instantiated signature",
                 block.id, instruction.id);
        }
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
                   !actual ||
                   // A local reference without a named region relates only
                   // its referent; the loan's own scope bounds the borrow.
                   !(expected.kind == SemanticType::Reference &&
                             !expected.referenceLifetime.specified() &&
                             actual->kind != SemanticType::Reference &&
                             expected.arguments.size() == 1
                         ? relations.types(*actual, expected.arguments.front(),
                                           expected.referenceAccess == AccessMode::Mutable ? 0 : 1)
                         : relations.types(*actual, expected))) {
          if (std::getenv("GTI_TRACE_SCOPED")) { // TEMP-TRACE
            const auto dump = [](auto &&self, const SemanticType &t, int d) -> void { // TEMP-TRACE
              std::fprintf(stderr, "%*s kind=%d class=%u refLt=%d:%llu lts=", d*2, "", (int)t.kind, (unsigned)t.classId, (int)t.referenceLifetime.kind, (unsigned long long)t.referenceLifetime.id); // TEMP-TRACE
              for (auto r : t.lifetimeArguments) std::fprintf(stderr, "%d:%llu,", (int)r.kind, (unsigned long long)r.id); // TEMP-TRACE
              std::fprintf(stderr, "\n"); // TEMP-TRACE
              lifetime_detail::children(t, [&](const SemanticType &c) { self(self, c, d+1); }); // TEMP-TRACE
            }; // TEMP-TRACE
            std::fprintf(stderr, "[stored-dest] inst=%llu opKind=%d expected:\n", (unsigned long long)instruction.id, (int)instruction.operands.front().kind); dump(dump, expected, 1); // TEMP-TRACE
            std::fprintf(stderr, "[stored-dest] actual:\n"); if (actual) dump(dump, *actual, 1); // TEMP-TRACE
            for (const auto &edge : body.lifetimeBody.obligations) std::fprintf(stderr, "[stored-dest] ob %d:%llu >= %d:%llu\n", (int)edge.longer.kind, (unsigned long long)edge.longer.id, (int)edge.shorter.kind, (unsigned long long)edge.shorter.id); // TEMP-TRACE
          } // TEMP-TRACE
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
  if (body.kind == MirBodyKind::Constructor) {
    const auto *constructor = program.findConstructorInstance(owner);
    const auto ownerType = constructor ? checkedMirCallableOwnerType(program, constructor->owner,
        constructor->ownerType, constructor->lifetimeContract) : std::nullopt;
    const auto view = ownerType ? mirClassLifetimeView(program, *ownerType) : std::nullopt;
    if (constructor)
      for (const auto &initializer : constructor->initializers) {
        if (!initializer.storesReference || !containsLifetimeMetadata(initializer.targetType))
          continue;
        const auto &expected = initializer.targetType;
        const MirLoan *publication = nullptr;
        for (const auto &loan : body.loans)
          if (loan.escapes && loan.storedPath == BorrowPath{fieldProjection(initializer.field)}) {
            if (publication) { publication = nullptr; break; }
            publication = &loan;
          }
        const MirInstruction *borrow = nullptr;
        if (publication && !publication->entry)
          for (const auto &block : body.blocks)
            for (const auto &instruction : block.instructions)
              if (!instruction.tombstone && instruction.kind == MirInstructionKind::Borrow &&
                  instruction.loan == publication->id) {
                if (borrow) { publication = nullptr; break; }
                borrow = &instruction;
              }
        std::optional<SemanticType> fieldType;
        if (view)
          for (const auto &field : view->instance->declaredFields)
            if (field.symbol == initializer.field) {
              if (fieldType) { fieldType.reset(); break; }
              fieldType = view->instantiate(field.type);
            }
        if (fieldType != std::optional<SemanticType>{expected} ||
            expected.kind != SemanticType::Reference || expected.arguments.size() != 1 ||
            initializer.arguments.size() != 1 || !publication ||
            (expected.referenceAccess == AccessMode::Mutable &&
             publication->access != AccessMode::Mutable) ||
            (!publication->entry && (!borrow || borrow->operands.size() != 1 ||
                                      borrow->hirValue != initializer.arguments.front()))) {
          reject("MIR reference field has no exact initializer publication", body.entry);
          continue;
        }
        const MirOperand input = publication->entry
            ? MirOperand{.kind = MirOperandKind::Loan, .loan = publication->id,
                         .type = expected.arguments.front()}
            : borrow->operands.front();
        auto actual = relations.actualType(input);
        if (input.kind == MirOperandKind::Loan)
          for (const auto &block : body.blocks)
            for (const auto &instruction : block.instructions)
              if (instruction.kind == MirInstructionKind::Call && instruction.loan == input.loan)
                if (const auto signature = appliedTargetSignature(program, instruction))
                  actual = signature->returnType;
        const auto referent = actual && actual->kind == SemanticType::Reference && actual->arguments.size() == 1
            ? std::optional{actual->arguments.front()} : actual;
        if (!referent ||
            !relations.borrowedOperand(input, expected.referenceLifetime, initializer.arguments.front()) ||
            !relations.types(*referent, expected.arguments.front(),
                             expected.referenceAccess == AccessMode::Mutable ? 0 : 1))
          reject("MIR reference field does not prove its exact stored lifetime and referent", body.entry);
      }
  }
  return result;
}

MirVerificationResult verifyMirLifetimeApplications(
    const MirProgram &program, const MirBody &body, std::size_t owner) {
  const auto declared = bodyLifetimeContract(program, body, owner);
  if (!declared)
    return {{MirVerificationError{.bodyKind = body.kind, .owner = owner,
        .message = "MIR lambda body lacks a valid environment and invocation contract"}}};
  return verifyLifetimeAssumptionCases(body, owner, *declared, [&](const auto &branch) {
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
                                    const MirLifetimeApplication &application,
                                    const SemanticType *receiverOwner) {
  const auto signature = instantiateMirLifetimeSignature(
      contract, SemanticType::Void, declared, application, receiverOwner);
  return signature &&
         canonicalLifetimeParameters(selected) ==
             canonicalLifetimeParameters(signature->parameterTypes);
}

bool mirLifetimeCallSignatureMatches(const MirInstruction &instruction,
                                     const MirFunctionInstance &target) {
  const auto signature = instantiateMirLifetimeSignature(
      target.lifetimeContract, target.returnType, target.parameterTypes,
      instruction.lifetimeApplication, target.owner ? &target.ownerType : nullptr);
  if (std::getenv("GTI_TRACE_MIR_ORIGIN") && (!signature || !appliedSignatureMatches(instruction, *signature))) { // TEMP-TRACE
    std::fprintf(stderr, "[sigm] target=%llu signature=%d typeBounds=%zu appTypeObl=%zu bindings=%zu\n", (unsigned long long)target.id, signature.has_value(), target.lifetimeContract.typeBounds.size(), instruction.lifetimeApplication.typeObligations.size(), instruction.lifetimeApplication.bindings.size()); // TEMP-TRACE
    for (const auto &b : target.lifetimeContract.typeBounds) // TEMP-TRACE
      std::fprintf(stderr, "[sigm]   bound class=%llu region=%d:%llu receiver=%d:%llu ownerClass=%llu implied=%d\n", (unsigned long long)b.type.classId, static_cast<int>(b.region.kind), (unsigned long long)b.region.id, static_cast<int>(target.lifetimeContract.receiver.kind), (unsigned long long)target.lifetimeContract.receiver.id, (unsigned long long)target.ownerType.classId, typeBoundImpliedByReceiver(b, target.lifetimeContract, target.ownerType)); // TEMP-TRACE
  } // TEMP-TRACE
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

namespace {
bool callableTargetRequirements(
    const MirProgram &program, const LifetimeSignatureContract &contract,
    std::span<const LifetimeBinding> bindings, const LifetimeSignatureContract &premises) {
  if (!contract.valid() || !contract.requirements.resolved())
    return false;
  return visitLifetimeAssumptionCases(premises, [&](const auto &branch) {
    const auto assumptions = expandedLifetimeAssumptions(program, branch);
    if (!assumptions)
      return false;
    LifetimeBounds bounds;
    for (const auto edge : assumptions->bounds)
      if (!bounds.assume(edge.longer, edge.shorter))
        return false;
    LifetimeInferenceConstraints graph(bounds);
    for (const auto edge : contract.bounds)
      if (!graph.proves(applyLifetime(edge.longer, bindings), applyLifetime(edge.shorter, bindings)))
        return false;
    for (const auto &bound : contract.typeBounds) {
      const auto type = substituteTypeLifetimes(bound.type, bindings);
      std::vector<SemanticType> pending;
      if (!type || !mirTypeOutlives(program, *type, applyLifetime(bound.region, bindings),
                                   *assumptions, graph, pending))
        return false;
    }
    return satisfiesLifetimeRequirements(program, contract, bindings, *assumptions, graph);
  }).proof == LifetimeFormulaProof::Proven;
}

} // namespace

bool mirCallableLifetimeTargetMatches(
    const MirProgram &program, const CallableLifetimeSignature &callable,
    const SemanticType &returnType, std::span<const SemanticType> parameters,
    const LifetimeSignatureContract &contract) {
  const auto bindings = callableLifetimeTargetBindings(
      returnType, parameters, contract, callable.signature);
  return bindings && callableTargetRequirements(program, contract, *bindings, callable.contract);
}

bool mirLambdaCallableLifetimeTargetMatches(
    const MirProgram &program, const CallableLifetimeSignature &callable,
    const MirLambdaInstance &target, const SemanticType &actual) {
  if (!mirLambdaLifetimeView(target, actual))
    return false;
  const auto fixed = lambdaCallableLifetimeTarget(target.type, actual,
      target.lifetimeContract, target.environmentContract, callable);
  if (!fixed)
    return false;
  auto premises = callable.contract;
  for (const auto region : fixed->environment.parameters)
    if (std::find(premises.parameters.begin(), premises.parameters.end(), region) == premises.parameters.end())
      premises.parameters.push_back(region);
  const auto composed = composeLambdaLifetimeContract(fixed->environment, premises);
  // The exact carrier type promises these formation facts. Every Closure
  // producer proves them separately; transporting this type cannot replace
  // its fixed environment with a fresh invocation choice.
  return composed && callableTargetRequirements(program, target.lifetimeContract,
                                                fixed->bindings, *composed);
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
