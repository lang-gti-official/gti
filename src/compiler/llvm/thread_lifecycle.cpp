#include "thread_lifecycle.h"

#include <algorithm>

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
