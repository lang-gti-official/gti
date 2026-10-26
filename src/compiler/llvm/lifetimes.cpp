#include "lifetimes.h"
#include "closure_environment.h"
#include "gti/mir_effects.h"
#include "memory.h"
#include "payloads.h"

#include <algorithm>
#include <set>

namespace lang::native_llvm {

Lifetimes::Lifetimes(Session &session, Failures &failures, Dispatch &dispatch,
                     FindBody findBody)
    : session(session), failures(failures), dispatch(dispatch),
      findBody(std::move(findBody)) {}

void Lifetimes::preflightCleanup(Session &session, Dispatch &dispatch,
                                 const MirDropType &recipe, bool replacement) {
  if (!dispatch.boundTo(session))
    throw std::invalid_argument(
        "LLVM cleanup preflight uses a foreign dispatch session");
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
      if (owner.polymorphic && selected.requiresActiveCleanup)
        dispatch.preflightState(owner.id);
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
  if ((!construct && !base) || instruction.tombstone ||
      instruction.constructorTarget ||
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
    dispatch.preflightState(object.id);
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
  if (object.polymorphic)
    for (const auto &view : dispatch.views(object.id))
      static_cast<void>(dispatch.table(object.id, view.viewClass));
  ir.CreateStore(llvm::Constant::getNullValue(storage), address);
  if (object.polymorphic)
    dispatch.install(ir, address, object.id);
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
  // Generated scalar transfer has no source callback or runtime failure point.
  // Copy logical fields only. Even a byte-identical static layout does not
  // authorize transporting source vptrs, padding, or active-state bytes.
  const auto copy = [&](const auto &self, const MirClassInstance &object,
                        llvm::Value *from, llvm::Value *to) -> void {
    const auto storage = session.layouts().get(object.type).storage;
    for (const auto &base : object.bases) {
      const auto index =
          session.layouts().baseIndex(object.type, base.instance);
      self(self, *session.program().mir().findClassInstance(base.instance),
           ir.CreateStructGEP(storage, from, index),
           ir.CreateStructGEP(storage, to, index));
    }
    for (const auto &field : object.declaredFields) {
      const auto index =
          session.layouts().fieldIndex(object.type, field.symbol);
      auto *value = ir.CreateLoad(session.layouts().get(field.type).storage,
                                  ir.CreateStructGEP(storage, from, index));
      ir.CreateStore(value, ir.CreateStructGEP(storage, to, index));
    }
    if (const auto index = session.layouts().activeIndex(object.type)) {
      // A copy constructs a new live subobject. A move transfers the actual
      // own-destructor permission, including already-suppressed state; it must
      // not unconditionally arm destination flags.
      llvm::Value *active = ir.getInt8(1);
      if (transfer.kind == ConstructorKind::Move)
        active = ir.CreateLoad(ir.getInt8Ty(),
                               ir.CreateStructGEP(storage, from, *index));
      ir.CreateStore(active, ir.CreateStructGEP(storage, to, *index));
    }
  };
  copy(copy, owner, source, destination);
  if (owner.polymorphic)
    dispatch.install(ir, destination, owner.id);
  // Suppression belongs to this verified whole-source transfer commit, never
  // operand materialization. All payload, destination flags and headers are
  // ready first; no source failure or callback can observe an intermediate.
  if (transfer.kind == ConstructorKind::Move)
    moved(ir, source, owner.type);
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
    if (selected.polymorphicCleanup)
      throw BackendUnsupportedError(
          "LLVM cleanup requires its later ownership/dispatch milestone");
    if (selected.type.kind == SemanticType::UniqueOwner) {
      if (selected.type.arguments.size() != 1)
        throw std::logic_error(
            "LLVM owner cleanup lost its exact pointee type");
      if (containsHierarchyStorage(program, selected.type.arguments.front()))
        throw BackendUnsupportedError(
            "LLVM owner cleanup for hierarchy pointees requires dynamic "
            "cleanup dispatch");
    } else if (selected.type.kind == SemanticType::Class) {
      const auto &owner = session.layouts().instance(selected.type);
      if (sharedOwnerRepresentation(program, owner)) {
        if (selected.type.arguments.size() != 1)
          throw std::logic_error(
              "LLVM shared cleanup lost its exact pointee type");
        if (containsHierarchyStorage(program, selected.type.arguments.front()))
          throw BackendUnsupportedError(
              "LLVM shared cleanup for hierarchy pointees requires dynamic "
              "cleanup dispatch");
      }
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

  // Inline hierarchy state retains O10's strict non-emitting preflight,
  // including replacement and dispatch checks. Owner handles are indirect;
  // `prove` checks each exact pointee separately before any helper/cache use.
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
    cleanups.emplace_back(recipe, function);
    context.stack.push_back({&selectedProof, function});
    llvm::IRBuilder<> ir(
        llvm::BasicBlock::Create(session.context(), "entry", function));
    llvm::Value *address = function->getArg(0);
    auto *output = function->getArg(1);
    auto *duringFailure = function->getArg(2);
    auto *failed = ir.CreateAlloca(ir.getInt1Ty());
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
      auto *done = llvm::BasicBlock::Create(session.context(),
                                            "shared.released", function);
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
      dispatch.install(ir, address, *recipe.classInstance);
      ir.CreateBr(ready);
      ir.SetInsertPoint(ready);
    }

    // Save the first normal-cleanup failure, drain remaining children, and stop
    // immediately on a second failure. A caller already unwinding owns its
    // first record, so return the first new failure without replacing that
    // record.
    const auto observe = [&](llvm::Value *status) {
      auto *bad =
          llvm::BasicBlock::Create(session.context(), "failed", function);
      auto *next =
          llvm::BasicBlock::Create(session.context(), "next", function);
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
      auto *stop =
          llvm::BasicBlock::Create(session.context(), "stop", function);
      ir.CreateCondBr(duringFailure, stop, resume);
      ir.SetInsertPoint(stop);
      ir.CreateRet(ir.getFalse());
      ir.SetInsertPoint(resume);
      ir.CreateStore(ir.getTrue(), failed);
      ir.CreateBr(next);
      ir.SetInsertPoint(next);
    };
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
      observe(ir.CreateCall(
          self(self, selectedProof.aggregate[index]),
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
      memory.releaseBytes(allocation);
    }
    ir.CreateRet(ir.CreateNot(ir.CreateLoad(ir.getInt1Ty(), failed)));
    context.stack.pop_back();
    return function;
  };
  return emit(emit, proof);
}

} // namespace lang::native_llvm
