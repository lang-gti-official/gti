#include "lifetimes.h"

#include <algorithm>
#include <set>

namespace lang::native_llvm {

Lifetimes::Lifetimes(Session &session, Failures &failures, Dispatch &dispatch,
                     FindBody findBody)
    : session(session), failures(failures), dispatch(dispatch),
      findBody(std::move(findBody)) {}

void Lifetimes::preflight(const MirDropType &recipe, bool replacement) const {
  std::vector<HirClassInstanceId> path;
  const auto visit = [&](const auto &self,
                         const MirDropType &selected) -> void {
    if (selected.polymorphicCleanup || selected.recursiveCleanup ||
        selected.uniquePointeeClass || selected.lambdaInstance ||
        selected.payloadVariant || selected.captureIndex ||
        selected.payloadField)
      throw BackendUnsupportedError(
          "LLVM cleanup requires its later ownership/dispatch milestone");
    if (selected.type.kind == SemanticType::Class) {
      const auto &owner = session.layouts().instance(selected.type);
      if (selected.classInstance != owner.id ||
          std::find(path.begin(), path.end(), owner.id) != path.end())
        throw std::logic_error(
            "LLVM static cleanup lost its finite class identity");
      if (replacement && owner.polymorphic)
        throw BackendUnsupportedError("LLVM polymorphic replacement requires a "
                                      "verified cleanup dispatch context");
      path.push_back(owner.id);
    } else if (selected.requiresActiveCleanup &&
               selected.type.kind != SemanticType::Array) {
      throw BackendUnsupportedError(
          "LLVM static cleanup requires addressable class/array recipes");
    }
    // fixedArray.destructor summarizes its leaf effect inventory; it is not
    // a source destructor/active byte belonging to the array storage itself.
    if (selected.destructor && !selected.fixedArray) {
      const auto *target =
          session.program().mir().findDestructorInstance(*selected.destructor);
      if (!target || target->owner != selected.classInstance ||
          !session.layouts().activeIndex(selected.type))
        throw std::logic_error(
            "LLVM static destructor lost its exact active-state witness");
    }
    if (selected.requiresActiveCleanup &&
        selected.type.kind == SemanticType::Array && !selected.fixedArray)
      throw BackendUnsupportedError(
          "LLVM array cleanup requires its exact fixed-array schedule");
    if (selected.fixedArray) {
      if (selected.type.kind != SemanticType::Array ||
          selected.aggregate.size() > 1 ||
          (selected.aggregate.empty() && selected.requiresActiveCleanup))
        throw std::logic_error(
            "LLVM array cleanup lost its sealed element recipe");
    } else if (!selected.aggregate.empty() &&
               selected.type.kind != SemanticType::Class) {
      throw BackendUnsupportedError(
          "LLVM static cleanup requires its exact aggregate representation");
    }
    for (const auto &child : selected.aggregate) {
      if (!selected.fixedArray) {
        if (child.uniqueOwnerField)
          static_cast<void>(session.layouts().fieldIndex(
              selected.type, child.uniqueOwnerField));
        else if (child.uniqueOwnerBase)
          static_cast<void>(session.layouts().baseIndex(
              selected.type, *child.uniqueOwnerBase));
        else
          throw BackendUnsupportedError(
              "LLVM cleanup subobject requires its later capability milestone");
      }
      self(self, child);
    }
    if (selected.type.kind == SemanticType::Class)
      path.pop_back();
  };
  visit(visit, recipe);
}

llvm::Function *Lifetimes::cleanupObservation(const MirDropType &recipe) {
  for (const auto &[previous, function] : observations)
    if (previous == recipe)
      return function;
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt1Ty(session.context()),
                              {session.layouts().pointer()}, false),
      llvm::GlobalValue::InternalLinkage,
      "__gti_native_cleanup_observation_" + std::to_string(observations.size()),
      session.module());
  observations.emplace_back(recipe, function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *address = function->getArg(0);
  // This is NOT an object-liveness predicate. Its caller already owns a
  // completed object/subobject obligation. False suppresses only header stores;
  // the original guarded bodies and complete child cleanup still run.
  if (!recipe.requiresActiveCleanup) {
    ir.CreateRet(ir.getFalse());
    return function;
  }
  auto *observed =
      llvm::BasicBlock::Create(session.context(), "observed", function);
  llvm::IRBuilder<>(observed).CreateRet(ir.getTrue());
  const auto witness = [&](llvm::Value *value) {
    auto *next = llvm::BasicBlock::Create(session.context(), "next", function);
    ir.CreateCondBr(value, observed, next);
    ir.SetInsertPoint(next);
  };
  if (recipe.destructor && !recipe.fixedArray) {
    const auto index = *session.layouts().activeIndex(recipe.type);
    auto *state = ir.CreateStructGEP(session.layouts().get(recipe.type).storage,
                                     address, index);
    witness(
        ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), state), ir.getInt8(0)));
  }
  if (recipe.fixedArray) {
    const auto count = recipe.type.arrayLength;
    if (count && !recipe.aggregate.empty()) {
      auto *entry = ir.GetInsertBlock();
      auto *loop =
          llvm::BasicBlock::Create(session.context(), "elements", function);
      auto *done =
          llvm::BasicBlock::Create(session.context(), "done", function);
      ir.CreateBr(loop);
      ir.SetInsertPoint(loop);
      auto *index = ir.CreatePHI(ir.getInt64Ty(), 2);
      index->addIncoming(ir.getInt64(0), entry);
      auto *element = ir.CreateGEP(session.layouts().get(recipe.type).storage,
                                   address, {ir.getInt64(0), index});
      witness(ir.CreateCall(cleanupObservation(recipe.aggregate.front()),
                            {element}));
      auto *next = ir.CreateAdd(index, ir.getInt64(1));
      index->addIncoming(next, ir.GetInsertBlock());
      ir.CreateCondBr(ir.CreateICmpULT(next, ir.getInt64(count)), loop, done);
      ir.SetInsertPoint(done);
    }
  } else {
    for (const auto &child : recipe.aggregate) {
      if (!child.requiresActiveCleanup)
        continue;
      const auto index = child.uniqueOwnerField
                             ? session.layouts().fieldIndex(
                                   recipe.type, child.uniqueOwnerField)
                             : session.layouts().baseIndex(
                                   recipe.type, *child.uniqueOwnerBase);
      auto *where = ir.CreateStructGEP(
          session.layouts().get(recipe.type).storage, address, index);
      witness(ir.CreateCall(cleanupObservation(child), {where}));
    }
  }
  ir.CreateRet(ir.getFalse());
  return function;
}

void Lifetimes::active(llvm::IRBuilder<> &ir, llvm::Value *address,
                       const SemanticType &type, bool value) {
  if (type.kind != SemanticType::Class)
    return;
  if (const auto index = session.layouts().activeIndex(type))
    ir.CreateStore(ir.getInt8(value),
                   ir.CreateStructGEP(session.layouts().get(type).storage,
                                      address, *index));
}

void Lifetimes::generatedEmpty(llvm::IRBuilder<> &ir, llvm::Value *address,
                               const MirBody &body,
                               const MirInstruction &instruction,
                               const MirConstructorInstance *constructor) {
  const bool construct = instruction.kind == MirInstructionKind::Construct;
  const bool base = instruction.kind == MirInstructionKind::InitializeBase;
  if (!address || !address->getType()->isPointerTy() || (!construct && !base) ||
      instruction.tombstone || instruction.constructorTarget ||
      instruction.constructorKind != ConstructorKind::Ordinary ||
      !instruction.operands.empty() || !instruction.parameterTypes.empty() ||
      !instruction.definedFailure.empty() || instruction.functionTarget ||
      instruction.bodyTarget || instruction.receiver ||
      instruction.objectAllocation != 0 || instruction.rawMemoryAccess ||
      instruction.objectLayoutType ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.info.type.kind != SemanticType::Class ||
      (construct && (!instruction.result || instruction.destination ||
                     instruction.constructorInitializer != 0 || constructor)) ||
      (base && instruction.result))
    throw std::invalid_argument("LLVM generated-empty initialization requires "
                                "an exact fresh default operation");
  const bool present = std::any_of(
      body.blocks.begin(), body.blocks.end(), [&](const auto &block) {
        return std::any_of(
            block.instructions.begin(), block.instructions.end(),
            [&](const auto &row) { return &row == &instruction; });
      });
  if (!present)
    throw std::invalid_argument(
        "LLVM generated-empty operation belongs to another body");
  if (construct) {
    const auto *result = body.findValue(*instruction.result);
    if (!result || result->id != *instruction.result ||
        result->definition != instruction.id ||
        result->info.type != instruction.info.type)
      throw std::invalid_argument(
          "LLVM generated-empty construction lost its exact result type");
  }
  const auto &object = session.layouts().instance(instruction.info.type);
  if ((object.kind != ClassKind::Class && object.kind != ClassKind::Struct) ||
      object.defaultConstructor != SpecialMemberStatus::Generated ||
      object.generatedDefaultRequiresSchedule ||
      !object.declaredFields.empty() || !object.fields.empty() ||
      !object.fieldDropOrder.empty() ||
      std::any_of(object.structuralBases.begin(), object.structuralBases.end(),
                  [](const auto &selected) { return !selected.interface; }) ||
      object.cAbiRecord || object.cAbiLayout || object.cOpaqueHandle ||
      object.unionLayout ||
      object.compilerCapability != CompilerCapabilityTypeKind::None ||
      std::any_of(object.bases.begin(), object.bases.end(),
                  [](const auto &selected) { return !selected.interface; }))
    throw BackendUnsupportedError(
        "LLVM generated-empty initialization requires metadata-only ordinary "
        "storage and no source schedule");
  // An interface flag alone is not a layout witness. Bind every permitted
  // row, including inherited interface rows, to its exact sealed concrete
  // instance and unique physical subobject before writing any storage.
  std::vector<HirBaseInstance> interfaces = object.bases;
  interfaces.insert(interfaces.end(), object.structuralBases.begin(),
                    object.structuralBases.end());
  std::set<HirClassInstanceId> checked;
  while (!interfaces.empty()) {
    const auto row = interfaces.back();
    interfaces.pop_back();
    const auto *selected =
        session.program().mir().findClassInstance(row.instance);
    if (!row.interface || !selected || selected->type != row.type ||
        selected->kind != ClassKind::Interface ||
        selected->defaultConstructor != SpecialMemberStatus::Generated ||
        selected->generatedDefaultRequiresSchedule ||
        !selected->declaredFields.empty() || !selected->fields.empty() ||
        !selected->fieldDropOrder.empty() || selected->destructor ||
        selected->requiresActiveDropState || selected->requiresActiveCleanup ||
        selected->cAbiRecord || selected->cAbiLayout ||
        selected->cOpaqueHandle || selected->unionLayout ||
        selected->compilerCapability != CompilerCapabilityTypeKind::None)
      throw BackendUnsupportedError(
          "LLVM generated-empty initialization lost a metadata-only interface "
          "subobject");
    static_cast<void>(session.layouts().get(row.type));
    static_cast<void>(session.layouts().baseSubobject(object.type, row.type));
    if (!checked.insert(row.instance).second)
      continue;
    interfaces.insert(interfaces.end(), selected->bases.begin(),
                      selected->bases.end());
    interfaces.insert(interfaces.end(), selected->structuralBases.begin(),
                      selected->structuralBases.end());
  }
  const auto storage = session.layouts().get(object.type).storage;
  if (base) {
    if (!constructor || &body != &constructor->body ||
        session.program().mir().findConstructorInstance(constructor->id) !=
            constructor ||
        constructor->definitionKind != MirDefinitionKind::Source ||
        instruction.constructorInitializer == 0 ||
        instruction.constructorInitializer > constructor->initializers.size() ||
        !instruction.destination)
      throw std::invalid_argument(
          "LLVM generated-empty base lost its exact constructor stage");
    const auto &selected =
        constructor->initializers[instruction.constructorInitializer - 1];
    const auto *destination = body.findPlace(*instruction.destination);
    const auto *parent =
        session.program().mir().findClassInstance(constructor->owner);
    if (selected.kind != ConstructorInitializerTargetKind::Base ||
        selected.base != object.id || selected.targetType != object.type ||
        !selected.generatedDefault || selected.constructorTarget ||
        !selected.arguments.empty() || selected.explicitArgumentCount != 0 ||
        selected.storesReference || selected.ownedParameter || !destination ||
        destination->root != MirPlaceRootKind::This ||
        destination->type != object.type ||
        destination->projections !=
            std::vector<MirPlaceProjection>{
                {.kind = MirProjectionKind::Base, .selection = object.id}} ||
        !parent ||
        std::none_of(parent->bases.begin(), parent->bases.end(),
                     [&](const auto &row) {
                       return !row.interface && row.instance == object.id &&
                              row.type == object.type;
                     }))
      throw std::invalid_argument(
          "LLVM generated-empty base lost its selected type or destination");
  }
  // The verified no-schedule operation has no source callbacks. Only empty
  // interface subobjects and private metadata exist here, never language
  // fields or existing assignment storage. Preflight every table before any
  // store; observable pure phases retain their separate rejection.
  if (object.polymorphic)
    for (const auto &view : dispatch.views(object.id))
      static_cast<void>(dispatch.table(object.id, view.viewClass));
  ir.CreateStore(llvm::Constant::getNullValue(storage), address);
  if (object.polymorphic)
    dispatch.install(ir, address, object.id);
  active(ir, address, object.type, true);
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
  }
  ir.CreateRetVoid();
  return function;
}
bool Lifetimes::needsMoveState(const SemanticType &type) const {
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

llvm::Function *Lifetimes::cleanup(const MirDropType &recipe,
                                   bool replacement) {
  preflight(recipe, replacement);
  for (const auto &[previous, function] : cleanups)
    if (previous == recipe)
      return function;
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

  if (recipe.requiresActiveCleanup && recipe.classInstance &&
      session.layouts().instance(recipe.type).polymorphic) {
    auto *install = llvm::BasicBlock::Create(session.context(),
                                             "dispatch.install", function);
    auto *ready =
        llvm::BasicBlock::Create(session.context(), "dispatch.ready", function);
    ir.CreateCondBr(ir.CreateCall(cleanupObservation(recipe), {address}),
                    install, ready);
    ir.SetInsertPoint(install);
    dispatch.install(ir, address, *recipe.classInstance);
    ir.CreateBr(ready);
    ir.SetInsertPoint(ready);
  }

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
