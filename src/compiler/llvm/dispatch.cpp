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
      const auto index = session.layouts().dispatchIndex(view->type);
      if (viewLayout.type != view->type || !index)
        throw std::logic_error("LLVM dispatch view lost its exact storage");
      DispatchViewPlan plan{.activeClass = activeClass,
                            .viewClass = id,
                            .path = path,
                            .offset = viewLayout.offset,
                            .dispatchIndex = *index};
      for (const auto &contract : view->virtualDispatch) {
        const auto selected = std::find_if(
            active->virtualDispatch.begin(), active->virtualDispatch.end(),
            [&](const auto &row) { return row.root == contract.root; });
        if (selected == active->virtualDispatch.end())
          throw std::logic_error("LLVM dispatch active state lost a view root");
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
            viewLayout.offset > maxAdjustment)
          throw BackendUnsupportedError(
              "LLVM dispatch receiver adjustment exceeds its signed domain");
        plan.slots.push_back(
            {.root = contract.root,
             .target = selected->target,
             .targetOwner = owner->id,
             .receiverAdjustment =
                 static_cast<std::int64_t>(targetLayout.offset) -
                 static_cast<std::int64_t>(viewLayout.offset),
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

VirtualRootAbi Dispatch::rootAbi(HirFunctionInstanceId root) {
  const auto found = targets_.find(root);
  if (found == targets_.end())
    throw std::logic_error("LLVM dispatch root has no sealed family");
  return session.abi().virtualRoot(root, found->second);
}

} // namespace lang::native_llvm
