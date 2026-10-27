#include "lifetimes.h"
#include "closure_environment.h"
#include "gti/mir_effects.h"
#include "memory.h"
#include "mutexes.h"
#include "payloads.h"
#include "polymorphic_storage.h"
#include "runtime_abi.h"
#include "threads.h"

#include <algorithm>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <set>

namespace lang::native_llvm {

Lifetimes::Lifetimes(Session &session, Failures &failures, Dispatch &dispatch,
                     FindBody findBody)
    : session(session), failures(failures), dispatch(dispatch),
      findBody(std::move(findBody)) {}

namespace {
// The class a polymorphic class step of a cleanup recipe runs under: the
// lifecycle-dispatch transition MIR records for its source destructor, or the
// step's own class while a generated cleanup ends its fields. The backend
// never selects a destructor's phase itself.
HirClassInstanceId destructionPhase(const MirProgram &program,
                                    const MirDropType &recipe) {
  if (!recipe.classInstance)
    throw std::logic_error("LLVM destruction phase lost its class step");
  const auto active = *recipe.classInstance;
  if (!recipe.destructor || recipe.fixedArray)
    return active;
  const auto *destructor = program.findDestructorInstance(*recipe.destructor);
  if (!destructor || destructor->owner != active)
    throw std::logic_error("LLVM destruction phase lost its destructor");
  if (!destructor->dispatchState ||
      destructor->dispatchState->activeClass != active)
    throw BackendUnsupportedError(
        "LLVM destructor requires its recorded lifecycle-dispatch transition");
  return destructor->dispatchState->activeClass;
}

// The exact dynamically typed unique-owner recipe: the owner's static pointee
// names only a view. Its complete object, cleanup and allocation identity all
// come from the installed dispatch descriptor at drop time.
const MirClassInstance &dynamicOwnerView(Session &session,
                                         const MirDropType &recipe) {
  if (!recipe.polymorphicCleanup ||
      recipe.type.kind != SemanticType::UniqueOwner ||
      recipe.type.arguments.size() != 1 ||
      recipe.type.arguments.front().kind != SemanticType::Class ||
      !recipe.requiresActiveCleanup || !recipe.uniquePointeeClass ||
      recipe.classInstance || recipe.lambdaInstance || recipe.destructor ||
      recipe.fixedArray || recipe.recursiveCleanup || !recipe.aggregate.empty())
    throw std::logic_error(
        "LLVM dynamic owner cleanup lost its exact descriptor recipe");
  const auto &view = session.layouts().instance(recipe.type.arguments.front());
  if (view.id != *recipe.uniquePointeeClass || !view.polymorphic)
    throw std::logic_error(
        "LLVM dynamic owner cleanup lost its polymorphic pointee view");
  if (!session.layouts().dispatchLocation(view.type))
    throw BackendUnsupportedError(
        "LLVM dynamic owner cleanup requires a dispatch-bearing pointee view");
  return view;
}
} // namespace

std::optional<MirDropType>
Lifetimes::completeCleanupRecipe(Session &session,
                                 HirClassInstanceId activeClass) {
  const auto &program = session.program().mir();
  const auto *object = program.findClassInstance(activeClass);
  if (!object || !object->polymorphic || object->abstract ||
      object->kind == ClassKind::Interface)
    throw std::logic_error(
        "LLVM complete cleanup requires a concrete polymorphic class");
  auto recipe = mirUniqueCleanupType(program, object->type);
  if (!recipe) {
    if (object->requiresActiveCleanup)
      throw std::logic_error(
          "LLVM dynamic owner lost its complete cleanup recipe");
    return std::nullopt;
  }
  if (recipe->type != object->type || recipe->classInstance != object->id ||
      recipe->polymorphicCleanup || recipe->recursiveCleanup)
    throw std::logic_error(
        "LLVM complete cleanup recipe lost its exact active class");
  return recipe;
}

llvm::Function *Lifetimes::completeCleanup(HirClassInstanceId activeClass) {
  if (const auto recipe = completeCleanupRecipe(session, activeClass))
    return cleanup(*recipe);
  if (!inertCleanup) {
    auto *pointer = session.layouts().pointer();
    inertCleanup = llvm::Function::Create(
        llvm::FunctionType::get(
            llvm::Type::getInt1Ty(session.context()),
            {pointer, pointer, llvm::Type::getInt1Ty(session.context())},
            false),
        llvm::GlobalValue::InternalLinkage, "__gti_native_inert_cleanup",
        session.module());
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", inertCleanup));
    ir.CreateRet(ir.getTrue());
  }
  return inertCleanup;
}

void Lifetimes::preflightCleanup(Session &session, Dispatch &dispatch,
                                 const MirDropType &recipe, bool replacement) {
  if (!dispatch.boundTo(session))
    throw std::invalid_argument(
        "LLVM cleanup preflight uses a foreign dispatch session");
  std::vector<HirClassInstanceId> path;
  const auto visit = [&](const auto &self,
                         const MirDropType &selected) -> void {
    if (selected.polymorphicCleanup) {
      // An owner handle holds no inline hierarchy storage. Its pointee's
      // complete cleanup is proved where that object was allocated.
      static_cast<void>(dynamicOwnerView(session, selected));
      return;
    }
    // A subobject without inline hierarchy storage has an ordinary recipe:
    // the shared cleanup planner proves and emits its owner, closure or
    // payload schedule, and preflights hierarchy pointees at owned edges.
    if (&selected != &recipe &&
        !containsHierarchyStorage(session.program().mir(), selected.type))
      return;
    // A payload field step names one exact field of one alternative; its
    // own recipe is that field's complete cleanup.
    if (selected.recursiveCleanup || selected.uniquePointeeClass ||
        selected.lambdaInstance || selected.captureIndex ||
        selected.payloadVariant.has_value() !=
            selected.payloadField.has_value())
      throw BackendUnsupportedError(
          "LLVM cleanup requires its later ownership/dispatch milestone");
    const auto *payload =
        selected.type.kind == SemanticType::Enum
            ? session.program().mir().findPayloadEnum(selected.type)
            : nullptr;
    if (selected.type.kind == SemanticType::Class) {
      const auto &owner = session.layouts().instance(selected.type);
      if (selected.classInstance != owner.id ||
          std::find(path.begin(), path.end(), owner.id) != path.end())
        throw std::logic_error(
            "LLVM static cleanup lost its finite class identity");
      // Only the replaced object itself may be a view inside a larger
      // object; its inline fields and bases are complete subobjects.
      if (replacement && owner.polymorphic && &selected == &recipe)
        throw BackendUnsupportedError("LLVM polymorphic replacement requires a "
                                      "verified cleanup dispatch context");
      if (owner.polymorphic && selected.requiresActiveCleanup)
        dispatch.preflightPhase(
            destructionPhase(session.program().mir(), selected));
      path.push_back(owner.id);
    } else if (selected.requiresActiveCleanup &&
               selected.type.kind != SemanticType::Array && !payload) {
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
               selected.type.kind != SemanticType::Class && !payload) {
      throw BackendUnsupportedError(
          "LLVM static cleanup requires its exact aggregate representation");
    }
    // Exact mutex storage keeps its payload inline behind the native shell:
    // its single child is that complete payload object.
    const auto mutexPayload =
        selected.type.kind == SemanticType::Class
            ? nativeMutexElement(session.program().mir(), selected.type,
                                 CompilerCapabilityTypeKind::MutexStorage)
            : std::nullopt;
    for (const auto &child : selected.aggregate) {
      if (payload) {
        // Each payload child names one exact field of one alternative.
        if (!child.payloadVariant || !child.payloadField ||
            *child.payloadVariant >= payload->variants.size() ||
            *child.payloadField >=
                payload->variants[*child.payloadVariant].payloadTypes.size() ||
            payload->variants[*child.payloadVariant]
                    .payloadTypes[*child.payloadField] != child.type)
          throw std::logic_error(
              "LLVM payload cleanup lost its exact alternative field");
        static_cast<void>(session.layouts().payload(selected.type));
      } else if (!selected.fixedArray) {
        if (child.uniqueOwnerField)
          static_cast<void>(session.layouts().fieldIndex(
              selected.type, child.uniqueOwnerField));
        else if (child.uniqueOwnerBase)
          static_cast<void>(session.layouts().baseIndex(
              selected.type, *child.uniqueOwnerBase));
        else if (mutexPayload && selected.aggregate.size() == 1 &&
                 child.type == *mutexPayload)
          static_cast<void>(session.layouts().mutexStorage(selected.type));
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
  // Only ordinary class and fixed-array storage expose exact per-subobject
  // active states. A live owner handle has cleanup to run; any other owned
  // representation (closure, payload or capability storage) conservatively
  // counts as observed, which only permits the enclosing header store.
  const auto ordinaryClass = [&](const MirDropType &selected) {
    if (selected.type.kind != SemanticType::Class || !selected.classInstance ||
        selected.lambdaInstance)
      return false;
    const auto &owner = session.layouts().instance(selected.type);
    return owner.id == *selected.classInstance &&
           owner.compilerCapability == CompilerCapabilityTypeKind::None &&
           !sharedOwnerRepresentation(session.program().mir(), owner);
  };
  if (!recipe.fixedArray && !ordinaryClass(recipe)) {
    if (recipe.type.kind == SemanticType::UniqueOwner) {
      ir.CreateRet(ir.CreateIsNotNull(
          ir.CreateLoad(session.layouts().pointer(), address)));
      return function;
    }
    ir.CreateRet(ir.getTrue());
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

void Lifetimes::prepareProgramStorage(
    const std::map<SymbolId, llvm::GlobalVariable *> &globals) {
  if (programRetirement || !programStorage.empty())
    throw std::logic_error("LLVM program retirement prepared twice");
  for (const auto &step : session.program().mir().programInitializationPlan().steps) {
    if (!step.nativeRetirement)
      continue;
    if (!mirDropIsNativeRetirement(session.program().mir(), *step.nativeRetirement))
      throw std::logic_error("LLVM program retirement lost its exact native recipe");
    auto *live = new llvm::GlobalVariable(
        session.module(), llvm::Type::getInt1Ty(session.context()), false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::getFalse(session.context()),
        "__gti_program_storage_live_" + std::to_string(step.id));
    programStorage.push_back({step.storageInitialization, globals.at(step.symbol),
                              live, *step.nativeRetirement});
  }
  if (programStorage.empty())
    return;
  programRetirement = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(session.context()), false),
      llvm::GlobalValue::InternalLinkage, "__gti_program_retire_native",
      session.module());
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", programRetirement));
  auto *run = llvm::BasicBlock::Create(session.context(), "ready", programRetirement);
  auto *done = llvm::BasicBlock::Create(session.context(), "done", programRetirement);
  auto *ready = ir.CreateCall(declareRuntimeService(
      session, RuntimeService::ProgramStorageRetirementReady));
  ir.CreateCondBr(ir.CreateICmpNE(ready, ir.getInt32(0)), run, done);
  ir.SetInsertPoint(run);
  auto *failure = ir.CreateAlloca(session.abi().failureChannel());
  for (auto item = programStorage.rbegin(); item != programStorage.rend(); ++item) {
    auto *run = llvm::BasicBlock::Create(session.context(), "retire", programRetirement);
    auto *next = llvm::BasicBlock::Create(session.context(), "next", programRetirement);
    ir.CreateCondBr(ir.CreateLoad(ir.getInt1Ty(), item->live), run, next);
    ir.SetInsertPoint(run);
    ir.CreateStore(ir.getFalse(), item->live);
    ir.CreateCall(cleanup(item->recipe), {item->storage, failure, ir.getFalse()});
    ir.CreateBr(next);
    ir.SetInsertPoint(next);
  }
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
  ir.CreateRetVoid();
  // Module lifetime also has an endpoint when a native host unloads a library.
  // Unlike terminal hosted failure, unloading cannot leave live roots behind.
  auto *unload = llvm::Function::Create(
      programRetirement->getFunctionType(), llvm::GlobalValue::InternalLinkage,
      "__gti_program_unload_native", session.module());
  ir.SetInsertPoint(llvm::BasicBlock::Create(session.context(), "entry", unload));
  ir.CreateCall(programRetirement);
  llvm::Value *live = ir.getFalse();
  for (const auto &storage : programStorage)
    live = ir.CreateOr(live, ir.CreateLoad(ir.getInt1Ty(), storage.live));
  auto *pinned = llvm::BasicBlock::Create(session.context(), "pinned", unload);
  auto *retired = llvm::BasicBlock::Create(session.context(), "retired", unload);
  ir.CreateCondBr(live, pinned, retired);
  ir.SetInsertPoint(pinned);
  ir.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&session.module(),
                                                      llvm::Intrinsic::trap));
  ir.CreateUnreachable();
  ir.SetInsertPoint(retired);
  ir.CreateRetVoid();
  llvm::appendToGlobalDtors(session.module(), unload, 65535);
}

void Lifetimes::publishProgramStorage(llvm::IRBuilder<> &ir,
                                     MirInstructionId instruction) {
  for (const auto &storage : programStorage)
    if (storage.initialization == instruction)
      ir.CreateStore(ir.getTrue(), storage.live);
}

void Lifetimes::retireProgramStorage(llvm::IRBuilder<> &ir) {
  if (!programRetirement)
    throw std::logic_error("LLVM retirement has no sealed process storage");
  ir.CreateCall(programRetirement);
}

bool Lifetimes::containsMutex(const SemanticType &type) const {
  if (type.kind == SemanticType::Array)
    return containsMutex(type.arguments.at(0));
  if (type.kind == SemanticType::Enum) {
    const auto *enumeration = session.program().mir().findPayloadEnum(type);
    if (enumeration)
      for (const auto &variant : enumeration->variants)
        for (const auto &payload : variant.payloadTypes)
          if (containsMutex(payload))
            return true;
    return false;
  }
  if (type.kind == SemanticType::Lambda) {
    const auto environment =
        closureEnvironmentLayout(session.program().mir(), type);
    if (!environment)
      return false;
    for (std::size_t index = 0; index < environment->captureTypes.size();
         ++index)
      if (!isBorrowingCapture(environment->captureModes[index]) &&
          containsMutex(environment->captureTypes[index]))
        return true;
    return false;
  }
  if (type.kind != SemanticType::Class)
    return false;
  const auto &owner = session.layouts().instance(type);
  if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage)
    return true;
  // Pointer-owning and other private capabilities do not inline source fields.
  if (owner.compilerCapability != CompilerCapabilityTypeKind::None)
    return false;
  for (const auto &field : owner.declaredFields)
    if (containsMutex(field.type))
      return true;
  for (const auto &base : owner.bases)
    if (!base.interface && containsMutex(base.type))
      return true;
  return false;
}

bool Lifetimes::transferMutexes(llvm::IRBuilder<> &ir, llvm::Value *destination,
                                llvm::Value *source, const SemanticType &type) {
  if (!containsMutex(type))
    return false;
  if (destination != source)
    ir.CreateCall(mutexTransfer(type), {destination, source});
  return true;
}

llvm::Function *Lifetimes::mutexTransfer(const SemanticType &type) {
  const auto layout = session.layouts().get(type);
  if (const auto found = mutexTransfers.find(layout.storage);
      found != mutexTransfers.end())
    return found->second;
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(
          llvm::Type::getVoidTy(session.context()),
          {session.layouts().pointer(), session.layouts().pointer()}, false),
      llvm::GlobalValue::InternalLinkage,
      "__gti_native_mutex_transfer_" + std::to_string(mutexTransfers.size()),
      session.module());
  mutexTransfers.emplace(layout.storage, function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  auto *destination = function->getArg(0);
  auto *source = function->getArg(1);
  auto *done = llvm::BasicBlock::Create(session.context(), "done", function);
  auto *run = llvm::BasicBlock::Create(session.context(), "transfer", function);
  ir.CreateCondBr(ir.CreateICmpEQ(destination, source), done, run);
  ir.SetInsertPoint(run);
  const auto transfer = [&](const SemanticType &part, llvm::Value *to,
                            llvm::Value *from) {
    if (!transferMutexes(ir, to, from, part)) {
      const auto child = session.layouts().get(part);
      ir.CreateMemCpy(to, llvm::MaybeAlign(child.alignment), from,
                      llvm::MaybeAlign(child.alignment), child.size);
    }
  };
  if (nativeMutexElement(session.program().mir(), type,
                         CompilerCapabilityTypeKind::MutexStorage)) {
    const auto mutex = session.layouts().mutexStorage(type);
    auto *state =
        ir.CreateStructGEP(layout.storage, source, mutex.payloadActiveIndex);
    auto *bits = ir.CreateLoad(ir.getInt8Ty(), state);
    auto *toState = ir.CreateStructGEP(layout.storage, destination,
                                       mutex.payloadActiveIndex);
    ir.CreateStore(ir.getInt8(0), toState);
    auto *live =
        llvm::BasicBlock::Create(session.context(), "shell.live", function);
    ir.CreateCondBr(
        ir.CreateICmpNE(
            ir.CreateAnd(bits, ir.getInt8(MutexStorageLayout::shellLive)),
            ir.getInt8(0)),
        live, done);
    ir.SetInsertPoint(live);
    Mutexes mutexes(session, ir);
    mutexes.initializeShell(destination, type);
    auto *payload =
        llvm::BasicBlock::Create(session.context(), "payload.live", function);
    auto *retire =
        llvm::BasicBlock::Create(session.context(), "shell.retire", function);
    ir.CreateCondBr(
        ir.CreateICmpNE(
            ir.CreateAnd(bits, ir.getInt8(MutexStorageLayout::payloadLive)),
            ir.getInt8(0)),
        payload, retire);
    ir.SetInsertPoint(payload);
    auto *fromPayload =
        ir.CreateStructGEP(layout.storage, source, mutex.payloadIndex);
    transfer(
        mutex.element,
        ir.CreateStructGEP(layout.storage, destination, mutex.payloadIndex),
        fromPayload);
    moved(ir, fromPayload, mutex.element);
    ir.CreateStore(ir.getInt8(MutexStorageLayout::shellLive |
                              MutexStorageLayout::payloadLive),
                   toState);
    ir.CreateBr(retire);
    ir.SetInsertPoint(retire);
    ir.CreateStore(ir.getInt8(0), state);
    mutexes.destroyShell(source, type);
  } else if (type.kind == SemanticType::Array && type.arrayLength) {
    auto *entry = ir.GetInsertBlock();
    auto *loop =
        llvm::BasicBlock::Create(session.context(), "elements", function);
    auto *end =
        llvm::BasicBlock::Create(session.context(), "elements.done", function);
    ir.CreateBr(loop);
    ir.SetInsertPoint(loop);
    auto *index = ir.CreatePHI(ir.getInt64Ty(), 2);
    index->addIncoming(ir.getInt64(0), entry);
    transfer(type.arguments.at(0),
             ir.CreateGEP(layout.storage, destination, {ir.getInt64(0), index}),
             ir.CreateGEP(layout.storage, source, {ir.getInt64(0), index}));
    auto *next = ir.CreateAdd(index, ir.getInt64(1));
    index->addIncoming(next, ir.GetInsertBlock());
    ir.CreateCondBr(ir.CreateICmpULT(next, ir.getInt64(type.arrayLength)), loop,
                    end);
    ir.SetInsertPoint(end);
  } else if (type.kind == SemanticType::Class) {
    const auto &owner = session.layouts().instance(type);
    if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage) {
      // transferMutexes already retired the consumed native identity.
      ir.CreateRetVoid();
      return function;
    }
    for (const auto &base : owner.bases) {
      if (base.interface)
        continue;
      const auto index = session.layouts().baseIndex(type, base.instance);
      transfer(base.type,
               ir.CreateStructGEP(layout.storage, destination, index),
               ir.CreateStructGEP(layout.storage, source, index));
    }
    for (const auto &field : owner.declaredFields) {
      const auto index = session.layouts().fieldIndex(type, field.symbol);
      transfer(field.type,
               ir.CreateStructGEP(layout.storage, destination, index),
               ir.CreateStructGEP(layout.storage, source, index));
    }
    if (const auto index = session.layouts().activeIndex(type))
      ir.CreateStore(
          ir.CreateLoad(ir.getInt8Ty(),
                        ir.CreateStructGEP(layout.storage, source, *index)),
          ir.CreateStructGEP(layout.storage, destination, *index));
  } else if (type.kind == SemanticType::Lambda) {
    const auto environment =
        closureEnvironmentLayout(session.program().mir(), type);
    for (std::size_t index = 0; index < environment->captureTypes.size();
         ++index) {
      auto *to = ir.CreateStructGEP(layout.storage, destination, index);
      auto *from = ir.CreateStructGEP(layout.storage, source, index);
      if (isBorrowingCapture(environment->captureModes[index]))
        ir.CreateStore(ir.CreateLoad(session.layouts().pointer(), from), to);
      else
        transfer(environment->captureTypes[index], to, from);
    }
  } else if (type.kind == SemanticType::Enum) {
    const auto &payload = session.layouts().payload(type);
    const auto *enumeration = session.program().mir().findPayloadEnum(type);
    auto *tag =
        ir.CreateLoad(ir.getInt32Ty(), payloadTagAddress(ir, payload, source));
    ir.CreateStore(tag, payloadTagAddress(ir, payload, destination));
    auto *dispatch = ir.CreateSwitch(tag, done);
    for (const auto &variant : enumeration->variants) {
      auto *block =
          llvm::BasicBlock::Create(session.context(), "variant", function);
      dispatch->addCase(ir.getInt32(variant.variantIndex), block);
      ir.SetInsertPoint(block);
      for (std::size_t index = 0; index < variant.payloadTypes.size(); ++index)
        transfer(variant.payloadTypes[index],
                 payloadFieldAddress(ir, payload, destination,
                                     variant.variantIndex, index),
                 payloadFieldAddress(ir, payload, source, variant.variantIndex,
                                     index));
      ir.CreateBr(done);
    }
    ir.SetInsertPoint(done);
    ir.CreateRetVoid();
    return function;
  } else {
    throw std::logic_error(
        "LLVM native mutex transfer lost its structural owner");
  }
  ir.CreateBr(done);
  ir.SetInsertPoint(done);
  ir.CreateRetVoid();
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

const MirClassInstance &
Lifetimes::preflightGeneratedEmpty(Session &session, Dispatch &dispatch,
                                   const MirBody &body,
                                   const MirInstruction &instruction,
                                   const MirConstructorInstance *constructor) {
  if (!dispatch.boundTo(session))
    throw std::invalid_argument(
        "LLVM generated-empty preflight uses a foreign dispatch session");
  const auto addresses = enumerateMirBodyAddresses(session.program().mir());
  if (std::none_of(addresses.begin(), addresses.end(), [&](const auto address) {
        return findMirBody(session.program().mir(), address) == &body;
      }))
    throw std::invalid_argument(
        "LLVM generated-empty preflight uses a foreign sealed body");
  const bool construct = instruction.kind == MirInstructionKind::Construct;
  const bool base = instruction.kind == MirInstructionKind::InitializeBase;
  // A unique-owner allocation constructs its pointee in the exact storage its
  // ObjectAllocation obligation already holds.
  const bool allocation =
      instruction.kind == MirInstructionKind::Call &&
      instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
      instruction.objectAllocation != 0 && instruction.result &&
      !instruction.destination && instruction.constructorInitializer == 0 &&
      !constructor && instruction.info.type.kind == SemanticType::UniqueOwner &&
      instruction.info.type.arguments.size() == 1;
  const auto &objectType = allocation ? instruction.info.type.arguments.front()
                                      : instruction.info.type;
  // A construction may name the hidden temporary MIR materializes for its
  // result as its own destination; placement proves that exact publication.
  const auto *materialized =
      construct && instruction.destination
          ? body.findPlace(*instruction.destination)
          : nullptr;
  const bool hidden = materialized &&
                      materialized->root == MirPlaceRootKind::Temporary &&
                      materialized->projections.empty() &&
                      materialized->type == objectType;
  if ((!construct && !base && !allocation) || instruction.tombstone ||
      instruction.constructorTarget ||
      instruction.constructorKind != ConstructorKind::Ordinary ||
      !instruction.operands.empty() || !instruction.parameterTypes.empty() ||
      !instruction.definedFailure.empty() || instruction.functionTarget ||
      instruction.bodyTarget || instruction.receiver ||
      (!allocation && instruction.objectAllocation != 0) ||
      instruction.rawMemoryAccess || instruction.objectLayoutType ||
      (!allocation && instruction.intrinsic != IntrinsicKind::None) ||
      objectType.kind != SemanticType::Class ||
      (construct && (!instruction.result ||
                     (instruction.destination && !hidden) ||
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
  if (construct || allocation) {
    const auto *result = body.findValue(*instruction.result);
    if (!result || result->id != *instruction.result ||
        result->definition != instruction.id ||
        result->info.type != instruction.info.type)
      throw std::invalid_argument(
          "LLVM generated-empty construction lost its exact result type");
  }
  if (allocation) {
    const auto *obligation =
        body.findDropObligation(instruction.objectAllocation);
    if (!obligation ||
        obligation->kind != MirDropObligationKind::ObjectAllocation ||
        obligation->dropType.type != instruction.info.type)
      throw std::invalid_argument(
          "LLVM generated-empty allocation lost its exact storage obligation");
  }
  const auto &object = session.layouts().instance(objectType);
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
    static_cast<void>(session.layouts().baseSubobject(
        object.type, row.type, "lifetimes.generatedEmptyInterface"));
    if (!checked.insert(row.instance).second)
      continue;
    interfaces.insert(interfaces.end(), selected->bases.begin(),
                      selected->bases.end());
    interfaces.insert(interfaces.end(), selected->structuralBases.begin(),
                      selected->structuralBases.end());
  }
  static_cast<void>(session.layouts().get(object.type));
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
  if (object.polymorphic)
    dispatch.preflightPhase(object.id);
  return object;
}

void Lifetimes::generatedEmpty(llvm::IRBuilder<> &ir, llvm::Value *address,
                               const MirBody &body,
                               const MirInstruction &instruction,
                               const MirConstructorInstance *constructor) {
  if (!address || !address->getType()->isPointerTy())
    throw std::invalid_argument(
        "LLVM generated-empty initialization requires pointer storage");
  const auto &object = preflightGeneratedEmpty(session, dispatch, body,
                                               instruction, constructor);
  const auto storage = session.layouts().get(object.type).storage;
  // The verified no-schedule operation has no source callbacks. Only empty
  // interface subobjects and private metadata exist here, never language
  // fields or existing assignment storage. Preflight every table before any
  // store; observable pure phases retain their separate rejection.
  if (object.polymorphic && !dispatch.pureState(object.id))
    for (const auto &view : dispatch.views(object.id))
      static_cast<void>(dispatch.table(object.id, view.viewClass));
  ir.CreateStore(llvm::Constant::getNullValue(storage), address);
  if (object.polymorphic)
    dispatch.installPhase(ir, address, object.id);
  active(ir, address, object.type, true);
}

FreshHierarchyTransfer
Lifetimes::preflightFreshTransfer(Session &session, Dispatch &dispatch,
                                  const MirBody &body,
                                  const MirInstruction &instruction) {
  if (!dispatch.boundTo(session))
    throw std::invalid_argument(
        "LLVM fresh transfer uses a foreign dispatch session");
  auto transfer =
      planFreshHierarchyTransfer(session.program().mir(), body, instruction);
  const auto &owner =
      *session.program().mir().findClassInstance(transfer.owner);
  // This performs all dispatch/ABI checks without publishing a table/helper or
  // touching either object's runtime state. A transfer never inherits source
  // dispatch metadata, including when its source is a derived base view.
  if (owner.polymorphic)
    dispatch.preflightState(owner.id);
  if (transfer.kind == ConstructorKind::Move)
    preflightGeneratedRelocation(session, dispatch, owner.type);
  const auto preflight = [&](const auto &self,
                             const MirClassInstance &object) -> void {
    static_cast<void>(session.layouts().get(object.type));
    if (object.requiresActiveDropState !=
        session.layouts().activeIndex(object.type).has_value())
      throw std::logic_error(
          "LLVM transfer active state disagrees with its sealed class");
    for (const auto &base : object.bases) {
      const auto *child =
          session.program().mir().findClassInstance(base.instance);
      if (!child || child->type != base.type)
        throw std::logic_error("LLVM transfer lost its exact base identity");
      static_cast<void>(
          session.layouts().baseIndex(object.type, base.instance));
      self(self, *child);
    }
    for (const auto &field : object.declaredFields) {
      static_cast<void>(
          session.layouts().fieldIndex(object.type, field.symbol));
      static_cast<void>(session.layouts().get(field.type));
      if (field.type.kind == SemanticType::Class) {
        const auto &child = session.layouts().instance(field.type);
        if (child.polymorphic)
          dispatch.preflightState(child.id);
        self(self, child);
      }
    }
  };
  preflight(preflight, owner);
  for (const auto identity : {transfer.sourceDrop, transfer.valueDrop})
    if (identity) {
      const auto *drop = body.findDropObligation(identity);
      if (!drop)
        throw std::logic_error("LLVM fresh transfer lost its sealed cleanup");
      preflightCleanup(session, dispatch, drop->dropType);
    }
  return transfer;
}

void Lifetimes::freshTransfer(llvm::IRBuilder<> &ir, llvm::Value *destination,
                              llvm::Value *source, const MirBody &body,
                              const MirInstruction &instruction) {
  if (!destination || !source || !destination->getType()->isPointerTy() ||
      !source->getType()->isPointerTy() || destination == source)
    throw std::invalid_argument(
        "LLVM fresh transfer requires distinct object addresses");
  const auto transfer =
      preflightFreshTransfer(session, dispatch, body, instruction);
  const auto &owner =
      *session.program().mir().findClassInstance(transfer.owner);
  if (transfer.kind == ConstructorKind::Move) {
    // The exact fresh-local MIR publication grants this relocation. Reuse the
    // typed emitter that rebuilds nested headers and transfers active flags;
    // never materialize the move as a raw aggregate load/store.
    ir.CreateCall(generatedRelocation(owner.type), {destination, source});
    return;
  }
  generatedCopy(ir, destination, source, owner.type);
}

void Lifetimes::generatedCopy(llvm::IRBuilder<> &ir, llvm::Value *destination,
                              llvm::Value *source, const SemanticType &type) {
  if (!destination || !source || !destination->getType()->isPointerTy() ||
      !source->getType()->isPointerTy() || destination == source)
    throw std::invalid_argument(
        "LLVM generated copy requires distinct object addresses");
  preflightGeneratedHierarchyCopy(session.program().mir(), type);
  const auto &owner = session.layouts().instance(type);
  if (owner.polymorphic)
    dispatch.preflightState(owner.id);
  // Generated inert copy has no source callback or runtime failure point.
  // Copy logical fields only. Even a byte-identical static layout does not
  // authorize transporting source vptrs, padding, or active-state bytes.
  const auto copy = [&](const auto &self, const MirClassInstance &object,
                        llvm::Value *from, llvm::Value *to, bool base) -> void {
    const auto storage = session.layouts().get(object.type).storage;
    for (const auto &base : object.bases) {
      const auto index =
          session.layouts().baseIndex(object.type, base.instance);
      self(self, *session.program().mir().findClassInstance(base.instance),
           ir.CreateStructGEP(storage, from, index),
           ir.CreateStructGEP(storage, to, index), true);
    }
    for (const auto &field : object.declaredFields) {
      const auto index =
          session.layouts().fieldIndex(object.type, field.symbol);
      auto *sourceField = ir.CreateStructGEP(storage, from, index);
      auto *destinationField = ir.CreateStructGEP(storage, to, index);
      if (field.type.kind == SemanticType::Class)
        self(self, session.layouts().instance(field.type), sourceField,
             destinationField, false);
      else
        ir.CreateStore(ir.CreateLoad(session.layouts().get(field.type).storage,
                                     sourceField),
                       destinationField);
    }
    if (!base && object.polymorphic)
      dispatch.install(ir, to, object.id);
  };
  copy(copy, owner, source, destination, false);
}

namespace {
void preflightRelocation(Session &session, Dispatch &dispatch,
                         const SemanticType &type, bool inert) {
  if (!dispatch.boundTo(session))
    throw std::invalid_argument(
        "LLVM generated relocation uses a foreign dispatch session");
  const auto &program = session.program().mir();
  std::vector<SemanticType> path;
  const auto visit = [&](const auto &self, const SemanticType &selected,
                         bool base) -> void {
    if (std::find(path.begin(), path.end(), selected) != path.end())
      throw std::logic_error("LLVM inert relocation has a recursive layout");
    path.push_back(selected);
    if (selected.kind == SemanticType::Class) {
      const auto &owner = session.layouts().instance(selected);
      if (owner.compilerCapability ==
          CompilerCapabilityTypeKind::PolymorphicStorage) {
        static_cast<void>(session.layouts().polymorphicStorage(selected));
        path.pop_back();
        return;
      }
      if (!inert && sharedOwnerRepresentation(program, owner)) {
        static_cast<void>(session.layouts().get(selected));
        path.pop_back();
        return;
      }
      if (!inert && nativeMutexElement(program, selected,
                                       CompilerCapabilityTypeKind::MutexStorage)) {
        // A mutex relocates by rebuilding its native identity; only its
        // payload transfers, through the payload's own relocation.
        self(self, session.layouts().mutexStorage(selected).element, false);
        path.pop_back();
        return;
      }
      if (!inert &&
          owner.compilerCapability != CompilerCapabilityTypeKind::None &&
          mirTypeRelocationKind(program, selected) == RelocationKind::Bitwise) {
        static_cast<void>(session.layouts().get(selected));
        path.pop_back();
        return;
      }
      if (!inert && owner.unionLayout && !owner.requiresActiveCleanup &&
          !owner.requiresActiveDropState && !owner.polymorphic &&
          owner.bases.empty() && !owner.traits.containsBorrowedState &&
          owner.moveConstructor == SpecialMemberStatus::Generated &&
          std::all_of(owner.declaredFields.begin(), owner.declaredFields.end(),
                      [&](const auto &field) {
                        return mirTypeRelocationKind(program, field.type) ==
                               RelocationKind::Bitwise;
                      })) {
        // A union has no active-member tag. Only its verified passive storage
        // can be transferred; visiting every overlapping member would be wrong.
        static_cast<void>(session.layouts().get(selected));
        path.pop_back();
        return;
      }
      if (owner.compilerCapability != CompilerCapabilityTypeKind::None ||
          (inert &&
           (owner.requiresActiveCleanup || owner.requiresActiveDropState)) ||
          owner.traits.containsBorrowedState ||
          owner.lifecycleBoundBorrowCarrier || owner.unionLayout ||
          owner.cOpaqueHandle || (!base && owner.abstract) ||
          (!base && owner.moveConstructor != SpecialMemberStatus::Generated))
        throw BackendUnsupportedError("LLVM relocation requires an implemented "
                                      "generated borrow-free record");
      static_cast<void>(session.layouts().get(selected));
      if (owner.requiresActiveDropState !=
          session.layouts().activeIndex(selected).has_value())
        throw std::logic_error("LLVM relocation lost its sealed active state");
      // Base metadata is installed by the complete object's dispatch state.
      // A field or array element is a distinct complete object of its own.
      if (!base && owner.polymorphic)
        dispatch.preflightState(owner.id);
      for (const auto &entry : owner.bases) {
        const auto *child = program.findClassInstance(entry.instance);
        if (!child || child->type != entry.type)
          throw std::logic_error("LLVM inert relocation lost its exact base");
        static_cast<void>(
            session.layouts().baseIndex(selected, entry.instance));
        self(self, entry.type, true);
      }
      for (const auto &field : owner.declaredFields) {
        static_cast<void>(session.layouts().fieldIndex(selected, field.symbol));
        self(self, field.type, false);
      }
    } else if (const auto *payload = program.findPayloadEnum(selected)) {
      static_cast<void>(session.layouts().payload(selected));
      for (const auto &variant : payload->variants)
        for (const auto &field : variant.payloadTypes)
          self(self, field, false);
    } else if (selected.kind == SemanticType::Array) {
      if (selected.arguments.size() != 1 || selected.arrayLengthParameterId)
        throw BackendUnsupportedError(
            "LLVM inert relocation requires a concrete array extent");
      static_cast<void>(session.layouts().get(selected));
      self(self, selected.arguments.front(), false);
    } else if (!inert && selected.kind == SemanticType::Lambda) {
      const auto environment = closureEnvironmentLayout(program, selected);
      if (!environment ||
          environment->captureTypes.size() !=
              environment->captureModes.size() ||
          environment->captureTypes.size() !=
              environment->captureRequiresActiveCleanup.size())
        throw BackendUnsupportedError(
            "LLVM closure relocation requires one compatible environment");
      static_cast<void>(session.layouts().get(selected));
      for (std::size_t index = 0; index < environment->captureTypes.size();
           ++index) {
        if (isBorrowingCapture(environment->captureModes[index])) {
          if (environment->captureRequiresActiveCleanup[index])
            throw std::logic_error(
                "borrowed LLVM capture acquired owned cleanup state");
        } else {
          self(self, environment->captureTypes[index], false);
        }
      }
    } else if (!inert && selected.kind == SemanticType::UniqueOwner) {
      // Relocation transfers a handle, not the pointee object. The eventual
      // cleanup consumer must separately prove its complete pointee recipe.
      if (selected.arguments.size() != 1)
        throw std::logic_error("LLVM relocation lost its exact owner pointee");
      if (!mirUniqueCleanupType(program, selected))
        throw BackendUnsupportedError(
            "LLVM owner relocation requires its sealed pointee identity");
      static_cast<void>(session.layouts().get(selected));
    } else {
      // Other structural representations need a typed relocation schedule.
      // Never silently byte-copy a self-relative representation.
      if (mirTypeRelocationKind(program, selected) != RelocationKind::Bitwise ||
          (inert && semanticTraits(selected).containsBorrowedState) ||
          selected.kind == SemanticType::UniqueOwner)
        throw BackendUnsupportedError("LLVM inert relocation requires an "
                                      "implemented typed representation");
      static_cast<void>(session.layouts().get(selected));
    }
    path.pop_back();
  };
  if (mirTypeRelocationKind(program, type) == RelocationKind::Unavailable)
    throw BackendUnsupportedError(
        "LLVM inert relocation requires a sealed generated relocation");
  visit(visit, type, false);
}
} // namespace

void Lifetimes::preflightInertRelocation(Session &session, Dispatch &dispatch,
                                         const SemanticType &type) {
  preflightRelocation(session, dispatch, type, true);
}

void Lifetimes::preflightGeneratedRelocation(Session &session,
                                             Dispatch &dispatch,
                                             const SemanticType &type) {
  preflightRelocation(session, dispatch, type, false);
}

llvm::FunctionType *Lifetimes::relocationType() const {
  return llvm::FunctionType::get(
      llvm::Type::getVoidTy(session.context()),
      {session.layouts().pointer(), session.layouts().pointer()}, false);
}

llvm::Function *Lifetimes::inertRelocation(const SemanticType &type) {
  preflightInertRelocation(session, dispatch, type);
  return generatedRelocation(type);
}

llvm::Function *Lifetimes::generatedRelocation(const SemanticType &type) {
  // Validate the complete finite layout and dispatch graph before publishing
  // even a helper declaration. Failure cannot leave a partial cached helper.
  preflightGeneratedRelocation(session, dispatch, type);
  for (const auto &[previous, function] : relocations)
    if (previous == type)
      return function;
  auto *function = llvm::Function::Create(
      relocationType(), llvm::GlobalValue::InternalLinkage,
      "__gti_generated_relocate_" + std::to_string(relocations.size()),
      session.module());
  relocations.emplace_back(type, function);
  function->addFnAttr(llvm::Attribute::NoUnwind);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  const auto emit = [&](const auto &self, const SemanticType &selected,
                        llvm::Value *destination, llvm::Value *source,
                        bool base) -> void {
    const auto layout = session.layouts().get(selected);
    if (selected.kind == SemanticType::Class) {
      const auto &owner = session.layouts().instance(selected);
      if (owner.compilerCapability ==
          CompilerCapabilityTypeKind::PolymorphicStorage) {
        PolymorphicStorage(session, *this, dispatch)
            .relocate(ir, selected, destination, source);
        return;
      }
      if (sharedOwnerRepresentation(session.program().mir(), owner)) {
        ir.CreateStore(ir.CreateLoad(layout.storage, source), destination);
        ir.CreateStore(llvm::Constant::getNullValue(layout.storage), source);
        return;
      }
      if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage) {
        // Native mutex state never moves. The destination receives a fresh
        // native mutex and the relocated payload; the source's native
        // identity is retired and its storage left without live state.
        const auto mutex = session.layouts().mutexStorage(selected);
        auto *from = ir.CreateStructGEP(mutex.object.storage, source,
                                        mutex.payloadActiveIndex);
        auto *bits = ir.CreateLoad(ir.getInt8Ty(), from);
        Mutexes mutexes(session, ir);
        mutexes.initializeShell(destination, selected);
        auto *payload = llvm::BasicBlock::Create(session.context(),
                                                 "mutex.payload", function);
        auto *retire = llvm::BasicBlock::Create(session.context(),
                                                "mutex.retire", function);
        ir.CreateCondBr(
            ir.CreateICmpNE(
                ir.CreateAnd(bits, ir.getInt8(MutexStorageLayout::payloadLive)),
                ir.getInt8(0)),
            payload, retire);
        ir.SetInsertPoint(payload);
        ir.CreateCall(generatedRelocation(mutex.element),
                      {ir.CreateStructGEP(mutex.object.storage, destination,
                                          mutex.payloadIndex),
                       ir.CreateStructGEP(mutex.object.storage, source,
                                          mutex.payloadIndex)});
        ir.CreateStore(ir.getInt8(MutexStorageLayout::shellLive |
                                  MutexStorageLayout::payloadLive),
                       ir.CreateStructGEP(mutex.object.storage, destination,
                                          mutex.payloadActiveIndex));
        ir.CreateBr(retire);
        ir.SetInsertPoint(retire);
        ir.CreateStore(ir.getInt8(0), from);
        mutexes.destroyShell(source, selected);
        return;
      }
      if (owner.unionLayout ||
          owner.compilerCapability != CompilerCapabilityTypeKind::None) {
        ir.CreateMemCpy(destination, llvm::Align(layout.alignment), source,
                        llvm::Align(layout.alignment), layout.size);
        return;
      }
      for (const auto &entry : owner.bases) {
        const auto index =
            session.layouts().baseIndex(selected, entry.instance);
        self(self, entry.type,
             ir.CreateStructGEP(layout.storage, destination, index),
             ir.CreateStructGEP(layout.storage, source, index), true);
      }
      for (const auto &field : owner.declaredFields) {
        const auto index = session.layouts().fieldIndex(selected, field.symbol);
        ir.CreateCall(generatedRelocation(field.type),
                      {ir.CreateStructGEP(layout.storage, destination, index),
                       ir.CreateStructGEP(layout.storage, source, index)});
      }
      if (!base && owner.polymorphic)
        dispatch.install(ir, destination, owner.id);
      if (const auto active = session.layouts().activeIndex(selected)) {
        auto *from = ir.CreateStructGEP(layout.storage, source, *active);
        auto *to = ir.CreateStructGEP(layout.storage, destination, *active);
        ir.CreateStore(ir.CreateLoad(ir.getInt8Ty(), from), to);
        ir.CreateStore(ir.getInt8(0), from);
      }
    } else if (const auto *payload =
                   session.program().mir().findPayloadEnum(selected)) {
      const auto physical = session.layouts().payload(selected);
      auto *tag = ir.CreateLoad(ir.getInt32Ty(),
                                payloadTagAddress(ir, physical, source));
      auto *done =
          llvm::BasicBlock::Create(session.context(), "payload.done", function);
      auto *invalid = llvm::BasicBlock::Create(session.context(),
                                               "payload.invalid", function);
      // Empty storage has no active payload to read. Publish the destination
      // tag only after every selected field has reached its final address.
      ir.CreateStore(ir.getInt32(PayloadLayout::inactive),
                     payloadTagAddress(ir, physical, destination));
      auto *select =
          ir.CreateSwitch(tag, invalid, payload->variants.size() + 1);
      select->addCase(ir.getInt32(PayloadLayout::inactive), done);
      for (std::size_t variant = 0; variant < payload->variants.size();
           ++variant) {
        auto *block = llvm::BasicBlock::Create(session.context(),
                                               "payload.variant", function);
        select->addCase(ir.getInt32(variant), block);
        ir.SetInsertPoint(block);
        const auto &fields = payload->variants[variant].payloadTypes;
        for (std::size_t field = 0; field < fields.size(); ++field)
          ir.CreateCall(
              generatedRelocation(fields[field]),
              {payloadFieldAddress(ir, physical, destination, variant, field),
               payloadFieldAddress(ir, physical, source, variant, field)});
        ir.CreateStore(tag, payloadTagAddress(ir, physical, destination));
        ir.CreateStore(ir.getInt32(PayloadLayout::inactive),
                       payloadTagAddress(ir, physical, source));
        ir.CreateBr(done);
      }
      ir.SetInsertPoint(invalid);
      ir.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>{},
                         {});
      ir.CreateUnreachable();
      ir.SetInsertPoint(done);
    } else if (selected.kind == SemanticType::Array) {
      if (!selected.arrayLength)
        return;
      auto *entry = ir.GetInsertBlock();
      auto *loop =
          llvm::BasicBlock::Create(session.context(), "elements", function);
      auto *done =
          llvm::BasicBlock::Create(session.context(), "done", function);
      ir.CreateBr(loop);
      ir.SetInsertPoint(loop);
      auto *index = ir.CreatePHI(ir.getInt64Ty(), 2);
      index->addIncoming(ir.getInt64(0), entry);
      ir.CreateCall(
          generatedRelocation(selected.arguments.front()),
          {ir.CreateGEP(layout.storage, destination, {ir.getInt64(0), index}),
           ir.CreateGEP(layout.storage, source, {ir.getInt64(0), index})});
      auto *next = ir.CreateAdd(index, ir.getInt64(1));
      index->addIncoming(next, ir.GetInsertBlock());
      ir.CreateCondBr(ir.CreateICmpULT(next, ir.getInt64(selected.arrayLength)),
                      loop, done);
      ir.SetInsertPoint(done);
    } else if (selected.kind == SemanticType::Lambda) {
      const auto environment =
          *closureEnvironmentLayout(session.program().mir(), selected);
      for (std::size_t index = 0; index < environment.captureTypes.size();
           ++index) {
        auto *to = ir.CreateStructGEP(layout.storage, destination, index);
        auto *from = ir.CreateStructGEP(layout.storage, source, index);
        if (isBorrowingCapture(environment.captureModes[index])) {
          // Relocating the closure preserves the already-verified loan; it
          // neither transfers nor retires the borrowed object's drop state.
          ir.CreateStore(ir.CreateLoad(session.layouts().pointer(), from), to);
        } else {
          ir.CreateCall(generatedRelocation(environment.captureTypes[index]),
                        {to, from});
        }
      }
    } else if (selected.kind == SemanticType::UniqueOwner) {
      ir.CreateStore(ir.CreateLoad(layout.storage, source), destination);
      ir.CreateStore(llvm::Constant::getNullValue(layout.storage), source);
    } else if (layout.size) {
      ir.CreateMemCpy(destination, llvm::Align(layout.alignment), source,
                      llvm::Align(layout.alignment), layout.size);
    }
  };
  emit(emit, type, function->getArg(0), function->getArg(1), false);
  ir.CreateRetVoid();
  return function;
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
    if (owner.compilerCapability ==
        CompilerCapabilityTypeKind::MutexLockToken) {
      ir.CreateStore(
          llvm::ConstantPointerNull::get(session.layouts().pointer()), address);
      ir.CreateRetVoid();
      return function;
    }
    if (owner.compilerCapability == CompilerCapabilityTypeKind::ThreadHandle) {
      // The join obligation (and any joined result) moved with the bytes.
      if (!nativeThreadHandle(session.program().mir(), type))
        throw BackendUnsupportedError(
            "LLVM thread handle move requires its exact capability");
      ir.CreateStore(llvm::Constant::getNullValue(storage), address);
      ir.CreateRetVoid();
      return function;
    }
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
  if (owner.compilerCapability == CompilerCapabilityTypeKind::MutexLockToken ||
      owner.compilerCapability == CompilerCapabilityTypeKind::MutexStorage ||
      owner.compilerCapability == CompilerCapabilityTypeKind::ThreadHandle)
    return true;
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

llvm::Function *Lifetimes::cleanup(const MirDropType &recipe,
                                   bool replacement) {
  struct CleanupProof {
    const MirDropType *recipe = nullptr;
    std::vector<CleanupProof> aggregate;
    std::optional<std::size_t> recursiveTarget;
  };
  struct ProofAncestor {
    const MirDropType *recipe = nullptr;
    std::size_t ownerDepth = 0;
  };

  const auto &program = session.program().mir();
  const auto activeCleanup =
      [&](const auto &self, const SemanticType &type) -> std::optional<bool> {
    switch (type.kind) {
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
      return true;
    case SemanticType::Array:
      return type.arguments.size() == 1 ? self(self, type.arguments.front())
                                        : std::nullopt;
    case SemanticType::Class: {
      const MirClassInstance *selected = nullptr;
      for (const auto &candidate : program.classInstances())
        if (candidate.type == type) {
          if (selected)
            return std::nullopt;
          selected = &candidate;
        }
      return selected ? std::optional<bool>{selected->requiresActiveCleanup}
                      : std::nullopt;
    }
    case SemanticType::Enum: {
      const auto *enumeration = program.findPayloadEnum(type);
      return enumeration
                 ? std::optional<bool>{enumeration->requiresActiveCleanup}
                 : std::optional<bool>{false};
    }
    case SemanticType::Lambda: {
      std::optional<bool> result;
      for (const auto &instance : program.lambdaInstances()) {
        if (instance.type != type)
          continue;
        if (instance.captureTypes.size() !=
            instance.captureRequiresActiveCleanup.size())
          return std::nullopt;
        const bool current =
            std::any_of(instance.captureRequiresActiveCleanup.begin(),
                        instance.captureRequiresActiveCleanup.end(),
                        [](bool value) { return value; });
        if (result && *result != current)
          return std::nullopt;
        result = current;
      }
      return result;
    }
    default:
      return false;
    }
  };
  std::vector<ProofAncestor> ancestors;
  const auto prove = [&](const auto &self, const MirDropType &selected,
                         const MirDropType &expected,
                         std::size_t ownerDepth) -> CleanupProof {
    if (selected.type != expected.type)
      throw std::logic_error("LLVM cleanup lost its exact sealed type");
    auto selectedHeader = selected;
    auto expectedHeader = expected;
    selectedHeader.aggregate.clear();
    expectedHeader.aggregate.clear();
    if (selected.type.kind == SemanticType::Lambda) {
      const auto *instance = closureCleanupInstance(program, selected);
      const auto environment = closureEnvironmentLayout(program, selected.type);
      if (!instance || !environment ||
          instance->captureTypes.size() != instance->captureModes.size() ||
          instance->captureTypes.size() !=
              instance->captureRequiresActiveCleanup.size() ||
          *environment != closureEnvironmentLayout(*instance))
        throw std::logic_error(
            "LLVM closure cleanup lost its exact compatible identity");
      // Physical agreement never selects a cleanup body. Keep the exact
      // instance in `selected`; ignore only the canonical query's first-row ID
      // while comparing the remaining sealed recipe shape.
      selectedHeader.lambdaInstance.reset();
      expectedHeader.lambdaInstance.reset();
    }
    if (selectedHeader != expectedHeader ||
        selected.aggregate.size() != expected.aggregate.size())
      throw std::logic_error("LLVM cleanup lost its exact sealed recipe graph");
    if (selected.polymorphicCleanup) {
      // Dynamic selection happens through the descriptor at drop time; the
      // static recipe has no pointee children to prove here.
      static_cast<void>(dynamicOwnerView(session, selected));
      return {.recipe = &selected};
    }
    CleanupProof result{.recipe = &selected};
    if (selected.recursiveCleanup) {
      for (std::size_t index = ancestors.size(); index != 0; --index) {
        const auto &ancestor = ancestors[index - 1];
        if (!ancestor.recipe->recursiveCleanup &&
            ancestor.recipe->type == selected.type &&
            ancestor.recipe->classInstance == selected.classInstance) {
          if (ownerDepth <= ancestor.ownerDepth)
            throw std::logic_error(
                "LLVM recursive cleanup requires an owning pointee edge");
          result.recursiveTarget = index - 1;
          return result;
        }
      }
      throw std::logic_error(
          "LLVM recursive cleanup lost its exact enclosing recipe");
    }

    // An indirect owner does not make its pointee's inline subobjects dynamic.
    // Prove their exact static schedules at the child edge, including dispatch
    // state, before publishing any helper. Recursive edges reuse that proof.
    // Replacing an owner retires its allocation; it does not replace a
    // hierarchy object in-place inside a still-live enclosing object.
    if (&selected != &recipe &&
        containsHierarchyStorage(program, selected.type))
      preflightCleanup(session, dispatch, selected,
                       replacement && ownerDepth == 0);

    ancestors.push_back({&selected, ownerDepth});
    const bool ownsChild = selected.type.kind == SemanticType::UniqueOwner;
    result.aggregate.reserve(selected.aggregate.size());
    for (std::size_t index = 0; index < selected.aggregate.size(); ++index)
      result.aggregate.push_back(
          self(self, selected.aggregate[index], expected.aggregate[index],
               ownerDepth + static_cast<std::size_t>(ownsChild)));
    ancestors.pop_back();
    return result;
  };
  const auto mirror = [&](const auto &self,
                          const MirDropType &selected) -> CleanupProof {
    if (selected.recursiveCleanup || selected.polymorphicCleanup)
      throw std::logic_error(
          "LLVM static cleanup acquired dynamic cleanup metadata");
    CleanupProof result{.recipe = &selected};
    result.aggregate.reserve(selected.aggregate.size());
    for (const auto &child : selected.aggregate)
      result.aggregate.push_back(self(self, child));
    return result;
  };

  // `prove` checks static hierarchy schedules at every owned child edge, not
  // only at the root. Dynamic-owner recipes remain independently unsupported.
  const bool inlineHierarchy = containsHierarchyStorage(program, recipe.type);
  const auto canonical = mirUniqueCleanupType(program, recipe.type);
  CleanupProof proof;
  if (canonical) {
    if (inlineHierarchy)
      preflightCleanup(session, dispatch, recipe, replacement);
    proof = prove(prove, recipe, *canonical, 0);
  } else {
    const auto active = activeCleanup(activeCleanup, recipe.type);
    if (!active || *active != recipe.requiresActiveCleanup)
      throw std::logic_error(
          "LLVM cleanup fallback lost its exact active-state fact");
    if (*active)
      throw BackendUnsupportedError(
          "LLVM active cleanup requires an exact family recipe");
    if (recipe.uniquePointeeClass || recipe.polymorphicCleanup ||
        recipe.recursiveCleanup || !recipe.aggregate.empty() ||
        recipe.uniqueOwnerField || recipe.uniqueOwnerBase ||
        recipe.captureIndex || recipe.payloadVariant || recipe.payloadField)
      throw std::logic_error(
          "LLVM cleanup fallback retained unsupported family metadata");
    if (recipe.type.kind == SemanticType::Array) {
      const auto *schedule = recipe.fixedArray ? &*recipe.fixedArray : nullptr;
      SemanticType leaf = recipe.type;
      std::vector<std::size_t> extents;
      while (leaf.kind == SemanticType::Array && leaf.arguments.size() == 1 &&
             !leaf.arrayLengthParameterId && leaf.arrayLength) {
        extents.push_back(leaf.arrayLength);
        SemanticType element = leaf.arguments.front();
        leaf = std::move(element);
      }
      const auto *leafClass =
          schedule && schedule->leafClassInstance
              ? program.findClassInstance(*schedule->leafClassInstance)
              : nullptr;
      const bool specialized = leaf.kind == SemanticType::Class && leafClass &&
                               leafClass->type.kind == SemanticType::Class &&
                               leafClass->type.classId == leaf.classId &&
                               leafClass->type.arguments.empty() &&
                               leafClass->type.valueArguments.empty();
      const bool exactLeaf =
          leaf.kind != SemanticType::Class
              ? schedule && !schedule->leafClassInstance && !recipe.destructor
              : leafClass && (leafClass->type == leaf || specialized) &&
                    leafClass->destructor == recipe.destructor;
      if (recipe.classInstance || recipe.lambdaInstance || !schedule ||
          schedule->extents != extents || schedule->leafType != leaf ||
          !exactLeaf)
        throw std::logic_error(
            "LLVM fixed-array cleanup lost its exact passive schedule");
    } else if (recipe.type.kind == SemanticType::Class) {
      const auto *owner = recipe.classInstance
                              ? program.findClassInstance(*recipe.classInstance)
                              : nullptr;
      if (!owner || recipe.lambdaInstance || recipe.fixedArray ||
          owner->type != recipe.type || owner->destructor != recipe.destructor)
        throw std::logic_error(
            "LLVM cleanup fallback lost its exact class identity");
      if (recipe.destructor) {
        const auto *target = program.findDestructorInstance(*recipe.destructor);
        if (!target || target->owner != owner->id)
          throw std::logic_error(
              "LLVM cleanup fallback selected a foreign destructor");
      }
    } else if (recipe.classInstance || recipe.lambdaInstance ||
               recipe.destructor || recipe.fixedArray) {
      throw std::logic_error(
          "LLVM cleanup fallback retained foreign type metadata");
    }
    preflightCleanup(session, dispatch, recipe, replacement);
    proof = mirror(mirror, recipe);
  }

  struct EmissionEntry {
    const CleanupProof *proof = nullptr;
    llvm::Function *function = nullptr;
  };
  struct EmissionContext {
    bool replacement = false;
    std::vector<EmissionEntry> stack;
  } context{.replacement = replacement};

  const auto emit = [&](const auto &self,
                        const CleanupProof &selectedProof) -> llvm::Function * {
    const auto &recipe = *selectedProof.recipe;
    if (selectedProof.recursiveTarget) {
      if (*selectedProof.recursiveTarget >= context.stack.size())
        throw std::logic_error(
            "LLVM recursive cleanup lost its validated ancestor");
      const auto &target = context.stack[*selectedProof.recursiveTarget];
      if (!target.proof || !target.function ||
          target.proof->recipe->recursiveCleanup ||
          target.proof->recipe->type != recipe.type ||
          target.proof->recipe->classInstance != recipe.classInstance)
        throw std::logic_error(
            "LLVM recursive cleanup changed its validated ancestor");
      return target.function;
    }
    for (const auto &[previous, function] : cleanups)
      if (previous == recipe)
        return function;
    auto *pointer = session.layouts().pointer();
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(
            llvm::Type::getInt1Ty(session.context()),
            {pointer, pointer, llvm::Type::getInt1Ty(session.context())},
            false),
        llvm::GlobalValue::InternalLinkage,
        "__gti_native_cleanup_" + std::to_string(cleanups.size()),
        session.module());
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
    function->setUWTableKind(llvm::UWTableKind::Sync);
#endif
    cleanups.emplace_back(recipe, function);
    context.stack.push_back({&selectedProof, function});
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    llvm::Value *address = function->getArg(0);
    auto *output = function->getArg(1);
    auto *duringFailure = function->getArg(2);
    auto *failed = ir.CreateAlloca(ir.getInt1Ty());
    llvm::Value *cleanupToken = nullptr;
    auto *scratch = ir.CreateAlloca(session.abi().failureChannel());
    ir.CreateStore(ir.getFalse(), failed);
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
    auto *unwindType =
        llvm::StructType::get(session.context(), {pointer, ir.getInt32Ty()});
    llvm::AllocaInst *unwindState = nullptr;
#endif
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
      auto *done = llvm::BasicBlock::Create(session.context(),
                                            "shared.released", function);
      ir.CreateCondBr(released.last, last, done);
      ir.SetInsertPoint(done);
      ir.CreateRet(ir.getTrue());
      ir.SetInsertPoint(last);
      // A polymorphic pointee is released through this handle's own view,
      // from which its descriptor selects the complete object and cleanup.
      sharedPointee = ir.CreateAlloca(pointer);
      ir.CreateStore(recipe.aggregate[0].polymorphicCleanup ? released.view
                                                            : released.allocation,
                     sharedPointee);
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
      ir.CreateStore(
          llvm::ConstantPointerNull::get(session.layouts().pointer()), address);
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

    if (recipe.requiresActiveCleanup && recipe.classInstance &&
        session.layouts().instance(recipe.type).polymorphic) {
      auto *install = llvm::BasicBlock::Create(session.context(),
                                               "dispatch.install", function);
      auto *ready = llvm::BasicBlock::Create(session.context(),
                                             "dispatch.ready", function);
      ir.CreateCondBr(ir.CreateCall(cleanupObservation(recipe), {address}),
                      install, ready);
      ir.SetInsertPoint(install);
      dispatch.installPhase(ir, address,
                            destructionPhase(session.program().mir(), recipe));
      ir.CreateBr(ready);
      ir.SetInsertPoint(ready);
    }

    // Save the first normal-cleanup failure, drain remaining children, and stop
    // immediately on a second failure. A caller already unwinding owns its
    // first record, so return the first new failure without replacing that
    // record.
    const auto observe = [&](llvm::Value *status) {
      if (!cleanupToken) {
        llvm::IRBuilder<> entry(&function->getEntryBlock(),
                                function->getEntryBlock().begin());
        cleanupToken = entry.CreateAlloca(session.layouts().pointer());
      }
      auto *bad =
          llvm::BasicBlock::Create(session.context(), "failed", function);
      auto *next =
          llvm::BasicBlock::Create(session.context(), "next", function);
      ir.CreateCondBr(status, next, bad);
      ir.SetInsertPoint(bad);
      // A published managed-worker emergency abandons the remaining schedule
      // by status. An abandoned child wrote no record to read.
      const auto abandonIfRequested = [&] {
        if (!failures.workerContainment())
          return;
        auto *abandon = llvm::BasicBlock::Create(session.context(),
                                                 "worker.abandon", function);
        auto *resume = llvm::BasicBlock::Create(session.context(),
                                                "worker.continue", function);
        ir.CreateCondBr(failures.abandoning(ir), abandon, resume);
        ir.SetInsertPoint(abandon);
        ir.CreateRet(ir.getFalse());
        ir.SetInsertPoint(resume);
      };
      abandonIfRequested();
      failures.checkIncomingCleanup(ir, scratch);
      abandonIfRequested();
      auto *secondary =
          llvm::BasicBlock::Create(session.context(), "secondary", function);
      auto *first =
          llvm::BasicBlock::Create(session.context(), "first", function);
      ir.CreateCondBr(ir.CreateLoad(ir.getInt1Ty(), failed), secondary, first);
      ir.SetInsertPoint(secondary);
      failures.terminateCleanup(ir, output, scratch);
      if (failures.workerContainment())
        ir.CreateRet(ir.getFalse());
      ir.SetInsertPoint(first);
      failures.copyChannel(ir, output, scratch);
      auto *resume =
          llvm::BasicBlock::Create(session.context(), "resume", function);
      auto *stop =
          llvm::BasicBlock::Create(session.context(), "stop", function);
      ir.CreateCondBr(duringFailure, stop, resume);
      ir.SetInsertPoint(stop);
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
      ir.CreateResume(ir.CreateLoad(unwindType, unwindState));
#else
      ir.CreateRet(ir.getFalse());
#endif
      ir.SetInsertPoint(resume);
      ir.CreateStore(failures.enterCleanup(ir, output), cleanupToken);
      ir.CreateStore(ir.getTrue(), failed);
      ir.CreateBr(next);
      ir.SetInsertPoint(next);
    };
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
    // A native exception must not skip the remaining verified child recipes.
    // Convert only this helper's unwind edge into the existing cleanup state;
    // after the schedule drains, resume the original token, never swallow it.
    const auto evaluationCall = [&](llvm::FunctionCallee callee,
                                    llvm::ArrayRef<llvm::Value *> arguments) {
      if (!unwindState) {
        auto personality = session.module().getOrInsertFunction(
            "__gxx_personality_v0",
            llvm::FunctionType::get(ir.getInt32Ty(), true));
        function->setPersonalityFn(
            llvm::cast<llvm::Constant>(personality.getCallee()));
        llvm::IRBuilder<> entry(&function->getEntryBlock(),
                                function->getEntryBlock().begin());
        unwindState = entry.CreateAlloca(unwindType, nullptr, "c04.unwind");
      }
      auto *next = llvm::BasicBlock::Create(session.context(),
                                            "c04.cleanup.next", function);
      auto *unwind = llvm::BasicBlock::Create(session.context(),
                                              "c04.cleanup.unwind", function);
      auto *call = ir.CreateInvoke(callee, next, unwind, arguments);
      ir.SetInsertPoint(unwind);
      auto *landing = ir.CreateLandingPad(unwindType, 0);
      landing->setCleanup(true);
      ir.CreateStore(landing, unwindState);
      auto transfer = session.module().getOrInsertFunction(
          "__gti_c04_evaluate_current",
          llvm::FunctionType::get(ir.getVoidTy(), {pointer, pointer}, false));
      ir.CreateCall(transfer, {scratch, ir.CreateExtractValue(landing, 0)})
          ->setDoesNotThrow();
      observe(ir.getFalse());
      ir.CreateBr(next);
      ir.SetInsertPoint(next);
      return call;
    };
#endif
    const auto child = [&](const MirDropType &selected, llvm::Value *where) {
      const auto edge =
          std::find_if(recipe.aggregate.begin(), recipe.aggregate.end(),
                       [&](const MirDropType &candidate) {
                         return &candidate == &selected;
                       });
      if (edge == recipe.aggregate.end() ||
          selectedProof.aggregate.size() != recipe.aggregate.size())
        throw std::logic_error(
            "LLVM cleanup emission lost its validated child edge");
      const auto index =
          static_cast<std::size_t>(edge - recipe.aggregate.begin());
      auto *target = self(self, selectedProof.aggregate[index]);
      const std::vector<llvm::Value *> arguments{
          where, scratch,
          ir.CreateOr(duringFailure, ir.CreateLoad(ir.getInt1Ty(), failed))};
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
      evaluationCall(target, arguments);
#else
      observe(ir.CreateCall(target, arguments));
#endif
    };
    llvm::Value *release = allocation;
    if (shared) {
      child(recipe.aggregate[0], sharedPointee);
      Memory memory(session, ir, {}, {}, {});
      memory.releaseBytes(sharedBlock);
    } else if (recipe.polymorphicCleanup) {
      // Capture the complete object and its selected cleanup from the live
      // descriptor before any lifecycle transition; that cleanup reinstalls
      // base states. The complete object's address is the allocation.
      const auto &view = dynamicOwnerView(session, recipe);
      if (!unique || objectType != view.type)
        throw std::logic_error(
            "LLVM dynamic owner cleanup lost its exact owner handle");
      const auto selected =
          dispatch.loadCompleteCleanup(ir, address, view.type);
      auto *type = llvm::FunctionType::get(
          ir.getInt1Ty(), {pointer, pointer, ir.getInt1Ty()}, false);
      const std::vector<llvm::Value *> arguments{
          selected.complete, scratch,
          ir.CreateOr(duringFailure, ir.CreateLoad(ir.getInt1Ty(), failed))};
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
      evaluationCall(llvm::FunctionCallee(type, selected.cleanup), arguments);
#else
      observe(ir.CreateCall(type, selected.cleanup, arguments));
#endif
      release = selected.complete;
    } else if (nativeMutexElement(program, recipe.type,
                                  CompilerCapabilityTypeKind::MutexStorage)) {
      const auto expected = mirUniqueCleanupType(program, recipe.type);
      if (!expected || !recipe.requiresActiveCleanup ||
          recipe.classInstance != expected->classInstance ||
          recipe.aggregate != expected->aggregate || recipe.destructor ||
          recipe.fixedArray)
        throw std::logic_error(
            "LLVM mutex cleanup lost its sealed shell/payload recipe");
      const auto layout = session.layouts().mutexStorage(recipe.type);
      auto *state = ir.CreateStructGEP(layout.object.storage, address,
                                       layout.payloadActiveIndex);
      auto *bits = ir.CreateLoad(ir.getInt8Ty(), state);
      auto *live = llvm::BasicBlock::Create(session.context(), "mutex.live", function);
      auto *done = llvm::BasicBlock::Create(session.context(), "mutex.done", function);
      ir.CreateCondBr(
          ir.CreateICmpNE(ir.CreateAnd(bits, ir.getInt8(MutexStorageLayout::shellLive)),
                           ir.getInt8(0)), live, done);
      ir.SetInsertPoint(live);
      ir.CreateStore(ir.getInt8(MutexStorageLayout::shellLive), state);
      if (!recipe.aggregate.empty()) {
        auto *payload = llvm::BasicBlock::Create(session.context(), "mutex.payload", function);
        auto *retire = llvm::BasicBlock::Create(session.context(), "mutex.retire", function);
        ir.CreateCondBr(
            ir.CreateICmpNE(ir.CreateAnd(bits, ir.getInt8(MutexStorageLayout::payloadLive)),
                             ir.getInt8(0)), payload, retire);
        ir.SetInsertPoint(payload);
        child(recipe.aggregate.front(),
              ir.CreateStructGEP(layout.object.storage, address, layout.payloadIndex));
        ir.CreateBr(retire);
        ir.SetInsertPoint(retire);
      }
      ir.CreateStore(ir.getInt8(0), state);
      Mutexes(session, ir).destroyShell(address, recipe.type);
      ir.CreateBr(done);
      ir.SetInsertPoint(done);
    } else if (recipe.type.kind == SemanticType::Enum &&
               session.program().mir().findPayloadEnum(recipe.type)) {
      const auto &layout = session.layouts().payload(recipe.type);
      auto *tagAddress = payloadTagAddress(ir, layout, address);
      auto *tag = ir.CreateLoad(ir.getInt32Ty(), tagAddress);
      ir.CreateStore(ir.getInt32(PayloadLayout::inactive), tagAddress);
      auto *done =
          llvm::BasicBlock::Create(session.context(), "payload.done", function);
      auto *dispatch = ir.CreateSwitch(tag, done);
      for (std::size_t variant = 0; variant < layout.variants.size();
           ++variant) {
        auto *run =
            llvm::BasicBlock::Create(session.context(), "payload", function);
        dispatch->addCase(ir.getInt32(variant), run);
        ir.SetInsertPoint(run);
        for (const auto &selected : recipe.aggregate)
          if (selected.payloadVariant == variant) {
            if (!selected.payloadField)
              throw std::logic_error(
                  "LLVM payload cleanup lost its exact field");
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
      const auto *owner =
          closureCleanupInstance(session.program().mir(), recipe);
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
        if (!selected.captureIndex ||
            *selected.captureIndex != expected[step] ||
            selected.type != owner->captureTypes[expected[step]] ||
            isBorrowingCapture(owner->captureModes[expected[step]]) ||
            selected.uniqueOwnerField != 0 || selected.uniqueOwnerBase ||
            selected.payloadVariant || selected.payloadField)
          throw std::logic_error(
              "LLVM closure cleanup lost an exact owned capture projection");
        child(selected, ir.CreateStructGEP(
                            session.layouts().get(recipe.type).storage, address,
                            static_cast<unsigned>(expected[step])));
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
        auto *run =
            llvm::BasicBlock::Create(session.context(), "body", function);
        auto *done =
            llvm::BasicBlock::Create(session.context(), "body.done", function);
        ir.CreateCondBr(ir.CreateICmpNE(ir.CreateLoad(ir.getInt8Ty(), state),
                                        ir.getInt8(0)),
                        run, done);
        ir.SetInsertPoint(run);
        ir.CreateStore(ir.getInt8(0), state);
        const auto abi = session.abi().gti(
            SemanticType::Void, {}, target->mayRaiseDefinedFailure, true);
        std::vector<llvm::Value *> arguments;
        if (abi.failureDestination)
          arguments.push_back(scratch);
        arguments.push_back(address);
        auto *callee = findBody({MirBodyKind::Destructor, target->id});
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
        auto *status = evaluationCall(callee, arguments);
#else
        auto *status = ir.CreateCall(callee, arguments);
#endif
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
          index = session.layouts().baseIndex(recipe.type,
                                              *selected.uniqueOwnerBase);
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
      memory.releaseBytes(release);
    }
    if (cleanupToken) {
      auto *failedResult = ir.CreateLoad(ir.getInt1Ty(), failed);
      auto *leave = llvm::BasicBlock::Create(session.context(),
                                             "cleanup.context.leave", function);
      auto *result = llvm::BasicBlock::Create(session.context(),
                                              "cleanup.result", function);
      ir.CreateCondBr(failedResult, leave, result);
      ir.SetInsertPoint(leave);
      failures.leaveCleanup(
          ir, ir.CreateLoad(session.layouts().pointer(), cleanupToken));
#if defined(GTI_LLVM_C04_NATIVE_UNWIND)
      ir.CreateResume(ir.CreateLoad(unwindType, unwindState));
      ir.SetInsertPoint(result);
      ir.CreateRet(ir.getTrue());
#else
      ir.CreateBr(result);
      ir.SetInsertPoint(result);
      ir.CreateRet(ir.CreateNot(failedResult));
#endif
    } else {
      ir.CreateRet(ir.getTrue());
    }
    context.stack.pop_back();
    return function;
  };
  return emit(emit, proof);
}

} // namespace lang::native_llvm
