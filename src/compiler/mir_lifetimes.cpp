#include "mir_lifetimes.h"
#include "gti/lifetime_types.h"

namespace lang {
MirLifetimeApplication
lowerMirLifetimeApplication(const HirLifetimeApplication &source) {
  MirLifetimeApplication result{.bindings = source.bindings,
                                .variables = source.variables,
                                .obligations = source.obligations,
                                .typeObligations = source.typeObligations,
                                .complete = source.complete};
  for (const auto &origin : source.origins)
    result.origins.push_back({.region = origin.region,
                              .key = origin.place,
                              .source = origin.source});
  return result;
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
  return source.bindings == lowered.bindings && preservesFacts(source, lowered);
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
       type.kind != SemanticType::FunctionSignature) ||
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

Lifetime applyLifetime(Lifetime value,
                       std::span<const LifetimeBinding> bindings) {
  for (const auto binding : bindings)
    if (binding.first == value)
      return binding.second;
  return value;
}
} // namespace

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
      !contract.typeBounds.empty() || contract.receiver != Lifetime{};
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
  if (!contract.valid() || !application.complete ||
      application.bindings.size() != contract.parameters.size() ||
      !scopedDeclarationType(returnType, contract.parameters))
    return std::nullopt;
  for (const auto &type : parameterTypes)
    if (!scopedDeclarationType(type, contract.parameters))
      return std::nullopt;
  for (const auto &bound : contract.typeBounds)
    if (!scopedDeclarationType(bound.type, contract.parameters))
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
      const auto owner = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &candidate) { return candidate.type == type; });
      if (owner == program.classInstances().end())
        return std::nullopt;
      for (const auto &field : owner->declaredFields)
        addType(field.type, bound.region);
      for (const auto &base : owner->structuralBases)
        if (!base.interface)
          addType(base.type, bound.region);
      continue;
    }
    if (type.kind == SemanticType::Enum) {
      if (const auto *enumeration = program.findPayloadEnum(type)) {
        for (const auto &variant : enumeration->variants)
          for (const auto &field : variant.payloadTypes)
            addType(field, bound.region);
      } else if (!program.findIntegralEnum(type)) {
        return std::nullopt;
      }
      continue;
    }
    if (type.kind == SemanticType::FunctionSignature) {
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
  if (type.kind != SemanticType::Class && type.kind != SemanticType::Enum)
    for (const auto lifetime : type.lifetimeArguments)
      if (!graph.proves(lifetime, region))
        return false;
  if (type.kind == SemanticType::StringView &&
      type.lifetimeArguments.size() != 1)
    return false;
  if (type.kind == SemanticType::Class) {
    const auto instance = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &candidate) { return candidate.type == type; });
    if (instance == program.classInstances().end())
      return false;
    for (const auto &field : instance->declaredFields)
      if (!mirTypeOutlives(program, field.type, region, contract, graph,
                           pending))
        return false;
    for (const auto &base : instance->structuralBases)
      if (!base.interface && !mirTypeOutlives(program, base.type, region,
                                              contract, graph, pending))
        return false;
    return true;
  }
  if (type.kind == SemanticType::Enum) {
    if (const auto *enumeration = program.findPayloadEnum(type)) {
      for (const auto &variant : enumeration->variants)
        for (const auto &field : variant.payloadTypes)
          if (!mirTypeOutlives(program, field, region, contract, graph,
                               pending))
            return false;
      return true;
    }
    return program.findIntegralEnum(type) != nullptr;
  }
  if (type.kind == SemanticType::FunctionSignature &&
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
} // namespace

MirVerificationResult
verifyMirLifetimeBodyFacts(const MirProgram &program, const MirBody &body,
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
           instruction.kind != MirInstructionKind::InitializeBase)) {
        reject("MIR instruction carries an incomplete or misplaced lifetime "
               "application",
               block.id, instruction.id);
        continue;
      }
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
  for (const auto &bound : facts.typeObligations) {
    std::vector<SemanticType> pending;
    if (!mirTypeOutlives(program, bound.type, bound.region, contract, graph,
                         pending))
      reject("MIR body cannot prove an exact type-outlives obligation");
  }
  return result;
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
    return instantiateMirLifetimeSignature(target->lifetimeContract,
                                           owner->type, target->parameterTypes,
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
      if (!stage || stage->kind != MirInstructionKind::CallInput)
        return current;
      if (stage->operands.size() != 1)
        return nullptr;
      source = stage->hirValue;
      current = &stage->operands.front();
    }
    return nullptr;
  }

  std::optional<SemanticType> actualType(const MirOperand &operand) const {
    HirValueId ignored = 0;
    const auto *source = sourceOperand(operand, ignored);
    if (!source)
      return std::nullopt;
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
             int variance = 1, std::size_t depth = 0) const {
    if (depth >= 256)
      return false;
    if (actual == expected)
      return true;
    if (!actual.quantifiedLifetimes.empty() ||
        !expected.quantifiedLifetimes.empty())
      return equivalentLifetimeTypes(actual, expected);
    if (actual.kind != expected.kind)
      return freeTypeLifetimes(actual).empty() &&
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
        if (const auto *instance = nominalClass(type))
          for (const auto &base : instance->structuralBases)
            self(self, base.type);
      };
      project(project, actual);
      return matches.size() == 1 &&
             types(matches.front(), expected, variance, depth + 1);
    }
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
      if (const auto *enumeration = program.findPayloadEnum(expected))
        nominal = &enumeration->variance;
      else if (const auto *enumeration = program.findIntegralEnum(expected))
        nominal = &enumeration->variance;
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
                             : expected.kind == SemanticType::StringView
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
                 mode(direction), depth + 1))
        return false;
    }
    return true;
  }

private:
  const MirClassInstance *nominalClass(const SemanticType &type) const {
    const auto found = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &instance) { return instance.type == type; });
    return found == program.classInstances().end() ? nullptr : &*found;
  }

  Lifetime declaredPlaceRegion(const MirPlace &place) const {
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
        const auto *owner = nominalClass(type);
        if (!owner)
          return {};
        const auto field = std::find_if(
            owner->declaredFields.begin(), owner->declaredFields.end(),
            [&](const auto &candidate) {
              return candidate.symbol == projection.field;
            });
        if (field == owner->declaredFields.end())
          return {};
        type = field->type;
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
        const auto *base = program.findClassInstance(projection.selection);
        if (!base)
          return {};
        type = base->type;
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

MirVerificationResult verifyMirLifetimeApplications(const MirProgram &program,
                                                    const MirBody &body,
                                                    std::size_t owner) {
  const auto &declaredContract = bodyLifetimeContract(program, body, owner);
  auto result =
      verifyMirLifetimeBodyFacts(program, body, declaredContract, owner);
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
      if (instruction.kind == MirInstructionKind::Call ||
          instruction.kind == MirInstructionKind::Construct ||
          instruction.kind == MirInstructionKind::InitializeBase) {
        const bool hasTarget = instruction.functionTarget ||
                               instruction.constructorTarget ||
                               instruction.lambdaTarget;
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
        if (!signature) {
          reject("MIR call lifetime substitution does not cover its exact "
                 "target contract",
                 block.id, instruction.id);
          continue;
        }
        const auto &application = instruction.lifetimeApplication;
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
std::optional<MirInstantiatedLifetimeSignature>
instantiateMirLifetimeTarget(const MirProgram &program,
                             const MirInstruction &instruction) {
  return appliedTargetSignature(program, instruction);
}
} // namespace lang
