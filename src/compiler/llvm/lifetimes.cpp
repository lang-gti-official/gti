#include "lifetimes.h"
#include "memory.h"

namespace lang::native_llvm {

Lifetimes::Lifetimes(Session &session, Failures &failures, FindBody findBody)
    : session(session), failures(failures), findBody(std::move(findBody)) {}

void Lifetimes::active(llvm::IRBuilder<> &ir, llvm::Value *address,
                       const SemanticType &type, bool value) {
  if (type.kind != SemanticType::Class)
    return;
  if (const auto index = session.layouts().activeIndex(type))
    ir.CreateStore(ir.getInt8(value),
                   ir.CreateStructGEP(session.layouts().get(type).storage,
                                      address, *index));
}

llvm::Function *Lifetimes::moveHelper(const SemanticType &type) {
  auto *storage = session.layouts().get(type).storage;
  if (const auto found = moves.find(storage); found != moves.end())
    return found->second;
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()),
                              {session.layouts().pointer()}, false),
      llvm::GlobalValue::InternalLinkage,
      "__gti_native_move_state_" + std::to_string(moves.size()),
      session.module());
  moves.emplace(storage, function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  llvm::Value *address = function->getArg(0);
  if (type.kind == SemanticType::UniqueOwner) {
    ir.CreateStore(llvm::ConstantPointerNull::get(session.layouts().pointer()),
                   address);
  } else if (type.kind == SemanticType::Array && type.arrayLength) {
    auto *entry = ir.GetInsertBlock();
    auto *loop =
        llvm::BasicBlock::Create(session.context(), "elements", function);
    auto *done = llvm::BasicBlock::Create(session.context(), "done", function);
    ir.CreateBr(loop);
    ir.SetInsertPoint(loop);
    auto *index = ir.CreatePHI(ir.getInt64Ty(), 2);
    index->addIncoming(ir.getInt64(0), entry);
    moved(ir, ir.CreateGEP(storage, address, {ir.getInt64(0), index}),
          type.arguments.at(0));
    auto *next = ir.CreateAdd(index, ir.getInt64(1));
    index->addIncoming(next, ir.GetInsertBlock());
    ir.CreateCondBr(ir.CreateICmpULT(next, ir.getInt64(type.arrayLength)), loop,
                    done);
    ir.SetInsertPoint(done);
  } else if (type.kind == SemanticType::Class) {
    const auto &owner = session.layouts().instance(type);
    active(ir, address, type, false);
    for (const auto &base : owner.bases)
      moved(
          ir,
          ir.CreateStructGEP(storage, address,
                             session.layouts().baseIndex(type, base.instance)),
          base.type);
    for (const auto &field : owner.declaredFields)
      moved(
          ir,
          ir.CreateStructGEP(storage, address,
                             session.layouts().fieldIndex(type, field.symbol)),
          field.type);
  }
  ir.CreateRetVoid();
  return function;
}
bool Lifetimes::needsMoveState(const SemanticType &type) const {
  if (type.kind == SemanticType::UniqueOwner)
    return true;
  if (type.kind == SemanticType::Array)
    return needsMoveState(type.arguments.at(0));
  if (type.kind != SemanticType::Class)
    return false;
  const auto &owner = session.layouts().instance(type);
  if (owner.requiresActiveDropState)
    return true;
  for (const auto &field : owner.declaredFields)
    if (needsMoveState(field.type))
      return true;
  for (const auto &base : owner.bases)
    if (needsMoveState(base.type))
      return true;
  return false;
}

void Lifetimes::moved(llvm::IRBuilder<> &ir, llvm::Value *address,
                      const SemanticType &type) {
  if (needsMoveState(type))
    ir.CreateCall(moveHelper(type), {address});
}

llvm::Function *Lifetimes::cleanup(const MirDropType &recipe) {
  for (const auto &[previous, function] : cleanups)
    if (previous == recipe)
      return function;
  if (recipe.recursiveCleanup) {
    for (auto entry = buildingCleanups.rbegin();
         entry != buildingCleanups.rend(); ++entry)
      if (!entry->first.recursiveCleanup && entry->first.type == recipe.type &&
          entry->first.classInstance == recipe.classInstance)
        return entry->second;
    throw std::logic_error(
        "LLVM recursive cleanup lost its exact enclosing recipe");
  }
  if (recipe.polymorphicCleanup || recipe.lambdaInstance ||
      recipe.payloadVariant)
    throw BackendUnsupportedError(
        "LLVM cleanup requires its later ownership/dispatch milestone");
  auto *pointer = session.layouts().pointer();
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(
          llvm::Type::getInt1Ty(session.context()),
          {pointer, pointer, llvm::Type::getInt1Ty(session.context())}, false),
      llvm::GlobalValue::InternalLinkage,
      "__gti_native_cleanup_" + std::to_string(cleanups.size()),
      session.module());
  cleanups.emplace_back(recipe, function);
  buildingCleanups.emplace_back(recipe, function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  llvm::Value *address = function->getArg(0);
  auto *output = function->getArg(1);
  auto *duringFailure = function->getArg(2);
  auto *failed = ir.CreateAlloca(ir.getInt1Ty());
  auto *scratch = ir.CreateAlloca(session.abi().failureChannel());
  ir.CreateStore(ir.getFalse(), failed);
  const bool unique = recipe.type.kind == SemanticType::UniqueOwner;
  llvm::Value *allocation = nullptr;
  if (unique) {
    if (recipe.type.arguments.size() != 1 || recipe.fixedArray ||
        (recipe.uniquePointeeClass &&
         session.layouts().instance(recipe.type.arguments[0]).id !=
             *recipe.uniquePointeeClass))
      throw std::logic_error(
          "LLVM unique cleanup lost its exact pointee recipe");
    allocation = ir.CreateLoad(pointer, address);
    ir.CreateStore(llvm::ConstantPointerNull::get(session.layouts().pointer()),
                   address);
    auto *live =
        llvm::BasicBlock::Create(session.context(), "owner.live", function);
    auto *empty =
        llvm::BasicBlock::Create(session.context(), "owner.empty", function);
    ir.CreateCondBr(ir.CreateIsNull(allocation), empty, live);
    ir.SetInsertPoint(empty);
    ir.CreateRet(ir.getTrue());
    ir.SetInsertPoint(live);
    address = allocation;
  }
  const auto &objectType = unique ? recipe.type.arguments[0] : recipe.type;

  // Save the first normal-cleanup failure, drain remaining children, and stop
  // immediately on a second failure. A caller already unwinding owns its first
  // record, so return the first new failure without replacing that record.
  const auto observe = [&](llvm::Value *status) {
    auto *bad = llvm::BasicBlock::Create(session.context(), "failed", function);
    auto *next = llvm::BasicBlock::Create(session.context(), "next", function);
    ir.CreateCondBr(status, next, bad);
    ir.SetInsertPoint(bad);
    auto *secondary =
        llvm::BasicBlock::Create(session.context(), "secondary", function);
    auto *first =
        llvm::BasicBlock::Create(session.context(), "first", function);
    ir.CreateCondBr(ir.CreateLoad(ir.getInt1Ty(), failed), secondary, first);
    ir.SetInsertPoint(secondary);
    failures.terminateCleanup(ir, output, scratch);
    ir.SetInsertPoint(first);
    ir.CreateStore(ir.CreateLoad(session.abi().failureChannel(), scratch),
                   output);
    auto *resume =
        llvm::BasicBlock::Create(session.context(), "resume", function);
    auto *stop = llvm::BasicBlock::Create(session.context(), "stop", function);
    ir.CreateCondBr(duringFailure, stop, resume);
    ir.SetInsertPoint(stop);
    ir.CreateRet(ir.getFalse());
    ir.SetInsertPoint(resume);
    ir.CreateStore(ir.getTrue(), failed);
    ir.CreateBr(next);
    ir.SetInsertPoint(next);
  };
  const auto child = [&](const MirDropType &selected, llvm::Value *where) {
    observe(ir.CreateCall(
        cleanup(selected),
        {where, scratch,
         ir.CreateOr(duringFailure, ir.CreateLoad(ir.getInt1Ty(), failed))}));
  };
  if (recipe.fixedArray) {
    if (recipe.type.kind != SemanticType::Array ||
        recipe.aggregate.size() > 1 ||
        (recipe.aggregate.empty() && recipe.requiresActiveCleanup))
      throw std::logic_error(
          "LLVM array cleanup lost its sealed element recipe");
    const auto count = recipe.type.arrayLength;
    if (count && !recipe.aggregate.empty()) {
      auto *entry = ir.GetInsertBlock();
      auto *loop =
          llvm::BasicBlock::Create(session.context(), "elements", function);
      auto *done =
          llvm::BasicBlock::Create(session.context(), "done", function);
      ir.CreateBr(loop);
      ir.SetInsertPoint(loop);
      auto *remaining = ir.CreatePHI(ir.getInt64Ty(), 2);
      remaining->addIncoming(ir.getInt64(count), entry);
      auto *index = ir.CreateSub(remaining, ir.getInt64(1));
      child(recipe.aggregate.front(),
            ir.CreateGEP(session.layouts().get(recipe.type).storage, address,
                         {ir.getInt64(0), index}));
      remaining->addIncoming(index, ir.GetInsertBlock());
      ir.CreateCondBr(ir.CreateICmpNE(index, ir.getInt64(0)), loop, done);
      ir.SetInsertPoint(done);
    }
  } else {
    if (recipe.destructor) {
      const auto *target =
          session.program().mir().findDestructorInstance(*recipe.destructor);
      if (!target || objectType.kind != SemanticType::Class)
        throw std::logic_error("LLVM cleanup lost its selected destructor");
      const auto index = session.layouts().activeIndex(objectType);
      if (!index)
        throw std::logic_error(
            "LLVM source destructor has no active-state representation");
      auto *state = ir.CreateStructGEP(
          session.layouts().get(objectType).storage, address, *index);
      auto *run = llvm::BasicBlock::Create(session.context(), "body", function);
      auto *done =
          llvm::BasicBlock::Create(session.context(), "body.done", function);
      ir.CreateCondBr(
          ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), state), ir.getInt8(0)),
          run, done);
      ir.SetInsertPoint(run);
      ir.CreateStore(ir.getInt8(0), state);
      const auto abi = session.abi().gti(SemanticType::Void, {},
                                         target->mayRaiseDefinedFailure, true);
      std::vector<llvm::Value *> arguments;
      if (abi.failureDestination)
        arguments.push_back(scratch);
      arguments.push_back(address);
      auto *status = ir.CreateCall(
          findBody({MirBodyKind::Destructor, target->id}), arguments);
      abi.applyTo(*status);
      if (abi.statusResult)
        observe(status);
      ir.CreateBr(done);
      ir.SetInsertPoint(done);
    }
    for (const auto &selected : recipe.aggregate) {
      if (unique) {
        if (recipe.aggregate.size() != 1 || selected.type != objectType ||
            selected.uniqueOwnerField || selected.uniqueOwnerBase ||
            selected.captureIndex || selected.payloadVariant)
          throw std::logic_error(
              "LLVM unique cleanup lost its unprojected pointee child");
        child(selected, address);
        continue;
      }
      unsigned index;
      if (selected.uniqueOwnerField)
        index = session.layouts().fieldIndex(recipe.type,
                                             selected.uniqueOwnerField);
      else if (selected.uniqueOwnerBase)
        index =
            session.layouts().baseIndex(recipe.type, *selected.uniqueOwnerBase);
      else
        throw BackendUnsupportedError(
            "LLVM cleanup subobject requires its later capability milestone");
      child(selected,
            ir.CreateStructGEP(session.layouts().get(recipe.type).storage,
                               address, index));
    }
  }
  if (unique) {
    Memory memory(session, ir, {}, {}, {});
    memory.releaseBytes(allocation);
  }
  ir.CreateRet(ir.CreateNot(ir.CreateLoad(ir.getInt1Ty(), failed)));
  buildingCleanups.pop_back();
  return function;
}

} // namespace lang::native_llvm
