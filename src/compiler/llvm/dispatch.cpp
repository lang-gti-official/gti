#include "dispatch.h"

#include <algorithm>
#include <limits>
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
      location->owner =
          session.layouts().baseSubobject(active->type, location->owner.type);
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

llvm::Function *Dispatch::adjustment(HirClassInstanceId activeClass,
                                     HirClassInstanceId sourceView,
                                     HirFunctionInstanceId root) {
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
  const auto entry = rootAbi(root);
  const auto plans = session.abi().virtualAdjustment(entry, target->id);
  // A failing terminal body can return its exact failure status. An infallible
  // terminal body needs the separately reviewed no-normal-exit lowering; do
  // not manufacture a successful return or assume an unreachable continuation.
  if (target->neverReturns && !plans.implementation.statusResult)
    throw BackendUnsupportedError("LLVM infallible terminal virtual target "
                                  "requires no-normal-exit lowering");
  const auto physical =
      session.layouts().instance(view->location.owner.type).id;
  const AdjustmentKey key{activeClass, sourceView,
                          physical,    root,
                          target->id,  selected->receiverAdjustment};
  if (const auto found = adjustments_.find(key); found != adjustments_.end())
    return found->second;
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
  for (const auto &parameter : plans.implementation.parameters)
    (void)channel(parameter);
  const auto output = session.layouts().get(target->returnType);
  const bool directResult =
      !plans.implementation.statusResult &&
      !plans.implementation.resultDestination &&
      !plans.implementation.type->getReturnType()->isVoidTy();
  if (!plans.entry.statusResult ||
      (directResult && !plans.entry.resultDestination))
    throw std::logic_error(
        "LLVM adjustment has inconsistent root result channels");
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
  for (const auto &parameter : plans.implementation.parameters) {
    llvm::Value *argument = function->getArg(channel(parameter));
    if (parameter.channel == AbiChannel::Receiver &&
        selected->receiverAdjustment != 0)
      argument = ir.CreateGEP(ir.getInt8Ty(), argument,
                              ir.getInt64(selected->receiverAdjustment));
    arguments.push_back(argument);
  }
  auto *status = ir.CreateCall(plans.implementation.type, callee, arguments);
  plans.implementation.applyTo(*status);
  if (plans.implementation.statusResult) {
    // The implementation owns publication to the shared success destination.
    // Failure records and partially initialized result bytes are not copied.
    ir.CreateRet(status);
  } else {
    if (directResult) {
      llvm::Value *result = status;
      if (target->returnType == SemanticType::Bool)
        result = ir.CreateZExt(result, ir.getInt8Ty());
      if (result->getType() != output.storage)
        throw std::logic_error(
            "LLVM adjustment result disagrees with target storage");
      ir.CreateAlignedStore(result,
                            function->getArg(*plans.entry.resultDestination),
                            llvm::Align(output.alignment));
    }
    ir.CreateRet(ir.getTrue());
  }
  session.requireDefinition(*function);
  adjustments_.emplace(key, function);
  return function;
}

llvm::GlobalVariable *Dispatch::table(HirClassInstanceId activeClass,
                                      HirClassInstanceId sourceView) {
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
  for (const auto &slot : view->slots) {
    const auto *target =
        session.program().mir().findFunctionInstance(slot.target);
    if (slot.pure || !target ||
        (target->neverReturns && !target->mayRaiseDefinedFailure))
      throw BackendUnsupportedError(
          "LLVM table publication requires admitted nonpure target outcomes");
    // Preflight all plans before demanding any definition or publishing data.
    (void)session.abi().virtualAdjustment(rootAbi(slot.root), slot.target);
  }
  std::vector<llvm::Constant *> slots;
  for (const auto &slot : view->slots)
    slots.push_back(adjustment(activeClass, view->profileOwner, slot.root));
  auto *type = llvm::ArrayType::get(session.layouts().pointer(), slots.size());
  auto *table = new llvm::GlobalVariable(
      session.module(), type, true, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantArray::get(type, slots),
      "__gti_dispatch_table_" + std::to_string(activeClass) + "_" +
          std::to_string(view->profileOwner));
  tables_.emplace(key, table);
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

VirtualCallPlan Dispatch::planCall(const MirInstruction &instruction) {
  const auto &program = session.program().mir();
  const auto *selected =
      instruction.functionTarget
          ? program.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.dispatch != CallDispatch::Virtual || !selected ||
      !selected->virtualMethod || selected->staticMember || !selected->owner ||
      instruction.tracksCaller != selected->tracksCaller ||
      selected->requiresUnsafe != (instruction.unsafeOperation ==
                                   UnsafeOperationKind::DeclaredUnsafeCall) ||
      instruction.parameterTypes != selected->parameterTypes ||
      instruction.info.type != selected->returnType ||
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
  location->owner =
      session.layouts().baseSubobject(source.type, location->owner.type);
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
  auto *slot =
      ir.CreateGEP(session.layouts().pointer(), table, ir.getInt64(plan.slot));
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
    const auto inverse = session.layouts().baseSubobject(owner.type, source);
    return inverse.offset == 0
               ? evaluatedReceiver
               : ir.CreateGEP(
                     ir.getInt8Ty(), evaluatedReceiver,
                     ir.getInt64(-static_cast<std::int64_t>(inverse.offset)));
  }
  return projectBase(session, ir, evaluatedReceiver, source,
                     session.layouts().baseSubobject(source, owner.type));
}

} // namespace lang::native_llvm
