#include "lifetimes.h"
#include "closure_environment.h"
#include "memory.h"
#include "payloads.h"

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
    if (sharedOwnerRepresentation(session.program().mir(), owner)) {
      ir.CreateStore(llvm::Constant::getNullValue(storage), address);
      ir.CreateRetVoid();
      return function;
    }
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
  if (type.kind == SemanticType::UniqueOwner)
    return true;
  if (type.kind == SemanticType::Array)
    return needsMoveState(type.arguments.at(0));
  if (type.kind == SemanticType::Enum)
    return session.program().mir().findPayloadEnum(type) != nullptr;
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
  if (sharedOwnerRepresentation(session.program().mir(), owner))
    return true;
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
  if (type.kind == SemanticType::Enum &&
      session.program().mir().findPayloadEnum(type)) {
    ir.CreateStore(
        ir.getInt32(PayloadLayout::inactive),
        payloadTagAddress(ir, session.layouts().payload(type), address));
    return;
  }
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
  if (recipe.polymorphicCleanup)
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
  auto *cleanupToken = ir.CreateAlloca(session.layouts().pointer());
  auto *scratch = ir.CreateAlloca(session.abi().failureChannel());
  ir.CreateStore(ir.getFalse(), failed);
  const bool unique = recipe.type.kind == SemanticType::UniqueOwner;
  const bool shared =
      recipe.type.kind == SemanticType::Class &&
      sharedOwnerRepresentation(session.program().mir(),
                                session.layouts().instance(recipe.type));
  llvm::Value *sharedBlock = nullptr;
  llvm::Value *sharedPointee = nullptr;
  if (shared) {
    if (!recipe.classInstance ||
        *recipe.classInstance != session.layouts().instance(recipe.type).id ||
        recipe.aggregate.size() != 1 ||
        recipe.aggregate[0].type !=
            SemanticType::uniqueOwnerOf(recipe.type.arguments[0]) ||
        recipe.aggregate[0].uniqueOwnerField ||
        recipe.aggregate[0].uniqueOwnerBase ||
        recipe.aggregate[0].captureIndex || recipe.destructor ||
        recipe.fixedArray)
      throw std::logic_error(
          "LLVM shared cleanup lost its exact unique pointee recipe");
    Memory memory(session, ir, {}, {}, {});
    const auto released = memory.releaseShared(address);
    sharedBlock = released.block;
    auto *last =
        llvm::BasicBlock::Create(session.context(), "shared.last", function);
    auto *done = llvm::BasicBlock::Create(session.context(), "shared.released",
                                          function);
    ir.CreateCondBr(released.last, last, done);
    ir.SetInsertPoint(done);
    ir.CreateRet(ir.getTrue());
    ir.SetInsertPoint(last);
    sharedPointee = ir.CreateAlloca(pointer);
    ir.CreateStore(released.allocation, sharedPointee);
  }
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
    failures.checkIncomingCleanup(ir, scratch);
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
    ir.CreateStore(failures.enterCleanup(ir, output), cleanupToken);
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
  if (shared) {
    child(recipe.aggregate[0], sharedPointee);
    Memory memory(session, ir, {}, {}, {});
    memory.releaseBytes(sharedBlock);
  } else if (recipe.type.kind == SemanticType::Enum &&
             session.program().mir().findPayloadEnum(recipe.type)) {
    const auto &layout = session.layouts().payload(recipe.type);
    auto *tagAddress = payloadTagAddress(ir, layout, address);
    auto *tag = ir.CreateLoad(ir.getInt32Ty(), tagAddress);
    ir.CreateStore(ir.getInt32(PayloadLayout::inactive), tagAddress);
    auto *done =
        llvm::BasicBlock::Create(session.context(), "payload.done", function);
    auto *dispatch = ir.CreateSwitch(tag, done);
    for (std::size_t variant = 0; variant < layout.variants.size(); ++variant) {
      auto *run =
          llvm::BasicBlock::Create(session.context(), "payload", function);
      dispatch->addCase(ir.getInt32(variant), run);
      ir.SetInsertPoint(run);
      for (const auto &selected : recipe.aggregate)
        if (selected.payloadVariant == variant) {
          if (!selected.payloadField)
            throw std::logic_error("LLVM payload cleanup lost its exact field");
          child(selected, payloadFieldAddress(ir, layout, address, variant,
                                              *selected.payloadField));
        }
      ir.CreateBr(done);
    }
    ir.SetInsertPoint(done);
  } else if (recipe.fixedArray) {
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
  auto *failedResult = ir.CreateLoad(ir.getInt1Ty(), failed);
  auto *leave = llvm::BasicBlock::Create(session.context(),
                                        "cleanup.context.leave", function);
  auto *result = llvm::BasicBlock::Create(session.context(),
                                         "cleanup.result", function);
  ir.CreateCondBr(failedResult, leave, result);
  ir.SetInsertPoint(leave);
  failures.leaveCleanup(
      ir, ir.CreateLoad(session.layouts().pointer(), cleanupToken));
  ir.CreateBr(result);
  ir.SetInsertPoint(result);
  ir.CreateRet(ir.CreateNot(failedResult));
  buildingCleanups.pop_back();
  return function;
}

} // namespace lang::native_llvm
