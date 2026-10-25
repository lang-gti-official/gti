#include "memory.h"

#include "../mir_verification_queries.h"

#include <algorithm>
#include <array>
#include <limits>
#include <llvm/Support/Alignment.h>

namespace lang::native_llvm {

bool nativeProcessMemoryBinding(const MirProgram &program,
                                const MirFunctionInstance &function) {
  const auto *signature =
      processMemoryRuntimeSignature(function.processMemoryOperation);
  return signature &&
         function.definitionKind == MirDefinitionKind::Declaration &&
         function.linkage == LanguageLinkage::C && !function.owner &&
         !function.staticMember && !function.mayRaiseDefinedFailure &&
         !function.requiresUnsafe && !function.tracksCaller &&
         !function.neverReturns && function.typeArguments.empty() &&
         function.valueArguments.empty() &&
         function.cThreadOperation == CThreadRuntimeOperation::None &&
         function.externalSymbol == signature->symbol &&
         function.returnType == signature->returnType &&
         function.parameterTypes == signature->parameters &&
         processMemoryRuntimeSelectedForTarget(signature->operation,
                                               program.target().os);
}

Memory::Memory(Session &session, llvm::IRBuilder<> &ir, Cleanup cleanup,
               MoveState moveState, Scalars::Guard guard)
    : session(session), ir(ir), cleanup(std::move(cleanup)),
      moveState(std::move(moveState)), guard(std::move(guard)) {}

llvm::Function *Memory::provider(ProcessMemoryRuntimeOperation operation) {
  const auto *signature = processMemoryRuntimeSignature(operation);
  if (!signature || !processMemoryRuntimeSelectedForTarget(
                        operation, session.program().mir().target().os))
    throw std::logic_error(
        "LLVM memory provider is not selected for this target");
  const auto plan =
      session.abi().nativeC(signature->returnType, signature->parameters);
  if (auto *existing = session.module().getNamedValue(signature->symbol)) {
    auto *function = llvm::dyn_cast<llvm::Function>(existing);
    if (!function || function->getFunctionType() != plan.type ||
        !function->hasExternalLinkage() || !function->isDeclaration() ||
        function->getCallingConv() != llvm::CallingConv::C)
      throw std::logic_error("conflicting LLVM process-memory provider");
    plan.applyTo(*function);
    return function;
  }
  auto *function =
      llvm::Function::Create(plan.type, llvm::GlobalValue::ExternalLinkage,
                             signature->symbol, session.module());
  plan.applyTo(*function);
  return function;
}

llvm::Value *Memory::providerCall(ProcessMemoryRuntimeOperation operation,
                                  std::span<llvm::Value *const> inputs) {
  auto *function = provider(operation);
  const auto *signature = processMemoryRuntimeSignature(operation);
  const auto plan =
      session.abi().nativeC(signature->returnType, signature->parameters);
  if (inputs.size() != plan.parameters.size())
    throw std::logic_error(
        "LLVM process-memory provider argument count mismatch");
  std::vector<llvm::Value *> arguments;
  for (std::size_t index = 0; index < plan.parameters.size(); ++index) {
    const auto &parameter = plan.parameters[index];
    if (parameter.channel != AbiChannel::Argument || parameter.indirect ||
        parameter.sourceIndex >= inputs.size() ||
        inputs[parameter.sourceIndex]->getType() !=
            plan.type->getParamType(index))
      throw std::logic_error(
          "LLVM process-memory provider lost its scalar ABI");
    arguments.push_back(inputs[parameter.sourceIndex]);
  }
  auto *call = ir.CreateCall(function, arguments);
  plan.applyTo(*call);
  return call;
}

llvm::Value *Memory::processCall(const MirFunctionInstance &target,
                                 std::span<llvm::Value *const> inputs) {
  if (!nativeProcessMemoryBinding(session.program().mir(), target))
    throw std::logic_error("LLVM memory call requires an exact sealed binding");
  return providerCall(target.processMemoryOperation, inputs);
}

llvm::Value *Memory::allocateBytes(std::uint64_t size,
                                   std::uint64_t alignment) {
  const auto scalar =
      session.program().mir().target().nativeCScalar(NativeCScalarKind::Size);
  if (!scalar || scalar->layout.sizeBytes == 0 ||
      scalar->layout.sizeBytes > sizeof(std::uint64_t))
    throw BackendUnsupportedError(
        "LLVM allocation requires a represented target size_t");
  const unsigned bits = scalar->layout.sizeBytes * 8;
  const auto limit = bits == 64 ? std::numeric_limits<std::uint64_t>::max()
                                : (std::uint64_t{1} << bits) - 1;
  if (!size || size > limit || !alignment || alignment > limit ||
      (alignment & (alignment - 1)))
    throw BackendUnsupportedError(
        "LLVM allocation extent/alignment is not representable");
  if (session.program().mir().target().os == "windows") {
    const std::array<llvm::Value *, 2> inputs{ir.getInt64(size),
                                              ir.getInt64(alignment)};
    return providerCall(ProcessMemoryRuntimeOperation::AlignedAllocate, inputs);
  }
  llvm::IRBuilder<> entry(
      &ir.GetInsertBlock()->getParent()->getEntryBlock(),
      ir.GetInsertBlock()->getParent()->getEntryBlock().begin());
  auto *status = entry.CreateAlloca(ir.getInt32Ty());
  const auto pointerSize = session.dataLayout().getPointerSize();
  const std::array<llvm::Value *, 3> inputs{
      ir.getInt64(size),
      ir.getInt64(std::max<std::uint64_t>(alignment, pointerSize)), status};
  return providerCall(ProcessMemoryRuntimeOperation::PosixAllocate, inputs);
}

llvm::Value *Memory::allocateObject(const MirInstruction &instruction) {
  if (instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1 ||
      (instruction.kind != MirInstructionKind::AllocateObject &&
       instruction.intrinsic != IntrinsicKind::AllocateUniqueOwner))
    throw std::logic_error("LLVM object allocation lost its sealed pointee");
  const auto layout = session.layouts().get(instruction.info.type.arguments[0]);
  auto *address = allocateBytes(layout.size, layout.alignment);
  guard(ir.CreateIsNull(address), {DefinedFailureCode::AllocationFailure,
                                   DefinedFailureDetail::UniqueOwner});
  return address;
}

void Memory::releaseBytes(llvm::Value *address) {
  const std::array<llvm::Value *, 1> inputs{address};
  (void)providerCall(session.program().mir().target().os == "windows"
                         ? ProcessMemoryRuntimeOperation::AlignedRelease
                         : ProcessMemoryRuntimeOperation::PosixRelease,
                     inputs);
}

bool uninitializedStorageRepresentation(const MirProgram &program,
                                        const MirClassInstance &instance) {
  return instance.compilerCapability ==
             CompilerCapabilityTypeKind::UninitializedStorage &&
         instance.type.kind == SemanticType::Class &&
         instance.type.arguments.size() == 1 &&
         mir_detail::completeObjectType(program, instance.type.arguments[0]) &&
         instance.declaredFields.empty() && instance.bases.empty() &&
         instance.structuralBases.empty() && !instance.destructor &&
         instance.destructorStatus == SpecialMemberStatus::Generated &&
         !instance.requiresActiveCleanup && !instance.requiresActiveDropState &&
         !instance.polymorphic && !instance.abstract && !instance.cAbiRecord &&
         !instance.cOpaqueHandle && !instance.cAbiLayout &&
         !instance.unionLayout;
}

bool nativeObjectConstructionShape(const MirProgram &program,
                                   const MirInstruction &instruction) {
  if (!mir_detail::objectConstruct(program, instruction))
    return false;
  const auto &type = instruction.parameterTypes[0].arguments[0];
  if (instruction.constructorTarget) {
    const auto *target =
        program.findConstructorInstance(*instruction.constructorTarget);
    const auto *owner =
        target ? program.findClassInstance(target->owner) : nullptr;
    return owner && owner->type == type &&
           instruction.parameterTypes.size() ==
               target->parameterTypes.size() + 1 &&
           std::equal(instruction.parameterTypes.begin() + 1,
                      instruction.parameterTypes.end(),
                      target->parameterTypes.begin());
  }
  if (type.kind == SemanticType::Class) {
    const auto found = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &owner) { return owner.type == type; });
    if (found == program.classInstances().end())
      return false;
    if (instruction.constructorKind == ConstructorKind::Ordinary)
      return instruction.operands.size() == 1 &&
             found->declaredFields.empty() && found->bases.empty() &&
             found->defaultConstructor == SpecialMemberStatus::Generated &&
             !found->generatedDefaultRequiresSchedule;
    return instruction.operands.size() == 2 &&
           (instruction.constructorKind == ConstructorKind::Move
                ? found->moveConstructor == SpecialMemberStatus::Generated
                : found->copyConstructor == SpecialMemberStatus::Generated);
  }
  // Exact completed values already carry their source-owned initialization.
  // A targetless default may not invent constructors for nested class values.
  if (instruction.operands.size() == 2)
    return true;
  const auto passive = [&](const auto &self,
                           const SemanticType &selected) -> bool {
    return selected.kind == SemanticType::Array
               ? selected.arguments.size() == 1 &&
                     self(self, selected.arguments[0])
               : isPassiveScalarValueType(selected) &&
                     selected != SemanticType::Void;
  };
  return passive(passive, type);
}

bool Memory::handles(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::ObjectSize:
  case IntrinsicKind::ObjectAlignment:
  case IntrinsicKind::ObjectDestroy:
  case IntrinsicKind::ObjectRelocate:
  case IntrinsicKind::ObjectRelocateRange:
  case IntrinsicKind::StorageBoundsCheck:
    return true;
  default:
    return false;
  }
}

void Memory::relocate(const MirInstruction &instruction,
                      std::span<llvm::Value *const> inputs) {
  if (!mir_detail::objectRelocate(session.program().mir(), instruction))
    throw std::logic_error("LLVM relocation lost its verified typed contract");
  const auto &type = instruction.parameterTypes.front().arguments.front();
  const auto layout = session.layouts().get(type);
  const bool range =
      instruction.intrinsic == IntrinsicKind::ObjectRelocateRange;
  auto *function = ir.GetInsertBlock()->getParent();
  llvm::BasicBlock *done = nullptr;
  if (range) {
    auto *copy =
        llvm::BasicBlock::Create(session.context(), "relocate.live", function);
    done =
        llvm::BasicBlock::Create(session.context(), "relocate.done", function);
    // The zero-count contract accesses neither address, including null.
    ir.CreateCondBr(ir.CreateICmpEQ(inputs[2], ir.getInt64(0)), done, copy);
    ir.SetInsertPoint(copy);
  }
  // sizeof(T), not an array layout's element stride: T itself may be an array.
  auto *bytes = range ? ir.CreateMul(inputs[2], ir.getInt64(layout.size))
                      : ir.getInt64(layout.size);
  // Transfer eligibility and nonoverlap are GTI facts/unsafe obligations, not
  // inferred from LLVM pointer types or the coincidence of object sizes.
  ir.CreateMemCpy(inputs[0], llvm::Align(layout.alignment), inputs[1],
                  llvm::Align(layout.alignment), bytes);
  if (!range)
    moveState(inputs[1], type);
  if (done) {
    ir.CreateBr(done);
    ir.SetInsertPoint(done);
  }
}

Memory::Result Memory::emit(const MirInstruction &instruction,
                            std::span<llvm::Value *const> inputs,
                            llvm::Value *failureChannel, bool duringFailure) {
  if (inputs.size() != instruction.operands.size())
    throw std::logic_error("LLVM memory operation lost its evaluated inputs");
  switch (instruction.intrinsic) {
  case IntrinsicKind::ObjectSize:
  case IntrinsicKind::ObjectAlignment: {
    if (!mir_detail::objectLayoutQuery(session.program().mir(), instruction))
      throw std::logic_error("LLVM object query lost its sealed type");
    const auto layout = session.layouts().get(*instruction.objectLayoutType);
    return {true, ir.getInt64(instruction.intrinsic == IntrinsicKind::ObjectSize
                                  ? layout.size
                                  : layout.alignment)};
  }
  case IntrinsicKind::ObjectRelocate:
  case IntrinsicKind::ObjectRelocateRange:
    relocate(instruction, inputs);
    return {true};
  case IntrinsicKind::ObjectDestroy: {
    if (!mir_detail::objectDestroy(session.program().mir(), instruction))
      throw std::logic_error("LLVM object destruction lost its exact cleanup");
    if (!instruction.elementCleanup)
      return {true}; // Passive lifetime end has no GTI cleanup to execute.
    if (!failureChannel) {
      if (instruction.definedFailure.propagation !=
          FailurePropagationKind::None)
        throw std::logic_error(
            "LLVM fallible destruction lost its failure channel");
      auto *function = ir.GetInsertBlock()->getParent();
      llvm::IRBuilder<> entry(&function->getEntryBlock(),
                              function->getEntryBlock().begin());
      failureChannel = entry.CreateAlloca(session.abi().failureChannel());
    }
    return {
        true, nullptr,
        ir.CreateCall(cleanup(*instruction.elementCleanup),
                      {inputs[0], failureChannel, ir.getInt1(duringFailure)})};
  }
  case IntrinsicKind::StorageBoundsCheck:
    if (inputs.size() != 2 || !inputs[0]->getType()->isIntegerTy(64) ||
        !inputs[1]->getType()->isIntegerTy(64))
      throw std::logic_error("LLVM storage bounds lost its index and extent");
    guard(ir.CreateICmpUGE(inputs[0], inputs[1]),
          {DefinedFailureCode::IndexOutOfBounds,
           DefinedFailureDetail::PrivateStorage});
    return {true};
  default:
    return {};
  }
}

} // namespace lang::native_llvm
