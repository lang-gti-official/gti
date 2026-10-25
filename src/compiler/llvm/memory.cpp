#include "memory.h"

#include "../mir_verification_queries.h"

#include <llvm/Support/Alignment.h>

namespace lang::native_llvm {

Memory::Memory(Session &session, llvm::IRBuilder<> &ir, Lifetimes &lifetimes,
               Scalars::Guard guard)
    : session(session), ir(ir), lifetimes(lifetimes), guard(std::move(guard)) {}

bool Memory::handles(IntrinsicKind intrinsic) {
  switch (intrinsic) {
  case IntrinsicKind::ObjectSize:
  case IntrinsicKind::ObjectAlignment:
  case IntrinsicKind::ObjectDestroy:
  case IntrinsicKind::ObjectRelocate:
  case IntrinsicKind::ObjectRelocateRange:
  case IntrinsicKind::StorageBoundsCheck:
  case IntrinsicKind::RequireAllocation:
  case IntrinsicKind::RequireVacantStorage:
  case IntrinsicKind::RequireLiveStorage:
  case IntrinsicKind::RequireRelocationCapacity:
  case IntrinsicKind::RequireRelocationSource:
  case IntrinsicKind::RequireRelocationDestination:
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
    lifetimes.moved(ir, inputs[1], type);
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
        ir.CreateCall(lifetimes.cleanup(*instruction.elementCleanup),
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
  case IntrinsicKind::RequireAllocation:
  case IntrinsicKind::RequireVacantStorage:
  case IntrinsicKind::RequireLiveStorage:
  case IntrinsicKind::RequireRelocationCapacity:
  case IntrinsicKind::RequireRelocationSource:
  case IntrinsicKind::RequireRelocationDestination: {
    const auto outcome = intrinsicBooleanFailureOutcome(instruction.intrinsic);
    if (!outcome || inputs.size() != 1 || !inputs[0]->getType()->isIntegerTy(1))
      throw std::logic_error(
          "LLVM slot check lost its source-computed condition");
    guard(ir.CreateNot(inputs[0]), *outcome);
    return {true};
  }
  default:
    return {};
  }
}

} // namespace lang::native_llvm
