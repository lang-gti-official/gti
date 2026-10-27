#include "threads.h"
#include "failure.h"
#include "gti/mir_effects.h"
#include "layout.h"
#include "lifetimes.h"
#include "memory.h"
#include "placement.h"
#include "runtime_abi.h"

#include <algorithm>
#include <llvm/IR/Intrinsics.h>

namespace lang::native_llvm {
namespace {

// Status and kind encodings of runtime_thread.h (versioned C ABI).
constexpr std::uint32_t spawnOk = 0;
constexpr std::uint32_t spawnResourceUnavailable = 1;
constexpr std::uint32_t spawnOperationNotPermitted = 2;
constexpr std::uint32_t joinOk = 0;
constexpr std::uint32_t joinNotJoinable = 1;
constexpr std::uint32_t joinFailed = 2;
constexpr std::uint32_t joinWorkerFailure = 3;
constexpr std::uint32_t failureNone = 0;
constexpr std::uint32_t failureOrdinary = 1;
constexpr std::uint32_t failureCleanup = 2;

bool supportedTarget(const MirProgram &program) {
  const auto &target = program.target();
  return target.executionProfile == ExecutionProfile::Concurrent &&
         target.runtimeCapabilities.threads &&
         ((target.os == "macos" && target.arch == "arm64") ||
          (target.os == "linux" && target.arch == "x86_64"));
}

const MirClassInstance *exactClass(const MirProgram &program,
                                   const SemanticType &type) {
  const MirClassInstance *owner = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == type) {
      if (owner)
        return nullptr;
      owner = &candidate;
    }
  return owner;
}

bool hierarchyFree(const MirProgram &program, const SemanticType &type) {
  try {
    return !containsHierarchyStorage(program, type);
  } catch (const BackendUnsupportedError &) {
    return false;
  }
}

bool threadCallMetadata(const MirProgram &program,
                        const MirInstruction &instruction) {
  return supportedTarget(program) &&
         instruction.kind == MirInstructionKind::Call &&
         instruction.operation == MirOperation::None &&
         !instruction.receiver && !instruction.functionTarget &&
         !instruction.constructorTarget && !instruction.lambdaTarget &&
         !instruction.bodyTarget && !instruction.nativeCalleeType &&
         !instruction.nativeCallbackAdapter &&
         !instruction.functionPointerFunctionTarget &&
         !instruction.functionPointerLambdaTarget &&
         instruction.closureCaptureTypes.empty() &&
         instruction.closureCaptureModes.empty() &&
         instruction.callableArguments.empty() &&
         !instruction.callableBoundary && !instruction.callableInvocation &&
         !instruction.destination && !instruction.callInputRole &&
         !instruction.retainedOwnerPlace &&
         !instruction.preparedParameterDrop &&
         !instruction.successResultDrop &&
         !instruction.successResultDestination &&
         !instruction.elementCleanup && !instruction.replacementCleanup &&
         !instruction.replacementPair && !instruction.objectAllocation &&
         !instruction.objectLayoutType && !instruction.rawMemoryAccess &&
         instruction.unsafeOperation == UnsafeOperationKind::None &&
         instruction.dispatch == CallDispatch::Static &&
         instruction.dispatchOwner == SemanticType::Unknown &&
         instruction.borrowArguments.empty() &&
         instruction.borrowFields.empty() && !instruction.borrowPlace &&
         !instruction.restoresReferenceResult &&
         instruction.restoredBorrowFields.empty() &&
         !instruction.dependentTextPublication &&
         !instruction.provisionalTextRelays && !instruction.tracksCaller &&
         !instruction.callerLocation && !instruction.forwardsCallerLocation &&
         !instruction.sourceLocation && !instruction.neverReturns &&
         !instruction.loan &&
         instruction.borrowOrigin == BorrowOriginKind::None &&
         !instruction.literal && instruction.safetyDischarges.empty() &&
         instruction.parameterTypes.size() == instruction.operands.size() &&
         validSynchronizationOperation(instruction.synchronization);
}

std::optional<ThreadHandleShape>
handleOperand(const MirProgram &program, const MirBody &body,
              const MirInstruction &instruction, std::size_t index,
              AccessMode access) {
  const auto &operand = instruction.operands.at(index);
  const auto *place = operand.place ? body.findPlace(operand.place) : nullptr;
  if (operand.kind != (access == AccessMode::Mutable
                           ? MirOperandKind::BorrowWrite
                           : MirOperandKind::BorrowRead) ||
      !place || operand.value || operand.loan || operand.literal ||
      operand.type.kind != SemanticType::Reference ||
      operand.type.arguments.size() != 1 ||
      operand.type.referenceAccess != access ||
      operand.type != instruction.parameterTypes.at(index) ||
      place->type != operand.type.arguments.front() ||
      (access == AccessMode::Mutable && place->access != AccessMode::Mutable))
    return {};
  return nativeThreadHandle(program, place->type);
}

bool valueOperand(const MirBody &body, const MirInstruction &instruction,
                  std::size_t index) {
  const auto &operand = instruction.operands.at(index);
  const auto *value = operand.value ? body.findValue(operand.value) : nullptr;
  return operand.kind == MirOperandKind::Value && value && !operand.place &&
         !operand.loan && !operand.literal &&
         operand.type == instruction.parameterTypes.at(index) &&
         value->info.type == operand.type;
}

bool exactResult(const MirBody &body, const MirInstruction &instruction,
                 const SemanticType &type) {
  if (instruction.info.type != type ||
      instruction.info.category != ValueCategory::Value)
    return false;
  if (type == SemanticType::Void)
    return !instruction.result;
  const auto *value =
      instruction.result ? body.findValue(*instruction.result) : nullptr;
  return value && value->info.type == type &&
         value->definition == instruction.id;
}

bool invokePaired(const MirBody &body, const MirInstruction &instruction) {
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions)
      if (&stage == &instruction || (!stage.tombstone && stage.id ==
                                                             instruction.id))
        return block.terminator.kind == MirTerminatorKind::Invoke &&
               block.terminator.invokeInstruction == instruction.id &&
               block.terminator.failureRecord &&
               body.findFailureRecord(block.terminator.failureRecord);
  return false;
}

// A consumed task value's drop obligation transfers to the operation.
bool transfersOut(const MirBody &body, const MirInstruction &instruction) {
  return std::all_of(
      instruction.lifecycle.begin(), instruction.lifecycle.end(),
      [&](const MirLifecycleEvent &event) {
        return event.kind == MirLifecycleEventKind::TransferOut &&
               body.findDropObligation(event.source) && !event.target &&
               !event.conditional && !event.failureCleanup;
      });
}

bool spawnAdapter(const MirProgram &program, MirBodyAddress address,
                  const MirInstruction &instruction,
                  const MirThreadTaskAdapter &adapter,
                  const SemanticType &task, const SemanticType &result,
                  bool scoped, std::size_t argument) {
  return adapter.scoped == scoped && adapter.scopeArgument == argument &&
         adapter.sourceBody == address &&
         adapter.spawnInstruction == instruction.id &&
         adapter.spawnValue == instruction.hirValue &&
         adapter.taskType == task && adapter.resultType == result &&
         nativeThreadTaskAdapter(program, adapter);
}

} // namespace

std::optional<ThreadHandleShape>
nativeThreadHandle(const MirProgram &program, const SemanticType &type) {
  if (!supportedTarget(program) || type.kind != SemanticType::Class ||
      type.arguments.size() > 1 || !type.valueArguments.empty())
    return {};
  const auto *owner = exactClass(program, type);
  if (!owner ||
      owner->compilerCapability != CompilerCapabilityTypeKind::ThreadHandle ||
      owner->abstract || owner->polymorphic || owner->cOpaqueHandle ||
      owner->cAbiRecord || owner->cAbiLayout || owner->unionLayout ||
      owner->destructor || owner->requiresActiveDropState ||
      owner->requiresActiveCleanup || !owner->bases.empty() ||
      !owner->structuralBases.empty() || !owner->declaredFields.empty() ||
      !owner->fields.empty() || !owner->fieldDropOrder.empty() ||
      owner->traits.copyable || owner->traits.containsBorrowedState)
    return {};
  ThreadHandleShape shape;
  if (!type.arguments.empty()) {
    const auto &result = type.arguments.front();
    if (result == SemanticType::Void || result == SemanticType::Unknown ||
        result.kind == SemanticType::Reference ||
        result.kind == SemanticType::RawPointer ||
        result.kind == SemanticType::TypeParameter ||
        result.kind == SemanticType::TypePack)
      return {};
    shape.resultBearing = true;
    shape.result = result;
  }
  return shape;
}

bool nativeThreadTaskAdapter(const MirProgram &program,
                             const MirThreadTaskAdapter &adapter) {
  const auto *sealed = program.findThreadTaskAdapter(adapter.id);
  if (!supportedTarget(program) || !sealed || *sealed != adapter ||
      adapter.invocation != CallableInvocationCapability::Once ||
      !adapter.catchesWorkerFailure || !adapter.catchesNativeExceptions ||
      adapter.functionTarget.has_value() == adapter.lambdaTarget.has_value() ||
      (adapter.scoped && adapter.resultType != SemanticType::Void) ||
      adapter.resultType == SemanticType::Unknown ||
      adapter.resultType.kind == SemanticType::Reference ||
      adapter.resultType.kind == SemanticType::RawPointer ||
      !hierarchyFree(program, adapter.taskType) ||
      (adapter.resultType != SemanticType::Void &&
       !hierarchyFree(program, adapter.resultType)))
    return false;
  if (adapter.functionTarget) {
    const auto *target = program.findFunctionInstance(*adapter.functionTarget);
    if (!target || target->definitionKind != MirDefinitionKind::Source ||
        target->linkage != LanguageLinkage::Gti ||
        target->returnType != adapter.resultType ||
        !target->parameterTypes.empty() || target->pureVirtual ||
        target->virtualMethod || target->tracksCaller ||
        target->requiresUnsafe)
      return false;
    if (adapter.taskType.hasFunctionItemShape()) {
      if (target->owner || adapter.taskType.functionId != target->declaration ||
          target->overloadedOperator ||
          adapter.targetInvocation != CallableInvocationCapability::Read)
        return false;
    } else {
      const auto *owner =
          target->owner ? program.findClassInstance(*target->owner) : nullptr;
      if (!owner || owner->type != adapter.taskType || owner->polymorphic ||
          target->staticMember ||
          target->overloadedOperator !=
              std::optional{OverloadedOperator::Call} ||
          adapter.targetInvocation !=
              callableInvocationCapability(target->receiverMutability))
        return false;
    }
  } else {
    const auto *target = program.findLambda(*adapter.lambdaTarget);
    if (!target || target->type != adapter.taskType ||
        target->returnType != adapter.resultType ||
        !target->parameterTypes.empty() ||
        adapter.targetInvocation != CallableInvocationCapability::Read)
      return false;
  }
  if (adapter.scoped)
    return true;
  // An unscoped task crosses into a runtime-owned box and a result back out;
  // both are verified typed relocations.
  return mirTypeRelocationKind(program, adapter.taskType) !=
             RelocationKind::Unavailable &&
         (adapter.resultType == SemanticType::Void ||
          mirTypeRelocationKind(program, adapter.resultType) !=
              RelocationKind::Unavailable);
}

bool isNativeThreadIntrinsic(IntrinsicKind intrinsic) {
  return intrinsic == IntrinsicKind::ThreadHandleEmpty ||
         intrinsic == IntrinsicKind::ThreadJoinable ||
         intrinsic == IntrinsicKind::ThreadSpawn ||
         intrinsic == IntrinsicKind::ThreadJoin ||
         intrinsic == IntrinsicKind::ThreadAutoJoin ||
         intrinsic == IntrinsicKind::ThreadResultTake ||
         intrinsic == IntrinsicKind::ThreadScope;
}

bool nativeThreadCall(const MirProgram &program, MirBodyAddress address,
                      const MirBody &body, const MirInstruction &instruction) {
  if (!isNativeThreadIntrinsic(instruction.intrinsic) ||
      !threadCallMetadata(program, instruction))
    return false;
  const auto &failure = instruction.definedFailure;
  const bool quiet = failure.empty() && instruction.localFailureSites.empty();
  const bool unsynchronized =
      instruction.synchronization.kind == SynchronizationOperationKind::None;
  const bool noTasks =
      !instruction.threadTaskAdapter && instruction.scopedThreadTasks.empty();
  switch (instruction.intrinsic) {
  case IntrinsicKind::ThreadHandleEmpty:
    return instruction.operands.empty() && quiet && unsynchronized &&
           noTasks && instruction.lifecycle.empty() &&
           nativeThreadHandle(program, instruction.info.type) &&
           exactResult(body, instruction, instruction.info.type);
  case IntrinsicKind::ThreadJoinable:
    return instruction.operands.size() == 1 && quiet && unsynchronized &&
           noTasks && instruction.lifecycle.empty() &&
           handleOperand(program, body, instruction, 0, AccessMode::ReadOnly) &&
           exactResult(body, instruction, SemanticType::Bool);
  case IntrinsicKind::ThreadResultTake: {
    if (instruction.operands.size() != 1 || !quiet || !unsynchronized ||
        !noTasks)
      return false;
    const auto handle =
        handleOperand(program, body, instruction, 0, AccessMode::Mutable);
    // A cleanup-owning result arms its exact obligation when taken.
    return handle && handle->resultBearing &&
           exactResult(body, instruction, handle->result) &&
           std::all_of(instruction.lifecycle.begin(),
                       instruction.lifecycle.end(),
                       [&](const MirLifecycleEvent &event) {
                         const auto *target =
                             body.findDropObligation(event.target);
                         return event.kind ==
                                    MirLifecycleEventKind::Initialize &&
                                !event.source && target &&
                                target->dropType.type == handle->result &&
                                !event.conditional && !event.failureCleanup;
                       });
  }
  case IntrinsicKind::ThreadSpawn: {
    if (instruction.operands.size() != 2 || !quiet ||
        instruction.synchronization.kind !=
            SynchronizationOperationKind::ThreadSpawn ||
        !instruction.threadTaskAdapter ||
        !instruction.scopedThreadTasks.empty() ||
        !transfersOut(body, instruction) || !valueOperand(body, instruction, 1) ||
        !exactResult(body, instruction, SemanticType::Int32))
      return false;
    const auto handle =
        handleOperand(program, body, instruction, 0, AccessMode::Mutable);
    const auto *adapter =
        program.findThreadTaskAdapter(*instruction.threadTaskAdapter);
    return handle && adapter &&
           spawnAdapter(program, address, instruction, *adapter,
                        instruction.operands[1].type,
                        handle->resultBearing ? handle->result
                                              : SemanticType::Void,
                        false, 0);
  }
  case IntrinsicKind::ThreadJoin:
  case IntrinsicKind::ThreadAutoJoin: {
    const bool automatic = instruction.intrinsic == IntrinsicKind::ThreadAutoJoin;
    if (instruction.operands.size() != 1 || !noTasks ||
        !instruction.lifecycle.empty() ||
        instruction.synchronization.kind !=
            SynchronizationOperationKind::ThreadJoin ||
        failure.propagation != FailurePropagationKind::TaskJoin ||
        !handleOperand(program, body, instruction, 0, AccessMode::Mutable) ||
        !exactResult(body, instruction,
                     automatic ? SemanticType(SemanticType::Void)
                               : SemanticType(SemanticType::Int32)) ||
        !invokePaired(body, instruction))
      return false;
    if (!automatic)
      return failure.localOrigins.empty() &&
             instruction.localFailureSites.empty();
    const DefinedFailureOutcome outcome{
        DefinedFailureCode::InfallibleHostOperationFailed,
        DefinedFailureDetail::AutomaticJoin};
    const auto *site = instruction.localFailureSites.size() == 1
                           ? program.failureMetadata().findSite(
                                 instruction.localFailureSites.front())
                           : nullptr;
    return failure.localOrigins.size() == 1 &&
           failure.localOrigins.front().outcomes ==
               std::vector<DefinedFailureOutcome>{outcome} &&
           site && site->outcomes == std::vector<DefinedFailureOutcome>{outcome} &&
           program.failureMetadata().siteFor(failure.localOrigins.front()) ==
               instruction.localFailureSites.front();
  }
  case IntrinsicKind::ThreadScope: {
    if (instruction.threadTaskAdapter ||
        instruction.scopedThreadTasks.size() != instruction.operands.size() ||
        instruction.synchronization.kind !=
            SynchronizationOperationKind::ThreadScope ||
        failure.propagation != FailurePropagationKind::TaskJoin ||
        !failure.localOrigins.empty() ||
        !instruction.localFailureSites.empty() ||
        !exactResult(body, instruction, SemanticType::Int32) ||
        !invokePaired(body, instruction))
      return false;
    if (!transfersOut(body, instruction))
      return false;
    for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
      const auto &task = instruction.scopedThreadTasks[index];
      const auto *adapter = program.findThreadTaskAdapter(task.adapter);
      const auto *value =
          valueOperand(body, instruction, index)
              ? body.findValue(instruction.operands[index].value)
              : nullptr;
      if (!adapter || !value ||
          !spawnAdapter(program, address, instruction, *adapter,
                        instruction.operands[index].type, SemanticType::Void,
                        true, index) ||
          (value->info.traits.containsBorrowedState
               ? !task.dependency || !body.findLoan(task.dependency)
               : task.dependency != 0))
        return false;
    }
    return true;
  }
  default:
    return false;
  }
}

ThreadRuntimeTypes threadRuntimeTypes(Session &session) {
  auto &context = session.context();
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *i64 = llvm::Type::getInt64Ty(context);
  auto *pointer = session.layouts().pointer();
  const auto named = [&](const char *name,
                         llvm::ArrayRef<llvm::Type *> fields) {
    if (auto *existing = llvm::StructType::getTypeByName(context, name))
      return existing;
    return llvm::StructType::create(context, fields, name);
  };
  ThreadRuntimeTypes types;
  types.spawnResult =
      named("gti.c.thread_spawn_result.v1", {i32, i32, pointer});
  types.joinResult = named(
      "gti.c.thread_join_result.v2",
      {i32, i32, i32, i32, session.abi().failureRecord(),
       session.abi().failureEmergency(), pointer, pointer});
  types.valueJoin = named("gti.c.thread_value_join_result.v2",
                          {types.joinResult, pointer, pointer, pointer});
  types.scopeResult = named("gti.c.thread_scope_result.v1",
                            {i32, i32, i64, i64, types.joinResult});
  types.scopedTask = named("gti.c.thread_scoped_task.v1", {pointer, pointer});
  const auto &layout = session.dataLayout();
  const auto size = [&](llvm::Type *type) {
    return layout.getTypeAllocSize(type).getFixedValue();
  };
  // runtime_thread.h on the reviewed 64-bit targets.
  if (size(types.spawnResult) != 16 || size(types.joinResult) != 1744 ||
      size(types.valueJoin) != 1768 || size(types.scopeResult) != 1768 ||
      size(types.scopedTask) != 16 ||
      layout.getStructLayout(types.joinResult)->getElementOffset(4) != 16 ||
      layout.getStructLayout(types.joinResult)->getElementOffset(5) != 584 ||
      layout.getStructLayout(types.joinResult)->getElementOffset(6) != 1728 ||
      layout.getStructLayout(types.scopeResult)->getElementOffset(4) != 24)
    throw BackendUnsupportedError(
        "LLVM managed-thread runtime records disagree with the C ABI");
  return types;
}

ThreadTasks::ThreadTasks(Session &session, Failures &failures,
                         Lifetimes &lifetimes, FindBody findBody,
                         FindAbi findAbi)
    : session(session), failures(failures), lifetimes(lifetimes),
      findBody(std::move(findBody)), findAbi(std::move(findAbi)) {}

llvm::Function *ThreadTasks::runtimeService(
    const char *name, llvm::Type *result,
    llvm::ArrayRef<llvm::Type *> parameters, llvm::Type *sret) {
  std::vector<llvm::Type *> lanes;
  if (sret)
    lanes.push_back(session.layouts().pointer());
  lanes.insert(lanes.end(), parameters.begin(), parameters.end());
  auto *type = llvm::FunctionType::get(result, lanes, false);
  if (auto *existing = session.module().getNamedValue(name)) {
    auto *function = llvm::dyn_cast<llvm::Function>(existing);
    if (!function || function->getFunctionType() != type)
      throw std::logic_error(std::string("conflicting LLVM thread service: ") +
                             name);
    return function;
  }
  auto *function = llvm::Function::Create(
      type, llvm::GlobalValue::ExternalLinkage, name, session.module());
  // The runtime contains every worker/native failure before returning.
  function->addFnAttr(llvm::Attribute::NoUnwind);
  if (sret) {
    function->addParamAttr(
        0, llvm::Attribute::getWithStructRetType(session.context(), sret));
    function->addParamAttr(0, llvm::Attribute::getWithAlignment(
                                  session.context(), llvm::Align(8)));
  }
  return function;
}

void ThreadTasks::require(llvm::IRBuilder<> &ir, llvm::Value *condition) {
  auto *function = ir.GetInsertBlock()->getParent();
  auto *valid =
      llvm::BasicBlock::Create(session.context(), "thread.valid", function);
  auto *invalid =
      llvm::BasicBlock::Create(session.context(), "thread.invalid", function);
  ir.CreateCondBr(condition, valid, invalid);
  ir.SetInsertPoint(invalid);
  // A malformed runtime result has no coherent ownership to report through.
  ir.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>{}, {});
  ir.CreateUnreachable();
  ir.SetInsertPoint(valid);
}

void ThreadTasks::freeBox(llvm::IRBuilder<> &ir, llvm::Value *box) {
  Memory(session, ir, {}, {}, {}).releaseBytes(box);
}

void ThreadTasks::storeValue(llvm::IRBuilder<> &ir, llvm::Value *destination,
                             llvm::Value *source, const SemanticType &type) {
  // One verified typed relocation into fresh storage. A native identity (for
  // example a mutex shell) is rebuilt by the generated relocation.
  if (mirTypeRelocationKind(session.program().mir(), type) ==
      RelocationKind::Bitwise) {
    const auto layout = session.layouts().get(type);
    if (layout.size)
      ir.CreateMemCpy(destination, llvm::Align(layout.alignment), source,
                      llvm::Align(layout.alignment), layout.size);
    return;
  }
  ir.CreateCall(lifetimes.generatedRelocation(type), {destination, source});
}

bool ThreadTasks::fallibleCleanup(const SemanticType &type) const {
  if (type == SemanticType::Void)
    return false;
  const auto &program = session.program().mir();
  const auto recipe = mirUniqueCleanupType(program, type);
  return recipe && recipe->requiresActiveCleanup &&
         mirDropMayRaiseDefinedFailure(program, *recipe);
}

llvm::Value *ThreadTasks::cleanupTaskValue(llvm::IRBuilder<> &ir,
                                           const SemanticType &type,
                                           llvm::Value *address,
                                           llvm::Value *liveFlag) {
  const auto recipe = type == SemanticType::Void
                          ? std::nullopt
                          : mirUniqueCleanupType(session.program().mir(), type);
  if (!recipe || !recipe->requiresActiveCleanup)
    return ir.getFalse();
  auto &context = session.context();
  auto *function = ir.GetInsertBlock()->getParent();
  llvm::IRBuilder<> entry(&function->getEntryBlock(),
                          function->getEntryBlock().begin());
  auto *done = llvm::BasicBlock::Create(context, "task.cleaned", function);
  llvm::IRBuilder<> join(done);
  auto *abandoned = join.CreatePHI(ir.getInt1Ty(), 4);
  if (liveFlag) {
    auto *run = llvm::BasicBlock::Create(context, "value.live", function);
    abandoned->addIncoming(ir.getFalse(), ir.GetInsertBlock());
    ir.CreateCondBr(
        ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), liveFlag), ir.getInt8(0)),
        run, done);
    ir.SetInsertPoint(run);
    ir.CreateStore(ir.getInt8(0), liveFlag);
  }
  // A worker cleanup emergency already owns the terminal envelope: the
  // remaining task state is abandoned exactly as the runtime boundary does.
  auto *kind = ir.CreateCall(
      declareRuntimeService(session, RuntimeService::ThreadCleanupFailureKind));
  auto *proceed = llvm::BasicBlock::Create(context, "task.cleanup", function);
  abandoned->addIncoming(ir.getTrue(), ir.GetInsertBlock());
  ir.CreateCondBr(ir.CreateICmpEQ(kind, ir.getInt32(failureCleanup)), done,
                  proceed);
  ir.SetInsertPoint(proceed);
  // During a pending worker failure the cleanup inherits an exact copy of its
  // record, so any further failure completes that worker's GTI-R0014 envelope.
  auto *unwinding = ir.CreateICmpEQ(kind, ir.getInt32(failureOrdinary));
  auto *primary = entry.CreateAlloca(session.abi().failureChannel());
  auto *inherit = llvm::BasicBlock::Create(context, "task.inherit", function);
  auto *clean = llvm::BasicBlock::Create(context, "task.run", function);
  auto *before = ir.GetInsertBlock();
  ir.CreateCondBr(unwinding, inherit, clean);
  ir.SetInsertPoint(inherit);
  auto *pointer = session.layouts().pointer();
  auto *copied = ir.CreateCall(
      declareRuntimeService(session, RuntimeService::CopyThreadCleanupPrimary),
      {failures.record(ir, primary),
       ir.CreateStructGEP(session.abi().failureChannel(), primary, 1)});
  require(ir, ir.CreateICmpNE(copied, ir.getInt32(0)));
  auto *entered = failures.enterCleanup(ir, primary);
  auto *inherited = ir.GetInsertBlock();
  ir.CreateBr(clean);
  ir.SetInsertPoint(clean);
  auto *token = ir.CreatePHI(pointer, 2);
  token->addIncoming(llvm::ConstantPointerNull::get(pointer), before);
  token->addIncoming(entered, inherited);
  auto *scratch = entry.CreateAlloca(session.abi().failureChannel());
  auto *status =
      ir.CreateCall(lifetimes.cleanup(*recipe), {address, scratch, unwinding});
  auto *ok = llvm::BasicBlock::Create(context, "task.cleanup.ok", function);
  auto *failed = llvm::BasicBlock::Create(context, "task.cleanup.failed",
                                          function);
  ir.CreateCondBr(status, ok, failed);
  ir.SetInsertPoint(failed);
  auto *emergency =
      llvm::BasicBlock::Create(context, "task.cleanup.emergency", function);
  auto *report =
      llvm::BasicBlock::Create(context, "task.cleanup.report", function);
  ir.CreateCondBr(failures.abandoning(ir), emergency, report);
  ir.SetInsertPoint(emergency);
  failures.clearAbandoning(ir);
  abandoned->addIncoming(ir.getTrue(), ir.GetInsertBlock());
  ir.CreateBr(done);
  ir.SetInsertPoint(report);
  // The first cleanup failure is this task's ordinary outcome: the worker sink
  // records it, and a caller-thread region captures it.
  failures.publishOrdinary(ir, scratch);
  ir.CreateBr(ok);
  ir.SetInsertPoint(ok);
  failures.leaveCleanup(ir, token);
  abandoned->addIncoming(ir.getFalse(), ir.GetInsertBlock());
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
  return abandoned;
}

llvm::StructType *ThreadTasks::captureBuffer() {
  if (!captureBuffer_) {
    auto *i32 = llvm::Type::getInt32Ty(session.context());
    auto *pointer = session.layouts().pointer();
    captureBuffer_ = llvm::StructType::create(
        session.context(),
        {i32, i32, session.abi().failureRecord(),
         session.abi().failureEmergency(), pointer, pointer},
        "gti.native.thread_capture.v1");
  }
  return captureBuffer_;
}

llvm::Function *ThreadTasks::captureCallback() {
  if (capture_)
    return capture_;
  auto *pointer = session.layouts().pointer();
  auto &context = session.context();
  capture_ = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {pointer, pointer}, false),
      llvm::GlobalValue::InternalLinkage, "__gti_native_v1_thread_capture",
      session.module());
  // Publication copies the borrowed view synchronously and never unwinds.
  capture_->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", capture_));
  auto *buffer = capture_->getArg(0);
  auto *view = capture_->getArg(1);
  auto *viewType = llvm::StructType::get(
      context, {ir.getInt32Ty(), ir.getInt32Ty(), pointer, pointer, pointer,
                pointer});
  auto *type = captureBuffer();
  auto *kind = ir.CreateLoad(ir.getInt32Ty(), ir.CreateStructGEP(viewType, view, 1));
  ir.CreateStore(kind, ir.CreateStructGEP(type, buffer, 0));
  ir.CreateStore(
      ir.CreateLoad(pointer, ir.CreateStructGEP(viewType, view, 4)),
      ir.CreateStructGEP(type, buffer, 4));
  ir.CreateStore(
      ir.CreateLoad(pointer, ir.CreateStructGEP(viewType, view, 5)),
      ir.CreateStructGEP(type, buffer, 5));
  const auto copy = [&](unsigned source, unsigned target, llvm::Type *record) {
    const auto alignment = session.dataLayout().getABITypeAlign(record);
    ir.CreateMemCpy(ir.CreateStructGEP(type, buffer, target), alignment,
                    ir.CreateLoad(pointer,
                                  ir.CreateStructGEP(viewType, view, source)),
                    alignment,
                    session.dataLayout().getTypeAllocSize(record).getFixedValue());
  };
  auto *ordinary = llvm::BasicBlock::Create(context, "ordinary", capture_);
  auto *cleanup = llvm::BasicBlock::Create(context, "cleanup", capture_);
  auto *done = llvm::BasicBlock::Create(context, "done", capture_);
  auto *select = ir.CreateSwitch(kind, done, 2);
  select->addCase(ir.getInt32(failureOrdinary), ordinary);
  select->addCase(ir.getInt32(failureCleanup), cleanup);
  ir.SetInsertPoint(ordinary);
  copy(2, 2, session.abi().failureRecord());
  ir.CreateBr(done);
  ir.SetInsertPoint(cleanup);
  copy(3, 3, session.abi().failureEmergency());
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
  ir.CreateRetVoid();
  return capture_;
}

llvm::Value *
ThreadTasks::callerContained(llvm::IRBuilder<> &ir,
                             const std::function<llvm::Value *()> &region) {
  auto &context = session.context();
  auto *function = ir.GetInsertBlock()->getParent();
  auto *pointer = session.layouts().pointer();
  auto *direct = llvm::BasicBlock::Create(context, "caller.worker", function);
  auto *guarded = llvm::BasicBlock::Create(context, "caller.sink", function);
  auto *done = llvm::BasicBlock::Create(context, "caller.done", function);
  // On a managed worker the worker sink records the outcome and the task
  // continues; this is the existing synchronous-adapter boundary.
  ir.CreateCondBr(failures.onWorker(ir), direct, guarded);
  ir.SetInsertPoint(direct);
  auto *directValue = region();
  auto *directEnd = ir.GetInsertBlock();
  ir.CreateBr(done);
  ir.SetInsertPoint(guarded);
  llvm::IRBuilder<> entry(&function->getEntryBlock(),
                          function->getEntryBlock().begin());
  auto *sinkType = llvm::StructType::get(
      context, {ir.getInt32Ty(), ir.getInt32Ty(), pointer, pointer, pointer,
                pointer});
  auto *sink = entry.CreateAlloca(sinkType);
  auto *buffer = entry.CreateAlloca(captureBuffer());
  ir.CreateStore(llvm::Constant::getNullValue(sinkType), sink);
  ir.CreateStore(ir.getInt32(1), ir.CreateStructGEP(sinkType, sink, 0));
  ir.CreateStore(ir.getInt32(failureNone),
                 ir.CreateStructGEP(captureBuffer(), buffer, 0));
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::EnterThreadFailureSink),
      {sink, captureCallback(), buffer});
  auto *guardedValue = region();
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::LeaveThreadFailureSink),
      {sink});
  auto *kind = ir.CreateLoad(ir.getInt32Ty(),
                             ir.CreateStructGEP(captureBuffer(), buffer, 0));
  auto *ordinary =
      llvm::BasicBlock::Create(context, "caller.ordinary", function);
  auto *cleanup = llvm::BasicBlock::Create(context, "caller.cleanup", function);
  auto *clear = llvm::BasicBlock::Create(context, "caller.clear", function);
  auto *select = ir.CreateSwitch(kind, clear, 2);
  select->addCase(ir.getInt32(failureOrdinary), ordinary);
  select->addCase(ir.getInt32(failureCleanup), cleanup);
  ir.SetInsertPoint(ordinary);
  auto *null = llvm::ConstantPointerNull::get(pointer);
  ir.CreateCall(declareRuntimeService(session, RuntimeService::TerminateFailure),
                {ir.CreateStructGEP(captureBuffer(), buffer, 2),
                 ir.CreateLoad(pointer,
                               ir.CreateStructGEP(captureBuffer(), buffer, 4)),
                 null, null});
  ir.CreateUnreachable();
  ir.SetInsertPoint(cleanup);
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::TerminateCleanupFailure),
      {ir.CreateStructGEP(captureBuffer(), buffer, 3),
       ir.CreateLoad(pointer, ir.CreateStructGEP(captureBuffer(), buffer, 4)),
       ir.CreateLoad(pointer, ir.CreateStructGEP(captureBuffer(), buffer, 5))});
  ir.CreateUnreachable();
  ir.SetInsertPoint(clear);
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
  if (!directValue)
    return nullptr;
  auto *merged = ir.CreatePHI(directValue->getType(), 2);
  merged->addIncoming(directValue, directEnd);
  merged->addIncoming(guardedValue, clear);
  return merged;
}

const ThreadTasks::Adapter &ThreadTasks::adapter(MirThreadTaskAdapterId id) {
  if (const auto found = adapters.find(id); found != adapters.end())
    return found->second;
  const auto &program = session.program().mir();
  const auto *facts = program.findThreadTaskAdapter(id);
  if (!facts || !nativeThreadTaskAdapter(program, *facts))
    throw BackendUnsupportedError(
        "LLVM managed task adapter lost its exact verified contract");
  if (!runtime.joinResult)
    runtime = threadRuntimeTypes(session);
  auto &result = adapters[id];
  result.facts = facts;
  auto &context = session.context();
  std::vector<llvm::Type *> fields{session.layouts().get(facts->taskType).storage};
  if (facts->resultType != SemanticType::Void) {
    fields.push_back(session.layouts().get(facts->resultType).storage);
    fields.push_back(llvm::Type::getInt8Ty(context));
  }
  result.box = llvm::StructType::get(context, fields);
  emitEntry(result);
  if (facts->scoped)
    return result;
  if (facts->resultType == SemanticType::Void)
    emitDestroy(result);
  else
    emitValueCallbacks(result);
  return result;
}

void ThreadTasks::emitEntry(Adapter &adapter) {
  const auto &facts = *adapter.facts;
  auto *pointer = session.layouts().pointer();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {pointer}, false),
      llvm::GlobalValue::InternalLinkage,
      "__gti_native_v1_thread_" + std::to_string(facts.id) + "_entry",
      session.module());
  adapter.entry = function;
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *box = function->getArg(0);
  // A scoped context is the borrowed task slot itself; an owned box stores
  // the task first, so both start at the task object.
  auto *task = ir.CreateStructGEP(adapter.box, box, 0);
  failures.markWorker(ir);
  const MirBodyAddress target =
      facts.functionTarget
          ? MirBodyAddress{MirBodyKind::Function, *facts.functionTarget}
          : MirBodyAddress{MirBodyKind::Lambda, *facts.lambdaTarget};
  const auto abi = findAbi(target);
  const bool valueBearing = facts.resultType != SemanticType::Void;
  llvm::Value *channel = nullptr;
  if (abi.failureDestination)
    channel = ir.CreateAlloca(session.abi().failureChannel());
  std::vector<llvm::Value *> arguments;
  for (const auto &parameter : abi.parameters) {
    switch (parameter.channel) {
    case AbiChannel::ResultDestination:
      if (!valueBearing)
        throw std::logic_error("LLVM void task acquired a result destination");
      arguments.push_back(ir.CreateStructGEP(adapter.box, box, 1));
      break;
    case AbiChannel::FailureDestination:
      arguments.push_back(channel);
      break;
    case AbiChannel::Receiver:
      arguments.push_back(task);
      break;
    case AbiChannel::CallerSite:
    case AbiChannel::CallerArtifact:
    case AbiChannel::Argument:
      throw BackendUnsupportedError(
          "LLVM managed task entry requires an exact zero-argument target");
    }
  }
  auto *call = ir.CreateCall(abi.type, findBody(target), arguments);
  abi.applyTo(*call);
  auto *success =
      llvm::BasicBlock::Create(session.context(), "task.success", function);
  if (abi.statusResult) {
    auto *failed =
        llvm::BasicBlock::Create(session.context(), "task.failed", function);
    ir.CreateCondBr(call, success, failed);
    ir.SetInsertPoint(failed);
    // An abandoned emergency was already published by the failing frame.
    auto *abandoned = llvm::BasicBlock::Create(session.context(),
                                               "task.abandoned", function);
    auto *publish =
        llvm::BasicBlock::Create(session.context(), "task.publish", function);
    ir.CreateCondBr(failures.abandoning(ir), abandoned, publish);
    ir.SetInsertPoint(abandoned);
    failures.clearAbandoning(ir);
    ir.CreateRetVoid();
    ir.SetInsertPoint(publish);
    failures.publishOrdinary(ir, channel);
    ir.CreateRetVoid();
  } else {
    ir.CreateBr(success);
  }
  ir.SetInsertPoint(success);
  if (valueBearing) {
    if (!abi.resultDestination) {
      if (call->getType()->isVoidTy())
        throw std::logic_error("LLVM value task returned no result");
      llvm::Value *value = call;
      if (facts.resultType == SemanticType::Bool)
        value = ir.CreateZExt(value, ir.getInt8Ty());
      ir.CreateStore(value, ir.CreateStructGEP(adapter.box, box, 1));
    }
    ir.CreateStore(ir.getInt8(1), ir.CreateStructGEP(adapter.box, box, 2));
  }
  ir.CreateRetVoid();
}

void ThreadTasks::emitDestroy(Adapter &adapter) {
  const auto &facts = *adapter.facts;
  auto *pointer = session.layouts().pointer();
  auto &context = session.context();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pointer}, false),
      llvm::GlobalValue::InternalLinkage,
      "__gti_native_v1_thread_" + std::to_string(facts.id) + "_destroy",
      session.module());
  adapter.destroy = function;
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", function));
  auto *box = function->getArg(0);
  // A cleanup emergency abandons the remaining task state, as the worker
  // envelope is terminal. Otherwise task cleanup runs and the box retires.
  auto *kind = ir.CreateCall(
      declareRuntimeService(session, RuntimeService::ThreadCleanupFailureKind));
  auto *run = llvm::BasicBlock::Create(context, "task.destroy", function);
  auto *abandon = llvm::BasicBlock::Create(context, "task.abandoned", function);
  ir.CreateCondBr(ir.CreateICmpEQ(kind, ir.getInt32(failureCleanup)), abandon,
                  run);
  ir.SetInsertPoint(abandon);
  ir.CreateRetVoid();
  ir.SetInsertPoint(run);
  auto *abandoned = cleanupTaskValue(ir, facts.taskType,
                                     ir.CreateStructGEP(adapter.box, box, 0));
  auto *release = llvm::BasicBlock::Create(context, "task.release", function);
  ir.CreateCondBr(abandoned, abandon, release);
  ir.SetInsertPoint(release);
  freeBox(ir, box);
  ir.CreateRetVoid();
}

void ThreadTasks::emitValueCallbacks(Adapter &adapter) {
  const auto &facts = *adapter.facts;
  auto *pointer = session.layouts().pointer();
  auto &context = session.context();
  const auto name = [&](const char *suffix) {
    return "__gti_native_v1_thread_" + std::to_string(facts.id) + suffix;
  };
  const auto create = [&](llvm::Type *result,
                          llvm::ArrayRef<llvm::Type *> parameters,
                          const char *suffix) {
    return llvm::Function::Create(
        llvm::FunctionType::get(result, parameters, false),
        llvm::GlobalValue::InternalLinkage, name(suffix), session.module());
  };
  // Every callback starts from the runtime's task-cleanup state. A worker
  // cleanup emergency abandons the remaining box, exactly once.
  const auto unlessEmergency = [&](llvm::IRBuilder<> &ir,
                                   llvm::Function *function,
                                   llvm::Value *abandonedResult) {
    auto *kind = ir.CreateCall(declareRuntimeService(
        session, RuntimeService::ThreadCleanupFailureKind));
    auto *run = llvm::BasicBlock::Create(context, "value.run", function);
    auto *abandoned =
        llvm::BasicBlock::Create(context, "value.abandoned", function);
    ir.CreateCondBr(ir.CreateICmpEQ(kind, ir.getInt32(failureCleanup)),
                    abandoned, run);
    ir.SetInsertPoint(abandoned);
    if (abandonedResult)
      ir.CreateRet(abandonedResult);
    else
      ir.CreateRetVoid();
    ir.SetInsertPoint(run);
    return abandoned;
  };
  const auto stopIf = [&](llvm::IRBuilder<> &ir, llvm::Function *function,
                          llvm::Value *abandoned, llvm::BasicBlock *exit) {
    auto *next = llvm::BasicBlock::Create(context, "value.next", function);
    ir.CreateCondBr(abandoned, exit, next);
    ir.SetInsertPoint(next);
  };
  {
    // Task cleanup after a successful entry; the box becomes the result.
    adapter.complete = create(pointer, {pointer}, "_complete");
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(context, "entry", adapter.complete));
    auto *box = adapter.complete->getArg(0);
    static_cast<void>(unlessEmergency(ir, adapter.complete, box));
    static_cast<void>(cleanupTaskValue(ir, facts.taskType,
                                       ir.CreateStructGEP(adapter.box, box, 0)));
    ir.CreateRet(box);
  }
  {
    // Spawn rollback or worker failure: any constructed result, then the task.
    adapter.abandonTask =
        create(llvm::Type::getVoidTy(context), {pointer}, "_abandon_task");
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(context, "entry", adapter.abandonTask));
    auto *box = adapter.abandonTask->getArg(0);
    auto *exit = unlessEmergency(ir, adapter.abandonTask, nullptr);
    stopIf(ir, adapter.abandonTask,
           cleanupTaskValue(ir, facts.resultType,
                            ir.CreateStructGEP(adapter.box, box, 1),
                            ir.CreateStructGEP(adapter.box, box, 2)),
           exit);
    stopIf(ir, adapter.abandonTask,
           cleanupTaskValue(ir, facts.taskType,
                            ir.CreateStructGEP(adapter.box, box, 0)),
           exit);
    freeBox(ir, box);
    ir.CreateRetVoid();
  }
  {
    // The joining thread takes the owned result exactly once.
    adapter.take = create(llvm::Type::getVoidTy(context), {pointer, pointer},
                          "_take_result");
    llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", adapter.take));
    auto *box = adapter.take->getArg(0);
    auto *live = ir.CreateStructGEP(adapter.box, box, 2);
    require(ir, ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), live),
                                ir.getInt8(0)));
    ir.CreateStore(ir.getInt8(0), live);
    storeValue(ir, adapter.take->getArg(1),
               ir.CreateStructGEP(adapter.box, box, 1), facts.resultType);
    freeBox(ir, box);
    ir.CreateRetVoid();
  }
  {
    adapter.abandonResult =
        create(llvm::Type::getVoidTy(context), {pointer}, "_abandon_result");
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(context, "entry", adapter.abandonResult));
    auto *box = adapter.abandonResult->getArg(0);
    auto *exit = unlessEmergency(ir, adapter.abandonResult, nullptr);
    stopIf(ir, adapter.abandonResult,
           cleanupTaskValue(ir, facts.resultType,
                            ir.CreateStructGEP(adapter.box, box, 1),
                            ir.CreateStructGEP(adapter.box, box, 2)),
           exit);
    freeBox(ir, box);
    ir.CreateRetVoid();
  }
}

llvm::Value *ThreadTasks::emit(llvm::IRBuilder<> &ir, const Stage &stage) {
  const auto &program = session.program().mir();
  const auto &instruction = stage.instruction;
  if (!nativeThreadCall(program, stage.address, stage.body, instruction) ||
      stage.inputs.size() != instruction.operands.size())
    throw BackendUnsupportedError(
        "LLVM managed-thread operation lost its exact sealed plan");
  if (!runtime.joinResult)
    runtime = threadRuntimeTypes(session);
  switch (instruction.intrinsic) {
  case IntrinsicKind::ThreadHandleEmpty: {
    auto *target = stage.destination();
    ir.CreateStore(llvm::Constant::getNullValue(
                       session.layouts().get(instruction.info.type).storage),
                   target);
    return target;
  }
  case IntrinsicKind::ThreadJoinable: {
    const auto &type = instruction.operands[0].type.arguments[0];
    auto *state = ir.CreateLoad(
        session.layouts().pointer(),
        ir.CreateStructGEP(session.layouts().get(type).storage,
                           stage.inputs[0], 0));
    return ir.CreateIsNotNull(state);
  }
  case IntrinsicKind::ThreadSpawn:
    return spawn(ir, stage);
  case IntrinsicKind::ThreadJoin:
    return join(ir, stage, false);
  case IntrinsicKind::ThreadAutoJoin:
    static_cast<void>(join(ir, stage, true));
    return nullptr;
  case IntrinsicKind::ThreadResultTake:
    return take(ir, stage);
  case IntrinsicKind::ThreadScope:
    return scope(ir, stage);
  default:
    throw std::logic_error("LLVM managed-thread dispatch lost its operation");
  }
}

llvm::Value *ThreadTasks::spawn(llvm::IRBuilder<> &ir, const Stage &stage) {
  const auto &program = session.program().mir();
  const auto &instruction = stage.instruction;
  const auto &handleType = instruction.operands[0].type.arguments[0];
  const auto &taskType = instruction.operands[1].type;
  const auto shape = *nativeThreadHandle(program, handleType);
  const auto &task = adapter(*instruction.threadTaskAdapter);
  auto *handleStorage = session.layouts().get(handleType).storage;
  auto *function = ir.GetInsertBlock()->getParent();
  auto &context = session.context();
  auto *statePointer = ir.CreateStructGEP(handleStorage, stage.inputs[0], 0);
  auto *taskValue = stage.inputs[1];
  const bool addressTask = addressValue(program, taskType);
  auto *done = llvm::BasicBlock::Create(context, "spawn.done", function);
  llvm::IRBuilder<> phiBuilder(done);
  auto *status = phiBuilder.CreatePHI(ir.getInt32Ty(), 3);
  // A consumed task that never reaches a worker is destroyed here, as the
  // runtime destroys it on native creation failure.
  const auto discard = [&](std::uint32_t code) {
    if (addressTask && fallibleCleanup(taskType))
      static_cast<void>(callerContained(ir, [&]() -> llvm::Value * {
        static_cast<void>(cleanupTaskValue(ir, taskType, taskValue));
        return nullptr;
      }));
    else if (addressTask)
      static_cast<void>(cleanupTaskValue(ir, taskType, taskValue));
    status->addIncoming(ir.getInt32(code), ir.GetInsertBlock());
    ir.CreateBr(done);
  };
  auto *occupied = llvm::BasicBlock::Create(context, "spawn.occupied", function);
  auto *allocate = llvm::BasicBlock::Create(context, "spawn.allocate", function);
  ir.CreateCondBr(
      ir.CreateIsNotNull(ir.CreateLoad(session.layouts().pointer(), statePointer)),
      occupied, allocate);
  ir.SetInsertPoint(occupied);
  discard(spawnOperationNotPermitted);
  ir.SetInsertPoint(allocate);
  const auto &layout = session.dataLayout();
  auto *box = Memory(session, ir, {}, {}, {})
                  .allocateStorage(
                      layout.getTypeAllocSize(task.box).getFixedValue(),
                      std::max<std::uint64_t>(
                          layout.getABITypeAlign(task.box).value(), 1));
  auto *exhausted =
      llvm::BasicBlock::Create(context, "spawn.exhausted", function);
  auto *start = llvm::BasicBlock::Create(context, "spawn.start", function);
  ir.CreateCondBr(ir.CreateIsNull(box), exhausted, start);
  ir.SetInsertPoint(exhausted);
  discard(spawnResourceUnavailable);
  ir.SetInsertPoint(start);
  auto *taskSlot = ir.CreateStructGEP(task.box, box, 0);
  if (addressTask) {
    storeValue(ir, taskSlot, taskValue, taskType);
  } else {
    llvm::Value *value = taskValue;
    if (taskType == SemanticType::Bool)
      value = ir.CreateZExt(value, ir.getInt8Ty());
    ir.CreateStore(value, taskSlot);
  }
  auto *pointer = session.layouts().pointer();
  const bool arm64 = program.target().arch == "arm64";
  auto *returned =
      arm64 ? static_cast<llvm::Type *>(
                  llvm::ArrayType::get(ir.getInt64Ty(), 2))
            : llvm::StructType::get(context, {ir.getInt64Ty(), pointer});
  if (shape.resultBearing)
    ir.CreateStore(ir.getInt8(0), ir.CreateStructGEP(task.box, box, 2));
  // Native creation failure runs task cleanup synchronously on this thread.
  const auto call = [&]() -> llvm::Value * {
    if (!shape.resultBearing)
      return ir.CreateCall(runtimeService("gti_rt_thread_spawn_v1", returned,
                                          {pointer, pointer, pointer}),
                           {task.entry, box, task.destroy});
    return ir.CreateCall(
        runtimeService("gti_rt_thread_spawn_value_v1", returned,
                       {pointer, pointer, pointer, pointer, pointer, pointer}),
        {task.entry, box, task.complete, task.abandonTask, task.take,
         task.abandonResult});
  };
  llvm::Value *spawned =
      fallibleCleanup(taskType) ? callerContained(ir, call) : call();
  auto *header = ir.CreateExtractValue(spawned, 0);
  auto *state = arm64 ? ir.CreateIntToPtr(ir.CreateExtractValue(spawned, 1),
                                          pointer)
                      : ir.CreateExtractValue(spawned, 1);
  auto *abiVersion = ir.CreateTrunc(header, ir.getInt32Ty());
  auto *code = ir.CreateTrunc(ir.CreateLShr(header, 32), ir.getInt32Ty());
  require(ir, ir.CreateAnd(
                  ir.CreateAnd(ir.CreateICmpEQ(abiVersion, ir.getInt32(1)),
                               ir.CreateICmpULE(code, ir.getInt32(2))),
                  ir.CreateICmpEQ(ir.CreateICmpEQ(code, ir.getInt32(spawnOk)),
                                  ir.CreateIsNotNull(state))));
  // A successful spawn transfers the native join obligation to the handle;
  // the runtime already destroyed the task on a creation failure.
  auto *adopted =
      ir.CreateSelect(ir.CreateICmpEQ(code, ir.getInt32(spawnOk)), state,
                      llvm::ConstantPointerNull::get(pointer));
  ir.CreateStore(adopted, statePointer);
  status->addIncoming(code, ir.GetInsertBlock());
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
  return status;
}

void ThreadTasks::workerFailure(llvm::IRBuilder<> &ir, const Stage &stage,
                                llvm::Value *join) {
  auto &context = session.context();
  auto *function = ir.GetInsertBlock()->getParent();
  auto *type = runtime.joinResult;
  auto *kind = ir.CreateLoad(ir.getInt32Ty(), ir.CreateStructGEP(type, join, 2));
  auto *cleanup = llvm::BasicBlock::Create(context, "worker.emergency", function);
  auto *ordinary = llvm::BasicBlock::Create(context, "worker.failure", function);
  auto *invalid = llvm::BasicBlock::Create(context, "worker.invalid", function);
  auto *select = ir.CreateSwitch(kind, invalid, 2);
  select->addCase(ir.getInt32(failureCleanup), cleanup);
  select->addCase(ir.getInt32(failureOrdinary), ordinary);
  ir.SetInsertPoint(invalid);
  ir.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>{}, {});
  ir.CreateUnreachable();
  ir.SetInsertPoint(cleanup);
  // The worker's exact GTI-R0014 envelope is terminal. A native worker joining
  // here forwards it as its own outcome and abandons its frames.
  auto *pointer = session.layouts().pointer();
  auto *envelope = ir.CreateStructGEP(type, join, 5);
  auto *emergencyType = session.abi().failureEmergency();
  auto *primaryArtifact =
      ir.CreateLoad(pointer, ir.CreateStructGEP(type, join, 6));
  auto *secondaryArtifact =
      ir.CreateLoad(pointer, ir.CreateStructGEP(type, join, 7));
  auto *primary = llvm::IRBuilder<>(&function->getEntryBlock(),
                                    function->getEntryBlock().begin())
                      .CreateAlloca(session.abi().failureChannel());
  auto *secondary = llvm::IRBuilder<>(&function->getEntryBlock(),
                                      function->getEntryBlock().begin())
                        .CreateAlloca(session.abi().failureChannel());
  const auto copyRecord = [&](llvm::Value *channel, unsigned field,
                              llvm::Value *artifact) {
    auto *recordType = session.abi().failureRecord();
    const auto alignment = session.dataLayout().getABITypeAlign(recordType);
    ir.CreateMemCpy(failures.record(ir, channel), alignment,
                    ir.CreateStructGEP(emergencyType, envelope, field),
                    alignment,
                    session.dataLayout().getTypeAllocSize(recordType)
                        .getFixedValue());
    ir.CreateStore(artifact,
                   ir.CreateStructGEP(session.abi().failureChannel(), channel,
                                      1));
  };
  copyRecord(primary, 2, primaryArtifact);
  copyRecord(secondary, 3, secondaryArtifact);
  failures.terminateCleanup(ir, primary, secondary);
  // Only a native worker returns here; its frames are abandoned by status.
  // Without containment (no native task in the program) the call is
  // terminal and its block already ends.
  if (failures.workerContainment())
    stage.failed();
  ir.SetInsertPoint(ordinary);
  {
    auto *recordType = session.abi().failureRecord();
    const auto alignment = session.dataLayout().getABITypeAlign(recordType);
    ir.CreateMemCpy(failures.record(ir, stage.failure), alignment,
                    ir.CreateStructGEP(type, join, 4), alignment,
                    session.dataLayout().getTypeAllocSize(recordType)
                        .getFixedValue());
    ir.CreateStore(ir.CreateLoad(pointer, ir.CreateStructGEP(type, join, 6)),
                   ir.CreateStructGEP(session.abi().failureChannel(),
                                      stage.failure, 1));
  }
  stage.failed();
}

llvm::Value *ThreadTasks::join(llvm::IRBuilder<> &ir, const Stage &stage,
                               bool automatic) {
  const auto &program = session.program().mir();
  const auto &instruction = stage.instruction;
  const auto &handleType = instruction.operands[0].type.arguments[0];
  const auto shape = *nativeThreadHandle(program, handleType);
  if (!stage.failure || !stage.failed)
    throw std::logic_error("LLVM managed join lost its verified failure edge");
  auto &context = session.context();
  auto *function = ir.GetInsertBlock()->getParent();
  auto *pointer = session.layouts().pointer();
  auto *handleStorage = session.layouts().get(handleType).storage;
  auto *statePointer = ir.CreateStructGEP(handleStorage, stage.inputs[0], 0);
  auto *wasJoinable =
      ir.CreateIsNotNull(ir.CreateLoad(pointer, statePointer));
  llvm::IRBuilder<> entry(&function->getEntryBlock(),
                          function->getEntryBlock().begin());
  auto *result = entry.CreateAlloca(shape.resultBearing ? runtime.valueJoin
                                                        : runtime.joinResult);
  result->setAlignment(llvm::Align(8));
  auto *joinService =
      shape.resultBearing
          ? runtimeService("gti_rt_thread_join_value_v2",
                           llvm::Type::getVoidTy(context), {pointer},
                           runtime.valueJoin)
          : runtimeService("gti_rt_thread_join_v2",
                           llvm::Type::getVoidTy(context), {pointer},
                           runtime.joinResult);
  ir.CreateCall(joinService, {result, statePointer})
      ->setAttributes(joinService->getAttributes());
  auto *join = shape.resultBearing
                   ? ir.CreateStructGEP(runtime.valueJoin, result, 0)
                   : static_cast<llvm::Value *>(result);
  auto *type = runtime.joinResult;
  const auto field = [&](unsigned index) {
    return ir.CreateLoad(ir.getInt32Ty(), ir.CreateStructGEP(type, join, index));
  };
  auto *abiVersion = field(0);
  auto *status = field(1);
  auto *kind = field(2);
  auto *reserved = field(3);
  auto *isJoinable = ir.CreateIsNotNull(ir.CreateLoad(pointer, statePointer));
  const auto is = [&](llvm::Value *value, std::uint32_t expected) {
    return ir.CreateICmpEQ(value, ir.getInt32(expected));
  };
  auto *consumed = ir.CreateOr(is(status, joinOk), is(status, joinWorkerFailure));
  // The same ownership invariants as the C++ oracle: a non-joinable handle
  // reports not-joinable, and only success or worker failure consumes state.
  auto *coherent = ir.CreateSelect(
      wasJoinable,
      ir.CreateAnd(ir.CreateNot(is(status, joinNotJoinable)),
                   ir.CreateICmpNE(consumed, isJoinable)),
      ir.CreateAnd(is(status, joinNotJoinable), ir.CreateNot(isJoinable)));
  auto *payload = ir.CreateICmpEQ(is(kind, failureNone),
                                  ir.CreateNot(is(status, joinWorkerFailure)));
  require(ir, ir.CreateAnd(
                  ir.CreateAnd(ir.CreateAnd(is(abiVersion, 2), is(reserved, 0)),
                               ir.CreateICmpULE(status, ir.getInt32(3))),
                  ir.CreateAnd(coherent, payload)));
  llvm::Value *resultContext = nullptr, *takeResult = nullptr,
              *abandonResult = nullptr;
  if (shape.resultBearing) {
    resultContext = ir.CreateLoad(
        pointer, ir.CreateStructGEP(runtime.valueJoin, result, 1));
    takeResult = ir.CreateLoad(
        pointer, ir.CreateStructGEP(runtime.valueJoin, result, 2));
    abandonResult = ir.CreateLoad(
        pointer, ir.CreateStructGEP(runtime.valueJoin, result, 3));
    auto *owns = ir.CreateAnd(
        ir.CreateAnd(ir.CreateIsNotNull(resultContext),
                     ir.CreateIsNotNull(takeResult)),
        ir.CreateIsNotNull(abandonResult));
    auto *none = ir.CreateAnd(
        ir.CreateAnd(ir.CreateIsNull(resultContext), ir.CreateIsNull(takeResult)),
        ir.CreateIsNull(abandonResult));
    require(ir, ir.CreateSelect(is(status, joinOk), owns, none));
  }
  auto *failed = llvm::BasicBlock::Create(context, "join.worker.failed", function);
  auto *continued = llvm::BasicBlock::Create(context, "join.continue", function);
  ir.CreateCondBr(is(status, joinWorkerFailure), failed, continued);
  ir.SetInsertPoint(failed);
  workerFailure(ir, stage, join);
  ir.SetInsertPoint(continued);
  if (shape.resultBearing) {
    auto *joined = llvm::BasicBlock::Create(context, "join.result", function);
    auto *after = llvm::BasicBlock::Create(context, "join.result.done", function);
    ir.CreateCondBr(is(status, joinOk), joined, after);
    ir.SetInsertPoint(joined);
    if (automatic) {
      // Automatic join destroys an unobserved result exactly once, on this
      // thread; a cleanup failure follows the synchronous-callback boundary.
      const auto abandon = [&]() -> llvm::Value * {
        ir.CreateCall(llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                              {pointer}, false),
                      abandonResult, {resultContext});
        return nullptr;
      };
      if (fallibleCleanup(shape.result))
        static_cast<void>(callerContained(ir, abandon));
      else
        static_cast<void>(abandon());
    } else {
      // The handle owns the joined result until the separate take.
      auto *contextSlot = ir.CreateStructGEP(handleStorage, stage.inputs[0], 1);
      require(ir, ir.CreateIsNull(ir.CreateLoad(pointer, contextSlot)));
      ir.CreateStore(resultContext, contextSlot);
      ir.CreateStore(takeResult,
                     ir.CreateStructGEP(handleStorage, stage.inputs[0], 2));
      ir.CreateStore(abandonResult,
                     ir.CreateStructGEP(handleStorage, stage.inputs[0], 3));
    }
    ir.CreateBr(after);
    ir.SetInsertPoint(after);
  }
  if (automatic) {
    // An environmental join failure retains its coherent native state in
    // terminal quarantine, then raises the automatic-join defined failure.
    auto *environmental =
        llvm::BasicBlock::Create(context, "join.environment", function);
    auto *joined = llvm::BasicBlock::Create(context, "join.complete", function);
    ir.CreateCondBr(is(status, joinFailed), environmental, joined);
    ir.SetInsertPoint(environmental);
    ir.CreateCall(declareRuntimeService(session, RuntimeService::QuarantineThread),
                  {statePointer});
    failures.originate(ir, stage.failure, instruction,
                       instruction.definedFailure.localOrigins.front()
                           .outcomes.front());
    stage.failed();
    ir.SetInsertPoint(joined);
    return nullptr;
  }
  return ir.CreateSelect(
      is(status, joinOk), ir.getInt32(0),
      ir.CreateSelect(is(status, joinNotJoinable), ir.getInt32(1),
                      ir.getInt32(2)));
}

llvm::Value *ThreadTasks::take(llvm::IRBuilder<> &ir, const Stage &stage) {
  const auto &program = session.program().mir();
  const auto &instruction = stage.instruction;
  const auto &handleType = instruction.operands[0].type.arguments[0];
  const auto shape = *nativeThreadHandle(program, handleType);
  auto *pointer = session.layouts().pointer();
  auto *handleStorage = session.layouts().get(handleType).storage;
  auto *contextSlot = ir.CreateStructGEP(handleStorage, stage.inputs[0], 1);
  auto *takeSlot = ir.CreateStructGEP(handleStorage, stage.inputs[0], 2);
  auto *abandonSlot = ir.CreateStructGEP(handleStorage, stage.inputs[0], 3);
  auto *resultContext = ir.CreateLoad(pointer, contextSlot);
  auto *takeResult = ir.CreateLoad(pointer, takeSlot);
  require(ir, ir.CreateAnd(ir.CreateIsNotNull(resultContext),
                           ir.CreateIsNotNull(takeResult)));
  auto *null = llvm::ConstantPointerNull::get(pointer);
  ir.CreateStore(null, contextSlot);
  ir.CreateStore(null, takeSlot);
  ir.CreateStore(null, abandonSlot);
  const bool address = addressValue(program, shape.result);
  llvm::Value *destination = nullptr;
  if (address) {
    destination = stage.destination();
  } else {
    auto *function = ir.GetInsertBlock()->getParent();
    llvm::IRBuilder<> entry(&function->getEntryBlock(),
                            function->getEntryBlock().begin());
    const auto layout = session.layouts().get(shape.result);
    auto *slot = entry.CreateAlloca(layout.storage);
    slot->setAlignment(llvm::Align(layout.alignment));
    destination = slot;
  }
  ir.CreateCall(llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                                        {pointer, pointer}, false),
                takeResult, {resultContext, destination});
  return address ? destination : stage.load(destination, shape.result);
}

llvm::Value *ThreadTasks::scope(llvm::IRBuilder<> &ir, const Stage &stage) {
  const auto &program = session.program().mir();
  const auto &instruction = stage.instruction;
  if (!stage.failure || !stage.failed)
    throw std::logic_error("LLVM task scope lost its verified failure edge");
  const auto count = instruction.operands.size();
  if (count == 0)
    return ir.getInt32(spawnOk);
  auto &context = session.context();
  auto *function = ir.GetInsertBlock()->getParent();
  auto *pointer = session.layouts().pointer();
  llvm::IRBuilder<> entry(&function->getEntryBlock(),
                          function->getEntryBlock().begin());
  // The scope borrows its consumed task values in their verified caller
  // storage through join-all, then cleans them here in reverse order.
  std::vector<llvm::Value *> slots;
  for (std::size_t index = 0; index < count; ++index) {
    const auto &type = instruction.operands[index].type;
    if (addressValue(program, type)) {
      slots.push_back(stage.inputs[index]);
      continue;
    }
    const auto layout = session.layouts().get(type);
    auto *slot = entry.CreateAlloca(layout.storage);
    slot->setAlignment(llvm::Align(layout.alignment));
    llvm::Value *value = stage.inputs[index];
    if (type == SemanticType::Bool)
      value = ir.CreateZExt(value, ir.getInt8Ty());
    ir.CreateStore(value, slot);
    slots.push_back(slot);
  }
  auto *descriptors = entry.CreateAlloca(
      llvm::ArrayType::get(runtime.scopedTask, count));
  auto *scratch =
      entry.CreateAlloca(llvm::ArrayType::get(pointer, count));
  auto *result = entry.CreateAlloca(runtime.scopeResult);
  result->setAlignment(llvm::Align(8));
  for (std::size_t index = 0; index < count; ++index) {
    const auto &task = adapter(instruction.scopedThreadTasks[index].adapter);
    auto *descriptor = ir.CreateInBoundsGEP(
        llvm::ArrayType::get(runtime.scopedTask, count), descriptors,
        {ir.getInt64(0), ir.getInt64(index)});
    ir.CreateStore(task.entry,
                   ir.CreateStructGEP(runtime.scopedTask, descriptor, 0));
    ir.CreateStore(slots[index],
                   ir.CreateStructGEP(runtime.scopedTask, descriptor, 1));
  }
  auto *scopeService = runtimeService("gti_rt_thread_scope_v1",
                                      llvm::Type::getVoidTy(context),
                                      {pointer, pointer, ir.getInt64Ty()},
                                      runtime.scopeResult);
  ir.CreateCall(scopeService, {result, descriptors, scratch, ir.getInt64(count)})
      ->setAttributes(scopeService->getAttributes());
  auto *scopeType = runtime.scopeResult;
  auto *joinType = runtime.joinResult;
  auto *abiVersion = ir.CreateLoad(ir.getInt32Ty(),
                                   ir.CreateStructGEP(scopeType, result, 0));
  auto *spawnStatus = ir.CreateLoad(ir.getInt32Ty(),
                                    ir.CreateStructGEP(scopeType, result, 1));
  auto *started = ir.CreateLoad(ir.getInt64Ty(),
                                ir.CreateStructGEP(scopeType, result, 2));
  auto *failedWorker = ir.CreateLoad(ir.getInt64Ty(),
                                     ir.CreateStructGEP(scopeType, result, 3));
  auto *worker = ir.CreateStructGEP(scopeType, result, 4);
  const auto workerField = [&](unsigned index) {
    return ir.CreateLoad(ir.getInt32Ty(),
                         ir.CreateStructGEP(joinType, worker, index));
  };
  auto *workerAbi = workerField(0);
  auto *workerStatus = workerField(1);
  auto *workerKind = workerField(2);
  auto *workerReserved = workerField(3);
  const auto is = [&](llvm::Value *value, std::uint32_t expected) {
    return ir.CreateICmpEQ(value, ir.getInt32(expected));
  };
  auto *workerFailed = is(workerStatus, joinWorkerFailure);
  llvm::Value *valid = ir.CreateAnd(
      ir.CreateAnd(is(abiVersion, 1), ir.CreateICmpULE(spawnStatus,
                                                        ir.getInt32(2))),
      ir.CreateAnd(ir.CreateICmpULE(started, ir.getInt64(count)),
                   ir.CreateOr(ir.CreateNot(is(spawnStatus, spawnOk)),
                               ir.CreateICmpEQ(started, ir.getInt64(count)))));
  valid = ir.CreateAnd(
      valid, ir.CreateAnd(ir.CreateAnd(is(workerAbi, 2), is(workerReserved, 0)),
                          ir.CreateOr(is(workerStatus, joinOk), workerFailed)));
  valid = ir.CreateAnd(
      valid,
      ir.CreateSelect(workerFailed,
                      ir.CreateAnd(ir.CreateICmpULT(failedWorker, started),
                                   ir.CreateNot(is(workerKind, failureNone))),
                      ir.CreateAnd(ir.CreateICmpEQ(failedWorker,
                                                   ir.getInt64(UINT64_MAX)),
                                   is(workerKind, failureNone))));
  for (std::size_t index = 0; index < count; ++index)
    valid = ir.CreateAnd(
        valid, ir.CreateIsNull(ir.CreateLoad(
                   pointer,
                   ir.CreateInBoundsGEP(llvm::ArrayType::get(pointer, count),
                                        scratch,
                                        {ir.getInt64(0), ir.getInt64(index)}))));
  require(ir, valid);
  // A worker's exact GTI-R0014 envelope is terminal before task cleanup.
  {
    auto *emergency =
        llvm::BasicBlock::Create(context, "scope.emergency", function);
    auto *ordinary = llvm::BasicBlock::Create(context, "scope.joined", function);
    ir.CreateCondBr(is(workerKind, failureCleanup), emergency, ordinary);
    ir.SetInsertPoint(emergency);
    workerFailure(ir, stage, worker);
    ir.SetInsertPoint(ordinary);
  }
  // All workers have joined. A worker failure is already the primary record;
  // otherwise the first task-cleanup failure becomes it. Either way later
  // cleanup inherits that context, exactly as an ordinary failure chain.
  auto *failing = entry.CreateAlloca(ir.getInt1Ty());
  ir.CreateStore(workerFailed, failing);
  auto *token = entry.CreateAlloca(pointer);
  ir.CreateStore(llvm::ConstantPointerNull::get(pointer), token);
  {
    auto *propagate =
        llvm::BasicBlock::Create(context, "scope.worker.failure", function);
    auto *clean = llvm::BasicBlock::Create(context, "scope.cleanup", function);
    ir.CreateCondBr(workerFailed, propagate, clean);
    ir.SetInsertPoint(propagate);
    auto *recordType = session.abi().failureRecord();
    const auto alignment = session.dataLayout().getABITypeAlign(recordType);
    ir.CreateMemCpy(failures.record(ir, stage.failure), alignment,
                    ir.CreateStructGEP(joinType, worker, 4), alignment,
                    session.dataLayout().getTypeAllocSize(recordType)
                        .getFixedValue());
    ir.CreateStore(
        ir.CreateLoad(pointer, ir.CreateStructGEP(joinType, worker, 6)),
        ir.CreateStructGEP(session.abi().failureChannel(), stage.failure, 1));
    ir.CreateStore(failures.enterCleanup(ir, stage.failure), token);
    ir.CreateBr(clean);
    ir.SetInsertPoint(clean);
  }
  auto *scratchChannel = entry.CreateAlloca(session.abi().failureChannel());
  for (std::size_t index = count; index != 0; --index) {
    const auto &type = instruction.operands[index - 1].type;
    const auto recipe = mirUniqueCleanupType(program, type);
    if (!recipe || !recipe->requiresActiveCleanup)
      continue;
    auto *status = ir.CreateCall(
        lifetimes.cleanup(*recipe),
        {slots[index - 1], scratchChannel, ir.CreateLoad(ir.getInt1Ty(), failing)});
    auto *next = llvm::BasicBlock::Create(context, "scope.next", function);
    auto *bad = llvm::BasicBlock::Create(context, "scope.cleanup.failed",
                                         function);
    ir.CreateCondBr(status, next, bad);
    ir.SetInsertPoint(bad);
    // A published worker emergency leaves through the verified edge, whose
    // failed-operation path abandons this frame before reading any record.
    auto *abandoned = llvm::BasicBlock::Create(context, "scope.abandoned",
                                               function);
    auto *retained = llvm::BasicBlock::Create(context, "scope.retained",
                                              function);
    ir.CreateCondBr(failures.abandoning(ir), abandoned, retained);
    ir.SetInsertPoint(abandoned);
    stage.failed();
    ir.SetInsertPoint(retained);
    // With an inherited primary, the cleanup helper's own incoming check has
    // already taken the terminal envelope, so a returned failure here is the
    // first cleanup failure of this scope.
    auto *first = llvm::BasicBlock::Create(context, "scope.first", function);
    auto *inconsistent =
        llvm::BasicBlock::Create(context, "scope.inconsistent", function);
    ir.CreateCondBr(ir.CreateLoad(ir.getInt1Ty(), failing), inconsistent,
                    first);
    ir.SetInsertPoint(inconsistent);
    ir.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>{},
                       {});
    ir.CreateUnreachable();
    ir.SetInsertPoint(first);
    failures.copyChannel(ir, stage.failure, scratchChannel);
    ir.CreateStore(ir.getTrue(), failing);
    ir.CreateStore(failures.enterCleanup(ir, stage.failure), token);
    ir.CreateBr(next);
    ir.SetInsertPoint(next);
  }
  failures.leaveCleanup(ir, ir.CreateLoad(pointer, token));
  auto *propagate = llvm::BasicBlock::Create(context, "scope.failed", function);
  auto *done = llvm::BasicBlock::Create(context, "scope.done", function);
  ir.CreateCondBr(ir.CreateLoad(ir.getInt1Ty(), failing), propagate, done);
  ir.SetInsertPoint(propagate);
  stage.failed();
  ir.SetInsertPoint(done);
  return spawnStatus;
}

} // namespace lang::native_llvm
