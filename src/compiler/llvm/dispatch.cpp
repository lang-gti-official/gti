#include "dispatch.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace lang::native_llvm {

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

} // namespace lang::native_llvm
