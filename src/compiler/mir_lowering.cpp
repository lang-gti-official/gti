#include "gti/mir_lowering.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"

namespace lang {

namespace {

[[nodiscard]] std::optional<MirCAbiRecordLayout>
lowerCAbiRecordLayout(const HirClassInstance &instance) {
  if (!instance.cAbiLayout) {
    return std::nullopt;
  }
  MirCAbiRecordLayout result{.sizeBytes = instance.cAbiLayout->sizeBytes,
                             .abiAlignmentBytes =
                                 instance.cAbiLayout->abiAlignmentBytes};
  result.fields.reserve(instance.cAbiLayout->fields.size());
  for (const CAbiRecordFieldLayout &field : instance.cAbiLayout->fields) {
    const auto source =
        std::find_if(instance.fields.begin(), instance.fields.end(),
                     [&](const HirClassField &candidate) {
                       return candidate.declaration == field.declaration;
                     });
    result.fields.push_back(
        {.field = source == instance.fields.end() ? 0 : source->info.symbol,
         .type = field.type,
         .offsetBytes = field.offsetBytes,
         .sizeBytes = field.sizeBytes,
         .abiAlignmentBytes = field.abiAlignmentBytes});
  }
  return result;
}

[[nodiscard]] std::optional<MirUnionLayout>
lowerUnionLayout(const HirClassInstance &instance) {
  if (!instance.unionLayout) {
    return std::nullopt;
  }
  MirUnionLayout result{.sizeBytes = instance.unionLayout->sizeBytes,
                        .abiAlignmentBytes =
                            instance.unionLayout->abiAlignmentBytes};
  result.fields.reserve(instance.unionLayout->fields.size());
  for (const UnionFieldLayout &field : instance.unionLayout->fields) {
    const auto source =
        std::find_if(instance.fields.begin(), instance.fields.end(),
                     [&](const HirClassField &candidate) {
                       return candidate.declaration == field.declaration;
                     });
    result.fields.push_back(
        {.field = source == instance.fields.end() ? 0 : source->info.symbol,
         .type = field.type,
         .sizeBytes = field.sizeBytes,
         .abiAlignmentBytes = field.abiAlignmentBytes});
  }
  return result;
}

[[nodiscard]] bool compilerTrustedSourceUnit(const HirProgram &program,
                                             SourceUnitId sourceUnit) {
  const auto &units = program.analysisSeal().sourceGraph.units;
  const auto found =
      std::find_if(units.begin(), units.end(),
                   [&](const auto &unit) { return unit.id == sourceUnit; });
  return found != units.end() && found->role != SourceUnitRole::Application;
}

[[nodiscard]] bool compilerTrustedSourcePath(const HirProgram &program,
                                             std::string_view sourcePath) {
  const auto &units = program.analysisSeal().sourceGraph.units;
  const auto found =
      std::find_if(units.begin(), units.end(),
                   [&](const auto &unit) { return unit.path == sourcePath; });
  return found != units.end() && found->role != SourceUnitRole::Application;
}

} // namespace
MirLoweringResult
MirLowerer::lower(const HirProgram &source,
                  const FailureMetadata &failureMetadata) const {
  MirLoweringResult provisional =
      lowerProgram(source, failureMetadata, MirDefinedFailureEffects{}, false);
  if (!provisional.valid()) {
    return provisional;
  }
  MirProgram provisionalVerification = provisional.program;
  for (MirFunctionInstance &function : provisionalVerification.functions) {
    if (function.entryKind == ProgramEntryKind::None) {
      continue;
    }
    function.entryKind = ProgramEntryKind::None;
    function.entryArgumentAppendTarget.reset();
  }
  const auto provisionalProof = verifyMirProgram(provisionalVerification);
  if (!provisionalProof.valid()) {
    for (const auto &error : provisionalProof.errors)
      provisional.errors.push_back({.body = {error.bodyKind, error.owner},
                                    .block = error.block,
                                    .instruction = error.instruction,
                                    .message = error.message});
    provisional.program.valid_ = false;
    return provisional;
  }
  // A function-level proof reads the lowered shape, and lowering with a
  // proved-false callee or destructor removes the failure edges that hid the
  // next proof. Re-lower until the derived result is its own fixed point.
  // Effects only ever move from conservative true toward proved false, so this
  // terminates; the bound keeps a non-converging program fail-closed below.
  constexpr int maximumEffectRounds = 8;
  MirDefinedFailureEffects effects =
      deriveMirDefinedFailureEffects(provisional.program);
  MirLoweringResult result =
      lowerProgram(source, failureMetadata, effects, true);
  MirDefinedFailureEffects finalEffects =
      deriveMirDefinedFailureEffects(result.program);
  for (int round = 0; round < maximumEffectRounds && effects != finalEffects;
       ++round) {
    effects = finalEffects;
    result = lowerProgram(source, failureMetadata, effects, true);
    finalEffects = deriveMirDefinedFailureEffects(result.program);
  }
  const bool exactSummaries =
      effects == finalEffects &&
      effects.functions.size() == result.program.functionInstances().size() &&
      effects.constructors.size() ==
          result.program.constructorInstances().size() &&
      effects.destructors.size() ==
          result.program.destructorInstances().size() &&
      std::equal(effects.functions.begin(), effects.functions.end(),
                 result.program.functionInstances().begin(),
                 [](bool mayRaise, const MirFunctionInstance &function) {
                   return mayRaise == function.mayRaiseDefinedFailure;
                 }) &&
      std::equal(effects.constructors.begin(), effects.constructors.end(),
                 result.program.constructorInstances().begin(),
                 [](bool mayRaise, const MirConstructorInstance &constructor) {
                   return mayRaise == constructor.mayRaiseDefinedFailure;
                 }) &&
      std::equal(effects.destructors.begin(), effects.destructors.end(),
                 result.program.destructorInstances().begin(),
                 [](bool mayRaise, const MirDestructorInstance &destructor) {
                   return mayRaise == destructor.mayRaiseDefinedFailure;
                 });
  result.program.valid_ = result.program.valid_ && exactSummaries &&
                          verifyMirProgram(result.program).valid();
  return result;
}

MirLoweringResult
MirLowerer::lowerProgram(const HirProgram &source,
                         const FailureMetadata &failureMetadata,
                         const MirDefinedFailureEffects &definedFailureEffects,
                         bool includeProgramInitialization) {
  MirLoweringResult result;
  if (!source.valid()) {
    result.program.valid_ = false;
    return result;
  }

  bool valid = verifyFailureMetadata(failureMetadata).valid();
  result.program.executionProfile_ = source.executionProfile();
  result.program.programKind_ = source.programKind();
  result.program.target_ = source.analysisSeal().target;
  result.program.failureMetadata_ = failureMetadata;
  for (const HirEnum &enumeration : source.enumDeclarations()) {
    if (!enumeration.payload) {
      result.program.integralEnums_.push_back(
          {.declaration = enumeration.declaration,
           .type = enumeration.type,
           .sourceUnit = enumeration.sourceUnit,
           .underlyingType = enumeration.underlyingType});
      continue;
    }
    MirPayloadEnum lowered{.declaration = enumeration.declaration,
                           .sourceUnit = enumeration.sourceUnit,
                           .type = enumeration.type,
                           .traits = enumeration.traits,
                           .requiresActiveCleanup =
                               enumeration.requiresActiveCleanup};
    lowered.variants.reserve(enumeration.enumerators.size());
    for (const HirEnumerator &variant : enumeration.enumerators) {
      lowered.variants.push_back({.variantIndex = variant.variantIndex,
                                  .payloadTypes = variant.payloadTypes,
                                  .payloadRequiresActiveCleanup =
                                      variant.payloadRequiresActiveCleanup});
    }
    result.program.payloadEnums_.push_back(std::move(lowered));
  }
  if (includeProgramInitialization) {
    result.program.moduleBody =
        lowerBody(source, failureMetadata, source.module(), MirBodyKind::Module,
                  SemanticType::Void, {}, definedFailureEffects, valid,
                  result.errors, 0, false, nullptr, nullptr, nullptr, nullptr,
                  &source.programInitializationPlan(),
                  &result.program.programInitialization);
  } else {
    HirBody provisionalModule;
    provisionalModule.placeDomain = source.module().placeDomain;
    result.program.moduleBody = lowerBody(
        source, failureMetadata, provisionalModule, MirBodyKind::Module,
        SemanticType::Void, {}, definedFailureEffects, valid, result.errors, 0);
  }

  result.program.classes.reserve(source.classInstances().size());
  for (const HirClassInstance &instance : source.classInstances()) {
    MirClassInstance lowered{
        .id = instance.id,
        .sourceUnit = instance.sourceUnit,
        .declaration = instance.declaration,
        .type = instance.type,
        .traits = instance.traits,
        .transferPolicy = instance.transferPolicy,
        .sharePolicy = instance.sharePolicy,
        .transferCondition = instance.transferCondition,
        .shareCondition = instance.shareCondition,
        .compilerCapability = instance.compilerCapability,
        .lifecycleBoundBorrowCarrier = instance.lifecycleBoundBorrowCarrier,
        .mutexGuardBorrowCarrier = instance.mutexGuardBorrowCarrier,
        .kind = instance.kind,
        .bases = instance.bases,
        .structuralBases = instance.bases,
        .abstract = instance.abstract,
        .polymorphic = instance.polymorphic,
        .cAbiRecord = instance.cAbiRecord,
        .cOpaqueHandle = instance.cOpaqueHandle,
        .cAbiLayout = lowerCAbiRecordLayout(instance),
        .unionLayout = lowerUnionLayout(instance),
        .defaultConstructor = instance.defaultConstructor,
        .generatedDefaultRequiresSchedule =
            instance.generatedDefaultRequiresSchedule,
        .copyConstructor = instance.copyConstructor,
        .moveConstructor = instance.moveConstructor,
        .copyAssignment = instance.copyAssignment,
        .moveAssignment = instance.moveAssignment,
        .destructorStatus = instance.destructorStatus,
        .destructor = instance.destructor,
        .requiresActiveDropState = instance.requiresActiveDropState,
        .requiresActiveCleanup = instance.requiresActiveCleanup};
    lowered.fields.reserve(instance.fields.size());
    for (const HirClassTypeAlias &alias : instance.typeAliases)
      lowered.typeAliases.push_back(
          {.declaration = alias.declaration, .type = alias.type});
    for (const HirClassField &field : instance.fields) {
      lowered.declaredFields.push_back(
          {.field = field.binding,
           .symbol = field.info.symbol,
           .type = field.info.type,
           .access = field.info.access,
           .dropKind = field.info.traits.drop,
           .requiresActiveCleanup = field.requiresActiveCleanup});
      if (field.info.traits.drop != DropKind::Lexical) {
        continue;
      }
      lowered.fields.push_back(
          {.field = field.binding,
           .symbol = field.info.symbol,
           .type = field.info.type,
           .dropKind = field.info.traits.drop,
           .requiresActiveCleanup = field.requiresActiveCleanup});
    }
    lowered.fieldInitializers =
        lowerBody(source, failureMetadata, instance.fieldInitializers,
                  MirBodyKind::FieldInitializers, SemanticType::Void, {},
                  definedFailureEffects, valid, result.errors, instance.id,
                  false, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  compilerTrustedSourceUnit(source, instance.sourceUnit));
    lowered.staticFieldInitializers =
        lowerBody(source, failureMetadata, instance.staticFieldInitializers,
                  MirBodyKind::StaticFieldInitializers, SemanticType::Void, {},
                  definedFailureEffects, valid, result.errors, instance.id,
                  false, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  compilerTrustedSourceUnit(source, instance.sourceUnit));
    for (auto field = instance.fields.rbegin(); field != instance.fields.rend();
         ++field) {
      if (field->info.traits.drop == DropKind::Lexical) {
        lowered.fieldDropOrder.push_back(
            {.field = field->binding,
             .symbol = field->info.symbol,
             .type = field->info.type,
             .requiresActiveCleanup = field->requiresActiveCleanup});
      }
    }
    result.program.classes.push_back(std::move(lowered));
  }

  result.program.functions.reserve(source.functionInstances().size());
  for (const HirFunctionInstance &instance : source.functionInstances()) {
    const bool implicitZeroReturn = !instance.owner &&
                                    instance.source != nullptr &&
                                    instance.source->name().lexeme == "main";
    std::vector<MirCallableParameter> callableParameters;
    callableParameters.reserve(instance.callableParameters.size());
    for (const HirCallableParameter &parameter : instance.callableParameters) {
      MirCallableParameter lowered{.parameterIndex = parameter.parameterIndex,
                                   .callableType = parameter.callableType,
                                   .access = parameter.access,
                                   .boundary = parameter.boundary,
                                   .ownedTransport = parameter.ownedTransport};
      lowered.signatures.reserve(parameter.signatures.size());
      for (const HirCallableSignature &signature : parameter.signatures) {
        lowered.signatures.push_back(
            {.returnType = signature.returnType,
             .parameterTypes = signature.parameterTypes,
             .requiredCapability = signature.requiredCapability,
             .selectedCapability = signature.selectedCapability,
             .functionTarget = signature.functionTarget,
             .lambdaTarget = signature.lambdaTarget,
             .nativeCalleeType = signature.nativeCalleeType});
      }
      lowered.forwardings.reserve(parameter.forwardings.size());
      for (const HirCallableForwarding &forwarding : parameter.forwardings) {
        lowered.forwardings.push_back(
            {.parameterIndex = forwarding.parameterIndex,
             .functionTarget = forwarding.functionTarget});
      }
      callableParameters.emplace_back(std::move(lowered));
    }
    result.program.functions.push_back(
        {.id = instance.id,
         .declaration = instance.declaration,
         .typeArguments = instance.typeArguments,
         .valueArguments = instance.valueArguments,
         .owner = instance.owner,
         .returnType = instance.returnType,
         .parameterTypes = instance.parameterTypes,
         .parameterBindings = instance.parameterBindings,
         .entryKind = instance.entryKind,
         .entryArgumentAppendTarget = instance.entryArgumentAppendTarget,
         .staticMember = instance.staticMember,
         .receiverMutability = instance.source == nullptr
                                   ? ReceiverMutability::ReadOnly
                                   : instance.source->receiverMutability(),
         .overloadedOperator =
             instance.source == nullptr || !instance.source->operatorName()
                 ? std::nullopt
                 : std::optional{instance.source->operatorName()->kind},
         .constexprFunction = instance.constexprFunction,
         .requiresUnsafe = instance.requiresUnsafe,
         .neverReturns = instance.neverReturns,
         .tracksCaller = instance.tracksCaller,
         .cThreadOperation = instance.cThreadOperation,
         .returnBorrowOrigin = instance.returnBorrowOrigin,
         .returnBorrowParameter = instance.returnBorrowParameter,
         .returnBorrowAccess = instance.returnBorrowAccess,
         .returnBorrowParameters = instance.returnBorrowParameters,
         .returnBorrowFields = instance.returnBorrowFields,
         .returnBorrowPlace = instance.returnBorrowPlace,
         .ownedPointeeProjection = instance.ownedPointeeProjection,
         .linkage = instance.linkage,
         .externalSymbol = instance.externalSymbol,
         .cArrayCountParameter = instance.cArrayCountParameter,
         .virtualMethod = instance.virtualMethod,
         .pureVirtual = instance.pureVirtual,
         .overrideMethod = instance.overrideMethod,
         .virtualRoots = instance.virtualRoots,
         .callableParameters = std::move(callableParameters),
         .definitionKind =
             instance.source == nullptr
                 ? MirFunctionInstance::DefinitionKind::Declaration
             : instance.source->runtimeBinding()
                 ? MirFunctionInstance::DefinitionKind::RuntimeBinding
             : instance.source->body() != nullptr
                 ? MirFunctionInstance::DefinitionKind::Source
                 : MirFunctionInstance::DefinitionKind::Declaration,
         .mayRaiseDefinedFailure =
             definedFailureEffects.functions.empty() || instance.id == 0 ||
             instance.id > definedFailureEffects.functions.size() ||
             definedFailureEffects.functions[instance.id - 1],
         .body = lowerBody(
             source, failureMetadata, instance.body, MirBodyKind::Function,
             instance.returnType, {}, definedFailureEffects, valid,
             result.errors, instance.id, implicitZeroReturn, nullptr, &instance,
             nullptr, nullptr, nullptr, nullptr,
             compilerTrustedSourceUnit(source, instance.sourceUnit))});
  }

  if (const std::optional<HirEmbeddingPlan> &sourcePlan =
          source.embeddingPlan()) {
    MirEmbeddingPlan plan{.abiVersion = sourcePlan->abiVersion};
    plan.exports.reserve(sourcePlan->exports.size());
    for (const HirEmbeddingExport &selected : sourcePlan->exports) {
      const MirFunctionInstance *function =
          result.program.findFunctionInstance(selected.function);
      if (function == nullptr ||
          function->declaration != selected.semanticFunction ||
          function->returnType != selected.returnType ||
          function->parameterTypes != selected.parameterTypes) {
        valid = false;
        continue;
      }
      plan.exports.push_back({.declaration = selected.semanticFunction,
                              .function = selected.function,
                              .qualifiedName = selected.qualifiedName,
                              .symbol = selected.symbol,
                              .returnType = selected.returnType,
                              .parameterTypes = selected.parameterTypes,
                              .requestSource = selected.requestSource});
    }
    if (plan.exports.size() != sourcePlan->exports.size()) {
      valid = false;
    }
    result.program.embeddingPlan_ = std::move(plan);
  }

  result.program.nativeCallbacks.reserve(
      source.nativeCallbackAdapters().size());
  for (const HirNativeCallbackAdapter &adapter :
       source.nativeCallbackAdapters()) {
    const MirFunctionInstance *target =
        result.program.findFunctionInstance(adapter.target);
    if (target == nullptr) {
      valid = false;
      continue;
    }
    result.program.nativeCallbacks.push_back(
        {.id = adapter.id,
         .target = adapter.target,
         .type = adapter.type,
         .cThreadCallback = adapter.cThreadCallback,
         .targetMayRaiseDefinedFailure = target->mayRaiseDefinedFailure,
         .failurePolicy = MirNativeCallbackFailurePolicy::TerminateInvocation,
         .catchesNativeExceptions = true});
  }

  result.program.constructors.reserve(source.constructorInstances().size());
  for (const HirConstructorInstance &instance : source.constructorInstances()) {
    const MirClassInstance *constructorOwner =
        result.program.findClassInstance(instance.owner);
    std::vector<MirConstructorInitializer> initializers;
    initializers.reserve(instance.initializers.size());
    for (const HirConstructorInitializer &initializer : instance.initializers) {
      initializers.push_back(
          {.kind = initializer.kind,
           .targetType = initializer.targetType,
           .field = initializer.field,
           .base = initializer.base,
           .constructorTarget = initializer.constructorTarget,
           .arguments = initializer.arguments,
           .explicitArgumentCount = initializer.explicitArgumentCount,
           .storesReference = initializer.storesReference,
           .borrowAccess = initializer.borrowAccess,
           .generatedDefault = initializer.generatedDefault,
           .ownedParameter = initializer.ownedParameter});
    }
    result.program.constructors.push_back(
        {.id = instance.id,
         .owner = instance.owner,
         .constexprConstructor = instance.constexprConstructor,
         .generatedDefault = instance.generatedDefault,
         .typeArguments = instance.typeArguments,
         .valueArguments = instance.valueArguments,
         .parameterTypes = instance.parameterTypes,
         .parameterBindings = instance.parameterBindings,
         .borrowOrigin = instance.borrowOrigin,
         .borrowParameter = instance.borrowParameter,
         .borrowAccess = instance.borrowAccess,
         .borrowParameters = instance.borrowParameters,
         .definitionKind =
             instance.generatedDefault || (instance.source != nullptr &&
                                           instance.source->body() != nullptr)
                 ? MirDefinitionKind::Source
                 : MirDefinitionKind::Declaration,
         .mayRaiseDefinedFailure =
             definedFailureEffects.constructors.empty() || instance.id == 0 ||
             instance.id > definedFailureEffects.constructors.size() ||
             definedFailureEffects.constructors[instance.id - 1],
         .initializers = std::move(initializers),
         .body = lowerBody(
             source, failureMetadata, instance.body, MirBodyKind::Constructor,
             SemanticType::Void, instance.initializerValues,
             definedFailureEffects, valid, result.errors, instance.id, false,
             &instance.initializers, nullptr, &instance, nullptr, nullptr,
             nullptr, compilerTrustedSourceUnit(source, instance.sourceUnit),
             constructorOwner)});
  }

  result.program.destructors.reserve(source.destructorInstances().size());
  for (const HirDestructorInstance &instance : source.destructorInstances()) {
    result.program.destructors.push_back(
        {.id = instance.id,
         .owner = instance.owner,
         .definitionKind =
             instance.source != nullptr && instance.source->body() != nullptr
                 ? MirDefinitionKind::Source
                 : MirDefinitionKind::Declaration,
         .mayRaiseDefinedFailure =
             definedFailureEffects.destructors.empty() || instance.id == 0 ||
             instance.id > definedFailureEffects.destructors.size() ||
             definedFailureEffects.destructors[instance.id - 1],
         .body = lowerBody(
             source, failureMetadata, instance.body, MirBodyKind::Destructor,
             SemanticType::Void, {}, definedFailureEffects, valid,
             result.errors, instance.id, false, nullptr, nullptr, nullptr,
             nullptr, nullptr, nullptr,
             compilerTrustedSourceUnit(source, instance.sourceUnit), nullptr,
             &instance)});
  }

  result.program.lambdas.reserve(source.lambdaInstances().size());
  for (const HirLambda &instance : source.lambdaInstances()) {
    MirLambdaInstance lowered{.id = instance.id,
                              .declaration = instance.declaration,
                              .type = instance.type,
                              .returnType = instance.returnType,
                              .parameterTypes = instance.parameterTypes,
                              .parameterBindings = instance.parameterBindings};
    lowered.captureTypes.reserve(instance.captures.size());
    lowered.captureModes.reserve(instance.captures.size());
    lowered.captureSymbols.reserve(instance.captures.size());
    lowered.captureRequiresActiveCleanup.reserve(instance.captures.size());
    for (std::size_t capture = 0; capture < instance.captures.size();
         ++capture) {
      lowered.captureTypes.push_back(instance.captures[capture].type);
      lowered.captureModes.push_back(instance.captures[capture].mode);
      lowered.captureSymbols.push_back(
          instance.captures[capture].bindingSymbol);
      lowered.captureRequiresActiveCleanup.push_back(
          capture < instance.captureRequiresActiveCleanup.size()
              ? instance.captureRequiresActiveCleanup[capture]
              : false);
    }
    lowered.body =
        lowerBody(source, failureMetadata, instance.body, MirBodyKind::Lambda,
                  instance.returnType, {}, definedFailureEffects, valid,
                  result.errors, instance.id, false, nullptr, nullptr, nullptr,
                  &instance, nullptr, nullptr,
                  instance.source != nullptr &&
                      compilerTrustedSourcePath(
                          source, instance.source->bracket().source));
    result.program.lambdas.push_back(std::move(lowered));
  }
  if (includeProgramInitialization) {
    valid =
        lowerHostedStartup(source, failureMetadata, result.program) && valid;
  }
  result.program.threadTasks.reserve(source.threadTaskAdapters().size());
  for (const HirThreadTaskAdapter &adapter : source.threadTaskAdapters()) {
    const HirFunctionInstance *functionTarget =
        adapter.functionTarget
            ? source.findFunctionInstance(*adapter.functionTarget)
            : nullptr;
    const HirLambda *lambdaTarget =
        adapter.lambdaTarget ? source.findLambda(*adapter.lambdaTarget)
                             : nullptr;
    const SemanticType resultType =
        functionTarget != nullptr ? functionTarget->returnType
        : lambdaTarget != nullptr ? lambdaTarget->returnType
                                  : SemanticType::Unknown;
    MirBodyAddress sourceBody;
    MirInstructionId spawnInstruction = 0;
    for (const MirBodyAddress address :
         enumerateMirBodyAddresses(result.program)) {
      const MirBody *body = findMirBody(result.program, address);
      if (body == nullptr) {
        continue;
      }
      for (const MirBlock &block : body->blocks) {
        const auto found = std::find_if(
            block.instructions.begin(), block.instructions.end(),
            [&](const MirInstruction &instruction) {
              return instruction.hirValue == adapter.spawnValue &&
                     instruction.threadTaskAdapter == adapter.id &&
                     instruction.intrinsic == IntrinsicKind::ThreadSpawn;
            });
        if (found != block.instructions.end()) {
          if (spawnInstruction != 0) {
            valid = false;
          }
          sourceBody = address;
          spawnInstruction = found->id;
        }
      }
    }
    if (spawnInstruction == 0 || adapter.id == 0 ||
        adapter.taskType == SemanticType::Unknown ||
        resultType == SemanticType::Unknown ||
        adapter.functionTarget.has_value() ==
            adapter.lambdaTarget.has_value() ||
        adapter.invocation != CallableInvocationCapability::Once) {
      valid = false;
      continue;
    }
    result.program.threadTasks.push_back(
        {.id = adapter.id,
         .taskType = adapter.taskType,
         .resultType = resultType,
         .spawnValue = adapter.spawnValue,
         .sourceBody = sourceBody,
         .spawnInstruction = spawnInstruction,
         .functionTarget = adapter.functionTarget,
         .lambdaTarget = adapter.lambdaTarget,
         .targetInvocation = adapter.targetInvocation,
         .invocation = adapter.invocation,
         .catchesWorkerFailure = true,
         .catchesNativeExceptions = true});
  }
  result.program.valid_ = valid;
  const MirProgramEffectSummaries effectSummaries =
      deriveMirProgramEffectSummaries(result.program);
  const bool completeEffects =
      effectSummaries.functions.size() == result.program.functions.size() &&
      effectSummaries.constructors.size() ==
          result.program.constructors.size() &&
      effectSummaries.destructors.size() == result.program.destructors.size() &&
      effectSummaries.lambdas.size() == result.program.lambdas.size();
  if (completeEffects) {
    for (std::size_t index = 0; index < result.program.functions.size();
         ++index) {
      result.program.functions[index].effectSummary =
          effectSummaries.functions[index];
    }
    for (std::size_t index = 0; index < result.program.constructors.size();
         ++index) {
      result.program.constructors[index].effectSummary =
          effectSummaries.constructors[index];
    }
    for (std::size_t index = 0; index < result.program.destructors.size();
         ++index) {
      result.program.destructors[index].effectSummary =
          effectSummaries.destructors[index];
    }
    for (std::size_t index = 0; index < result.program.lambdas.size();
         ++index) {
      result.program.lambdas[index].effectSummary =
          effectSummaries.lambdas[index];
    }
  }
  result.program.valid_ = result.program.valid_ && completeEffects;
  return result;
}

} // namespace lang
