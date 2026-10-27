#include "memory.h"

#include "../mir_verification_queries.h"
#include "gti/mir_effects.h"

#include <algorithm>
#include <array>
#include <limits>
#include <llvm/Support/Alignment.h>

namespace lang::native_llvm {

bool nativeProcessMemoryBinding(const MirProgram &program,
                                const MirFunctionInstance &function) {
  const auto *signature =
      processMemoryRuntimeSignature(function.processMemoryOperation);
  // Declaration-level mayRaiseDefinedFailure is conservative, not this C
  // ABI's transport. The sealed source call must independently carry no GTI
  // failure operation; neither value of that declaration bit grants one.
  return signature &&
         function.definitionKind == MirDefinitionKind::Declaration &&
         function.linkage == LanguageLinkage::C && !function.owner &&
         !function.staticMember && !function.cArrayCountParameter &&
         !function.virtualMethod && !function.pureVirtual &&
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

bool nativeProcessMemoryCall(const MirProgram &program,
                             const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None ||
      instruction.intrinsic != IntrinsicKind::None ||
      !instruction.functionTarget || instruction.receiver ||
      instruction.constructorTarget || instruction.lambdaTarget ||
      instruction.bodyTarget || instruction.nativeCalleeType ||
      instruction.nativeCallbackAdapter || instruction.threadTaskAdapter ||
      !instruction.scopedThreadTasks.empty() ||
      instruction.dispatch != CallDispatch::Static || instruction.loan ||
      instruction.tracksCaller || instruction.neverReturns ||
      !instruction.definedFailure.empty() ||
      !instruction.callableArguments.empty() ||
      instruction.callableInvocation || instruction.callableBoundary ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget)
    return false;
  const auto *target =
      program.findFunctionInstance(*instruction.functionTarget);
  if (!target || !nativeProcessMemoryBinding(program, *target) ||
      instruction.parameterTypes != target->parameterTypes ||
      instruction.info.type != target->returnType ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.operands.size() != target->parameterTypes.size() ||
      instruction.result.has_value() ==
          (target->returnType == SemanticType::Void))
    return false;
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    const auto &operand = instruction.operands[index];
    if (operand.type != target->parameterTypes[index] || operand.place ||
        operand.loan ||
        !((operand.kind == MirOperandKind::Value && operand.value &&
           !operand.literal) ||
          (operand.kind == MirOperandKind::Constant && !operand.value &&
           operand.literal)))
      return false;
  }
  const bool pointer =
      target->returnType.kind == SemanticType::RawPointer ||
      std::any_of(target->parameterTypes.begin(), target->parameterTypes.end(),
                  [](const auto &type) {
                    return type.kind == SemanticType::RawPointer;
                  });
  return instruction.unsafeOperation ==
         (pointer ? UnsafeOperationKind::ForeignPointerCall
                  : UnsafeOperationKind::None);
}

Memory::Memory(Session &session, llvm::IRBuilder<> &ir, Cleanup cleanup,
               Relocate relocateObject, Scalars::Guard guard)
    : session(session), ir(ir), cleanup(std::move(cleanup)),
      relocateObject(std::move(relocateObject)), guard(std::move(guard)) {}

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

Memory::SharedRelease Memory::releaseShared(llvm::Value *handle) {
  const auto &layout = session.layouts().sharedOwner();
  auto *value = ir.CreateLoad(layout.handle.storage, handle);
  ir.CreateStore(llvm::Constant::getNullValue(layout.handle.storage), handle);
  auto *block = ir.CreateExtractValue(value, layout.handleBlock);
  auto *function = ir.GetInsertBlock()->getParent();
  auto *empty = ir.GetInsertBlock();
  auto *live =
      llvm::BasicBlock::Create(session.context(), "release.live", function);
  auto *done =
      llvm::BasicBlock::Create(session.context(), "release.done", function);
  ir.CreateCondBr(ir.CreateIsNull(block), done, live);
  ir.SetInsertPoint(live);
  // Read immutable allocation identity while this reference still protects
  // the block. A non-last release may not touch the block after decrement.
  auto *allocation = ir.CreateLoad(
      session.layouts().pointer(),
      ir.CreateStructGEP(layout.block.storage, block, layout.blockAllocation));
  auto *strong =
      ir.CreateStructGEP(layout.block.storage, block, layout.blockStrong);
  auto *previous =
      ir.CreateAtomicRMW(llvm::AtomicRMWInst::Sub, strong, ir.getInt64(1),
                         llvm::Align(layout.block.alignment),
                         llvm::AtomicOrdering::AcquireRelease);
  auto *last = ir.CreateICmpEQ(previous, ir.getInt64(1));
  ir.CreateBr(done);
  auto *released = ir.GetInsertBlock();
  ir.SetInsertPoint(done);
  auto *lastResult = ir.CreatePHI(ir.getInt1Ty(), 2);
  lastResult->addIncoming(ir.getFalse(), empty);
  lastResult->addIncoming(last, released);
  auto *allocationResult = ir.CreatePHI(session.layouts().pointer(), 2);
  allocationResult->addIncoming(
      llvm::ConstantPointerNull::get(session.layouts().pointer()), empty);
  allocationResult->addIncoming(allocation, released);
  return {lastResult, allocationResult, block};
}

llvm::Value *Memory::shared(const MirInstruction &instruction,
                            std::span<llvm::Value *const> inputs) {
  const auto &layout = session.layouts().sharedOwner();
  const auto intrinsic = instruction.intrinsic;
  if (intrinsic == IntrinsicKind::SharedOwnerSwap) {
    auto *left = ir.CreateLoad(layout.handle.storage, inputs[0]);
    auto *right = ir.CreateLoad(layout.handle.storage, inputs[1]);
    ir.CreateStore(right, inputs[0]);
    ir.CreateStore(left, inputs[1]);
    return nullptr;
  }
  const bool creates = intrinsic == IntrinsicKind::SharedOwnerEmpty ||
                       intrinsic == IntrinsicKind::SharedOwnerFromUnique ||
                       intrinsic == IntrinsicKind::SharedOwnerClone;
  llvm::Value *result = nullptr;
  if (creates) {
    llvm::IRBuilder<> entry(
        &ir.GetInsertBlock()->getParent()->getEntryBlock(),
        ir.GetInsertBlock()->getParent()->getEntryBlock().begin());
    result = entry.CreateAlloca(layout.handle.storage);
    ir.CreateStore(llvm::Constant::getNullValue(layout.handle.storage), result);
  }
  if (intrinsic == IntrinsicKind::SharedOwnerEmpty)
    return result;
  if (intrinsic == IntrinsicKind::SharedOwnerFromUnique) {
    auto *function = ir.GetInsertBlock()->getParent();
    auto *promote =
        llvm::BasicBlock::Create(session.context(), "promote.live", function);
    auto *done =
        llvm::BasicBlock::Create(session.context(), "promote.done", function);
    ir.CreateCondBr(ir.CreateIsNull(inputs[0]), done, promote);
    ir.SetInsertPoint(promote);
    auto *block = allocateBytes(layout.block.size, layout.block.alignment);
    guard(ir.CreateIsNull(block), {DefinedFailureCode::AllocationFailure,
                                   DefinedFailureDetail::SharedOwner});
    ir.CreateStore(
        ir.getInt64(1),
        ir.CreateStructGEP(layout.block.storage, block, layout.blockStrong));
    ir.CreateStore(inputs[0], ir.CreateStructGEP(layout.block.storage, block,
                                                 layout.blockAllocation));
    llvm::Value *value = llvm::Constant::getNullValue(layout.handle.storage);
    value = ir.CreateInsertValue(value, block, layout.handleBlock);
    value = ir.CreateInsertValue(value, inputs[0], layout.handleView);
    ir.CreateStore(value, result);
    ir.CreateBr(done);
    ir.SetInsertPoint(done);
    return result;
  }
  auto *value = ir.CreateLoad(layout.handle.storage, inputs[0]);
  auto *block = ir.CreateExtractValue(value, layout.handleBlock);
  auto *view = ir.CreateExtractValue(value, layout.handleView);
  if (intrinsic == IntrinsicKind::SharedOwnerIsNull)
    return ir.CreateIsNull(block);
  if (intrinsic == IntrinsicKind::SharedOwnerBorrow) {
    if (instruction.definedFailure.localOrigins.size() != 1 ||
        instruction.definedFailure.localOrigins[0].outcomes.size() != 1 ||
        instruction.definedFailure.localOrigins[0].outcomes[0].code !=
            DefinedFailureCode::EmptyOwnerAccess)
      throw std::logic_error(
          "LLVM shared borrow lost its exact access failure");
    guard(ir.CreateIsNull(view),
          instruction.definedFailure.localOrigins[0].outcomes[0]);
    return view;
  }
  auto *function = ir.GetInsertBlock()->getParent();
  auto *empty = ir.GetInsertBlock();
  auto *live =
      llvm::BasicBlock::Create(session.context(), "shared.live", function);
  auto *done =
      llvm::BasicBlock::Create(session.context(), "shared.done", function);
  ir.CreateCondBr(ir.CreateIsNull(block), done, live);
  ir.SetInsertPoint(live);
  auto *strong =
      ir.CreateStructGEP(layout.block.storage, block, layout.blockStrong);
  auto *count = ir.CreateLoad(ir.getInt64Ty(), strong);
  count->setAlignment(llvm::Align(layout.block.alignment));
  count->setAtomic(llvm::AtomicOrdering::Monotonic);
  if (intrinsic == IntrinsicKind::SharedOwnerUseCount) {
    ir.CreateBr(done);
    ir.SetInsertPoint(done);
    auto *observed = ir.CreatePHI(ir.getInt64Ty(), 2);
    observed->addIncoming(ir.getInt64(0), empty);
    observed->addIncoming(count, live);
    return observed;
  }
  if (intrinsic != IntrinsicKind::SharedOwnerClone)
    throw std::logic_error("LLVM shared dispatch lost its sealed operation");
  auto *retry =
      llvm::BasicBlock::Create(session.context(), "retain.retry", function);
  auto *retained =
      llvm::BasicBlock::Create(session.context(), "retained", function);
  ir.CreateBr(retry);
  ir.SetInsertPoint(retry);
  auto *expected = ir.CreatePHI(ir.getInt64Ty(), 2);
  expected->addIncoming(count, live);
  guard(ir.CreateICmpEQ(expected,
                        ir.getInt64(std::numeric_limits<std::uint64_t>::max())),
        {DefinedFailureCode::AllocationFailure,
         DefinedFailureDetail::SharedOwner});
  // Nonwrapping relaxed retain: there is no transient zero/overflow state
  // that a concurrent last-release could mistake for an empty block.
  auto *exchange = ir.CreateAtomicCmpXchg(
      strong, expected, ir.CreateAdd(expected, ir.getInt64(1)),
      llvm::Align(layout.block.alignment), llvm::AtomicOrdering::Monotonic,
      llvm::AtomicOrdering::Monotonic);
  expected->addIncoming(ir.CreateExtractValue(exchange, 0),
                        ir.GetInsertBlock());
  ir.CreateCondBr(ir.CreateExtractValue(exchange, 1), retained, retry);
  ir.SetInsertPoint(retained);
  ir.CreateStore(value, result);
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
  return result;
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

bool sharedOwnerRepresentation(const MirProgram &program,
                               const MirClassInstance &instance) {
  return instance.compilerCapability ==
             CompilerCapabilityTypeKind::SharedOwner &&
         instance.type.kind == SemanticType::Class &&
         instance.type.arguments.size() == 1 &&
         mir_detail::completeObjectType(program, instance.type.arguments[0]) &&
         instance.declaredFields.empty() && instance.bases.empty() &&
         instance.structuralBases.empty() && !instance.destructor &&
         instance.destructorStatus == SpecialMemberStatus::Generated &&
         !instance.requiresActiveDropState && !instance.polymorphic &&
         !instance.abstract && !instance.cAbiRecord &&
         !instance.cOpaqueHandle && !instance.cAbiLayout &&
         !instance.unionLayout;
}

bool polymorphicStorageRepresentation(const MirProgram &program,
                                      const MirClassInstance &instance) {
  if (instance.compilerCapability !=
          CompilerCapabilityTypeKind::PolymorphicStorage ||
      instance.type.kind != SemanticType::Class ||
      instance.type.arguments.size() != 1 ||
      instance.type.valueArguments.size() != 2 ||
      !instance.declaredFields.empty() || !instance.bases.empty() ||
      !instance.structuralBases.empty() || instance.destructor ||
      instance.destructorStatus != SpecialMemberStatus::Generated ||
      instance.requiresActiveCleanup || instance.requiresActiveDropState ||
      instance.polymorphic || instance.abstract || instance.cAbiRecord ||
      instance.cOpaqueHandle || instance.cAbiLayout || instance.unionLayout)
    return false;
  const auto &bytes = instance.type.valueArguments[0];
  const auto &alignment = instance.type.valueArguments[1];
  if (bytes.kind != CompileTimeValue::UInt64 || bytes.value == 0 ||
      bytes.value > 4096 || alignment.kind != CompileTimeValue::UInt64 ||
      alignment.value == 0 || alignment.value > 4096 ||
      (alignment.value & (alignment.value - 1)) != 0)
    return false;
  const MirClassInstance *interface = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == instance.type.arguments.front()) {
      if (interface)
        return false;
      interface = &candidate;
    }
  return interface && interface->polymorphic &&
         interface->compilerCapability == CompilerCapabilityTypeKind::None;
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

bool nativeUniqueConstructionShape(const MirProgram &program,
                                   const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::AllocateUniqueOwner ||
      !instruction.result ||
      instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1)
    return false;
  const auto &type = instruction.info.type.arguments[0];
  if (type.kind != SemanticType::Class)
    return type != SemanticType::Void &&
           (isPassiveScalarValueType(type) ||
            type == SemanticType::StringView ||
            type.kind == SemanticType::RawPointer ||
            type.kind == SemanticType::NativeFunction) &&
           !instruction.constructorTarget && instruction.operands.size() <= 1 &&
           instruction.operands.size() == instruction.parameterTypes.size() &&
           (instruction.operands.empty() ||
            instruction.parameterTypes[0] == type);
  const auto found = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [&](const auto &owner) { return owner.type == type; });
  if (found == program.classInstances().end() || found->abstract ||
      found->kind == ClassKind::Interface)
    return false;
  // A concrete polymorphic object is constructed only through its selected
  // source constructor, which installs its exact active dispatch state.
  if (found->polymorphic && !instruction.constructorTarget)
    return false;
  if (instruction.constructorTarget) {
    const auto *target =
        program.findConstructorInstance(*instruction.constructorTarget);
    return target && target->owner == found->id &&
           instruction.parameterTypes == target->parameterTypes;
  }
  if (!mir_detail::generatedUniqueConstruction(instruction))
    return false;
  if (instruction.constructorKind == ConstructorKind::Ordinary)
    return found->defaultConstructor == SpecialMemberStatus::Generated &&
           !found->generatedDefaultRequiresSchedule &&
           found->declaredFields.empty() && found->bases.empty();
  return instruction.constructorKind == ConstructorKind::Move
             ? found->moveConstructor == SpecialMemberStatus::Generated
             : found->copyConstructor == SpecialMemberStatus::Generated;
}

bool Memory::handles(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::ObjectSize:
  case IntrinsicKind::ObjectAlignment:
  case IntrinsicKind::ObjectDestroy:
  case IntrinsicKind::ObjectRelocate:
  case IntrinsicKind::ObjectRelocateRange:
  case IntrinsicKind::RestoringSwap:
  case IntrinsicKind::RestoringIndexedSwap:
  case IntrinsicKind::StorageBoundsCheck:
  case IntrinsicKind::UniqueOwnerBorrow:
  case IntrinsicKind::UniqueOwnerBorrowMut:
  case IntrinsicKind::UniqueOwnerIsNull:
  case IntrinsicKind::UniqueOwnerSwap:
  case IntrinsicKind::UniqueOwnerUpcast:
  case IntrinsicKind::SharedOwnerEmpty:
  case IntrinsicKind::SharedOwnerFromUnique:
  case IntrinsicKind::SharedOwnerClone:
  case IntrinsicKind::SharedOwnerBorrow:
  case IntrinsicKind::SharedOwnerIsNull:
  case IntrinsicKind::SharedOwnerUseCount:
  case IntrinsicKind::SharedOwnerSwap:
    return true;
  default:
    return false;
  }
}

void Memory::transfer(llvm::Value *destination, llvm::Value *source,
                      const SemanticType &type) {
  if (mirTypeRelocationKind(session.program().mir(), type) !=
      RelocationKind::Bitwise) {
    if (!relocateObject)
      throw std::logic_error(
          "LLVM structural relocation lost its typed transfer");
    relocateObject(destination, source, type);
    return;
  }
  const auto layout = session.layouts().get(type);
  ir.CreateMemCpy(destination, llvm::Align(layout.alignment), source,
                  llvm::Align(layout.alignment), layout.size);
}

llvm::Value *Memory::held(const SemanticType &type) {
  auto *function = ir.GetInsertBlock()->getParent();
  llvm::IRBuilder<> entry(&function->getEntryBlock(),
                          function->getEntryBlock().begin());
  const auto layout = session.layouts().get(type);
  auto *slot = entry.CreateAlloca(layout.storage);
  slot->setAlignment(llvm::Align(layout.alignment));
  return slot;
}

void Memory::relocate(const MirInstruction &instruction,
                      std::span<llvm::Value *const> inputs) {
  if (!mir_detail::objectRelocate(session.program().mir(), instruction))
    throw std::logic_error("LLVM relocation lost its verified typed contract");
  const auto &type = instruction.parameterTypes.front().arguments.front();
  const auto layout = session.layouts().get(type);
  const bool range =
      instruction.intrinsic == IntrinsicKind::ObjectRelocateRange;
  if (mirTypeRelocationKind(session.program().mir(), type) !=
      RelocationKind::Bitwise) {
    if (range)
      throw std::logic_error(
          "LLVM structural relocation lost its typed transfer");
    transfer(inputs[0], inputs[1], type);
    return;
  }
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
  case IntrinsicKind::SharedOwnerEmpty:
  case IntrinsicKind::SharedOwnerFromUnique:
  case IntrinsicKind::SharedOwnerClone:
  case IntrinsicKind::SharedOwnerBorrow:
  case IntrinsicKind::SharedOwnerIsNull:
  case IntrinsicKind::SharedOwnerUseCount:
  case IntrinsicKind::SharedOwnerSwap:
    return {true, shared(instruction, inputs)};
  case IntrinsicKind::UniqueOwnerBorrow:
  case IntrinsicKind::UniqueOwnerBorrowMut:
  case IntrinsicKind::UniqueOwnerIsNull: {
    auto *address =
        inputs[0]; // Verified copy-load of the unique handle, not its slot.
    if (instruction.intrinsic == IntrinsicKind::UniqueOwnerIsNull)
      return {true, ir.CreateIsNull(address)};
    if (instruction.definedFailure.localOrigins.size() != 1 ||
        instruction.definedFailure.localOrigins[0].outcomes.size() != 1 ||
        instruction.definedFailure.localOrigins[0].outcomes[0].code !=
            DefinedFailureCode::EmptyOwnerAccess)
      throw std::logic_error(
          "LLVM owner borrow lost its sealed access failure");
    guard(ir.CreateIsNull(address),
          instruction.definedFailure.localOrigins[0].outcomes[0]);
    return {true, address};
  }
  case IntrinsicKind::UniqueOwnerUpcast: {
    // The verified public base conversion keeps one owned allocation and
    // selects its base view at the exact static subobject offset. An empty
    // owner stays empty. Cleanup and allocation identity stay with the
    // installed dispatch descriptor, never with this static view.
    if (instruction.operands.size() != 1 ||
        instruction.operands[0].type.kind != SemanticType::UniqueOwner ||
        instruction.operands[0].type.arguments.size() != 1 ||
        instruction.info.type.kind != SemanticType::UniqueOwner ||
        instruction.info.type.arguments.size() != 1)
      throw std::logic_error("LLVM owner upcast lost its sealed owner types");
    const auto view = session.layouts().baseSubobject(
        instruction.operands[0].type.arguments[0],
        instruction.info.type.arguments[0], "memory.uniqueOwnerUpcast");
    if (view.offset == 0)
      return {true, inputs[0]};
    auto *adjusted =
        ir.CreateGEP(ir.getInt8Ty(), inputs[0],
                     ir.getInt64(static_cast<std::int64_t>(view.offset)));
    return {true, ir.CreateSelect(ir.CreateIsNull(inputs[0]),
                                  llvm::ConstantPointerNull::get(
                                      session.layouts().pointer()),
                                  adjusted)};
  }
  case IntrinsicKind::UniqueOwnerSwap: {
    auto *left = ir.CreateLoad(session.layouts().pointer(), inputs[0]);
    auto *right = ir.CreateLoad(session.layouts().pointer(), inputs[1]);
    ir.CreateStore(right, inputs[0]);
    ir.CreateStore(left, inputs[1]);
    return {true};
  }
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
  case IntrinsicKind::RestoringSwap: {
    // Two disjoint initialized places exchange representations through one
    // held object. Each transfer retires its source and every retired place
    // is refilled at once, so both places end initialized and the held object
    // ends retired and is never destroyed. No user code or failure point lies
    // between the three transfers.
    if (inputs.size() != 2 || instruction.parameterTypes.size() != 2 ||
        instruction.parameterTypes[0].arguments.size() != 1)
      throw std::logic_error("LLVM restoring swap lost its exact places");
    const auto &type = instruction.parameterTypes[0].arguments.front();
    auto *slot = held(type);
    transfer(slot, inputs[0], type);
    transfer(inputs[0], inputs[1], type);
    transfer(inputs[1], slot, type);
    return {true};
  }
  case IntrinsicKind::RestoringIndexedSwap: {
    if (inputs.size() != 3 || instruction.parameterTypes.size() != 3 ||
        instruction.parameterTypes[0].arguments.size() != 1)
      throw std::logic_error("LLVM indexed swap lost its exact storage");
    const auto &storage = instruction.parameterTypes[0].arguments.front();
    if (storage.kind != SemanticType::Array ||
        storage.arguments.size() != 1 || !inputs[1]->getType()->isIntegerTy(64) ||
        !inputs[2]->getType()->isIntegerTy(64))
      throw std::logic_error("LLVM indexed swap lost its fixed array");
    const auto &element = storage.arguments.front();
    auto *extent = ir.getInt64(storage.arrayLength);
    guard(ir.CreateOr(ir.CreateICmpUGE(inputs[1], extent),
                      ir.CreateICmpUGE(inputs[2], extent)),
          {DefinedFailureCode::IndexOutOfBounds,
           DefinedFailureDetail::PrivateStorage});
    // Exchanging an element with itself changes nothing. Skip the transfers
    // rather than relocate a place onto itself.
    auto *function = ir.GetInsertBlock()->getParent();
    auto *exchange =
        llvm::BasicBlock::Create(session.context(), "swap.distinct", function);
    auto *done =
        llvm::BasicBlock::Create(session.context(), "swap.done", function);
    ir.CreateCondBr(ir.CreateICmpEQ(inputs[1], inputs[2]), done, exchange);
    ir.SetInsertPoint(exchange);
    auto *array = session.layouts().get(storage).storage;
    auto *left = ir.CreateInBoundsGEP(array, inputs[0],
                                      {ir.getInt64(0), inputs[1]});
    auto *right = ir.CreateInBoundsGEP(array, inputs[0],
                                       {ir.getInt64(0), inputs[2]});
    auto *slot = held(element);
    transfer(slot, left, element);
    transfer(left, right, element);
    transfer(right, slot, element);
    ir.CreateBr(done);
    ir.SetInsertPoint(done);
    return {true};
  }
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
