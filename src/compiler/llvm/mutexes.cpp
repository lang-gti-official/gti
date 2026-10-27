#include "mutexes.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "runtime_abi.h"

#include <algorithm>
#include <llvm/IR/Intrinsics.h>

namespace lang::native_llvm {

std::optional<SemanticType>
nativeMutexElement(const MirProgram &program, const SemanticType &type,
                   CompilerCapabilityTypeKind kind) {
  const auto &target = program.target();
  if (!((target.os == "macos" && target.arch == "arm64") ||
        (target.os == "linux" && target.arch == "x86_64")) ||
      (kind != CompilerCapabilityTypeKind::MutexStorage &&
       kind != CompilerCapabilityTypeKind::MutexLockToken) ||
      type.kind != SemanticType::Class || type.arguments.size() != 1 ||
      type.arguments[0] == SemanticType::Void ||
      type.arguments[0].kind == SemanticType::Reference ||
      type.arguments[0].kind == SemanticType::RawPointer)
    return {};
  const MirClassInstance *owner = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == type) {
      if (owner)
        return {};
      owner = &candidate;
    }
  if (!owner || owner->compilerCapability != kind || owner->abstract ||
      owner->polymorphic || owner->cOpaqueHandle || owner->cAbiRecord ||
      owner->cAbiLayout || owner->unionLayout || owner->destructor ||
      owner->requiresActiveDropState || !owner->bases.empty() ||
      !owner->structuralBases.empty() || !owner->declaredFields.empty() ||
      !owner->fields.empty() || !owner->fieldDropOrder.empty() ||
      owner->requiresActiveCleanup !=
          (kind == CompilerCapabilityTypeKind::MutexStorage))
    return {};
  return type.arguments[0];
}

std::optional<SemanticType>
nativeMutexConstructor(const MirProgram &program,
                       const MirConstructorInstance &constructor) {
  const auto *owner = program.findClassInstance(constructor.owner);
  const auto payload =
      owner ? nativeMutexElement(program, owner->type,
                                 CompilerCapabilityTypeKind::MutexStorage)
            : std::nullopt;
  const auto &body = constructor.body;
  if (payload && mirOwnedStoragePayloadInitialization(program, constructor) &&
      program.target().executionProfile == ExecutionProfile::Concurrent &&
      program.target().runtimeCapabilities.threads)
    return payload;
  if (!payload ||
      program.target().executionProfile != ExecutionProfile::Concurrent ||
      !program.target().runtimeCapabilities.threads ||
      mirTypeRelocationKind(program, *payload) == RelocationKind::Unavailable ||
      constructor.definitionKind != MirDefinitionKind::Source ||
      constructor.parameterTypes != std::vector<SemanticType>{*payload} ||
      constructor.parameterBindings.size() != 1 ||
      !constructor.parameterBindings[0] || !constructor.initializers.empty() ||
      !constructor.typeArguments.empty() ||
      !constructor.valueArguments.empty() || constructor.requiresUnsafe ||
      constructor.borrowOrigin != BorrowOriginKind::None ||
      constructor.effectSummary.receiverEscapes ||
      std::any_of(constructor.effectSummary.argumentsEscape.begin(),
                  constructor.effectSummary.argumentsEscape.end(),
                  [](bool escapes) { return escapes; }) ||
      !body.values.empty() || !body.loans.empty() ||
      !body.fullExpressions.empty() || body.places.size() != 1 ||
      body.dropObligations.size() > 1 ||
      !verifyMirBody(body, constructor.id).valid())
    return {};
  const auto &parameter = body.places.front();
  if (parameter.root != MirPlaceRootKind::Binding ||
      parameter.binding != constructor.parameterBindings[0] ||
      parameter.type != *payload || !parameter.initiallyAvailable ||
      !parameter.projections.empty())
    return {};
  // A payload with active cleanup carries its exact unique recipe. One
  // without keeps MIR's passive binding obligation: the consumed parameter's
  // Drop then has nothing to retire, whatever the payload's source type.
  const auto parameterRecipe = mirUniqueCleanupType(program, *payload);
  const auto passive = [&](const MirDropType &drop) {
    return !parameterRecipe && drop.type == *payload &&
           !drop.requiresActiveCleanup && !drop.destructor &&
           !drop.fixedArray && !drop.uniquePointeeClass &&
           !drop.polymorphicCleanup && !drop.uniqueOwnerField &&
           !drop.uniqueOwnerBase && !drop.captureIndex &&
           !drop.payloadVariant && !drop.payloadField &&
           !drop.recursiveCleanup && drop.aggregate.empty();
  };
  for (const auto &drop : body.dropObligations) {
    if (drop.kind != MirDropObligationKind::Binding ||
        drop.place != parameter.id || drop.binding != parameter.binding ||
        !(parameterRecipe ? drop.dropType == *parameterRecipe
                          : passive(drop.dropType)) ||
        !drop.initiallyActive)
      return {};
  }
  for (const auto &block : body.blocks) {
    if (!block.parameters.empty())
      return {};
    for (const auto &instruction : block.instructions)
      if (instruction.kind != MirInstructionKind::Drop &&
          !(instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.cleanupBoundaryEnd && instruction.lifecycle.empty()))
        return {};
    if (block.terminator.kind != MirTerminatorKind::Return &&
        block.terminator.kind != MirTerminatorKind::Invoke &&
        block.terminator.kind != MirTerminatorKind::PropagateFailure)
      return {};
  }
  return payload;
}

bool Mutexes::handles(IntrinsicKind intrinsic) {
  return intrinsic == IntrinsicKind::MutexLock ||
         intrinsic == IntrinsicKind::MutexUnlock ||
         intrinsic == IntrinsicKind::MutexGuardActive ||
         intrinsic == IntrinsicKind::MutexGuardRead ||
         intrinsic == IntrinsicKind::MutexGuardReadMut;
}

namespace {
bool mutexCallMetadata(const MirProgram &program,
                        const MirInstruction &instruction) {
  const auto &target = program.target();
  return target.executionProfile == ExecutionProfile::Concurrent &&
      target.runtimeCapabilities.threads &&
      ((target.os == "macos" && target.arch == "arm64") ||
       (target.os == "linux" && target.arch == "x86_64")) &&
      instruction.kind == MirInstructionKind::Call &&
      instruction.operation == MirOperation::None && !instruction.receiver &&
      !instruction.functionTarget && !instruction.constructorTarget &&
      !instruction.lambdaTarget && !instruction.bodyTarget &&
      !instruction.nativeCalleeType && !instruction.nativeCallbackAdapter &&
      !instruction.threadTaskAdapter && instruction.scopedThreadTasks.empty() &&
      !instruction.functionPointerFunctionTarget &&
      !instruction.functionPointerLambdaTarget &&
      instruction.closureCaptureTypes.empty() &&
      instruction.closureCaptureModes.empty() &&
      instruction.callableArguments.empty() && !instruction.callableBoundary &&
      !instruction.callableInvocation && !instruction.destination &&
      !instruction.callInputRole && !instruction.retainedOwnerPlace &&
      !instruction.preparedParameterDrop && !instruction.successResultDrop &&
      !instruction.successResultDestination &&
      instruction.lifecycle.empty() && !instruction.elementCleanup &&
      !instruction.replacementCleanup && !instruction.replacementPair &&
      !instruction.objectAllocation && !instruction.objectLayoutType &&
      !instruction.rawMemoryAccess &&
      instruction.unsafeOperation == UnsafeOperationKind::None &&
      instruction.dispatch == CallDispatch::Static &&
      instruction.dispatchOwner == SemanticType::Unknown &&
      instruction.borrowArguments.empty() && instruction.borrowFields.empty() &&
      !instruction.borrowPlace && !instruction.restoresReferenceResult &&
      instruction.restoredBorrowFields.empty() &&
      !instruction.dependentTextPublication && !instruction.provisionalTextRelays;
}
} // namespace

bool nativeMutexLockFailure(const MirProgram &program,
                             const MirInstruction &instruction) {
  const DefinedFailureOutcome outcome{
      DefinedFailureCode::InfallibleHostOperationFailed,
      DefinedFailureDetail::MutexLock};
  const auto *site = instruction.localFailureSites.size() == 1
                         ? program.failureMetadata().findSite(
                               instruction.localFailureSites.front())
                         : nullptr;
  return mutexCallMetadata(program, instruction) &&
         instruction.intrinsic == IntrinsicKind::MutexLockFailure &&
         instruction.operands.empty() && instruction.parameterTypes.empty() &&
         !instruction.result && instruction.info.type == SemanticType::Void &&
         instruction.info.category == ValueCategory::Value &&
         !instruction.loan && instruction.borrowOrigin == BorrowOriginKind::None &&
         !instruction.borrowArgument &&
         instruction.borrowAccess == AccessMode::ReadOnly &&
         instruction.synchronization == SynchronizationOperation{} &&
         instruction.definedFailure.propagation == FailurePropagationKind::None &&
         instruction.definedFailure.localOrigins.size() == 1 &&
         instruction.definedFailure.localOrigins.front().outcomes ==
             std::vector<DefinedFailureOutcome>{outcome} &&
         site &&
         program.failureMetadata().siteFor(
             instruction.definedFailure.localOrigins.front()) ==
             instruction.localFailureSites.front() &&
         site->outcomes == std::vector<DefinedFailureOutcome>{outcome};
}

bool nativeMutexCall(const MirProgram &program, const MirBody &body,
                     const MirInstruction &instruction) {
  if (!Mutexes::handles(instruction.intrinsic) ||
      !mutexCallMetadata(program, instruction) ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      instruction.parameterTypes.size() != instruction.operands.size() ||
      !validSynchronizationOperation(instruction.synchronization))
    return false;
  const bool active = instruction.intrinsic == IntrinsicKind::MutexGuardActive;
  const bool lock = instruction.intrinsic == IntrinsicKind::MutexLock;
  const bool unlock = instruction.intrinsic == IntrinsicKind::MutexUnlock;
  const bool mut = instruction.intrinsic == IntrinsicKind::MutexGuardReadMut;
  if (instruction.operands.size() != (active ? 1U : 2U))
    return false;
  const auto reference = [&](unsigned index, CompilerCapabilityTypeKind kind,
                             AccessMode access) -> std::optional<SemanticType> {
    const auto &operand = instruction.operands[index];
    const auto *place = body.findPlace(operand.place);
    if (operand.type.kind != SemanticType::Reference ||
        operand.type.arguments.size() != 1 ||
        operand.type.referenceAccess != access ||
        operand.type != instruction.parameterTypes[index] || !operand.place ||
        operand.value || operand.loan || operand.literal || !place ||
        place->type != operand.type.arguments[0] ||
        (access == AccessMode::Mutable &&
         place->access != AccessMode::Mutable) ||
        operand.kind != (access == AccessMode::Mutable
                             ? MirOperandKind::BorrowWrite
                             : MirOperandKind::BorrowRead))
      return {};
    return nativeMutexElement(program, operand.type.arguments[0], kind);
  };
  const auto token =
      reference(active ? 0 : 1, CompilerCapabilityTypeKind::MutexLockToken,
                lock || unlock ? AccessMode::Mutable : AccessMode::ReadOnly);
  if (!token)
    return false;
  if (!active) {
    const auto storage = reference(0, CompilerCapabilityTypeKind::MutexStorage,
                                   AccessMode::ReadOnly);
    if (!storage || *storage != *token)
      return false;
    const auto &storageType = instruction.operands[0].type.arguments[0];
    const auto &tokenType = instruction.operands[1].type.arguments[0];
    const MirClassInstance *storageOwner = nullptr, *tokenOwner = nullptr;
    for (const auto &owner : program.classInstances()) {
      if (owner.type == storageType)
        storageOwner = &owner;
      if (owner.type == tokenType)
        tokenOwner = &owner;
    }
    if (!storageOwner || !tokenOwner ||
        storageOwner->sourceUnit != tokenOwner->sourceUnit)
      return false;
  }
  const auto result = active   ? SemanticType(SemanticType::Bool)
                      : lock   ? SemanticType(SemanticType::Int32)
                      : unlock ? SemanticType(SemanticType::Void)
                               : *token;
  const auto access = mut ? AccessMode::Mutable : AccessMode::ReadOnly;
  if (instruction.info.type != result ||
      instruction.info.category != (active || lock || unlock
                                        ? ValueCategory::Value
                                        : ValueCategory::Place) ||
      instruction.info.access != access ||
      (result == SemanticType::Void ? instruction.result.has_value()
                                    : !instruction.result))
    return false;
  if (instruction.result) {
    const auto *value = body.findValue(*instruction.result);
    if (!value || value->info.type != result ||
        value->info.category != instruction.info.category ||
        value->info.access != instruction.info.access ||
        value->sourceValue != instruction.hirValue ||
        value->definition != instruction.id)
      return false;
  }
  if (instruction.synchronization.kind !=
      (lock     ? SynchronizationOperationKind::MutexLock
       : unlock ? SynchronizationOperationKind::MutexUnlock
                : SynchronizationOperationKind::None))
    return false;
  if (active || lock || unlock)
    return !instruction.loan &&
           instruction.borrowOrigin == BorrowOriginKind::None &&
           !instruction.borrowArgument &&
           instruction.borrowAccess == AccessMode::ReadOnly;
  const auto *loan =
      instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
  return instruction.borrowOrigin == BorrowOriginKind::Argument &&
         instruction.borrowArgument == 0 &&
         instruction.borrowAccess == access && loan &&
         loan->kind == MirLoanKind::CallResult && !loan->lifecycleBound &&
         !loan->entry && !loan->escapes && !loan->parent &&
         loan->source == instruction.operands[0].place &&
         loan->access == access && loan->producedBy == instruction.hirValue;
}

void Mutexes::require(llvm::Value *condition) {
  auto *function = ir.GetInsertBlock()->getParent();
  auto *valid =
      llvm::BasicBlock::Create(session.context(), "mutex.valid", function);
  auto *invalid =
      llvm::BasicBlock::Create(session.context(), "mutex.invalid", function);
  ir.CreateCondBr(condition, valid, invalid);
  ir.SetInsertPoint(invalid);
  // A malformed private token or retired storage terminates as the generated
  // C++ representation does.
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::ContractTerminate));
  ir.CreateUnreachable();
  ir.SetInsertPoint(valid);
}

void Mutexes::initializeShell(llvm::Value *address, const SemanticType &type) {
  const auto layout = session.layouts().mutexStorage(type);
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::MutexInitialize),
      {ir.CreateStructGEP(layout.object.storage, address, layout.nativeIndex)});
  ir.CreateStore(ir.getInt8(MutexStorageLayout::shellLive),
                 ir.CreateStructGEP(layout.object.storage, address,
                                    layout.payloadActiveIndex));
}

void Mutexes::destroyShell(llvm::Value *address, const SemanticType &type) {
  const auto layout = session.layouts().mutexStorage(type);
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::MutexDestroy),
      {ir.CreateStructGEP(layout.object.storage, address, layout.nativeIndex)});
}

void Mutexes::initializeToken(llvm::Value *address, const SemanticType &type) {
  if (!nativeMutexElement(session.program().mir(), type,
                          CompilerCapabilityTypeKind::MutexLockToken))
    throw BackendUnsupportedError(
        "LLVM mutex token requires its exact capability");
  ir.CreateStore(llvm::ConstantPointerNull::get(session.layouts().pointer()),
                 address);
}

void Mutexes::moveToken(llvm::Value *destination, llvm::Value *source,
                        const SemanticType &type) {
  if (!nativeMutexElement(session.program().mir(), type,
                          CompilerCapabilityTypeKind::MutexLockToken))
    throw BackendUnsupportedError(
        "LLVM mutex token move requires its exact capability");
  if (destination == source)
    throw BackendUnsupportedError(
        "LLVM mutex token transfer requires distinct fresh storage");
  ir.CreateStore(ir.CreateLoad(session.layouts().pointer(), source),
                 destination);
  initializeToken(source, type);
}

llvm::Value *Mutexes::emit(const MirBody &body,
                           const MirInstruction &instruction,
                           std::span<llvm::Value *const> inputs) {
  if (!nativeMutexCall(session.program().mir(), body, instruction) ||
      inputs.size() != instruction.operands.size() ||
      std::any_of(inputs.begin(), inputs.end(), [&](auto *value) {
        return !value || value->getType() != session.layouts().pointer();
      }))
    throw BackendUnsupportedError(
        "LLVM mutex operation lost its exact sealed plan");
  const bool active = instruction.intrinsic == IntrinsicKind::MutexGuardActive;
  auto *token =
      ir.CreateLoad(session.layouts().pointer(), inputs[active ? 0 : 1]);
  if (active)
    return ir.CreateIsNotNull(token);
  const auto layout =
      session.layouts().mutexStorage(instruction.operands[0].type.arguments[0]);
  require(ir.CreateICmpEQ(
      ir.CreateLoad(ir.getInt8Ty(),
                    ir.CreateStructGEP(layout.object.storage, inputs[0],
                                       layout.payloadActiveIndex)),
      ir.getInt8(MutexStorageLayout::shellLive |
                 MutexStorageLayout::payloadLive)));
  auto *native =
      ir.CreateStructGEP(layout.object.storage, inputs[0], layout.nativeIndex);
  if (instruction.intrinsic == IntrinsicKind::MutexLock) {
    require(ir.CreateIsNull(token));
    auto *status = ir.CreateCall(
        declareRuntimeService(session, RuntimeService::MutexLock), {native});
    ir.CreateStore(
        ir.CreateSelect(
            ir.CreateICmpEQ(status, ir.getInt32(0)), inputs[0],
            llvm::ConstantPointerNull::get(session.layouts().pointer())),
        inputs[1]);
    return status;
  }
  require(ir.CreateICmpEQ(token, inputs[0]));
  if (instruction.intrinsic == IntrinsicKind::MutexUnlock) {
    ir.CreateCall(declareRuntimeService(session, RuntimeService::MutexUnlock),
                  {native});
    initializeToken(inputs[1], instruction.operands[1].type.arguments[0]);
    return nullptr;
  }
  return ir.CreateStructGEP(layout.object.storage, inputs[0],
                            layout.payloadIndex);
}

} // namespace lang::native_llvm
