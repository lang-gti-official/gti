#include "thread_locals.h"
#include "runtime_abi.h"

namespace lang::native_llvm {

ThreadLocals::ThreadLocals(Session &session, Failures &failures,
                           Lifetimes &lifetimes, FindBody findBody,
                           BodyAbi bodyAbi)
    : session(session), failures(failures), lifetimes(lifetimes),
      findBody(std::move(findBody)), bodyAbi(std::move(bodyAbi)) {
  auto *pointer = session.layouts().pointer();
  registrationType =
      llvm::StructType::create(session.context(),
                               {pointer, pointer, pointer, pointer,
                                llvm::Type::getInt8Ty(session.context())},
                               "gti_thread_local_cleanup_entry_v1");
}

void ThreadLocals::allocate(
    std::map<SymbolId, llvm::GlobalVariable *> &globals) {
  llvm::IRBuilder<> ir(session.context());
  Scalars scalars(session, ir, [](llvm::Value *, DefinedFailureOutcome) {
    throw std::logic_error("LLVM passive TLS cannot perform runtime checking");
  });
  for (const auto &storage : session.program().mir().threadLocalStorage()) {
    const auto datum = [&](llvm::Type *type, llvm::Constant *initial,
                           const char *suffix) {
      return new llvm::GlobalVariable(
          session.module(), type, false, llvm::GlobalValue::InternalLinkage,
          initial,
          "__gti_native_tls_" + std::to_string(storage.symbol) + suffix,
          nullptr, llvm::GlobalVariable::GeneralDynamicTLSModel);
    };
    auto *type = session.layouts().get(storage.type).storage;
    Slot slot;
    llvm::Constant *initial = llvm::Constant::getNullValue(type);
    if (storage.dataInitializer) {
      initial = llvm::cast<llvm::Constant>(
          scalars.literal(*storage.dataInitializer, storage.type));
      if (storage.type == SemanticType::Bool)
        initial = llvm::ConstantInt::get(
            type, llvm::cast<llvm::ConstantInt>(initial)->getZExtValue());
    }
    slot.storage = datum(type, initial, "_value");
    if (!storage.dataInitializer) {
      slot.state = datum(ir.getInt8Ty(), ir.getInt8(0), "_state");
      slot.registration = datum(registrationType,
                                llvm::Constant::getNullValue(registrationType),
                                "_registration");
    }
    globals.emplace(storage.symbol, slot.storage);
    slots.emplace(storage.symbol, slot);
  }
}

llvm::Function *ThreadLocals::cleanup(const MirThreadLocalStorage &storage,
                                      Slot &slot) {
  if (slot.cleanup)
    return slot.cleanup;
  auto *pointer = session.layouts().pointer();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt1Ty(session.context()),
                              {pointer, pointer, pointer, pointer}, false),
      llvm::GlobalValue::InternalLinkage,
      "__gti_native_tls_cleanup_" + std::to_string(storage.symbol),
      session.module());
  function->addRetAttr(llvm::Attribute::ZExt);
  slot.cleanup = function;
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  // Closing is permanent, even for a passive destructor-free dynamic slot.
  ir.CreateStore(ir.getInt8(3), slot.state);
  if (!storage.requiresActiveCleanup) {
    ir.CreateRet(ir.getTrue());
    return function;
  }
  auto *channel = ir.CreateAlloca(session.abi().failureChannel());
  auto *entry = ir.GetInsertBlock();
  auto *inherit =
      llvm::BasicBlock::Create(session.context(), "primary.enter", function);
  auto *invoke =
      llvm::BasicBlock::Create(session.context(), "cleanup", function);
  auto *hasPrimary = ir.CreateIsNotNull(function->getArg(2));
  ir.CreateCondBr(hasPrimary, inherit, invoke);
  ir.SetInsertPoint(inherit);
  // The registry owns this borrowed record and its possibly distinct artifact.
  // An existing outer cleanup context remains authoritative.
  auto *token = ir.CreateCall(
      declareRuntimeService(session, RuntimeService::EnterFailureCleanup),
      {function->getArg(2), function->getArg(3)});
  ir.CreateBr(invoke);
  ir.SetInsertPoint(invoke);
  auto *cleanupToken = ir.CreatePHI(pointer, 2);
  cleanupToken->addIncoming(llvm::ConstantPointerNull::get(pointer), entry);
  cleanupToken->addIncoming(token, inherit);
  auto *status = ir.CreateCall(
      lifetimes.cleanup(storage.cleanup),
      {function->getArg(0), channel, hasPrimary});
  failures.leaveCleanup(ir, cleanupToken);
  auto *success =
      llvm::BasicBlock::Create(session.context(), "success", function);
  auto *failed =
      llvm::BasicBlock::Create(session.context(), "failed", function);
  ir.CreateCondBr(status, success, failed);
  ir.SetInsertPoint(failed);
  ir.CreateStore(ir.CreateLoad(session.abi().failureRecord(),
                               failures.record(ir, channel)),
                 function->getArg(1));
  ir.CreateRet(ir.getFalse());
  ir.SetInsertPoint(success);
  ir.CreateRet(ir.getTrue());
  return function;
}

void ThreadLocals::ensure(llvm::IRBuilder<> &ir, SymbolId symbol,
                          llvm::Value *failure, Scalars::Guard guard,
                          std::function<void()> propagate) {
  const auto &storage = *session.program().mir().findThreadLocalStorage(symbol);
  if (storage.dataInitializer)
    return;
  auto &slot = slots.at(symbol);
  auto *function = ir.GetInsertBlock()->getParent();
  auto *slow =
      llvm::BasicBlock::Create(session.context(), "tls.cold", function);
  auto *done =
      llvm::BasicBlock::Create(session.context(), "tls.ready", function);
  auto *state = ir.CreateLoad(ir.getInt8Ty(), slot.state);
  ir.CreateCondBr(ir.CreateICmpEQ(state, ir.getInt8(2)), done, slow);
  ir.SetInsertPoint(slow);
  const auto check = [&](llvm::Value *condition, DefinedFailureDetail detail) {
    guard(condition,
          {DefinedFailureCode::HostedRuntimeContractFailure, detail});
  };
  check(ir.CreateICmpEQ(state, ir.getInt8(1)),
        DefinedFailureDetail::RecursiveThreadLocalInitialization);
  check(ir.CreateICmpNE(ir.CreateCall(declareRuntimeService(
                            session, RuntimeService::ThreadLocalsClosing)),
                        ir.getInt32(0)),
        DefinedFailureDetail::ThreadLocalInitializationDuringShutdown);
  check(ir.CreateICmpEQ(ir.CreateCall(declareRuntimeService(
                            session, RuntimeService::ThreadLocalsAttached)),
                        ir.getInt32(0)),
        DefinedFailureDetail::ThreadLocalOutsideAttachedLifetime);
  ir.CreateStore(ir.getInt8(1), slot.state);
  const MirBodyAddress address{MirBodyKind::ThreadLocalInitializer, symbol};
  const auto abi = bodyAbi(address);
  std::vector<llvm::Value *> arguments;
  if (abi.resultDestination)
    arguments.push_back(slot.storage);
  if (abi.failureDestination)
    arguments.push_back(failure);
  auto *result = ir.CreateCall(findBody(address), arguments);
  abi.applyTo(*result);
  if (abi.statusResult) {
    auto *success = llvm::BasicBlock::Create(session.context(),
                                             "tls.initialized", function);
    auto *failed =
        llvm::BasicBlock::Create(session.context(), "tls.failed", function);
    ir.CreateCondBr(result, success, failed);
    ir.SetInsertPoint(failed);
    ir.CreateStore(ir.getInt8(0), slot.state);
    // The owning Body routes failure through its exact MIR Invoke edge.
    propagate();
    ir.SetInsertPoint(success);
  }
  if (!abi.resultDestination && !abi.statusResult)
    ir.CreateStore(result, slot.storage);
  ir.CreateStore(ir.getInt8(2), slot.state);
  ir.CreateStore(slot.storage,
                 ir.CreateStructGEP(registrationType, slot.registration, 1));
  ir.CreateStore(cleanup(storage, slot),
                 ir.CreateStructGEP(registrationType, slot.registration, 2));
  ir.CreateStore(failures.artifact(),
                 ir.CreateStructGEP(registrationType, slot.registration, 3));
  ir.CreateCall(
      declareRuntimeService(session, RuntimeService::RegisterThreadLocal),
      {slot.registration});
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
}

} // namespace lang::native_llvm
