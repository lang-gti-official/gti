#include "atomics.h"

#include "layout.h"
#include "runtime_abi.h"

#include <algorithm>
#include <llvm/Support/Alignment.h>

namespace lang::native_llvm {
namespace {

bool scalarOperand(const MirBody &body, const MirOperand &operand,
                   const SemanticType &type) {
  if (operand.type != type || operand.loan)
    return false;
  if (operand.kind == MirOperandKind::Copy ||
      operand.kind == MirOperandKind::Move) {
    const auto *place = body.findPlace(operand.place);
    return operand.place && !operand.value && !operand.literal && place &&
           place->type == type;
  }
  if (operand.place)
    return false;
  if (operand.kind == MirOperandKind::Constant)
    return operand.literal && !operand.value;
  const auto *value = body.findValue(operand.value);
  return operand.kind == MirOperandKind::Value && operand.value &&
         !operand.literal && value && value->info.type == type;
}

bool referenceOperand(const MirBody &body, const MirOperand &operand,
                      const SemanticType &referent, AccessMode access) {
  const auto *place = body.findPlace(operand.place);
  return operand.type == SemanticType::referenceTo(referent, access) &&
         operand.kind == (access == AccessMode::Mutable
                              ? MirOperandKind::BorrowWrite
                              : MirOperandKind::BorrowRead) &&
         operand.place && !operand.value && !operand.loan && !operand.literal &&
         place && place->type == referent &&
         (access == AccessMode::ReadOnly || place->access == access);
}

bool waiting(IntrinsicKind intrinsic) {
  return intrinsic == IntrinsicKind::AtomicWait ||
         intrinsic == IntrinsicKind::AtomicNotifyOne ||
         intrinsic == IntrinsicKind::AtomicNotifyAll;
}

llvm::AtomicOrdering ordering(AtomicMemoryOrder order) {
  switch (order) {
  case AtomicMemoryOrder::Relaxed:
    return llvm::AtomicOrdering::Monotonic;
  case AtomicMemoryOrder::Acquire:
    return llvm::AtomicOrdering::Acquire;
  case AtomicMemoryOrder::Release:
    return llvm::AtomicOrdering::Release;
  case AtomicMemoryOrder::AcquireRelease:
    return llvm::AtomicOrdering::AcquireRelease;
  case AtomicMemoryOrder::SequentiallyConsistent:
    return llvm::AtomicOrdering::SequentiallyConsistent;
  case AtomicMemoryOrder::Count:
    break;
  }
  throw std::logic_error("LLVM atomic operation lost its verified order");
}

} // namespace

bool nativeAtomicScalar(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Bool:
  case SemanticType::Int8:
  case SemanticType::Int16:
  case SemanticType::Int32:
  case SemanticType::Int64:
  case SemanticType::UInt8:
  case SemanticType::UInt16:
  case SemanticType::UInt32:
  case SemanticType::UInt64:
    return type.arguments.empty();
  default:
    return false;
  }
}

std::optional<SemanticType> nativeAtomicElement(const MirProgram &program,
                                                const SemanticType &storage) {
  if (storage.kind != SemanticType::Class || storage.arguments.size() != 1 ||
      !nativeAtomicScalar(storage.arguments[0]))
    return {};
  const MirClassInstance *owner = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == storage) {
      if (owner)
        return {};
      owner = &candidate;
    }
  if (!owner ||
      owner->compilerCapability != CompilerCapabilityTypeKind::AtomicStorage ||
      owner->polymorphic || owner->abstract || owner->cOpaqueHandle ||
      owner->cAbiRecord || owner->cAbiLayout || owner->unionLayout ||
      owner->destructor || !owner->fields.empty() ||
      !owner->fieldDropOrder.empty() || !owner->bases.empty() ||
      !owner->structuralBases.empty() || !owner->declaredFields.empty() ||
      owner->requiresActiveCleanup || owner->requiresActiveDropState)
    return {};
  return storage.arguments[0];
}

bool Atomics::handles(IntrinsicKind intrinsic) {
  return intrinsic == IntrinsicKind::AtomicLoad ||
         intrinsic == IntrinsicKind::AtomicStore ||
         intrinsic == IntrinsicKind::AtomicExchange ||
         intrinsic == IntrinsicKind::AtomicCompareExchange ||
         waiting(intrinsic);
}

namespace {
std::optional<SemanticType>
atomicCallElement(const MirProgram &program, const MirBody &body,
                  const MirInstruction &instruction) {
  if (program.target().executionProfile != ExecutionProfile::Concurrent ||
      !program.target().runtimeCapabilities.threads ||
      !Atomics::handles(instruction.intrinsic) ||
      instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.nativeCalleeType || instruction.nativeCallbackAdapter ||
      instruction.threadTaskAdapter || !instruction.scopedThreadTasks.empty() ||
      instruction.loan || instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.lifecycle.empty() || instruction.tracksCaller ||
      instruction.neverReturns || !instruction.callableArguments.empty() ||
      instruction.callableInvocation || instruction.callableBoundary ||
      instruction.functionPointerFunctionTarget ||
      instruction.functionPointerLambdaTarget ||
      instruction.dispatch != CallDispatch::Static ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.parameterTypes.size() != instruction.operands.size() ||
      instruction.operands.empty() ||
      !validSynchronizationOperation(instruction.synchronization))
    return {};
  const auto &storage = instruction.operands[0].type;
  if (storage.kind != SemanticType::Reference || storage.arguments.size() != 1)
    return {};
  const auto element = nativeAtomicElement(program, storage.arguments[0]);
  if (!element || !referenceOperand(body, instruction.operands[0],
                                    storage.arguments[0], AccessMode::ReadOnly))
    return {};
  return element;
}
} // namespace

bool nativeAtomicCall(const MirProgram &program, const MirBody &body,
                      const MirInstruction &instruction) {
  if (waiting(instruction.intrinsic))
    return false;
  const auto element = atomicCallElement(program, body, instruction);
  if (!element)
    return false;
  const bool store = instruction.intrinsic == IntrinsicKind::AtomicStore;
  const bool compare =
      instruction.intrinsic == IntrinsicKind::AtomicCompareExchange;
  const auto arity = compare                                              ? 4U
                     : instruction.intrinsic == IntrinsicKind::AtomicLoad ? 1U
                                                                          : 2U;
  const auto expectedKind =
      compare ? SynchronizationOperationKind::AtomicCompareExchange
      : store ? SynchronizationOperationKind::AtomicStore
      : instruction.intrinsic == IntrinsicKind::AtomicExchange
          ? SynchronizationOperationKind::AtomicReadModifyWrite
          : SynchronizationOperationKind::AtomicLoad;
  if (instruction.synchronization.kind != expectedKind ||
      instruction.info.type != (store     ? SemanticType::Void
                                : compare ? SemanticType::Bool
                                          : *element) ||
      instruction.result.has_value() == store ||
      instruction.operands.size() != arity)
    return false;
  for (std::size_t index = 0; index < instruction.operands.size(); ++index)
    if (instruction.parameterTypes[index] != instruction.operands[index].type)
      return false;
  for (std::size_t index = 1; index < instruction.operands.size(); ++index) {
    if (compare && index == 3) {
      if (!referenceOperand(body, instruction.operands[index], *element,
                            AccessMode::Mutable))
        return false;
    } else if (!scalarOperand(body, instruction.operands[index], *element)) {
      return false;
    }
  }
  return true;
}

bool nativeAtomicWaitingCall(const MirProgram &program, const MirBody &body,
                             const MirInstruction &instruction) {
  if (!waiting(instruction.intrinsic))
    return false;
  const auto element = atomicCallElement(program, body, instruction);
  const bool wait = instruction.intrinsic == IntrinsicKind::AtomicWait;
  const auto &target = program.target();
  if (!element || program.programKind() != ProgramKind::Executable ||
      !((target.os == "macos" && target.arch == "arm64") ||
        (target.os == "linux" && target.arch == "x86_64")) ||
      instruction.info.type != SemanticType::Void || instruction.result ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      instruction.borrowArgument ||
      instruction.borrowAccess != AccessMode::ReadOnly ||
      instruction.operands.size() != (wait ? 2U : 1U) ||
      instruction.synchronization.kind !=
          (wait ? SynchronizationOperationKind::AtomicWait
                : SynchronizationOperationKind::AtomicNotify))
    return false;
  for (std::size_t index = 0; index < instruction.operands.size(); ++index)
    if (instruction.parameterTypes[index] != instruction.operands[index].type)
      return false;
  if (!wait)
    return true;
  const auto &old = instruction.operands[1];
  const auto *value = body.findValue(old.value);
  return old.kind == MirOperandKind::Value && old.value && !old.place &&
         !old.loan && !old.literal && old.type == *element && value &&
         value->info.type == *element;
}

std::optional<SemanticType>
nativeAtomicConstructor(const MirProgram &program,
                        const MirConstructorInstance &constructor) {
  const auto *owner = program.findClassInstance(constructor.owner);
  const auto element =
      owner ? nativeAtomicElement(program, owner->type) : std::nullopt;
  const auto &body = constructor.body;
  if (!element ||
      program.target().executionProfile != ExecutionProfile::Concurrent ||
      !program.target().runtimeCapabilities.threads ||
      constructor.definitionKind != MirDefinitionKind::Source ||
      constructor.parameterTypes != std::vector<SemanticType>{*element} ||
      constructor.parameterBindings.size() != 1 ||
      !constructor.parameterBindings[0] || !constructor.initializers.empty() ||
      !constructor.typeArguments.empty() ||
      !constructor.valueArguments.empty() || constructor.requiresUnsafe ||
      constructor.effectSummary.receiverEscapes ||
      std::any_of(constructor.effectSummary.argumentsEscape.begin(),
                  constructor.effectSummary.argumentsEscape.end(),
                  [](bool escapes) { return escapes; }) ||
      body.blocks.size() != 1 || body.blocks[0].id != body.entry ||
      !body.blocks[0].instructions.empty() ||
      body.blocks[0].terminator.kind != MirTerminatorKind::Return ||
      body.blocks[0].terminator.value || !body.values.empty() ||
      !body.loans.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.fullExpressions.empty())
    return {};
  auto effects = constructor.effectSummary.traits;
  effects.speculatable = effects.removableWhenUnused = effects.reorderable =
      false;
  if (effects != MirEffectTraits{})
    return {};
  return element;
}

Atomics::Atomics(Session &session, llvm::IRBuilder<> &ir)
    : session(session), ir(ir) {}

llvm::Value *Atomics::lane(llvm::Value *input, const SemanticType &element) {
  return element.kind == SemanticType::Bool
             ? ir.CreateZExt(input, ir.getInt8Ty())
             : input;
}

llvm::Value *Atomics::value(llvm::Value *input, const SemanticType &element) {
  return element.kind == SemanticType::Bool
             ? ir.CreateTrunc(input, ir.getInt1Ty())
             : input;
}

llvm::Value *Atomics::emit(const MirBody &body,
                           const MirInstruction &instruction,
                           std::span<llvm::Value *const> inputs) {
  if ((!nativeAtomicCall(session.program().mir(), body, instruction) &&
       !nativeAtomicWaitingCall(session.program().mir(), body, instruction)) ||
      inputs.size() != instruction.operands.size())
    throw BackendUnsupportedError(
        "LLVM atomic call lost its exact staged plan");
  const auto layout = session.layouts().atomicStorage(
      instruction.operands[0].type.arguments[0]);
  const auto scalar = session.layouts().get(layout.element);
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const bool address =
        index == 0 ||
        (instruction.intrinsic == IntrinsicKind::AtomicCompareExchange &&
         index == 3);
    if (!inputs[index] ||
        inputs[index]->getType() !=
            (address ? session.layouts().pointer() : scalar.value))
      throw BackendUnsupportedError(
          "LLVM atomic input lost its exact materialized scalar/address lane");
  }
  if (!session.supportsNativeAtomic(*ir.GetInsertBlock()->getParent(),
                                    layout.lane->getBitWidth(),
                                    layout.object.alignment))
    throw BackendUnsupportedError(
        "LLVM target atomic width/alignment requires an unavailable runtime");
  if (waiting(instruction.intrinsic)) {
    const auto width = ir.getInt32(layout.lane->getBitWidth() / 8);
    if (instruction.intrinsic == IntrinsicKind::AtomicWait) {
      const auto order = *instruction.synchronization.order;
      const auto encoded = order == AtomicMemoryOrder::Relaxed   ? 0U
                           : order == AtomicMemoryOrder::Acquire ? 1U
                                                                 : 2U;
      // Preserve the same-width bit pattern for signed lanes; bool first gains
      // its canonical byte representation. Neither conversion is numeric.
      auto *old = ir.CreateZExtOrTrunc(lane(inputs[1], layout.element),
                                       ir.getInt64Ty());
      ir.CreateCall(declareRuntimeService(session, RuntimeService::AtomicWait),
                    {inputs[0], old, width, ir.getInt32(encoded)});
    } else {
      auto *bucketType =
          llvm::StructType::get(ir.getInt32Ty(), ir.getInt32Ty(),
                                llvm::ArrayType::get(ir.getInt8Ty(), 120));
      auto *domainType = llvm::ArrayType::get(bucketType, 256);
      if (session.module().getDataLayout().getTypeAllocSize(bucketType) != 128)
        throw BackendUnsupportedError(
            "LLVM wait domain lost its v1 bucket stride");
      constexpr auto name = "gti_rt_atomic_wait_domain_v1";
      auto *domain = session.module().getNamedGlobal(name);
      if (session.module().getNamedValue(name) &&
          (!domain || domain->getValueType() != domainType ||
           domain->isConstant() || !domain->isDeclaration() ||
           !domain->hasExternalLinkage() || domain->isThreadLocal() ||
           domain->getAddressSpace() != 0 ||
           domain->getAlign().valueOrOne() < llvm::Align(64)))
        throw BackendUnsupportedError(
            "LLVM wait domain conflicts with its exact runtime ABI");
      if (!domain) {
        domain = new llvm::GlobalVariable(session.module(), domainType, false,
                                          llvm::GlobalValue::ExternalLinkage,
                                          nullptr, name);
        domain->setAlignment(llvm::Align(64));
      }
      auto *index = ir.CreateAnd(
          ir.CreateXor(
              ir.CreateLShr(ir.CreatePtrToInt(inputs[0], ir.getInt64Ty()), 3),
              ir.getInt64(layout.lane->getBitWidth() / 8)),
          ir.getInt64(255));
      const bool x86 = session.program().mir().target().arch == "x86_64";
      auto *metadata = ir.CreateInBoundsGEP(
          domainType, domain,
          {ir.getInt64(0), index, ir.getInt32(x86 ? 0 : 1)});
      llvm::Value *registered;
      if (x86) {
        auto *old = ir.CreateAtomicRMW(
            llvm::AtomicRMWInst::And, metadata, ir.getInt32(~UINT32_C(1)),
            llvm::Align(4), llvm::AtomicOrdering::AcquireRelease,
            llvm::SyncScope::System);
        registered =
            ir.CreateICmpNE(ir.CreateAnd(old, ir.getInt32(1)), ir.getInt32(0));
      } else {
        ir.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent,
                       llvm::SyncScope::System);
        auto *count =
            ir.CreateAlignedLoad(ir.getInt32Ty(), metadata, llvm::Align(4));
        count->setAtomic(llvm::AtomicOrdering::Monotonic,
                         llvm::SyncScope::System);
        registered = ir.CreateICmpNE(count, ir.getInt32(0));
      }
      auto *function = ir.GetInsertBlock()->getParent();
      auto *notify = llvm::BasicBlock::Create(session.context(),
                                              "notify.registered", function);
      auto *done =
          llvm::BasicBlock::Create(session.context(), "notify.done", function);
      ir.CreateCondBr(registered, notify, done);
      ir.SetInsertPoint(notify);
      ir.CreateCall(declareRuntimeService(
                        session, RuntimeService::AtomicNotifyRegistered),
                    {inputs[0], width,
                     ir.getInt32(instruction.intrinsic ==
                                 IntrinsicKind::AtomicNotifyAll)});
      ir.CreateBr(done);
      ir.SetInsertPoint(done);
    }
    return nullptr;
  }
  const auto order = ordering(*instruction.synchronization.order);
  const llvm::Align alignment(layout.object.alignment);
  switch (instruction.intrinsic) {
  case IntrinsicKind::AtomicLoad: {
    auto *loaded = ir.CreateAlignedLoad(layout.lane, inputs[0], alignment);
    loaded->setAtomic(order, llvm::SyncScope::System);
    return value(loaded, layout.element);
  }
  case IntrinsicKind::AtomicStore: {
    auto *stored = ir.CreateAlignedStore(lane(inputs[1], layout.element),
                                         inputs[0], alignment);
    stored->setAtomic(order, llvm::SyncScope::System);
    return nullptr;
  }
  case IntrinsicKind::AtomicExchange:
    return value(ir.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, inputs[0],
                                    lane(inputs[1], layout.element), alignment,
                                    order, llvm::SyncScope::System),
                 layout.element);
  case IntrinsicKind::AtomicCompareExchange: {
    auto *exchange = ir.CreateAtomicCmpXchg(
        inputs[0], lane(inputs[1], layout.element),
        lane(inputs[2], layout.element), alignment, order,
        ordering(*instruction.synchronization.failureOrder),
        llvm::SyncScope::System);
    exchange->setWeak(false);
    ir.CreateAlignedStore(
        lane(value(ir.CreateExtractValue(exchange, 0), layout.element),
             layout.element),
        inputs[3],
        llvm::Align(session.layouts().get(layout.element).alignment));
    return ir.CreateExtractValue(exchange, 1);
  }
  default:
    throw std::logic_error("LLVM atomic dispatch lost its operation");
  }
}

void Atomics::initialize(const MirConstructorInstance &constructor,
                         llvm::Value *destination, llvm::Value *input) {
  const auto element =
      nativeAtomicConstructor(session.program().mir(), constructor);
  if (!element)
    throw BackendUnsupportedError(
        "LLVM atomic construction lost its exact owner");
  const auto &owner =
      *session.program().mir().findClassInstance(constructor.owner);
  const auto layout = session.layouts().atomicStorage(owner.type);
  // This is exclusive construction before publication, not an ordinary access
  // to shared atomic storage. No extra state or implicit notification is added.
  ir.CreateAlignedStore(lane(input, *element), destination,
                        llvm::Align(layout.object.alignment));
}

} // namespace lang::native_llvm
