#include "thread_lifecycle.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace lang::native_llvm {

bool nativeCThreadLifecycleBinding(const MirProgram &program,
                                   const MirFunctionInstance &function) {
  const auto &target = program.target();
  const auto &plan = program.hostedStartupPlan();
  const bool begin =
      function.cThreadOperation == CThreadRuntimeOperation::Begin;
  const bool end = function.cThreadOperation == CThreadRuntimeOperation::End;
  return (begin || end) && plan && program.requiresCThreadRuntime() &&
         target.executionProfile == ExecutionProfile::Concurrent &&
         target.runtimeCapabilities.threads &&
         ((target.os == "macos" && target.arch == "arm64") ||
          (target.os == "linux" && target.arch == "x86_64")) &&
         function.id == (begin ? plan->cThreadBegin : plan->cThreadEnd) &&
         function.definitionKind == MirDefinitionKind::Declaration &&
         function.linkage == LanguageLinkage::C && !function.owner &&
         !function.staticMember && !function.cArrayCountParameter &&
         !function.virtualMethod && !function.pureVirtual &&
         !function.requiresUnsafe && !function.tracksCaller &&
         !function.neverReturns && function.typeArguments.empty() &&
         function.valueArguments.empty() &&
         function.processMemoryOperation ==
             ProcessMemoryRuntimeOperation::None &&
         function.returnType == SemanticType::Void &&
         function.parameterTypes.empty() &&
         function.externalSymbol ==
             (begin ? "gti_rt_cthread_begin_v1" : "gti_rt_cthread_end_v1");
}

bool nativeCThreadLifecycleCall(const MirProgram &program, const MirBody &body,
                                const MirInstruction &instruction) {
  if (body.kind != MirBodyKind::HostedStartup ||
      instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None ||
      instruction.intrinsic != IntrinsicKind::None ||
      !instruction.functionTarget || instruction.receiver ||
      instruction.constructorTarget || instruction.lambdaTarget ||
      instruction.bodyTarget || instruction.nativeCalleeType ||
      instruction.nativeCallbackAdapter || instruction.threadTaskAdapter ||
      !instruction.scopedThreadTasks.empty() ||
      instruction.dispatch != CallDispatch::Static || instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      instruction.borrowArgument ||
      instruction.borrowAccess != AccessMode::ReadOnly ||
      instruction.tracksCaller || instruction.neverReturns ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.callableArguments.empty() ||
      instruction.callableInvocation || instruction.callableBoundary ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget || instruction.result ||
      instruction.destination || !instruction.lifecycle.empty() ||
      !instruction.parameterTypes.empty() || !instruction.operands.empty() ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.synchronization != SynchronizationOperation{})
    return false;
  const auto *function =
      program.findFunctionInstance(*instruction.functionTarget);
  if (!function || !nativeCThreadLifecycleBinding(program, *function))
    return false;
  const auto *operation = program.hostedStartupPlan()->findOperation(
      instruction.hostedStartupOperation);
  return operation && operation->instruction == instruction.id &&
         !operation->terminator && !operation->place && !operation->value &&
         !operation->dropObligation && !operation->failureRecord &&
         operation->failureBehavior == MirHostedStartupFailureBehavior::None &&
         operation->kind ==
             (function->cThreadOperation == CThreadRuntimeOperation::Begin
                  ? MirHostedStartupOperationKind::BeginCThreadRuntime
                  : MirHostedStartupOperationKind::EndCThreadRuntime);
}

bool nativeCThreadOperationBinding(const MirProgram &program,
                                   const MirFunctionInstance &function) {
  const auto &target = program.target();
  const auto u64 = SemanticType(SemanticType::UInt64);
  const auto i64 = SemanticType(SemanticType::Int64);
  const auto i32 = SemanticType(SemanticType::Int32);
  const auto context = SemanticType::rawPointerTo(SemanticType::Void);
  struct Row {
    CThreadRuntimeOperation operation;
    std::string_view symbol;
    SemanticType result;
    std::vector<SemanticType> parameters;
  };
  const std::array<Row, 14> rows{{
      {CThreadRuntimeOperation::Create,
       "gti_rt_cthread_create_v1",
       i32,
       {SemanticType::rawPointerTo(u64),
        SemanticType::nativeFunctionType(i32, std::array{context}), context}},
      {CThreadRuntimeOperation::Join,
       "gti_rt_cthread_join_v1",
       i32,
       {u64, SemanticType::rawPointerTo(i32)}},
      {CThreadRuntimeOperation::InvokeOnceInitializer,
       "gti_rt_once_invoke_v1",
       SemanticType::Void,
       {SemanticType::nativeFunctionType(SemanticType::Void, {})}},
      {CThreadRuntimeOperation::MutexInit,
       "gti_rt_mtx_init_v1",
       i32,
       {SemanticType::rawPointerTo(u64), i32}},
      {CThreadRuntimeOperation::MutexDestroy,
       "gti_rt_mtx_destroy_v1",
       SemanticType::Void,
       {u64}},
      {CThreadRuntimeOperation::MutexLock, "gti_rt_mtx_lock_v1", i32, {u64}},
      {CThreadRuntimeOperation::MutexTryLock,
       "gti_rt_mtx_trylock_v1",
       i32,
       {u64}},
      {CThreadRuntimeOperation::MutexTimedLock,
       "gti_rt_mtx_timedlock_v1",
       i32,
       {u64, i64, i64}},
      {CThreadRuntimeOperation::MutexUnlock,
       "gti_rt_mtx_unlock_v1",
       i32,
       {u64}},
      {CThreadRuntimeOperation::ConditionInit,
       "gti_rt_cnd_init_v1",
       i32,
       {SemanticType::rawPointerTo(u64)}},
      {CThreadRuntimeOperation::ConditionDestroy,
       "gti_rt_cnd_destroy_v1",
       SemanticType::Void,
       {u64}},
      {CThreadRuntimeOperation::ConditionSignal,
       "gti_rt_cnd_signal_v1",
       i32,
       {u64}},
      {CThreadRuntimeOperation::ConditionBroadcast,
       "gti_rt_cnd_broadcast_v1",
       i32,
       {u64}},
      {CThreadRuntimeOperation::ConditionWait,
       "gti_rt_cnd_wait_v1",
       i32,
       {u64, u64}},
  }};
  const Row timedWait{CThreadRuntimeOperation::ConditionTimedWait,
                      "gti_rt_cnd_timedwait_v1",
                      i32,
                      {u64, u64, i64, i64}};
  const auto matches = [&](const Row &row) {
    return function.cThreadOperation == row.operation &&
           function.externalSymbol == row.symbol &&
           function.returnType == row.result &&
           function.parameterTypes == row.parameters;
  };
  return target.executionProfile == ExecutionProfile::Concurrent &&
         target.runtimeCapabilities.threads &&
         ((target.os == "macos" && target.arch == "arm64") ||
          (target.os == "linux" && target.arch == "x86_64")) &&
         function.definitionKind == MirDefinitionKind::Declaration &&
         function.linkage == LanguageLinkage::C && !function.owner &&
         !function.staticMember && !function.cArrayCountParameter &&
         !function.virtualMethod && !function.pureVirtual &&
         !function.tracksCaller && !function.neverReturns &&
         function.typeArguments.empty() && function.valueArguments.empty() &&
         function.callableParameters.empty() &&
         function.processMemoryOperation ==
             ProcessMemoryRuntimeOperation::None &&
         (std::any_of(rows.begin(), rows.end(), matches) || matches(timedWait));
}

SynchronizationOperationKind
cThreadOperationSynchronization(CThreadRuntimeOperation operation) {
  switch (operation) {
  case CThreadRuntimeOperation::Create:
    return SynchronizationOperationKind::ThreadSpawn;
  case CThreadRuntimeOperation::Join:
    return SynchronizationOperationKind::ThreadJoin;
  case CThreadRuntimeOperation::MutexLock:
  case CThreadRuntimeOperation::MutexTryLock:
  case CThreadRuntimeOperation::MutexTimedLock:
    return SynchronizationOperationKind::MutexLock;
  case CThreadRuntimeOperation::MutexUnlock:
    return SynchronizationOperationKind::MutexUnlock;
  case CThreadRuntimeOperation::ConditionWait:
  case CThreadRuntimeOperation::ConditionTimedWait:
    return SynchronizationOperationKind::ConditionWait;
  case CThreadRuntimeOperation::ConditionSignal:
  case CThreadRuntimeOperation::ConditionBroadcast:
    return SynchronizationOperationKind::ConditionNotify;
  default:
    return SynchronizationOperationKind::None;
  }
}

bool nativeCThreadLifecycleOperation(
    const MirProgram &program, const MirHostedStartupOperation &operation) {
  const auto &plan = program.hostedStartupPlan();
  const auto *canonical = plan ? plan->findOperation(operation.id) : nullptr;
  if (!canonical || *canonical != operation)
    return false;
  const auto *body = program.hostedStartup();
  const auto *block = body ? body->findBlock(operation.block) : nullptr;
  if (!block)
    return false;
  const auto instruction =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const auto &candidate) {
                     return candidate.id == operation.instruction;
                   });
  return instruction != block->instructions.end() &&
         instruction->hostedStartupOperation == operation.id &&
         nativeCThreadLifecycleCall(program, *body, *instruction);
}

} // namespace lang::native_llvm
