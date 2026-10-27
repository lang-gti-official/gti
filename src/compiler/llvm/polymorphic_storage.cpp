#include "polymorphic_storage.h"

#include <algorithm>
#include <llvm/IR/Intrinsics.h>

namespace lang::native_llvm {
namespace {
void requireInsertion(Session &session, const llvm::IRBuilder<> &ir) {
  if (!ir.GetInsertBlock() || !ir.GetInsertBlock()->getParent() ||
      ir.GetInsertBlock()->getParent()->getParent() != &session.module())
    throw std::invalid_argument(
        "LLVM slot operation uses a foreign IR builder");
}
} // namespace

PolymorphicStorage::PolymorphicStorage(Session &session, Lifetimes &lifetimes,
                                       Dispatch &dispatch)
    : session(session), lifetimes(lifetimes), dispatch(dispatch) {
  if (!lifetimes.boundTo(session) || !dispatch.boundTo(session))
    throw std::invalid_argument(
        "LLVM polymorphic storage uses a foreign session");
}

bool PolymorphicStorage::handles(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::PolymorphicStorageHasValue:
  case IntrinsicKind::PolymorphicStorageConstruct:
  case IntrinsicKind::PolymorphicStorageRead:
  case IntrinsicKind::PolymorphicStorageReadMut:
  case IntrinsicKind::PolymorphicStorageReset:
    return true;
  default:
    return false;
  }
}

void PolymorphicStorage::preflight(const MirBody &body,
                                   const MirInstruction &instruction) {
  const auto &program = session.program().mir();
  const auto addresses = enumerateMirBodyAddresses(program);
  const bool ownedBody =
      std::any_of(addresses.begin(), addresses.end(), [&](const auto address) {
        return findMirBody(program, address) == &body;
      });
  const bool ownedInstruction = std::any_of(
      body.blocks.begin(), body.blocks.end(), [&](const auto &block) {
        return std::any_of(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &row) { return &row == &instruction; });
      });
  // The session's verified program already owns the exact intrinsic lifecycle
  // and loan contract. A detached instruction copy is not that proof.
  if (!ownedBody || !ownedInstruction || instruction.tombstone ||
      instruction.kind != MirInstructionKind::Call ||
      !handles(instruction.intrinsic) || instruction.operands.empty())
    throw std::invalid_argument(
        "LLVM slot operation requires its sealed MIR instruction");
  const auto &slot = instruction.operands.front().type;
  static_cast<void>(session.layouts().polymorphicStorage(slot));
  if (instruction.intrinsic == IntrinsicKind::PolymorphicStorageConstruct) {
    const auto &model = instruction.operands.at(1).type;
    Lifetimes::preflightInertRelocation(session, dispatch, model);
    static_cast<void>(
        session.layouts().baseSubobject(model, slot.arguments.at(0),
                                        "polymorphicStorage.slotModel"));
  }
}

void PolymorphicStorage::preflightDefaultConstructor(
    const MirConstructorInstance &constructor) {
  const auto &program = session.program().mir();
  const auto *owner = program.findClassInstance(constructor.owner);
  if (program.findConstructorInstance(constructor.id) != &constructor ||
      !owner || constructor.definitionKind != MirDefinitionKind::Source ||
      constructor.generatedDefault || !constructor.parameterTypes.empty() ||
      !constructor.parameterBindings.empty() ||
      !constructor.initializers.empty() || constructor.dispatchState ||
      constructor.body.kind != MirBodyKind::Constructor ||
      findMirBody(program, {MirBodyKind::Constructor, constructor.id}) !=
          &constructor.body)
    throw std::invalid_argument(
        "LLVM empty slot requires its sealed source default constructor");
  static_cast<void>(session.layouts().polymorphicStorage(owner->type));
}

void PolymorphicStorage::initializeDefaultConstructor(
    llvm::IRBuilder<> &ir, const MirConstructorInstance &constructor,
    llvm::Value *receiver) {
  preflightDefaultConstructor(constructor);
  initialize(ir,
             session.program().mir().findClassInstance(constructor.owner)->type,
             receiver);
}

llvm::StructType *PolymorphicStorage::descriptorType() {
  return llvm::StructType::get(
      session.context(),
      {session.layouts().pointer(),
       session.dataLayout().getIntPtrType(session.context())});
}

llvm::GlobalVariable *
PolymorphicStorage::descriptor(const SemanticType &slot,
                               const SemanticType &model) {
  const auto &interface = slot.arguments.at(0);
  for (const auto &entry : descriptors)
    if (entry.model == model && entry.interface == interface)
      return entry.value;
  const auto offset = session.layouts().baseSubobject(model, interface,
                                             "polymorphicStorage.interface")
                            .offset;
  auto *relocate = lifetimes.inertRelocation(model);
  auto *type = descriptorType();
  auto *value = new llvm::GlobalVariable(
      session.module(), type, true, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantStruct::get(
          type,
          {relocate,
           llvm::ConstantInt::get(
               session.dataLayout().getIntPtrType(session.context()), offset)}),
      "__gti_inline_model_" +
          std::to_string(session.layouts().instance(model).id) + "_as_" +
          std::to_string(session.layouts().instance(interface).id));
  descriptors.push_back({model, interface, value});
  return value;
}

llvm::Value *
PolymorphicStorage::hasValue(llvm::IRBuilder<> &ir,
                             const PolymorphicStorageLayout &layout,
                             llvm::Value *address) {
  return ir.CreateIsNotNull(
      ir.CreateLoad(session.layouts().pointer(),
                    ir.CreateStructGEP(layout.type.storage, address,
                                       layout.operationsIndex)));
}

void PolymorphicStorage::guard(llvm::IRBuilder<> &ir, llvm::Value *condition) {
  auto *function = ir.GetInsertBlock()->getParent();
  auto *ready =
      llvm::BasicBlock::Create(session.context(), "slot.ready", function);
  auto *invalid =
      llvm::BasicBlock::Create(session.context(), "slot.invalid", function);
  ir.CreateCondBr(condition, ready, invalid);
  ir.SetInsertPoint(invalid);
  ir.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>{}, {});
  ir.CreateUnreachable();
  ir.SetInsertPoint(ready);
}

void PolymorphicStorage::initialize(llvm::IRBuilder<> &ir,
                                    const SemanticType &type,
                                    llvm::Value *destination) {
  requireInsertion(session, ir);
  const auto layout = session.layouts().polymorphicStorage(type);
  if (!destination || destination->getType() != session.layouts().pointer())
    throw std::invalid_argument("LLVM slot initialization requires storage");
  auto *empty = llvm::ConstantPointerNull::get(session.layouts().pointer());
  ir.CreateStore(empty, ir.CreateStructGEP(layout.type.storage, destination,
                                           layout.operationsIndex));
  ir.CreateStore(empty, ir.CreateStructGEP(layout.type.storage, destination,
                                           layout.interfaceIndex));
}

llvm::Value *PolymorphicStorage::emit(llvm::IRBuilder<> &ir,
                                      const MirBody &body,
                                      const MirInstruction &instruction,
                                      std::span<llvm::Value *const> inputs) {
  requireInsertion(session, ir);
  preflight(body, instruction);
  if (inputs.size() != instruction.operands.size() ||
      std::any_of(inputs.begin(), inputs.end(), [&](const auto *input) {
        return !input || input->getType() != session.layouts().pointer();
      }))
    throw std::invalid_argument(
        "LLVM slot operation requires evaluated addresses");
  const auto &type = instruction.operands.front().type;
  const auto layout = session.layouts().polymorphicStorage(type);
  auto *slot = inputs.front();
  switch (instruction.intrinsic) {
  case IntrinsicKind::PolymorphicStorageHasValue:
    return hasValue(ir, layout, slot);
  case IntrinsicKind::PolymorphicStorageReset:
    // The sealed payload family has inert cleanup. Ending its stored lifetime
    // therefore needs no source call, allocation or failure transport.
    initialize(ir, type, slot);
    return nullptr;
  case IntrinsicKind::PolymorphicStorageRead:
  case IntrinsicKind::PolymorphicStorageReadMut:
    guard(ir, hasValue(ir, layout, slot));
    return ir.CreateLoad(
        session.layouts().pointer(),
        ir.CreateStructGEP(layout.type.storage, slot, layout.interfaceIndex));
  case IntrinsicKind::PolymorphicStorageConstruct: {
    const auto &model = instruction.operands[1].type;
    const auto payload = session.layouts().get(model);
    auto *operations = descriptor(type, model);
    guard(ir, ir.CreateAnd(
                  ir.CreateNot(hasValue(ir, layout, slot)),
                  ir.getInt1(payload.size <= layout.capacity &&
                             payload.alignment <= layout.payloadAlignment)));
    auto *destination =
        ir.CreateStructGEP(layout.type.storage, slot, layout.payloadIndex);
    ir.CreateCall(lifetimes.inertRelocation(model), {destination, inputs[1]});
    const auto offset =
        session.layouts().baseSubobject(model, type.arguments[0],
                                        "polymorphicStorage.readModel")
            .offset;
    auto *view = ir.CreateGEP(ir.getInt8Ty(), destination, ir.getInt64(offset));
    ir.CreateStore(view, ir.CreateStructGEP(layout.type.storage, slot,
                                            layout.interfaceIndex));
    // Publish only after all non-failing payload and interface writes finish.
    ir.CreateStore(operations, ir.CreateStructGEP(layout.type.storage, slot,
                                                  layout.operationsIndex));
    return nullptr;
  }
  default:
    throw std::logic_error("LLVM slot operation lost its preflight");
  }
}

void PolymorphicStorage::relocate(llvm::IRBuilder<> &ir,
                                  const SemanticType &type,
                                  llvm::Value *destination,
                                  llvm::Value *source) {
  requireInsertion(session, ir);
  const auto layout = session.layouts().polymorphicStorage(type);
  if (!destination || !source || destination == source ||
      destination->getType() != session.layouts().pointer() ||
      source->getType() != session.layouts().pointer())
    throw std::invalid_argument(
        "LLVM slot relocation requires distinct fresh storage");
  initialize(ir, type, destination);
  auto *function = ir.GetInsertBlock()->getParent();
  auto *live =
      llvm::BasicBlock::Create(session.context(), "slot.move", function);
  auto *done =
      llvm::BasicBlock::Create(session.context(), "slot.done", function);
  ir.CreateCondBr(hasValue(ir, layout, source), live, done);
  ir.SetInsertPoint(live);
  auto *operations = ir.CreateLoad(
      session.layouts().pointer(),
      ir.CreateStructGEP(layout.type.storage, source, layout.operationsIndex));
  auto *relocator =
      ir.CreateLoad(session.layouts().pointer(),
                    ir.CreateStructGEP(descriptorType(), operations, 0));
  auto *offset =
      ir.CreateLoad(session.dataLayout().getIntPtrType(session.context()),
                    ir.CreateStructGEP(descriptorType(), operations, 1));
  auto *payload =
      ir.CreateStructGEP(layout.type.storage, destination, layout.payloadIndex);
  auto *from =
      ir.CreateStructGEP(layout.type.storage, source, layout.payloadIndex);
  auto *call =
      ir.CreateCall(lifetimes.relocationType(), relocator, {payload, from});
  call->setDoesNotThrow();
  ir.CreateStore(ir.CreateGEP(ir.getInt8Ty(), payload, offset),
                 ir.CreateStructGEP(layout.type.storage, destination,
                                    layout.interfaceIndex));
  ir.CreateStore(operations,
                 ir.CreateStructGEP(layout.type.storage, destination,
                                    layout.operationsIndex));
  initialize(ir, type, source);
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
}

} // namespace lang::native_llvm
