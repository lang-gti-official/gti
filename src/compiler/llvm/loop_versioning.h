#pragma once

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/DomTreeUpdater.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <optional>

namespace lang::native_llvm {
// Shared mechanics only: each caller owns its independent legality proof.
inline bool hasCheckedLoopMarker(const llvm::Loop &loop) {
  if (auto *id = loop.getLoopID())
    for (unsigned index = 1; index < id->getNumOperands(); ++index) {
      auto *entry = llvm::dyn_cast_or_null<llvm::MDNode>(id->getOperand(index));
      auto *name =
          entry && entry->getNumOperands()
              ? llvm::dyn_cast_or_null<llvm::MDString>(entry->getOperand(0))
              : nullptr;
      if (name && ((name->getString().starts_with("gti.") &&
                    name->getString().ends_with(".versioned")) ||
                   name->getString() == "llvm.loop.disable_nonforced" ||
                   name->getString() == "llvm.loop.isvectorized"))
        return true;
    }
  return false;
}

inline void markCheckedLoop(llvm::Loop &loop, llvm::StringRef versionedMarker) {
  auto &context = loop.getHeader()->getContext();
  llvm::SmallVector<llvm::Metadata *, 8> operands{nullptr};
  if (auto *id = loop.getLoopID())
    for (unsigned index = 1; index < id->getNumOperands(); ++index)
      operands.push_back(id->getOperand(index));
  operands.push_back(llvm::MDNode::get(
      context, {llvm::MDString::get(context, versionedMarker)}));
  auto *id = llvm::MDNode::getDistinct(context, operands);
  id->replaceOperandWith(0, id);
  loop.setLoopID(id);
}

inline std::optional<unsigned> checkedLoopCloneCost(const llvm::Loop &loop) {
  unsigned count = 0;
  if (loop.getNumBlocks() > 16)
    return std::nullopt;
  for (auto *block : loop.blocks()) {
    if (block->isEHPad() || block->hasAddressTaken())
      return std::nullopt;
    for (auto &instruction : *block) {
      if (++count > 128 || instruction.getType()->isTokenTy() ||
          llvm::isa<llvm::AllocaInst, llvm::InvokeInst, llvm::CallBrInst,
                    llvm::IndirectBrInst>(instruction))
        return std::nullopt;
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
        if (call->isConvergent() || call->cannotDuplicate() ||
            call->hasFnAttr(llvm::Attribute::ReturnsTwice))
          return std::nullopt;
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
        if (intrinsic->getIntrinsicID() ==
            llvm::Intrinsic::experimental_noalias_scope_decl)
          return std::nullopt;
    }
  }
  return count;
}

struct CheckedLoopVersions {
  llvm::BasicBlock *fast;
  llvm::BasicBlock *slow;
  llvm::Loop *slowLoop;
};

inline CheckedLoopVersions cloneCheckedFallback(llvm::Loop &loop,
                                                llvm::LoopInfo &loops,
                                                llvm::DominatorTree &dt) {
  // Callers require LoopSimplify and LCSSA: all external definitions then
  // flow through the exit PHIs repaired below, including shared exits.
  auto *guard = loop.getLoopPreheader();
  llvm::DomTreeUpdater updater(dt, llvm::DomTreeUpdater::UpdateStrategy::Eager);
  auto *fast = llvm::SplitBlock(guard, guard->getTerminator()->getIterator(),
                                &updater, &loops, nullptr, "checked.fast");
  llvm::ValueToValueMapTy map;
  llvm::SmallVector<llvm::BasicBlock *, 16> copies;
  auto *slowLoop = llvm::cloneLoopWithPreheader(
      loop.getHeader(), guard, &loop, map, ".checked", &loops, &dt, copies);
  llvm::remapInstructionsInBlocks(copies, map);
  auto *slow = llvm::cast<llvm::BasicBlock>(map.lookup(fast));
  llvm::SmallVector<llvm::BasicBlock *, 8> exits;
  llvm::SmallPtrSet<llvm::BasicBlock *, 8> seen;
  loop.getExitBlocks(exits);
  for (auto *exit : exits) {
    if (!seen.insert(exit).second)
      continue;
    for (auto &phi : exit->phis()) {
      const auto size = phi.getNumIncomingValues();
      for (unsigned index = 0; index < size; ++index) {
        auto *from = phi.getIncomingBlock(index);
        if (!loop.contains(from))
          continue;
        auto *value = phi.getIncomingValue(index);
        llvm::Value *mapped = map.lookup(value);
        phi.addIncoming(mapped ? mapped : value,
                        llvm::cast<llvm::BasicBlock>(map.lookup(from)));
      }
    }
  }
  return {fast, slow, slowLoop};
}
} // namespace lang::native_llvm
