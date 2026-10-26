#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/layout.h"
#include "../src/compiler/llvm/lifetimes.h"
#include "gti/mir_effects.h"

#include <set>

#if !defined(GTI_LLVM_C04_MIR_EVALUATION)
#error "C04 worker oracle must not enter production targets"
#endif

namespace lang::native_llvm {
std::vector<HirFunctionInstanceId> c04ScopedTargets(
    const Session &session, MirBodyAddress address,
    const MirInstruction &instruction) {
  const auto &program = session.program().mir();
  const auto reject = []() -> void {
    throw BackendUnsupportedError("C04 worker scope lost its exact sealed task contract");
  };
  if (program.executionProfile() != ExecutionProfile::Concurrent ||
      instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ThreadScope ||
      !instruction.result || instruction.receiver || instruction.threadTaskAdapter ||
      instruction.functionTarget || instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.constructorTarget || instruction.dispatch != CallDispatch::Static ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      !instruction.localFailureSites.empty() || !instruction.definedFailure.localOrigins.empty() ||
      instruction.info.type != SemanticType::Int32 ||
      instruction.operands.empty() || instruction.operands.size() > 8 ||
      instruction.parameterTypes.size() != instruction.operands.size() ||
      instruction.scopedThreadTasks.size() != instruction.operands.size() ||
      instruction.synchronization.kind != SynchronizationOperationKind::ThreadScope ||
      !validSynchronizationOperation(instruction.synchronization) ||
      instruction.definedFailure.propagation != FailurePropagationKind::TaskJoin)
    reject();
  std::vector<HirFunctionInstanceId> targets;
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    const auto *adapter = program.findThreadTaskAdapter(instruction.scopedThreadTasks[index].adapter);
    if (!adapter || !adapter->scoped || adapter->scopeArgument != index ||
        adapter->sourceBody != address || adapter->spawnInstruction != instruction.id ||
        adapter->spawnValue != instruction.hirValue ||
        adapter->taskType != instruction.operands[index].type ||
        adapter->taskType != instruction.parameterTypes[index] ||
        adapter->resultType != SemanticType::Void || !adapter->functionTarget ||
        adapter->lambdaTarget || adapter->invocation != CallableInvocationCapability::Once ||
        !adapter->catchesWorkerFailure || !adapter->catchesNativeExceptions)
      reject();
    const auto *source = findMirBody(program, address);
    const auto dependency = instruction.scopedThreadTasks[index].dependency;
    const auto &operand = instruction.operands[index];
    const auto *value = source && operand.kind == MirOperandKind::Value
                            ? source->findValue(operand.value) : nullptr;
    if (!value || value->info.type != operand.type ||
        (value->info.traits.containsBorrowedState
             ? dependency == 0 || !source->findLoan(dependency) : dependency != 0))
      reject();
    const auto *target = program.findFunctionInstance(*adapter->functionTarget);
    const auto *owner = target && target->owner ? program.findClassInstance(*target->owner) : nullptr;
    if (!target || !owner || owner->type != adapter->taskType ||
        target->definitionKind != MirDefinitionKind::Source || target->linkage != LanguageLinkage::Gti ||
        !target->parameterTypes.empty() || target->returnType != SemanticType::Void ||
        target->tracksCaller || target->requiresUnsafe || target->pureVirtual ||
        target->overloadedOperator != std::optional{OverloadedOperator::Call} ||
        adapter->targetInvocation != callableInvocationCapability(target->receiverMutability))
      reject();
    std::set<std::pair<MirBodyKind, std::uint64_t>> visited;
    const auto nonNested = [&](const auto &self, MirBodyAddress candidate) -> void {
      if (!visited.emplace(candidate.kind, candidate.owner).second)
        return;
      const auto *body = findMirBody(program, candidate);
      if (!body)
        reject();
      if (candidate.kind == MirBodyKind::Destructor && !body->dropObligations.empty())
        throw BackendUnsupportedError("C04 worker cleanup comparison requires a leaf destructor");
      for (const auto &block : body->blocks)
        for (const auto &stage : block.instructions) {
          if (stage.tombstone)
            continue;
          if (candidate.kind == MirBodyKind::Destructor &&
              (stage.definedFailure.mayUnwind() || stage.kind == MirInstructionKind::Call ||
               stage.kind == MirInstructionKind::CallBody))
            throw BackendUnsupportedError("C04 fallible worker cleanup requires the emergency-handoff comparison");
          if (stage.intrinsic == IntrinsicKind::ThreadScope)
            throw BackendUnsupportedError("C04 nested worker cleanup containment is not yet compared");
          if (stage.functionTarget && stage.intrinsic == IntrinsicKind::None)
            self(self, {MirBodyKind::Function, *stage.functionTarget});
          if (stage.functionPointerFunctionTarget)
            self(self, {MirBodyKind::Function, *stage.functionPointerFunctionTarget});
          if (stage.constructorTarget)
            self(self, {MirBodyKind::Constructor, *stage.constructorTarget});
          if (stage.bodyTarget)
            self(self, *stage.bodyTarget);
        }
      for (const auto &drop : body->dropObligations)
        for (auto destructor : mirDropDestructorTargets(drop.dropType))
          self(self, {MirBodyKind::Destructor, destructor});
    };
    nonNested(nonNested, {MirBodyKind::Function, target->id});
    targets.push_back(target->id);
  }
  return targets;
}

llvm::Value *emitC04Scope(Session &session, llvm::IRBuilder<> &ir,
    MirBodyAddress address, const MirInstruction &instruction,
    llvm::ArrayRef<llvm::Value *> inputs, llvm::Value *failure, Lifetimes &lifetimes) {
  const auto targets = c04ScopedTargets(session, address, instruction);
  if (inputs.size() != targets.size())
    throw BackendUnsupportedError("C04 worker inputs disagree with the sealed task group");
  auto *pointer = session.layouts().pointer();
  auto *descriptor = llvm::StructType::get(session.context(), {pointer, pointer, pointer});
  auto *array = llvm::ArrayType::get(descriptor, targets.size());
  llvm::IRBuilder<> entry(&ir.GetInsertBlock()->getParent()->getEntryBlock(),
                         ir.GetInsertBlock()->getParent()->getEntryBlock().begin());
  auto *tasks = entry.CreateAlloca(array, nullptr, "c04.scoped.tasks");
  for (std::size_t index = 0; index < targets.size(); ++index) {
    const auto *target = session.program().mir().findFunctionInstance(targets[index]);
    const auto abi = session.abi().gti(SemanticType::Void, {}, target->mayRaiseDefinedFailure, true);
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
    const std::vector<AbiChannel> channels{AbiChannel::Receiver};
#else
    // This bounded oracle currently requires a fallible void source receiver.
    const std::vector<AbiChannel> channels{AbiChannel::FailureDestination, AbiChannel::Receiver};
#endif
    if (abi.parameters.size() != channels.size() || inputs[index]->getType() != pointer)
      throw BackendUnsupportedError("C04 worker receiver requires its planned candidate ABI");
    for (std::size_t lane = 0; lane < channels.size(); ++lane)
      if (abi.parameters[lane].channel != channels[lane])
        throw BackendUnsupportedError("C04 worker candidate acquired an unplanned ABI channel");
    auto *slot = ir.CreateInBoundsGEP(array, tasks, {ir.getInt32(0), ir.getInt64(index)});
    ir.CreateStore(session.abi().declareFunction(targets[index]), ir.CreateStructGEP(descriptor, slot, 0));
    ir.CreateStore(inputs[index], ir.CreateStructGEP(descriptor, slot, 1));
    const auto recipe = mirUniqueCleanupType(session.program().mir(), instruction.operands[index].type);
    llvm::Value *cleanup = llvm::ConstantPointerNull::get(pointer);
    if (recipe) {
      auto *helper = lifetimes.cleanup(*recipe);
      auto *expected = llvm::FunctionType::get(ir.getInt1Ty(), {pointer, pointer, ir.getInt1Ty()}, false);
      if (helper->getFunctionType() != expected)
        throw BackendUnsupportedError("C04 task cleanup lost its existing recipe ABI");
      cleanup = helper;
    }
    ir.CreateStore(cleanup, ir.CreateStructGEP(descriptor, slot, 2));
  }
  // This is a test-only C runtime query envelope, not a GTI worker ABI.
  auto helper = session.module().getOrInsertFunction("__gti_c04_worker_scope",
      llvm::FunctionType::get(ir.getInt32Ty(), {pointer, pointer, ir.getInt64Ty()}, false));
  auto *call = ir.CreateCall(helper, {failure, tasks, ir.getInt64(targets.size())});
  call->setDoesNotThrow();
  return call;
}
} // namespace lang::native_llvm
