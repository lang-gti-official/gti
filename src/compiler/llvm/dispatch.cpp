#include "dispatch.h"
#include "reachability.h"

#include <algorithm>
#include <limits>
#include <llvm/Support/MathExtras.h>
#include <stdexcept>
#include <unordered_set>

namespace lang::native_llvm {

namespace {
SemanticType receiverType(const MirInstruction &instruction) {
  if (!instruction.receiver)
    throw std::invalid_argument(
        "LLVM member invocation has no sealed receiver");
  auto type = instruction.receiver->type;
  if (instruction.unsafeOperation == UnsafeOperationKind::RawMember &&
      type.kind == SemanticType::RawPointer && type.arguments.size() == 1) {
    auto pointee = type.arguments.front();
    type = std::move(pointee);
  }
  if (type.kind != SemanticType::Class)
    throw BackendUnsupportedError(
        "LLVM member receiver requires its exact concrete class type");
  return type;
}

bool sameCallContract(const MirFunctionInstance &left,
                      const MirFunctionInstance &right) {
  return left.returnType == right.returnType &&
         left.parameterTypes == right.parameterTypes &&
         left.receiverMutability == right.receiverMutability &&
         left.overloadedOperator == right.overloadedOperator &&
         left.requiresUnsafe == right.requiresUnsafe &&
         left.tracksCaller == right.tracksCaller &&
         left.neverReturns == right.neverReturns &&
         left.linkage == right.linkage &&
         left.returnBorrowOrigin == right.returnBorrowOrigin &&
         left.returnBorrowParameter == right.returnBorrowParameter &&
         left.returnBorrowAccess == right.returnBorrowAccess &&
         left.returnBorrowParameters == right.returnBorrowParameters &&
         left.returnBorrowFields == right.returnBorrowFields &&
         left.completeReturnBorrow == right.completeReturnBorrow &&
         left.returnBorrowPlace == right.returnBorrowPlace &&
         left.restoresReferenceResult == right.restoresReferenceResult &&
         left.restoredBorrowFields == right.restoredBorrowFields;
}

llvm::Value *projectBase(Session &session, llvm::IRBuilder<> &ir,
                         llvm::Value *receiver, SemanticType source,
                         const BaseSubobjectLayout &path) {
  for (const auto index : path.indices) {
    receiver = ir.CreateStructGEP(session.layouts().get(source).storage,
                                  receiver, index);
    source = session.layouts().instance(source).bases.at(index).type;
  }
  if (source != path.type)
    throw std::logic_error("LLVM receiver projection lost its physical owner");
  return receiver;
}
} // namespace

Dispatch::Dispatch(Session &session) : session(session) {
  for (const auto &owner : session.program().mir().classInstances())
    for (const auto &selection : owner.virtualDispatch) {
      const auto *target =
          session.program().mir().findFunctionInstance(selection.target);
      if (!target)
        throw std::logic_error("LLVM dispatch lost a sealed target");
      auto &family = targets_[selection.root];
      if (!target->pureVirtual)
        family.push_back(selection.target);
    }
  for (auto &[root, family] : targets_) {
    (void)root;
    std::sort(family.begin(), family.end());
    family.erase(std::unique(family.begin(), family.end()), family.end());
  }
}

Dispatch::Dispatch(Session &session, const LlvmAdmissionPlan &admission)
    : Dispatch(session) {
  if (&admission.program() != &session.program() ||
      admission.options().optimization != session.options().optimization ||
      admission.options().debugInfo != session.options().debugInfo)
    throw std::invalid_argument(
        "LLVM dispatch admission identity differs from its session");
  this->admission = &admission;
}

void Dispatch::preflightSourceBodies(HirClassInstanceId activeClass) const {
  for (const auto target : dispatchSourceBodies(session.program(), activeClass))
    if (admission && std::find(admission->requiredBodies().begin(),
                               admission->requiredBodies().end(),
                               target) == admission->requiredBodies().end())
      throw BackendUnsupportedError(
          "LLVM source dispatch implementation is outside checked admission");
}

const std::vector<DispatchViewPlan> &
Dispatch::views(HirClassInstanceId activeClass) {
  if (const auto found = views_.find(activeClass); found != views_.end())
    return found->second;
  const auto &program = session.program().mir();
  const auto *active = program.findClassInstance(activeClass);
  if (!active)
    throw std::logic_error("LLVM dispatch requires an exact active class");
  std::vector<DispatchViewPlan> result;
  std::unordered_set<HirClassInstanceId> seen;
  std::vector<HirClassInstanceId> path;
  const auto append = [&](const auto &self, HirClassInstanceId id) -> void {
    const auto *view = program.findClassInstance(id);
    if (!view || !seen.insert(id).second)
      throw std::logic_error("LLVM dispatch view path is missing or ambiguous");
    if (view->polymorphic) {
      const auto viewLayout = session.layouts().basePath(active->type, path);
      auto location = session.layouts().dispatchLocation(view->type);
      if (viewLayout.type != view->type || !location)
        throw std::logic_error("LLVM dispatch view lost its exact storage");
      location->owner = session.layouts().baseSubobject(
          active->type, location->owner.type, "dispatch.activeView");
      DispatchViewPlan plan{.activeClass = activeClass,
                            .viewClass = id,
                            .path = path,
                            .offset = viewLayout.offset,
                            .location = *location,
                            .profileOwner = id};
      const auto shared = std::find_if(
          result.begin(), result.end(), [&](const auto &candidate) {
            return candidate.location == plan.location;
          });
      if (shared != result.end()) {
        plan.profileOwner = shared->profileOwner;
        plan.slots = shared->slots;
      } else
        for (const auto root : slotProfile(id)) {
          const auto selected = std::find_if(
              active->virtualDispatch.begin(), active->virtualDispatch.end(),
              [&](const auto &row) { return row.root == root; });
          if (selected == active->virtualDispatch.end())
            throw std::logic_error(
                "LLVM dispatch active state lost a view root");
          const auto *target = program.findFunctionInstance(selected->target);
          const auto *owner = program.findClassInstance(selected->targetOwner);
          const auto targetLayout =
              session.layouts().basePath(active->type, selected->targetPath);
          if (!target || !owner || target->owner != owner->id ||
              targetLayout.type != owner->type)
            throw std::logic_error("LLVM dispatch lost its target owner path");
          constexpr auto maxAdjustment = static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max());
          if (targetLayout.offset > maxAdjustment ||
              location->owner.offset > maxAdjustment)
            throw BackendUnsupportedError(
                "LLVM dispatch receiver adjustment exceeds its signed domain");
          plan.slots.push_back(
              {.root = root,
               .target = selected->target,
               .targetOwner = owner->id,
               .receiverAdjustment =
                   static_cast<std::int64_t>(targetLayout.offset) -
                   static_cast<std::int64_t>(location->owner.offset),
               .pure = target->pureVirtual});
        }
      result.push_back(std::move(plan));
    }
    for (const auto &base : view->bases) {
      path.push_back(base.instance);
      self(self, base.instance);
      path.pop_back();
    }
  };
  append(append, activeClass);
  return views_.emplace(activeClass, std::move(result)).first->second;
}

const std::vector<HirFunctionInstanceId> &
Dispatch::slotProfile(HirClassInstanceId view) {
  if (const auto found = profiles_.find(view); found != profiles_.end())
    return found->second;
  const auto *owner = session.program().mir().findClassInstance(view);
  if (!owner)
    throw std::logic_error("LLVM dispatch slot profile has no sealed owner");
  std::vector<HirFunctionInstanceId> profile;
  if (const auto *base = session.layouts().primaryBase(owner->type);
      base && session.layouts().instance(base->type).polymorphic)
    profile = slotProfile(base->instance);
  std::vector<HirFunctionInstanceId> appended;
  for (const auto &row : owner->virtualDispatch)
    if (std::find(profile.begin(), profile.end(), row.root) == profile.end())
      appended.push_back(row.root);
  std::sort(appended.begin(), appended.end());
  if (std::adjacent_find(appended.begin(), appended.end()) != appended.end())
    throw std::logic_error("LLVM dispatch slot profile repeats a sealed root");
  profile.insert(profile.end(), appended.begin(), appended.end());
  return profiles_.emplace(view, std::move(profile)).first->second;
}

VirtualRootAbi Dispatch::rootAbi(HirFunctionInstanceId root) {
  const auto found = targets_.find(root);
  if (found == targets_.end())
    throw std::logic_error("LLVM dispatch root has no sealed family");
  return session.abi().virtualRoot(root, found->second);
}

Dispatch::AdjustmentPlan
Dispatch::planAdjustment(HirClassInstanceId activeClass,
                         HirClassInstanceId sourceView,
                         HirFunctionInstanceId root) {
  preflightSourceBodies(activeClass);
  const auto &family = views(activeClass);
  const auto view =
      std::find_if(family.begin(), family.end(), [&](const auto &row) {
        return row.viewClass == sourceView;
      });
  const auto &profile = slotProfile(sourceView);
  if (view == family.end() ||
      std::find(profile.begin(), profile.end(), root) == profile.end())
    throw std::invalid_argument(
        "LLVM adjustment root is outside its sealed source view");
  const auto selected =
      std::find_if(view->slots.begin(), view->slots.end(),
                   [&](const auto &slot) { return slot.root == root; });
  if (selected == view->slots.end())
    throw std::logic_error("LLVM adjustment lost its exact table slot");
  if (selected->pure)
    throw BackendUnsupportedError(
        "LLVM pure invocation has no admitted execution outcome");
  const auto *target =
      session.program().mir().findFunctionInstance(selected->target);
  if (!target)
    throw std::logic_error("LLVM adjustment lost its sealed body");
  if (target->definitionKind != MirDefinitionKind::Source)
    throw BackendUnsupportedError(
        "LLVM adjustment requires a sealed source implementation");
  const auto entry = rootAbi(root);
  const auto plans = session.abi().virtualAdjustment(entry, target->id);
  // A failing terminal body can return its exact failure status. An infallible
  // terminal body needs the separately reviewed no-normal-exit lowering; do
  // not manufacture a successful return or assume an unreachable continuation.
  if (target->neverReturns && !plans.implementation.statusResult)
    throw BackendUnsupportedError("LLVM infallible terminal virtual target "
                                  "requires no-normal-exit lowering");
  const auto channel = [&](AbiParameter parameter) -> unsigned {
    for (unsigned index = 0; index < plans.entry.parameters.size(); ++index) {
      const auto &source = plans.entry.parameters[index];
      if (source.channel == parameter.channel &&
          source.sourceIndex == parameter.sourceIndex &&
          source.indirect == parameter.indirect &&
          source.extension == parameter.extension)
        return index;
    }
    throw std::logic_error(
        "LLVM adjustment cannot map an implementation ABI channel");
  };
  std::vector<unsigned> channels;
  for (const auto &parameter : plans.implementation.parameters)
    channels.push_back(channel(parameter));
  const auto output = session.layouts().get(target->returnType);
  const bool directResult =
      !plans.implementation.statusResult &&
      !plans.implementation.resultDestination &&
      !plans.implementation.type->getReturnType()->isVoidTy();
  if (!plans.entry.statusResult ||
      (directResult && !plans.entry.resultDestination))
    throw std::logic_error(
        "LLVM adjustment has inconsistent root result channels");
  if (directResult) {
    auto *result = target->returnType == SemanticType::Bool
                       ? llvm::Type::getInt8Ty(session.context())
                       : plans.implementation.type->getReturnType();
    if (result != output.storage)
      throw std::logic_error(
          "LLVM adjustment result disagrees with target storage");
  }
  return {.selected = *selected,
          .physicalOwner =
              session.layouts().instance(view->location.owner.type).id,
          .abi = plans,
          .result = output,
          .channels = std::move(channels),
          .directResult = directResult};
}

bool Dispatch::pureState(HirClassInstanceId activeClass) {
  for (const auto &view : views(activeClass))
    for (const auto &slot : view.slots)
      if (slot.pure)
        return true;
  return false;
}

bool Dispatch::abstractPhase(HirClassInstanceId activeClass) {
  const auto *active = session.program().mir().findClassInstance(activeClass);
  if (!active)
    throw std::invalid_argument("LLVM dispatch phase has no sealed class");
  if (!pureState(activeClass))
    return false;
  if (!active->abstract)
    throw std::logic_error("LLVM dispatch phase selects a pure concrete state");
  return true;
}

void Dispatch::preflightPhase(HirClassInstanceId activeClass) {
  if (abstractPhase(activeClass))
    preflightSourceBodies(activeClass);
  else
    preflightState(activeClass);
}

void Dispatch::installPhase(llvm::IRBuilder<> &ir, llvm::Value *completeObject,
                            HirClassInstanceId activeClass) {
  if (!abstractPhase(activeClass))
    install(ir, completeObject, activeClass);
}

void Dispatch::preflightState(HirClassInstanceId activeClass) {
  preflightSourceBodies(activeClass);
  const auto &active = *session.program().mir().findClassInstance(activeClass);
  const auto &family = views(activeClass);
  for (const auto &view : family) {
    const auto *owner =
        session.program().mir().findClassInstance(view.viewClass);
    const auto path = session.layouts().basePath(active.type, view.path);
    const auto physical = session.layouts().baseSubobject(
        active.type, view.location.owner.type, "dispatch.viewDisplacement");
    const auto indexBits = session.dataLayout().getIndexSizeInBits(0);
    if (!llvm::isUIntN(indexBits - 1, physical.offset))
      throw BackendUnsupportedError(
          "LLVM dispatch owner offset exceeds its target address-index range");
    if (!owner || view.activeClass != activeClass || path.type != owner->type ||
        path.offset != view.offset || physical != view.location.owner)
      throw std::logic_error(
          "LLVM dispatch state lost its exact physical view path");
    const auto *storage = llvm::dyn_cast<llvm::StructType>(
        session.layouts().get(physical.type).storage);
    if (!storage || view.location.index >= storage->getNumElements() ||
        storage->getElementType(view.location.index) !=
            session.layouts().pointer())
      throw std::logic_error(
          "LLVM dispatch state lost its physical table-pointer storage");
    const auto profileOwner =
        std::find_if(family.begin(), family.end(), [&](const auto &row) {
          return row.viewClass == view.profileOwner;
        });
    const auto &profile = slotProfile(view.profileOwner);
    if (profileOwner == family.end() ||
        profileOwner->location != view.location ||
        profile.size() != view.slots.size())
      throw std::logic_error(
          "LLVM dispatch state lost its shared physical slot profile");
    for (std::size_t index = 0; index < profile.size(); ++index) {
      if (profile[index] != view.slots[index].root)
        throw std::logic_error(
            "LLVM dispatch state lost its exact root slot order");
      const auto planned =
          planAdjustment(activeClass, view.profileOwner, profile[index]);
      if (planned.selected != view.slots[index])
        throw std::logic_error(
            "LLVM dispatch state lost its exact shared slot implementation");
    }
  }
}

llvm::Function *Dispatch::adjustment(HirClassInstanceId activeClass,
                                     HirClassInstanceId sourceView,
                                     HirFunctionInstanceId root) {
  const auto planned = planAdjustment(activeClass, sourceView, root);
  const auto &plans = planned.abi;
  const auto &selected = planned.selected;
  const auto *target =
      session.program().mir().findFunctionInstance(selected.target);
  const AdjustmentKey key{activeClass, sourceView, planned.physicalOwner,
                          root,        target->id, selected.receiverAdjustment};
  if (const auto found = adjustments_.find(key); found != adjustments_.end())
    return found->second;
  auto *callee = session.abi().declareFunction(target->id);
  auto *function = llvm::Function::Create(
      plans.entry.type, llvm::GlobalValue::InternalLinkage,
      "__gti_dispatch_adjust_" + std::to_string(activeClass) + "_" +
          std::to_string(sourceView) + "_" + std::to_string(root),
      session.module());
  plans.entry.applyTo(*function);
  llvm::IRBuilder<> ir(
      llvm::BasicBlock::Create(session.context(), "entry", function));
  std::vector<llvm::Value *> arguments;
  for (std::size_t index = 0; index < plans.implementation.parameters.size();
       ++index) {
    const auto &parameter = plans.implementation.parameters[index];
    llvm::Value *argument = function->getArg(planned.channels[index]);
    if (parameter.channel == AbiChannel::Receiver &&
        selected.receiverAdjustment != 0)
      argument = ir.CreateGEP(ir.getInt8Ty(), argument,
                              ir.getInt64(selected.receiverAdjustment));
    arguments.push_back(argument);
  }
  auto *status = ir.CreateCall(plans.implementation.type, callee, arguments);
  plans.implementation.applyTo(*status);
  if (plans.implementation.statusResult) {
    // The implementation owns publication to the shared success destination.
    // Failure records and partially initialized result bytes are not copied.
    ir.CreateRet(status);
  } else {
    if (planned.directResult) {
      llvm::Value *result = status;
      if (target->returnType == SemanticType::Bool)
        result = ir.CreateZExt(result, ir.getInt8Ty());
      ir.CreateAlignedStore(result,
                            function->getArg(*plans.entry.resultDestination),
                            llvm::Align(planned.result.alignment));
    }
    ir.CreateRet(ir.getTrue());
  }
  session.requireDefinition(*function);
  adjustments_.emplace(key, function);
  return function;
}

llvm::StructType *Dispatch::tableType(std::size_t slots) const {
  // Fixed header: the active-object displacement, then the complete-object
  // cleanup selected at allocation (null unless dynamically owned). The slot
  // array follows at a position independent of the active class.
  auto *offset = session.dataLayout().getIndexType(session.layouts().pointer());
  return llvm::StructType::get(
      session.context(),
      {offset, session.layouts().pointer(),
       llvm::ArrayType::get(session.layouts().pointer(), slots)});
}

bool Dispatch::dynamicallyOwned(HirClassInstanceId activeClass) {
  if (!dynamicOwners_) {
    std::set<HirClassInstanceId> owned;
    const auto &program = session.program();
    const auto visit = [&](MirBodyAddress address) {
      const auto *body = findMirBody(program.mir(), address);
      if (!body)
        throw std::logic_error("LLVM dynamic owner demand lost its body");
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions)
          if (const auto selected =
                  dynamicOwnerAllocation(program.mir(), instruction))
            owned.insert(*selected);
    };
    // Bound sessions consume exactly the admitted closure; an unbound
    // structural probe conservatively inspects every sealed body.
    if (admission)
      for (const auto address : admission->requiredBodies())
        visit(address);
    else
      for (const auto address : enumerateMirBodyAddresses(program.mir()))
        visit(address);
    for (const auto id : owned) {
      const auto *object = program.mir().findClassInstance(id);
      if (!object || !object->polymorphic || object->abstract ||
          object->kind == ClassKind::Interface)
        throw std::logic_error(
            "LLVM dynamic owner allocation lost its concrete active class");
    }
    dynamicOwners_ = std::move(owned);
  }
  return dynamicOwners_->contains(activeClass);
}

void Dispatch::bindCompleteCleanup(CompleteCleanup provider) {
  if (!provider || completeCleanup_)
    throw std::logic_error(
        "LLVM complete cleanup selection must be bound exactly once");
  completeCleanup_ = std::move(provider);
}

LoadedCompleteCleanup
Dispatch::loadCompleteCleanup(llvm::IRBuilder<> &ir, llvm::Value *view,
                              const SemanticType &viewType) {
  if (!view || view->getType() != session.layouts().pointer() ||
      viewType.kind != SemanticType::Class)
    throw std::invalid_argument(
        "LLVM complete cleanup requires an exact class view address");
  const auto &owner = session.layouts().instance(viewType);
  const auto location = session.layouts().dispatchLocation(viewType);
  if (!owner.polymorphic || !location)
    throw BackendUnsupportedError(
        "LLVM complete cleanup requires a dispatch-bearing class view");
  auto *physical = projectBase(session, ir, view, viewType, location->owner);
  auto *field =
      ir.CreateStructGEP(session.layouts().get(location->owner.type).storage,
                         physical, location->index);
  auto *table = ir.CreateLoad(session.layouts().pointer(), field);
  auto *header = tableType(0);
  auto *offset = ir.CreateLoad(header->getElementType(0),
                               ir.CreateStructGEP(header, table, 0));
  auto *cleanup = ir.CreateLoad(session.layouts().pointer(),
                                ir.CreateStructGEP(header, table, 1));
  return {ir.CreateGEP(ir.getInt8Ty(), physical, offset), cleanup};
}

llvm::GlobalVariable *Dispatch::table(HirClassInstanceId activeClass,
                                      HirClassInstanceId sourceView) {
  preflightState(activeClass);
  const auto &family = views(activeClass);
  const auto view =
      std::find_if(family.begin(), family.end(), [&](const auto &row) {
        return row.viewClass == sourceView;
      });
  if (view == family.end())
    throw std::invalid_argument("LLVM table has no sealed source view");
  const TableKey key{activeClass, view->location.owner.indices,
                     view->location.index};
  if (const auto found = tables_.find(key); found != tables_.end())
    return found->second;
  // Publish the descriptor identity before selecting its contents: the
  // complete cleanup may itself install this active state.
  auto *type = tableType(view->slots.size());
  auto *table = new llvm::GlobalVariable(
      session.module(), type, true, llvm::GlobalValue::InternalLinkage, nullptr,
      "__gti_dispatch_table_" + std::to_string(activeClass) + "_" +
          std::to_string(view->profileOwner));
  tables_.emplace(key, table);
  std::vector<llvm::Constant *> slots;
  for (const auto &slot : view->slots)
    slots.push_back(adjustment(activeClass, view->profileOwner, slot.root));
  auto *offsetType = llvm::cast<llvm::IntegerType>(type->getElementType(0));
  auto *offset = llvm::ConstantInt::getSigned(
      offsetType, -static_cast<std::int64_t>(view->location.owner.offset));
  llvm::Constant *cleanup =
      llvm::ConstantPointerNull::get(session.layouts().pointer());
  if (dynamicallyOwned(activeClass)) {
    if (!completeCleanup_)
      throw std::logic_error(
          "LLVM dynamically owned descriptor has no bound cleanup selection");
    cleanup = completeCleanup_(activeClass);
    if (!cleanup)
      throw std::logic_error(
          "LLVM dynamically owned descriptor lost its complete cleanup");
  }
  auto *entries = llvm::ConstantArray::get(
      llvm::cast<llvm::ArrayType>(type->getElementType(2)), slots);
  table->setInitializer(
      llvm::ConstantStruct::get(type, {offset, cleanup, entries}));
  return table;
}

void Dispatch::install(llvm::IRBuilder<> &ir, llvm::Value *completeObject,
                       HirClassInstanceId activeClass) {
  const auto *active = session.program().mir().findClassInstance(activeClass);
  if (!active)
    throw std::invalid_argument(
        "LLVM dispatch installation has no sealed active class");
  std::map<TableKey, std::pair<DispatchLocation, llvm::GlobalVariable *>>
      selected;
  for (const auto &view : views(activeClass)) {
    const TableKey key{activeClass, view.location.owner.indices,
                       view.location.index};
    if (!selected.contains(key))
      selected.emplace(
          key, std::pair{view.location, table(activeClass, view.viewClass)});
  }
  for (const auto &[key, entry] : selected) {
    (void)key;
    const auto &[location, table] = entry;
    auto type = active->type;
    auto *address = completeObject;
    for (const auto index : location.owner.indices) {
      address = ir.CreateStructGEP(session.layouts().get(type).storage, address,
                                   index);
      type = session.layouts().instance(type).bases.at(index).type;
    }
    if (type != location.owner.type)
      throw std::logic_error(
          "LLVM dispatch installation lost its physical owner");
    address = ir.CreateStructGEP(session.layouts().get(type).storage, address,
                                 location.index);
    ir.CreateStore(table, address);
  }
}

llvm::Value *Dispatch::activeObject(llvm::IRBuilder<> &ir, llvm::Value *view,
                                    const SemanticType &viewType) {
  if (!view || view->getType() != session.layouts().pointer() ||
      viewType.kind != SemanticType::Class)
    throw std::invalid_argument(
        "LLVM active-object recovery requires an exact class view address");
  const auto location = session.layouts().dispatchLocation(viewType);
  if (!location)
    throw BackendUnsupportedError(
        "LLVM active-object recovery requires a dispatch-bearing class view");
  auto *owner = projectBase(session, ir, view, viewType, location->owner);
  auto *field =
      ir.CreateStructGEP(session.layouts().get(location->owner.type).storage,
                         owner, location->index);
  auto *table = ir.CreateLoad(session.layouts().pointer(), field);
  auto *header = tableType(0);
  auto *offset = ir.CreateLoad(header->getElementType(0),
                               ir.CreateStructGEP(header, table, 0));
  return ir.CreateGEP(ir.getInt8Ty(), owner, offset);
}

VirtualCallPlan Dispatch::planCall(const MirInstruction &instruction) {
  const auto &program = session.program().mir();
  const auto *selected =
      instruction.functionTarget
          ? program.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  // A reference result is presented as a place of its referent type.
  const auto resultContract =
      instruction.info.category == ValueCategory::Place
          ? SemanticType::referenceTo(instruction.info.type,
                                      instruction.info.access)
          : instruction.info.type;
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.dispatch != CallDispatch::Virtual || !selected ||
      !selected->virtualMethod || selected->staticMember || !selected->owner ||
      instruction.tracksCaller != selected->tracksCaller ||
      selected->requiresUnsafe != (instruction.unsafeOperation ==
                                   UnsafeOperationKind::DeclaredUnsafeCall) ||
      instruction.parameterTypes != selected->parameterTypes ||
      resultContract != selected->returnType ||
      instruction.definedFailure.propagation !=
          FailurePropagationKind::VirtualCall)
    throw std::invalid_argument(
        "LLVM virtual invocation lacks its exact sealed call contract");
  const auto &source = session.layouts().instance(receiverType(instruction));
  const auto *selectedOwner = program.findClassInstance(*selected->owner);
  if (!selectedOwner)
    throw std::logic_error("LLVM named virtual member has no sealed owner");
  std::vector<HirFunctionInstanceId> roots;
  const MirFunctionInstance *contract = nullptr;
  std::optional<VirtualRootAbi> common;
  for (const auto &namedRoot : selectedOwner->virtualDispatch) {
    if (namedRoot.target != selected->id)
      continue;
    const auto sourceRoot = std::find_if(
        source.virtualDispatch.begin(), source.virtualDispatch.end(),
        [&](const auto &row) { return row.root == namedRoot.root; });
    if (sourceRoot == source.virtualDispatch.end())
      throw BackendUnsupportedError(
          "LLVM named virtual family is absent from the sealed receiver view");
    const auto &row = *sourceRoot;
    const auto *root = program.findFunctionInstance(row.root);
    if (!root || namedRoot.targetOwner != selectedOwner->id ||
        std::find(selected->virtualRoots.begin(), selected->virtualRoots.end(),
                  root->declaration) == selected->virtualRoots.end())
      throw std::logic_error(
          "LLVM named member lost its concrete sealed root family");
    const auto abi = rootAbi(root->id);
    if (root->returnType != selected->returnType ||
        root->parameterTypes != selected->parameterTypes ||
        root->receiverMutability != selected->receiverMutability ||
        root->overloadedOperator != selected->overloadedOperator ||
        (contract &&
         (!sameCallContract(*contract, *root) || common->call != abi.call)))
      throw BackendUnsupportedError("LLVM virtual roots do not retain one "
                                    "equivalent sealed call contract");
    if (!contract) {
      contract = root;
      common = abi;
    }
    // Consume the verified root path; the backend does not select a source
    // member or resolve ambiguity/accessibility from this physical query.
    const auto path = session.layouts().basePath(source.type, row.rootPath);
    if (!root->owner ||
        session.layouts().instance(path.type).id != *root->owner)
      throw std::logic_error("LLVM virtual root lost its sealed owner path");
    roots.push_back(root->id);
  }
  if (roots.empty())
    throw BackendUnsupportedError(
        "LLVM virtual call has no root in its sealed receiver view");
  std::sort(roots.begin(), roots.end());
  const auto root = roots.front();
  const auto &function = *program.findFunctionInstance(root);
  const auto &owner = *program.findClassInstance(*function.owner);
  auto location = session.layouts().dispatchLocation(owner.type);
  if (!location)
    throw std::logic_error(
        "LLVM virtual root has no physical dispatch pointer");
  location->owner = session.layouts().baseSubobject(
      source.type, location->owner.type, "dispatch.virtualRoot");
  const auto &profile = slotProfile(owner.id);
  const auto slot = std::find(profile.begin(), profile.end(), root);
  if (slot == profile.end())
    throw std::logic_error("LLVM virtual root has no stable slot");
  return {.root = root,
          .sourceClass = source.id,
          .rootOwner = owner.id,
          .location = *location,
          .slot = static_cast<std::size_t>(slot - profile.begin()),
          .abi = rootAbi(root)};
}

LoadedVirtualCall Dispatch::loadCall(llvm::IRBuilder<> &ir,
                                     const MirInstruction &instruction,
                                     const VirtualCallPlan &plan,
                                     llvm::Value *evaluatedReceiver) {
  if (!evaluatedReceiver || plan != planCall(instruction))
    throw std::invalid_argument(
        "LLVM virtual call plan is foreign or inconsistent");
  const auto &source =
      *session.program().mir().findClassInstance(plan.sourceClass);
  auto *receiver = projectBase(session, ir, evaluatedReceiver, source.type,
                               plan.location.owner);
  auto *field = ir.CreateStructGEP(
      session.layouts().get(plan.location.owner.type).storage, receiver,
      plan.location.index);
  auto *table = ir.CreateLoad(session.layouts().pointer(), field);
  auto *entries = ir.CreateStructGEP(tableType(0), table, 2);
  auto *slot = ir.CreateGEP(session.layouts().pointer(), entries,
                            ir.getInt64(plan.slot));
  return {ir.CreateLoad(session.layouts().pointer(), slot), receiver};
}

llvm::Value *Dispatch::directReceiver(llvm::IRBuilder<> &ir,
                                      const MirInstruction &instruction,
                                      llvm::Value *evaluatedReceiver) {
  const auto &program = session.program().mir();
  const auto *target =
      instruction.functionTarget
          ? program.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.dispatch != CallDispatch::Static || !target ||
      !target->owner || target->staticMember || !evaluatedReceiver)
    throw std::invalid_argument(
        "LLVM direct member has no sealed receiver presentation");
  const auto source = receiverType(instruction);
  const auto &owner = *program.findClassInstance(*target->owner);
  if (isMirExactReceiverPresentation(program, instruction, source)) {
    if (target->receiverMutability != ReceiverMutability::ReadOnly)
      throw std::invalid_argument(
          "LLVM exact interface presentation requires readonly access");
    const auto inverse = session.layouts().baseSubobject(
        owner.type, source, "dispatch.exactReceiverPresentation");
    return inverse.offset == 0
               ? evaluatedReceiver
               : ir.CreateGEP(
                     ir.getInt8Ty(), evaluatedReceiver,
                     ir.getInt64(-static_cast<std::int64_t>(inverse.offset)));
  }
  auto adjustment = session.layouts().findBaseSubobject(source, owner.type);
  if (!adjustment)
    throw std::logic_error(
        "LLVM direct member receiver is not a sealed subobject of its owner"
        " (receiver instance " +
        std::to_string(session.layouts().instance(source).id) + " class " +
        std::to_string(source.classId) + ", owner instance " +
        std::to_string(owner.id) + " class " +
        std::to_string(owner.type.classId) + ", function instance " +
        std::to_string(target->id) + " declaration " +
        std::to_string(target->declaration) + ")");
  return projectBase(session, ir, evaluatedReceiver, source, *adjustment);
}

} // namespace lang::native_llvm
