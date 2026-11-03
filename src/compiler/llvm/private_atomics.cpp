#include "private_atomics.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/LowerAtomic.h>

namespace lang::native_llvm {
namespace {
struct Plan {
  llvm::SmallVector<llvm::AtomicRMWInst *, 8> updates;
};

bool supported(llvm::AtomicRMWInst::BinOp operation) {
  switch (operation) {
  case llvm::AtomicRMWInst::Add:
  case llvm::AtomicRMWInst::Sub:
  case llvm::AtomicRMWInst::And:
  case llvm::AtomicRMWInst::Or:
  case llvm::AtomicRMWInst::Xor:
  case llvm::AtomicRMWInst::Xchg:
    return true;
  default:
    return false;
  }
}

// Capture annotations alone are insufficient: even a nocapture callee can
// temporarily expose the address to another thread before returning. Admit a
// complete closed use graph instead. One scalar cell may have zero-offset
// source-wrapper views, but no alternate address or externally observable use.
bool planFor(llvm::AllocaInst &allocation, const llvm::DataLayout &layout,
             Plan &plan) {
  if (!allocation.isStaticAlloca() || allocation.getAddressSpace() != 0 ||
      !llvm::isa<llvm::ConstantInt>(allocation.getArraySize()) ||
      !llvm::cast<llvm::ConstantInt>(allocation.getArraySize())->isOne())
    return false;
  auto size = allocation.getAllocationSize(layout);
  if (!size || size->isScalable())
    return false;
  llvm::SmallPtrSet<llvm::Value *, 16> seen;
  llvm::SmallVector<llvm::Value *, 16> pending{&allocation};
  llvm::Type *cell = nullptr;
  unsigned budget = 512;
  auto acceptType = [&](llvm::Type *type) {
    if (!type->isIntegerTy() || (cell && cell != type) ||
        layout.getTypeStoreSize(type).getFixedValue() > size->getFixedValue())
      return false;
    cell = type;
    return true;
  };
  while (!pending.empty()) {
    auto *pointer = pending.pop_back_val();
    if (!seen.insert(pointer).second)
      continue;
    for (auto *user : pointer->users()) {
      if (budget == 0)
        return false;
      --budget;
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
        if (gep->getPointerOperand() != pointer || !gep->hasAllZeroIndices())
          return false;
        pending.push_back(gep);
      } else if (auto *cast = llvm::dyn_cast<llvm::BitCastInst>(user)) {
        if (!cast->getType()->isPointerTy())
          return false;
        pending.push_back(cast);
      } else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
        if (load->getPointerOperand() != pointer || load->isVolatile() ||
            (load->isAtomic() &&
             load->getOrdering() != llvm::AtomicOrdering::Monotonic) ||
            !acceptType(load->getType()))
          return false;
      } else if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
        if (store->getPointerOperand() != pointer || store->isVolatile() ||
            (store->isAtomic() &&
             store->getOrdering() != llvm::AtomicOrdering::Monotonic) ||
            !acceptType(store->getValueOperand()->getType()))
          return false;
      } else if (auto *update = llvm::dyn_cast<llvm::AtomicRMWInst>(user)) {
        if (update->getPointerOperand() != pointer || update->isVolatile() ||
            update->getOrdering() != llvm::AtomicOrdering::Monotonic ||
            !supported(update->getOperation()) ||
            !acceptType(update->getType()))
          return false;
        plan.updates.push_back(update);
      } else if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user)) {
        if (!intrinsic->isLifetimeStartOrEnd() &&
            !llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic))
          return false;
      } else {
        return false;
      }
    }
  }
  return !plan.updates.empty();
}

class PrivateAtomicPass : public llvm::PassInfoMixin<PrivateAtomicPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses) {
    auto result = optimizePrivateAtomics(function, analyses);
    if (result.areAllPreserved())
      return result;
    analyses.invalidate(function, result);
    llvm::SROAPass(llvm::SROAOptions::ModifyCFG).run(function, analyses);
    return llvm::PreservedAnalyses::none();
  }
};
} // namespace

llvm::PreservedAnalyses
optimizePrivateAtomics(llvm::Function &function,
                       llvm::FunctionAnalysisManager &) {
  if (function.isDeclaration() ||
      function.hasFnAttribute(llvm::Attribute::OptimizeNone))
    return llvm::PreservedAnalyses::all();
  bool changed = false;
  const auto &layout = function.getParent()->getDataLayout();
  // Plan each allocation before changing any of its users. A rejected graph
  // retains every RMW, including ones encountered before the rejecting use.
  for (auto &instruction : function.getEntryBlock()) {
    auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    if (!allocation)
      continue;
    Plan plan;
    if (!planFor(*allocation, layout, plan))
      continue;
    for (auto *update : plan.updates)
      changed |= llvm::lowerAtomicRMWInst(update);
  }
  // LLVM SROA already supports nonvolatile private atomic loads/stores. The
  // lowering utility supplies ordinary wrapping arithmetic without invented
  // nowrap flags, preserves returned old values, and leaves all shared atomics
  // and fences alone. No module-wide LowerAtomic pass is used.
  return changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

void registerPrivateAtomicPass(llvm::PassBuilder &builder) {
  builder.registerVectorizerStartEPCallback(
      [](llvm::FunctionPassManager &pipeline, llvm::OptimizationLevel level) {
        if (level != llvm::OptimizationLevel::O2 &&
            level != llvm::OptimizationLevel::O3)
          return;
        pipeline.addPass(PrivateAtomicPass());
      });
}
} // namespace lang::native_llvm
