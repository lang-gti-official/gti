#include "lifetimes.h"
#include "closure_environment.h"

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
  auto *address = function->getArg(0);
  if (type.kind == SemanticType::Array && type.arrayLength) {
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
  } else if (type.kind == SemanticType::Lambda) {
    const auto environment =
        closureEnvironmentLayout(session.program().mir(), type);
    if (!environment ||
        environment->captureTypes.size() != environment->captureModes.size() ||
        environment->captureTypes.size() !=
            environment->captureRequiresActiveCleanup.size())
      throw BackendUnsupportedError(
          "LLVM lambda move state requires one compatible environment");
    for (std::size_t index = 0; index < environment->captureTypes.size();
         ++index) {
      if (isBorrowingCapture(environment->captureModes[index])) {
        if (environment->captureRequiresActiveCleanup[index])
          throw std::logic_error(
              "borrowed LLVM capture acquired owned cleanup state");
        continue;
      }
      if (needsMoveState(environment->captureTypes[index]))
        moved(
            ir,
            ir.CreateStructGEP(storage, address, static_cast<unsigned>(index)),
            environment->captureTypes[index]);
    }
  }
  ir.CreateRetVoid();
  return function;
}
bool Lifetimes::needsMoveState(const SemanticType &type) const {
  if (type.kind == SemanticType::Array)
    return needsMoveState(type.arguments.at(0));
  if (type.kind == SemanticType::Lambda) {
    const auto environment =
        closureEnvironmentLayout(session.program().mir(), type);
    if (!environment ||
        environment->captureTypes.size() != environment->captureModes.size() ||
        environment->captureTypes.size() !=
            environment->captureRequiresActiveCleanup.size())
      throw BackendUnsupportedError(
          "LLVM lambda move state requires one compatible environment");
    for (std::size_t index = 0; index < environment->captureTypes.size();
         ++index) {
      if (isBorrowingCapture(environment->captureModes[index])) {
        if (environment->captureRequiresActiveCleanup[index])
          throw std::logic_error(
              "borrowed LLVM capture acquired owned cleanup state");
        continue;
      }
      const bool nested = needsMoveState(environment->captureTypes[index]);
      if (nested != environment->captureRequiresActiveCleanup[index])
        throw std::logic_error(
            "LLVM capture move state disagrees with its sealed cleanup fact");
      if (nested)
        return true;
    }
    return false;
  }
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
  if (recipe.polymorphicCleanup || recipe.recursiveCleanup ||
      recipe.uniquePointeeClass || recipe.payloadVariant)
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
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *address = function->getArg(0);
  auto *output = function->getArg(1);
  auto *duringFailure = function->getArg(2);
  auto *failed = ir.CreateAlloca(ir.getInt1Ty());
  auto *scratch = ir.CreateAlloca(session.abi().failureChannel());
  ir.CreateStore(ir.getFalse(), failed);

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
  } else if (recipe.lambdaInstance) {
    const auto *owner = closureCleanupInstance(session.program().mir(), recipe);
    if (owner == nullptr ||
        owner->captureTypes.size() != owner->captureModes.size() ||
        owner->captureTypes.size() !=
            owner->captureRequiresActiveCleanup.size())
      throw std::logic_error(
          "LLVM closure cleanup lost its sealed lambda identity");

    std::vector<std::size_t> expected;
    for (std::size_t index = owner->captureTypes.size(); index != 0; --index)
      if (owner->captureRequiresActiveCleanup[index - 1])
        expected.push_back(index - 1);
    if (recipe.requiresActiveCleanup != !expected.empty() ||
        recipe.aggregate.size() != expected.size())
      throw std::logic_error(
          "LLVM closure cleanup lost its sealed active-capture schedule");

    for (std::size_t step = 0; step < recipe.aggregate.size(); ++step) {
      const auto &selected = recipe.aggregate[step];
      if (!selected.captureIndex || *selected.captureIndex != expected[step] ||
          selected.type != owner->captureTypes[expected[step]] ||
          isBorrowingCapture(owner->captureModes[expected[step]]) ||
          selected.uniqueOwnerField != 0 || selected.uniqueOwnerBase ||
          selected.payloadVariant || selected.payloadField)
        throw std::logic_error(
            "LLVM closure cleanup lost an exact owned capture projection");
      child(selected,
            ir.CreateStructGEP(session.layouts().get(recipe.type).storage,
                               address, static_cast<unsigned>(expected[step])));
    }
  } else {
    if (recipe.type.kind == SemanticType::Lambda)
      throw std::logic_error(
          "LLVM closure cleanup has no concrete lambda identity");
    if (recipe.destructor) {
      const auto *target =
          session.program().mir().findDestructorInstance(*recipe.destructor);
      if (!target || recipe.type.kind != SemanticType::Class)
        throw std::logic_error("LLVM cleanup lost its selected destructor");
      const auto index = session.layouts().activeIndex(recipe.type);
      if (!index)
        throw std::logic_error(
            "LLVM source destructor has no active-state representation");
      auto *state = ir.CreateStructGEP(
          session.layouts().get(recipe.type).storage, address, *index);
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
  ir.CreateRet(ir.CreateNot(ir.CreateLoad(ir.getInt1Ty(), failed)));
  return function;
}

} // namespace lang::native_llvm
