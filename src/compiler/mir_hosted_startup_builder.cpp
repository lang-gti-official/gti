#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_lowering.h"
#include "gti/mir_verifier.h"

namespace lang {
bool MirLowerer::lowerHostedStartup(const HirProgram &source,
                                    const FailureMetadata &failureMetadata,
                                    MirProgram &program) {
  const std::optional<HirHostedProgramEntryPlan> &hirPlan =
      source.hostedProgramEntryPlan();
  if (!hirPlan) {
    program.hostedStartupPlan_.reset();
    program.hostedStartupBody.reset();
    return true;
  }

  const MirFunctionInstance *entry =
      program.findFunctionInstance(hirPlan->entry);
  const MirFunctionInstance *append =
      program.findFunctionInstance(hirPlan->appendFunction);
  const MirConstructorInstance *vectorConstructor =
      program.findConstructorInstance(hirPlan->vectorConstructor);
  const MirConstructorInstance *stringConstructor =
      program.findConstructorInstance(hirPlan->stringConstructor);
  bool valid = entry != nullptr && entry->entryKind == hirPlan->kind &&
               entry->returnType == SemanticType::Int32 &&
               hirPlan->sourceUnit != 0 &&
               hirPlan->mainAnchor.end >= hirPlan->mainAnchor.start &&
               hirPlan->mainAnchor.line >= 1;
  const bool callsProgramInitialization = std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });

  MirHostedStartupPlan plan;
  plan.kind = hirPlan->kind;
  plan.entry = hirPlan->entry;
  plan.appendFunction = hirPlan->appendFunction;
  plan.vectorConstructor = hirPlan->vectorConstructor;
  plan.stringConstructor = hirPlan->stringConstructor;
  plan.sourceAnchor = {.sourceUnit = hirPlan->sourceUnit,
                       .start = hirPlan->mainAnchor.start,
                       .end = hirPlan->mainAnchor.end,
                       .line = hirPlan->mainAnchor.line};
  plan.programInitializationTarget = {.kind = MirBodyKind::Module};
  plan.exitPolicy = MirHostedStartupExitPolicy::ImmediateExit70;
  if (program.requiresCThreadRuntime()) {
    for (const MirFunctionInstance &function : program.functionInstances()) {
      if (function.cThreadOperation == CThreadRuntimeOperation::Begin) {
        valid = valid && plan.cThreadBegin == 0;
        plan.cThreadBegin = function.id;
      } else if (function.cThreadOperation == CThreadRuntimeOperation::End) {
        valid = valid && plan.cThreadEnd == 0;
        plan.cThreadEnd = function.id;
      }
    }
    valid = valid && plan.cThreadBegin != 0 && plan.cThreadEnd != 0;
  }

  MirBody body;
  body.kind = MirBodyKind::HostedStartup;
  body.returnType = SemanticType::Int32;
  std::size_t maximumBodyDomain = 0;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    if (const MirBody *candidate = findMirBody(program, address)) {
      maximumBodyDomain =
          std::max(maximumBodyDomain, candidate->placeDomain.body);
    }
  }
  body.placeDomain = {.snapshot = program.module().placeDomain.snapshot,
                      .body = maximumBodyDomain + 1};
  valid = valid && body.placeDomain.snapshot != 0 && body.placeDomain.body != 0;

  MirInstructionId nextInstruction = 1;
  MirTemporaryId nextTemporary = 1;
  // The hosted schedule is a closed, bounded graph. Reserve for every normal,
  // primary-cleanup, and first-secondary block before retaining references.
  body.blocks.reserve(64);
  const auto appendBlock = [&](MirFailureRecordId activeFailure =
                                   0) -> MirBlock & {
    MirBlock block;
    block.id = body.blocks.size() + 1;
    block.activeFailure = activeFailure;
    body.blocks.push_back(std::move(block));
    return body.blocks.back();
  };
  const auto beginOperation = [&](MirHostedStartupOperationKind kind,
                                  MirHostedStartupFailureBehavior behavior,
                                  MirBlockId block) {
    const MirHostedStartupOperationId id = plan.operations.size() + 1;
    plan.operations.push_back(
        {.id = id, .kind = kind, .failureBehavior = behavior, .block = block});
    return id;
  };
  const auto appendValue = [&](MirHostedStartupOperationId operation,
                               MirBlockId block, MirInstructionId instruction,
                               ExpressionInfo info) {
    const MirValueId id = body.values.size() + 1;
    body.values.push_back({.id = id,
                           .hostedStartupOperation = operation,
                           .sourceValue = 0,
                           .info = std::move(info),
                           .definitionBlock = block,
                           .definition = instruction});
    body.valueUses.emplace_back();
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.value != 0) {
      valid = false;
    }
    row.value = id;
    return id;
  };
  const auto appendPlace = [&](MirHostedStartupOperationId operation,
                               MirPlace place) {
    const MirPlaceId id = body.places.size() + 1;
    place.id = id;
    place.hostedStartupOperation = operation;
    body.places.push_back(std::move(place));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.place != 0) {
      valid = false;
    }
    row.place = id;
    return id;
  };
  const auto appendDrop = [&](MirHostedStartupOperationId operation,
                              MirDropObligation drop) {
    const MirDropObligationId id = body.dropObligations.size() + 1;
    drop.id = id;
    drop.hostedStartupOperation = operation;
    drop.constructionOrder = id;
    body.dropObligations.push_back(std::move(drop));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.dropObligation != 0) {
      valid = false;
    }
    row.dropObligation = id;
    return id;
  };
  const auto appendInstruction = [&](MirHostedStartupOperationId operation,
                                     MirBlock &block,
                                     MirInstruction instruction) {
    instruction.id = nextInstruction++;
    instruction.hostedStartupOperation = operation;
    block.instructions.push_back(std::move(instruction));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.instruction != 0 || row.terminator) {
      valid = false;
    }
    row.instruction = block.instructions.back().id;
    return block.instructions.back().id;
  };
  const auto terminate = [&](MirHostedStartupOperationId operation,
                             MirBlock &block, MirTerminator terminator) {
    terminator.hostedStartupOperation = operation;
    block.terminator = std::move(terminator);
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.instruction != 0 || row.terminator) {
      valid = false;
    }
    row.terminator = true;
  };
  const auto generatedInfo = [](const SemanticType &type,
                                ValueCategory category = ValueCategory::Value,
                                AccessMode access = AccessMode::ReadOnly) {
    return ExpressionInfo{.type = type,
                          .category = category,
                          .access = access,
                          .traits = semanticTraits(type)};
  };
  const auto exactLocalFailure = [&](DefinedFailureOperation operation) {
    std::vector<FailureSiteId> sites;
    sites.reserve(operation.localOrigins.size());
    for (const DefinedFailureOrigin &origin : operation.localOrigins) {
      const std::optional<FailureSiteId> site = failureMetadata.siteFor(origin);
      if (!site || *site == 0) {
        valid = false;
        sites.push_back(0);
      } else {
        sites.push_back(*site);
      }
    }
    return std::pair{std::move(operation), std::move(sites)};
  };
  const auto propagation = [](FailurePropagationKind kind, bool mayRaise) {
    DefinedFailureOperation result;
    result.propagation = mayRaise ? kind : FailurePropagationKind::None;
    return result;
  };
  const auto failureBehavior = [](bool mayRaise) {
    return mayRaise ? MirHostedStartupFailureBehavior::Propagate
                    : MirHostedStartupFailureBehavior::None;
  };
  const bool moduleMayRaise = std::any_of(
      program.module().blocks.begin(), program.module().blocks.end(),
      [](const MirBlock &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [](const MirInstruction &instruction) {
                             return !instruction.definedFailure.empty();
                           });
      });
  const auto generatedDropType = [&](const SemanticType &type,
                                     HirClassInstanceId owner) {
    const MirClassInstance *instance = program.findClassInstance(owner);
    if (instance == nullptr || instance->type != type ||
        semanticTraits(type).drop != DropKind::Lexical ||
        !instance->requiresActiveCleanup) {
      valid = false;
    }
    if (auto cleanup = mirUniqueCleanupType(program, type))
      return *cleanup;
    return MirDropType{
        .type = type,
        .classInstance = owner == 0 ? std::nullopt : std::optional{owner},
        .destructor = instance == nullptr ? std::nullopt : instance->destructor,
        .requiresActiveCleanup =
            instance != nullptr && instance->requiresActiveCleanup};
  };
  const auto appendFailureRecord = [&](MirHostedStartupOperationId operation,
                                       MirFailureRecord record) {
    const MirFailureRecordId id = body.failureRecords.size() + 1;
    record.id = id;
    record.hostedStartupOperation = operation;
    body.failureRecords.push_back(std::move(record));
    MirHostedStartupOperation &row = plan.operations[operation - 1];
    if (row.failureRecord != 0) {
      valid = false;
    }
    row.failureRecord = id;
    return id;
  };
  const auto appendCleanupBoundary =
      [&](MirHostedStartupOperationId operation,
          std::vector<MirDropObligationId> obligations) {
        const std::size_t id = body.cleanupBoundaries.size() + 1;
        body.cleanupBoundaries.push_back(
            {.id = id,
             .hostedStartupOperation = operation,
             .kind = MirCleanupBoundaryKind::Failure,
             .obligations = std::move(obligations)});
        MirHostedStartupOperation &row = plan.operations[operation - 1];
        if (row.cleanupBoundary != 0) {
          valid = false;
        }
        row.cleanupBoundary = id;
        return id;
      };
  const auto destructorMayRaise = [&](MirDropObligationId obligation) {
    const MirDropObligation *drop = body.findDropObligation(obligation);
    return drop != nullptr &&
           mirDropMayRaiseDefinedFailure(program, drop->dropType);
  };
  const auto emitFailureCleanup =
      [&](MirBlock &parameterBlock, MirFailureRecordId primary,
          const std::vector<MirDropObligationId> &activeDrops) {
        MirBlock *cleanup = &parameterBlock;
        std::vector<MirDropObligationId> boundaryDrops;
        boundaryDrops.reserve(activeDrops.size());
        for (auto candidate = activeDrops.rbegin();
             candidate != activeDrops.rend(); ++candidate) {
          const MirDropObligation *drop = body.findDropObligation(*candidate);
          const MirPlace *place =
              drop == nullptr ? nullptr : body.findPlace(drop->place);
          if (drop == nullptr || place == nullptr) {
            valid = false;
            continue;
          }
          const bool mayRaise = destructorMayRaise(*candidate);
          const MirHostedStartupOperationId dropOperation =
              beginOperation(MirHostedStartupOperationKind::DropFailureCleanup,
                             failureBehavior(mayRaise), cleanup->id);
          MirInstruction cleanupDrop;
          cleanupDrop.kind = MirInstructionKind::Drop;
          cleanupDrop.destination = drop->place;
          cleanupDrop.definedFailure =
              propagation(FailurePropagationKind::Destructor, mayRaise);
          cleanupDrop.info = generatedInfo(
              drop->dropType.type, ValueCategory::Place, AccessMode::Mutable);
          cleanupDrop.lifecycle = {{.kind = MirLifecycleEventKind::Drop,
                                    .source = *candidate,
                                    .failureCleanup = true}};
          const MirInstructionId dropInstruction = appendInstruction(
              dropOperation, *cleanup, std::move(cleanupDrop));
          boundaryDrops.push_back(*candidate);

          if (!mayRaise) {
            continue;
          }
          const MirBlockId producerBlock = cleanup->id;
          MirBlock &normal = appendBlock(primary);
          MirBlock &secondaryBlock = appendBlock(primary);
          const MirHostedStartupOperationId routeOperation = beginOperation(
              MirHostedStartupOperationKind::RouteCleanupFailure,
              MirHostedStartupFailureBehavior::None, producerBlock);
          const MirFailureRecordId secondary = appendFailureRecord(
              routeOperation, {.producerBlock = producerBlock,
                               .producerInstruction = dropInstruction,
                               .parameterBlock = secondaryBlock.id});
          secondaryBlock.failureParameter = secondary;
          terminate(routeOperation, *cleanup,
                    {.kind = MirTerminatorKind::Invoke,
                     .invokeInstruction = dropInstruction,
                     .failureRecord = secondary,
                     .target = normal.id,
                     .elseTarget = secondaryBlock.id});

          const MirHostedStartupOperationId terminateOperation = beginOperation(
              MirHostedStartupOperationKind::TerminateCleanupFailure,
              MirHostedStartupFailureBehavior::None, secondaryBlock.id);
          terminate(terminateOperation, secondaryBlock,
                    {.kind = MirTerminatorKind::TerminateCleanupFailure,
                     .failureRecord = secondary});
          cleanup = &normal;
        }

        if (!boundaryDrops.empty()) {
          const MirHostedStartupOperationId boundaryOperation = beginOperation(
              MirHostedStartupOperationKind::EndFailureCleanup,
              MirHostedStartupFailureBehavior::None, cleanup->id);
          const std::size_t boundary = appendCleanupBoundary(
              boundaryOperation, std::move(boundaryDrops));
          appendInstruction(boundaryOperation, *cleanup,
                            {.kind = MirInstructionKind::Lifecycle,
                             .cleanupBoundaryEnd = boundary});
        }
        const MirHostedStartupOperationId containOperation =
            beginOperation(MirHostedStartupOperationKind::ContainFailure,
                           MirHostedStartupFailureBehavior::None, cleanup->id);
        terminate(containOperation, *cleanup,
                  {.kind = MirTerminatorKind::ContainFailure,
                   .failureRecord = primary});
      };
  const auto routeOperationFailure =
      [&](MirBlock *&producer, MirInstructionId instruction,
          const std::vector<MirDropObligationId> &activeDrops,
          MirDropObligationId successDrop = 0) {
        if (producer == nullptr || producer->instructions.empty() ||
            producer->instructions.back().id != instruction) {
          valid = false;
          return;
        }
        const MirBlockId producerBlock = producer->id;
        MirBlock &normal = appendBlock();
        MirBlock &failureBlock = appendBlock();
        const MirHostedStartupOperationId routeOperation = beginOperation(
            MirHostedStartupOperationKind::RouteOperationFailure,
            MirHostedStartupFailureBehavior::None, producerBlock);
        const MirFailureRecordId primary = appendFailureRecord(
            routeOperation, {.producerBlock = producerBlock,
                             .producerInstruction = instruction,
                             .parameterBlock = failureBlock.id});
        failureBlock.failureParameter = primary;
        failureBlock.activeFailure = primary;
        MirTerminator invoke{.kind = MirTerminatorKind::Invoke,
                             .invokeInstruction = instruction,
                             .failureRecord = primary,
                             .target = normal.id,
                             .elseTarget = failureBlock.id};
        if (successDrop != 0) {
          invoke.successLifecycle = {{.kind = MirLifecycleEventKind::Initialize,
                                      .target = successDrop}};
        }
        terminate(routeOperation, *producer, std::move(invoke));
        emitFailureCleanup(failureBlock, primary, activeDrops);
        producer = &normal;
      };

  MirBlock &entryBlock = appendBlock();
  body.entry = entryBlock.id;
  if (!program.threadLocalStorage().empty()) {
    const auto operation =
        beginOperation(MirHostedStartupOperationKind::BeginThreadLocals,
                       MirHostedStartupFailureBehavior::None, entryBlock.id);
    MirInstruction begin;
    begin.kind = MirInstructionKind::BeginThreadLocals;
    begin.info = generatedInfo(SemanticType::Void);
    appendInstruction(operation, entryBlock, std::move(begin));
  }
  const auto drainThreadLocals = [&](MirBlock &block) {
    if (program.threadLocalStorage().empty())
      return;
    const auto operation =
        beginOperation(MirHostedStartupOperationKind::DrainThreadLocals,
                       MirHostedStartupFailureBehavior::None, block.id);
    MirInstruction drain;
    drain.kind = MirInstructionKind::DrainThreadLocals;
    drain.info = generatedInfo(SemanticType::Void);
    appendInstruction(operation, block, std::move(drain));
  };
  const auto cThreadBoundary = [&](MirBlock &block, bool begin) {
    if (!program.requiresCThreadRuntime()) {
      return;
    }
    const auto operation = beginOperation(
        begin ? MirHostedStartupOperationKind::BeginCThreadRuntime
              : MirHostedStartupOperationKind::EndCThreadRuntime,
        MirHostedStartupFailureBehavior::None, block.id);
    MirInstruction call;
    call.kind = MirInstructionKind::Call;
    call.functionTarget = begin ? plan.cThreadBegin : plan.cThreadEnd;
    call.info = generatedInfo(SemanticType::Void);
    appendInstruction(operation, block, std::move(call));
  };

  if (hirPlan->kind == ProgramEntryKind::NoArguments) {
    MirBlock *startupBlock = &entryBlock;
    valid = valid && entry != nullptr && entry->parameterTypes.empty() &&
            hirPlan->appendFunction == 0 && hirPlan->vectorConstructor == 0 &&
            hirPlan->stringConstructor == 0 && hirPlan->validateCount.empty() &&
            hirPlan->convertCount.empty();

    if (callsProgramInitialization) {
      const MirHostedStartupOperationId bodyCallOperation = beginOperation(
          MirHostedStartupOperationKind::CallProgramInitialization,
          failureBehavior(moduleMayRaise), startupBlock->id);
      MirInstruction bodyCall;
      bodyCall.kind = MirInstructionKind::CallBody;
      bodyCall.bodyTarget = plan.programInitializationTarget;
      bodyCall.definedFailure =
          propagation(FailurePropagationKind::BodyCall, moduleMayRaise);
      bodyCall.info = generatedInfo(SemanticType::Void);
      const MirInstructionId bodyCallInstruction = appendInstruction(
          bodyCallOperation, *startupBlock, std::move(bodyCall));
      if (moduleMayRaise) {
        routeOperationFailure(startupBlock, bodyCallInstruction, {});
      }
    }

    cThreadBoundary(*startupBlock, true);
    const bool entryMayRaise =
        entry != nullptr && entry->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId entryCallOperation =
        beginOperation(MirHostedStartupOperationKind::CallEntry,
                       failureBehavior(entryMayRaise), startupBlock->id);
    const MirInstructionId entryCallInstruction = nextInstruction;
    const MirValueId entryResult =
        appendValue(entryCallOperation, startupBlock->id, entryCallInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction entryCall;
    entryCall.kind = MirInstructionKind::Call;
    entryCall.result = entryResult;
    entryCall.functionTarget = hirPlan->entry;
    entryCall.definedFailure =
        propagation(FailurePropagationKind::DirectCall, entryMayRaise);
    entryCall.info = generatedInfo(SemanticType::Int32);
    appendInstruction(entryCallOperation, *startupBlock, std::move(entryCall));
    if (entryMayRaise) {
      routeOperationFailure(startupBlock, entryCallInstruction, {});
    }
    plan.entryResult = entryResult;

    drainThreadLocals(*startupBlock);
    cThreadBoundary(*startupBlock, false);

    const MirHostedStartupOperationId returnOperation =
        beginOperation(MirHostedStartupOperationKind::ReturnEntry,
                       MirHostedStartupFailureBehavior::None, startupBlock->id);
    MirTerminator returnEntry;
    returnEntry.kind = MirTerminatorKind::Return;
    returnEntry.value = MirOperand{.kind = MirOperandKind::Value,
                                   .value = entryResult,
                                   .type = SemanticType::Int32};
    terminate(returnOperation, *startupBlock, std::move(returnEntry));
  } else {
    MirBlock *setupBlock = &entryBlock;
    const bool exactOwnedShape =
        hirPlan->kind == ProgramEntryKind::OwnedArguments && entry != nullptr &&
        entry->parameterTypes.size() == 2 &&
        entry->parameterTypes.front() == SemanticType::Int32 &&
        entry->parameterTypes.back().kind == SemanticType::Class &&
        entry->parameterTypes.back().arguments.size() == 1 &&
        append != nullptr && vectorConstructor != nullptr &&
        stringConstructor != nullptr &&
        hirPlan->appendFunction == entry->entryArgumentAppendTarget.value_or(0);
    valid = valid && exactOwnedShape && !hirPlan->validateCount.empty() &&
            !hirPlan->convertCount.empty();
    const SemanticType vectorType =
        exactOwnedShape ? entry->parameterTypes.back() : SemanticType::Unknown;
    const SemanticType stringType =
        exactOwnedShape ? vectorType.arguments.front() : SemanticType::Unknown;

    const auto [validateFailure, validateSites] =
        exactLocalFailure(hirPlan->validateCount);
    const MirHostedStartupOperationId validateOperation =
        beginOperation(MirHostedStartupOperationKind::ValidateArgumentCount,
                       MirHostedStartupFailureBehavior::Detect, setupBlock->id);
    const MirInstructionId validateInstruction = nextInstruction;
    const MirValueId validatedCount =
        appendValue(validateOperation, setupBlock->id, validateInstruction,
                    generatedInfo(SemanticType::Int64));
    MirInstruction validate;
    validate.kind = MirInstructionKind::Load;
    validate.result = validatedCount;
    validate.definedFailure = validateFailure;
    validate.localFailureSites = validateSites;
    validate.info = generatedInfo(SemanticType::Int64);
    appendInstruction(validateOperation, *setupBlock, std::move(validate));
    routeOperationFailure(setupBlock, validateInstruction, {});

    const auto [convertFailure, convertSites] =
        exactLocalFailure(hirPlan->convertCount);
    const MirHostedStartupOperationId convertOperation =
        beginOperation(MirHostedStartupOperationKind::ConvertArgumentCount,
                       MirHostedStartupFailureBehavior::Detect, setupBlock->id);
    const MirInstructionId convertInstruction = nextInstruction;
    const MirValueId stabilizedCount =
        appendValue(convertOperation, setupBlock->id, convertInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction convert;
    convert.kind = MirInstructionKind::Compute;
    convert.result = stabilizedCount;
    convert.operands = {{.kind = MirOperandKind::Value,
                         .value = validatedCount,
                         .type = SemanticType::Int64}};
    // Native argc conversion is a sealed hosted-boundary operation rather
    // than an ordinary source-language numeric conversion.
    convert.operation = MirOperation::None;
    convert.definedFailure = convertFailure;
    convert.localFailureSites = convertSites;
    convert.info = generatedInfo(SemanticType::Int32);
    appendInstruction(convertOperation, *setupBlock, std::move(convert));
    routeOperationFailure(setupBlock, convertInstruction, {});
    plan.stabilizedCount = stabilizedCount;

    if (callsProgramInitialization) {
      const MirHostedStartupOperationId bodyCallOperation = beginOperation(
          MirHostedStartupOperationKind::CallProgramInitialization,
          failureBehavior(moduleMayRaise), setupBlock->id);
      MirInstruction bodyCall;
      bodyCall.kind = MirInstructionKind::CallBody;
      bodyCall.bodyTarget = plan.programInitializationTarget;
      bodyCall.definedFailure =
          propagation(FailurePropagationKind::BodyCall, moduleMayRaise);
      bodyCall.info = generatedInfo(SemanticType::Void);
      const MirInstructionId bodyCallInstruction = appendInstruction(
          bodyCallOperation, *setupBlock, std::move(bodyCall));
      if (moduleMayRaise) {
        routeOperationFailure(setupBlock, bodyCallInstruction, {});
      }
    }

    cThreadBoundary(*setupBlock, true);
    const bool vectorMayRaise = vectorConstructor != nullptr &&
                                vectorConstructor->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId vectorOperation =
        beginOperation(MirHostedStartupOperationKind::ConstructArgumentVector,
                       failureBehavior(vectorMayRaise), setupBlock->id);
    const MirInstructionId vectorInstruction = nextInstruction;
    const MirValueId vectorValue =
        appendValue(vectorOperation, setupBlock->id, vectorInstruction,
                    generatedInfo(vectorType));
    const MirPlaceId vectorPlace =
        appendPlace(vectorOperation, {.root = MirPlaceRootKind::Value,
                                      .value = vectorValue,
                                      .type = vectorType,
                                      .access = AccessMode::Mutable,
                                      .traits = semanticTraits(vectorType)});
    const MirDropObligationId vectorDrop = appendDrop(
        vectorOperation,
        {.kind = MirDropObligationKind::Value,
         .place = vectorPlace,
         .generatedValue = vectorValue,
         .dropType = generatedDropType(
             vectorType,
             vectorConstructor == nullptr ? 0 : vectorConstructor->owner)});
    MirInstruction constructVector;
    constructVector.kind = MirInstructionKind::Construct;
    constructVector.result = vectorValue;
    constructVector.constructorTarget = hirPlan->vectorConstructor;
    constructVector.definedFailure =
        propagation(FailurePropagationKind::Constructor, vectorMayRaise);
    constructVector.info = generatedInfo(vectorType);
    if (vectorMayRaise) {
      constructVector.successResultDrop = vectorDrop;
    } else {
      constructVector.lifecycle = {
          {.kind = MirLifecycleEventKind::Initialize, .target = vectorDrop}};
    }
    appendInstruction(vectorOperation, *setupBlock, std::move(constructVector));
    if (vectorMayRaise) {
      routeOperationFailure(setupBlock, vectorInstruction, {}, vectorDrop);
    }
    plan.argumentVector = vectorValue;
    plan.argumentVectorPlace = vectorPlace;

    const MirHostedStartupOperationId initializeIndexOperation =
        beginOperation(MirHostedStartupOperationKind::InitializeArgumentIndex,
                       MirHostedStartupFailureBehavior::None, setupBlock->id);
    const MirPlaceId indexPlace =
        appendPlace(initializeIndexOperation,
                    {.root = MirPlaceRootKind::Temporary,
                     .temporary = nextTemporary++,
                     .type = SemanticType::Int32,
                     .access = AccessMode::Mutable,
                     .traits = semanticTraits(SemanticType::Int32)});
    MirInstruction initializeIndex;
    initializeIndex.kind = MirInstructionKind::Initialize;
    initializeIndex.destination = indexPlace;
    initializeIndex.operands = {{.kind = MirOperandKind::Constant,
                                 .literal = Literal{std::uint64_t{0}},
                                 .type = SemanticType::Int32}};
    initializeIndex.info = generatedInfo(
        SemanticType::Int32, ValueCategory::Place, AccessMode::Mutable);
    appendInstruction(initializeIndexOperation, *setupBlock,
                      std::move(initializeIndex));
    plan.argumentIndexPlace = indexPlace;

    MirBlock &loopHeader = appendBlock();
    const MirHostedStartupOperationId enterLoopOperation =
        beginOperation(MirHostedStartupOperationKind::EnterArgumentLoop,
                       MirHostedStartupFailureBehavior::None, setupBlock->id);
    MirTerminator enterLoop;
    enterLoop.kind = MirTerminatorKind::Goto;
    enterLoop.target = loopHeader.id;
    terminate(enterLoopOperation, *setupBlock, std::move(enterLoop));

    const MirHostedStartupOperationId loadIndexOperation =
        beginOperation(MirHostedStartupOperationKind::LoadArgumentIndex,
                       MirHostedStartupFailureBehavior::None, loopHeader.id);
    const MirInstructionId loadIndexInstruction = nextInstruction;
    const MirValueId indexValue =
        appendValue(loadIndexOperation, loopHeader.id, loadIndexInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction loadIndex;
    loadIndex.kind = MirInstructionKind::Load;
    loadIndex.result = indexValue;
    loadIndex.operands = {{.kind = MirOperandKind::Copy,
                           .place = indexPlace,
                           .type = SemanticType::Int32}};
    loadIndex.info = generatedInfo(SemanticType::Int32);
    appendInstruction(loadIndexOperation, loopHeader, std::move(loadIndex));

    const MirHostedStartupOperationId testIndexOperation =
        beginOperation(MirHostedStartupOperationKind::TestArgumentIndex,
                       MirHostedStartupFailureBehavior::None, loopHeader.id);
    const MirInstructionId testIndexInstruction = nextInstruction;
    const MirValueId hasArgument =
        appendValue(testIndexOperation, loopHeader.id, testIndexInstruction,
                    generatedInfo(SemanticType::Bool));
    MirInstruction testIndex;
    testIndex.kind = MirInstructionKind::Compute;
    testIndex.result = hasArgument;
    testIndex.operands = {{.kind = MirOperandKind::Value,
                           .value = indexValue,
                           .type = SemanticType::Int32},
                          {.kind = MirOperandKind::Value,
                           .value = stabilizedCount,
                           .type = SemanticType::Int32}};
    testIndex.operation = MirOperation::Less;
    testIndex.info = generatedInfo(SemanticType::Bool);
    appendInstruction(testIndexOperation, loopHeader, std::move(testIndex));

    MirBlock &loopBody = appendBlock();
    MirBlock &entryCallBlock = appendBlock();
    MirBlock *iterationBlock = &loopBody;
    const MirHostedStartupOperationId branchLoopOperation =
        beginOperation(MirHostedStartupOperationKind::BranchArgumentLoop,
                       MirHostedStartupFailureBehavior::None, loopHeader.id);
    MirTerminator branchLoop;
    branchLoop.kind = MirTerminatorKind::Branch;
    branchLoop.value = MirOperand{.kind = MirOperandKind::Value,
                                  .value = hasArgument,
                                  .type = SemanticType::Bool};
    branchLoop.target = loopBody.id;
    branchLoop.elseTarget = entryCallBlock.id;
    terminate(branchLoopOperation, loopHeader, std::move(branchLoop));

    const MirHostedStartupOperationId viewOperation = beginOperation(
        MirHostedStartupOperationKind::ReadArgumentView,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId viewInstruction = nextInstruction;
    const MirValueId argumentView =
        appendValue(viewOperation, iterationBlock->id, viewInstruction,
                    generatedInfo(SemanticType::StringView));
    MirInstruction readView;
    readView.kind = MirInstructionKind::Compute;
    readView.result = argumentView;
    readView.operands = {{.kind = MirOperandKind::Value,
                          .value = indexValue,
                          .type = SemanticType::Int32}};
    readView.operation = MirOperation::None;
    readView.info = generatedInfo(SemanticType::StringView);
    appendInstruction(viewOperation, *iterationBlock, std::move(readView));

    const MirHostedStartupOperationId stringInputOperation = beginOperation(
        MirHostedStartupOperationKind::PrepareStringConstructorArgument,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId stringInputInstruction = nextInstruction;
    const MirValueId preparedView = appendValue(
        stringInputOperation, iterationBlock->id, stringInputInstruction,
        generatedInfo(SemanticType::StringView));
    MirInstruction prepareView;
    prepareView.kind = MirInstructionKind::CallInput;
    prepareView.callInputRole = MirCallInputRole::Argument;
    prepareView.callInputKind = HirCallInputKind::Value;
    prepareView.result = preparedView;
    prepareView.operands = {{.kind = MirOperandKind::Value,
                             .value = argumentView,
                             .type = SemanticType::StringView}};
    prepareView.info = generatedInfo(SemanticType::StringView);
    appendInstruction(stringInputOperation, *iterationBlock,
                      std::move(prepareView));

    const bool stringMayRaise = stringConstructor != nullptr &&
                                stringConstructor->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId stringOperation =
        beginOperation(MirHostedStartupOperationKind::ConstructArgumentString,
                       failureBehavior(stringMayRaise), iterationBlock->id);
    const MirInstructionId stringInstruction = nextInstruction;
    const MirValueId stringValue =
        appendValue(stringOperation, iterationBlock->id, stringInstruction,
                    generatedInfo(stringType));
    const MirPlaceId stringPlace =
        appendPlace(stringOperation, {.root = MirPlaceRootKind::Value,
                                      .value = stringValue,
                                      .type = stringType,
                                      .access = AccessMode::Mutable,
                                      .traits = semanticTraits(stringType)});
    const MirDropObligationId stringDrop = appendDrop(
        stringOperation,
        {.kind = MirDropObligationKind::Value,
         .place = stringPlace,
         .generatedValue = stringValue,
         .dropType = generatedDropType(
             stringType,
             stringConstructor == nullptr ? 0 : stringConstructor->owner)});
    MirInstruction constructString;
    constructString.kind = MirInstructionKind::Construct;
    constructString.result = stringValue;
    constructString.operands = {{.kind = MirOperandKind::Value,
                                 .value = preparedView,
                                 .type = SemanticType::StringView}};
    constructString.parameterTypes = {SemanticType::StringView};
    constructString.constructorTarget = hirPlan->stringConstructor;
    constructString.definedFailure =
        propagation(FailurePropagationKind::Constructor, stringMayRaise);
    constructString.info = generatedInfo(stringType);
    if (stringMayRaise) {
      constructString.successResultDrop = stringDrop;
    } else {
      constructString.lifecycle = {
          {.kind = MirLifecycleEventKind::Initialize, .target = stringDrop}};
    }
    appendInstruction(stringOperation, *iterationBlock,
                      std::move(constructString));
    if (stringMayRaise) {
      routeOperationFailure(iterationBlock, stringInstruction, {vectorDrop},
                            stringDrop);
    }

    const MirHostedStartupOperationId appendReceiverOperation = beginOperation(
        MirHostedStartupOperationKind::PrepareAppendReceiver,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId appendReceiverInstruction = nextInstruction;
    const MirValueId preparedReceiver =
        appendValue(appendReceiverOperation, iterationBlock->id,
                    appendReceiverInstruction, generatedInfo(vectorType));
    MirInstruction prepareReceiver;
    prepareReceiver.kind = MirInstructionKind::CallInput;
    prepareReceiver.callInputRole = MirCallInputRole::Receiver;
    prepareReceiver.callInputKind = HirCallInputKind::MutableBorrow;
    prepareReceiver.result = preparedReceiver;
    prepareReceiver.operands = {{.kind = MirOperandKind::BorrowWrite,
                                 .place = vectorPlace,
                                 .type = vectorType}};
    prepareReceiver.info = generatedInfo(vectorType);
    appendInstruction(appendReceiverOperation, *iterationBlock,
                      std::move(prepareReceiver));

    const MirHostedStartupOperationId appendArgumentOperation = beginOperation(
        MirHostedStartupOperationKind::PrepareAppendArgumentMove,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId appendArgumentInstruction = nextInstruction;
    const MirValueId preparedString =
        appendValue(appendArgumentOperation, iterationBlock->id,
                    appendArgumentInstruction, generatedInfo(stringType));
    const MirPlaceId preparedStringPlace = appendPlace(
        appendArgumentOperation, {.root = MirPlaceRootKind::Temporary,
                                  .temporary = nextTemporary++,
                                  .type = stringType,
                                  .access = AccessMode::Mutable,
                                  .traits = semanticTraits(stringType)});
    const MirDropObligationId preparedStringDrop = appendDrop(
        appendArgumentOperation,
        {.kind = MirDropObligationKind::PreparedParameter,
         .place = preparedStringPlace,
         .generatedValue = preparedString,
         .dropType = generatedDropType(
             stringType,
             stringConstructor == nullptr ? 0 : stringConstructor->owner)});
    MirInstruction prepareString;
    prepareString.kind = MirInstructionKind::CallInput;
    prepareString.callInputRole = MirCallInputRole::Argument;
    prepareString.callInputKind = HirCallInputKind::MoveValue;
    prepareString.preparedParameterDrop = preparedStringDrop;
    prepareString.result = preparedString;
    prepareString.destination = preparedStringPlace;
    prepareString.operands = {{.kind = MirOperandKind::Value,
                               .value = stringValue,
                               .type = stringType}};
    prepareString.info = generatedInfo(stringType);
    prepareString.lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                                .source = stringDrop,
                                .target = preparedStringDrop}};
    appendInstruction(appendArgumentOperation, *iterationBlock,
                      std::move(prepareString));

    const bool appendMayRaise =
        append != nullptr && append->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId appendCallOperation =
        beginOperation(MirHostedStartupOperationKind::CallAppend,
                       failureBehavior(appendMayRaise), iterationBlock->id);
    MirInstruction appendCall;
    appendCall.kind = MirInstructionKind::Call;
    appendCall.receiver = MirOperand{.kind = MirOperandKind::Value,
                                     .value = preparedReceiver,
                                     .type = vectorType};
    appendCall.operands = {{.kind = MirOperandKind::Value,
                            .value = preparedString,
                            .type = stringType}};
    appendCall.parameterTypes = {stringType};
    appendCall.functionTarget = hirPlan->appendFunction;
    appendCall.definedFailure =
        propagation(FailurePropagationKind::DirectCall, appendMayRaise);
    appendCall.info = generatedInfo(SemanticType::Void);
    appendCall.lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                             .source = preparedStringDrop}};
    const MirInstructionId appendCallInstruction = appendInstruction(
        appendCallOperation, *iterationBlock, std::move(appendCall));
    if (appendMayRaise) {
      routeOperationFailure(iterationBlock, appendCallInstruction,
                            {vectorDrop});
    }

    // The verified loop edge establishes index < count <= INT32_MAX.
    // Keep the arithmetic and commit explicit, as for source scalar updates.
    const MirHostedStartupOperationId nextIndexOperation = beginOperation(
        MirHostedStartupOperationKind::ComputeNextArgumentIndex,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirValueId nextIndexValue = appendValue(
        nextIndexOperation, iterationBlock->id, nextInstruction,
        generatedInfo(SemanticType::Int32));
    MirInstruction nextIndex;
    nextIndex.kind = MirInstructionKind::Compute;
    nextIndex.result = nextIndexValue;
    nextIndex.operands = {{.kind = MirOperandKind::Value,
                           .value = indexValue,
                           .type = SemanticType::Int32},
                          {.kind = MirOperandKind::Constant,
                           .literal = Literal{std::uint64_t{1}},
                           .type = SemanticType::Int32}};
    nextIndex.operation = MirOperation::Add;
    nextIndex.info = generatedInfo(SemanticType::Int32);
    appendInstruction(nextIndexOperation, *iterationBlock, std::move(nextIndex));

    const MirHostedStartupOperationId advanceOperation = beginOperation(
        MirHostedStartupOperationKind::AdvanceArgumentIndex,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    const MirInstructionId advanceInstruction = nextInstruction;
    const MirValueId advancedIndex =
        appendValue(advanceOperation, iterationBlock->id, advanceInstruction,
                    generatedInfo(SemanticType::Int32, ValueCategory::Place,
                                  AccessMode::Mutable));
    MirInstruction advance;
    advance.kind = MirInstructionKind::Assign;
    advance.result = advancedIndex;
    advance.destination = indexPlace;
    advance.operands = {{.kind = MirOperandKind::Value,
                         .value = nextIndexValue,
                         .type = SemanticType::Int32}};
    advance.operation = MirOperation::Assign;
    advance.info = generatedInfo(SemanticType::Int32, ValueCategory::Place,
                                 AccessMode::Mutable);
    appendInstruction(advanceOperation, *iterationBlock, std::move(advance));

    const MirHostedStartupOperationId continueOperation = beginOperation(
        MirHostedStartupOperationKind::ContinueArgumentLoop,
        MirHostedStartupFailureBehavior::None, iterationBlock->id);
    MirTerminator continueLoop;
    continueLoop.kind = MirTerminatorKind::Goto;
    continueLoop.target = loopHeader.id;
    terminate(continueOperation, *iterationBlock, std::move(continueLoop));

    MirBlock *finalBlock = &entryCallBlock;
    const MirHostedStartupOperationId entryCountOperation =
        beginOperation(MirHostedStartupOperationKind::PrepareEntryCount,
                       MirHostedStartupFailureBehavior::None, finalBlock->id);
    const MirInstructionId entryCountInstruction = nextInstruction;
    const MirValueId preparedCount =
        appendValue(entryCountOperation, finalBlock->id, entryCountInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction prepareCount;
    prepareCount.kind = MirInstructionKind::CallInput;
    prepareCount.callInputRole = MirCallInputRole::Argument;
    prepareCount.callInputKind = HirCallInputKind::Value;
    prepareCount.result = preparedCount;
    prepareCount.operands = {{.kind = MirOperandKind::Value,
                              .value = stabilizedCount,
                              .type = SemanticType::Int32}};
    prepareCount.info = generatedInfo(SemanticType::Int32);
    appendInstruction(entryCountOperation, *finalBlock,
                      std::move(prepareCount));

    const MirHostedStartupOperationId entryArgumentsOperation =
        beginOperation(MirHostedStartupOperationKind::PrepareEntryArgumentsMove,
                       MirHostedStartupFailureBehavior::None, finalBlock->id);
    const MirInstructionId entryArgumentsInstruction = nextInstruction;
    const MirValueId preparedArguments =
        appendValue(entryArgumentsOperation, finalBlock->id,
                    entryArgumentsInstruction, generatedInfo(vectorType));
    const MirPlaceId preparedArgumentsPlace = appendPlace(
        entryArgumentsOperation, {.root = MirPlaceRootKind::Temporary,
                                  .temporary = nextTemporary++,
                                  .type = vectorType,
                                  .access = AccessMode::Mutable,
                                  .traits = semanticTraits(vectorType)});
    const MirDropObligationId preparedArgumentsDrop = appendDrop(
        entryArgumentsOperation,
        {.kind = MirDropObligationKind::PreparedParameter,
         .place = preparedArgumentsPlace,
         .generatedValue = preparedArguments,
         .dropType = generatedDropType(
             vectorType,
             vectorConstructor == nullptr ? 0 : vectorConstructor->owner)});
    MirInstruction prepareArguments;
    prepareArguments.kind = MirInstructionKind::CallInput;
    prepareArguments.callInputRole = MirCallInputRole::Argument;
    prepareArguments.callInputIndex = 1;
    prepareArguments.callInputKind = HirCallInputKind::MoveValue;
    prepareArguments.preparedParameterDrop = preparedArgumentsDrop;
    prepareArguments.result = preparedArguments;
    prepareArguments.destination = preparedArgumentsPlace;
    prepareArguments.operands = {{.kind = MirOperandKind::Value,
                                  .value = vectorValue,
                                  .type = vectorType}};
    prepareArguments.info = generatedInfo(vectorType);
    prepareArguments.lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                                   .source = vectorDrop,
                                   .target = preparedArgumentsDrop}};
    appendInstruction(entryArgumentsOperation, *finalBlock,
                      std::move(prepareArguments));

    const bool entryMayRaise =
        entry != nullptr && entry->mayRaiseDefinedFailure;
    const MirHostedStartupOperationId entryCallOperation =
        beginOperation(MirHostedStartupOperationKind::CallEntry,
                       failureBehavior(entryMayRaise), finalBlock->id);
    const MirInstructionId entryCallInstruction = nextInstruction;
    const MirValueId entryResult =
        appendValue(entryCallOperation, finalBlock->id, entryCallInstruction,
                    generatedInfo(SemanticType::Int32));
    MirInstruction entryCall;
    entryCall.kind = MirInstructionKind::Call;
    entryCall.result = entryResult;
    entryCall.operands = {{.kind = MirOperandKind::Value,
                           .value = preparedCount,
                           .type = SemanticType::Int32},
                          {.kind = MirOperandKind::Value,
                           .value = preparedArguments,
                           .type = vectorType}};
    entryCall.parameterTypes = {SemanticType::Int32, vectorType};
    entryCall.functionTarget = hirPlan->entry;
    entryCall.definedFailure =
        propagation(FailurePropagationKind::DirectCall, entryMayRaise);
    entryCall.info = generatedInfo(SemanticType::Int32);
    entryCall.lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                            .source = preparedArgumentsDrop}};
    appendInstruction(entryCallOperation, *finalBlock, std::move(entryCall));
    if (entryMayRaise) {
      routeOperationFailure(finalBlock, entryCallInstruction, {});
    }
    plan.entryResult = entryResult;

    drainThreadLocals(*finalBlock);
    cThreadBoundary(*finalBlock, false);

    const MirHostedStartupOperationId returnOperation =
        beginOperation(MirHostedStartupOperationKind::ReturnEntry,
                       MirHostedStartupFailureBehavior::None, finalBlock->id);
    MirTerminator returnEntry;
    returnEntry.kind = MirTerminatorKind::Return;
    returnEntry.value = MirOperand{.kind = MirOperandKind::Value,
                                   .value = entryResult,
                                   .type = SemanticType::Int32};
    terminate(returnOperation, *finalBlock, std::move(returnEntry));
  }

  rebuildMirReachability(body);
  valid = rebuildMirValueUses(body) && valid;
  program.hostedStartupPlan_ = std::move(plan);
  program.hostedStartupBody = std::move(body);
  return valid;
}
} // namespace lang
