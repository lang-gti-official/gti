#include "checked_ranges.h"
#include "loop_versioning.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>

namespace lang::native_llvm {
namespace {
constexpr llvm::StringLiteral marker = "gti.range.versioned";
#if LLVM_VERSION_MAJOR >= 23
using ConditionalBranch = llvm::CondBrInst;
#else
using ConditionalBranch = llvm::BranchInst;
#endif

ConditionalBranch *conditional(llvm::BasicBlock *block) {
  auto *branch = llvm::dyn_cast<ConditionalBranch>(block->getTerminator());
#if LLVM_VERSION_MAJOR < 23
  if (branch && !branch->isConditional())
    return nullptr;
#endif
  return branch;
}

struct Plan {
  llvm::Loop *loop;
  llvm::PHINode *index;
  llvm::Value *start;
  llvm::Value *stop;
  llvm::Value *length;
  bool increasing;
  unsigned cost;
  llvm::SmallVector<std::pair<llvm::ICmpInst *, bool>, 4> checks;
};

bool available(llvm::Value *value, llvm::Loop &loop, llvm::DominatorTree &dt) {
  return loop.isLoopInvariant(value) &&
         (!llvm::isa<llvm::Instruction>(value) ||
          dt.dominates(llvm::cast<llvm::Instruction>(value),
                       loop.getLoopPreheader()->getTerminator()));
}

// This is an independent modular induction proof. Do not use SCEV exit
// counts, nowrap flags, or the bounds check being removed to establish it.
// Increasing: start < stop; the next value visits [start+1, stop] and the
// equality latch stops before wrap. Decreasing: start > 0; an unsigned
// next > stop latch cannot continue from next == 0, so current is in
// [1, start] and next in [0, start-1]. Other early exits only shorten a scan.
std::optional<Plan> planFor(llvm::Loop &loop, llvm::DominatorTree &dt,
                            unsigned remaining) {
  if (!loop.isInnermost() || !loop.getLoopPreheader() || !loop.getLoopLatch() ||
      !loop.isLoopSimplifyForm() || !loop.isLCSSAForm(dt) ||
      hasCheckedLoopMarker(loop))
    return std::nullopt;
  const auto cost = checkedLoopCloneCost(loop);
  if (!cost || *cost > remaining)
    return std::nullopt;
  for (auto &index : loop.getHeader()->phis()) {
    if (!index.getType()->isIntegerTy() || index.getNumIncomingValues() != 2)
      continue;
    auto *start = index.getIncomingValueForBlock(loop.getLoopPreheader());
    auto *step = llvm::dyn_cast<llvm::BinaryOperator>(
        index.getIncomingValueForBlock(loop.getLoopLatch()));
    if (!step || !available(start, loop, dt) || step->getOperand(0) != &index)
      continue;
    auto *amount = llvm::dyn_cast<llvm::ConstantInt>(step->getOperand(1));
    if (!amount)
      continue;
    const bool increasing =
        step->getOpcode() == llvm::Instruction::Add && amount->isOne();
    const bool decreasing =
        (step->getOpcode() == llvm::Instruction::Sub && amount->isOne()) ||
        (step->getOpcode() == llvm::Instruction::Add && amount->isMinusOne());
    if (!increasing && !decreasing)
      continue;
    llvm::Value *stop = nullptr;
    llvm::BasicBlock *controlSuccess = nullptr;
    ConditionalBranch *control = nullptr;
    // The normal rotated form tests next in the latch. The unrotated
    // increasing form tests current in the header before any removed check.
    // Both give the same inclusive maximum, stop, for a next-value access.
    for (const bool headerControl : {false, true}) {
      if (headerControl && !increasing)
        continue;
      auto *branch =
          conditional(headerControl ? loop.getHeader() : loop.getLoopLatch());
      if (!branch || loop.contains(branch->getSuccessor(0)) ==
                         loop.contains(branch->getSuccessor(1)))
        continue;
      auto *comparison = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
      if (!comparison)
        continue;
      const bool continuesTrue = loop.contains(branch->getSuccessor(0));
      auto predicate = comparison->getPredicate();
      if (!continuesTrue)
        predicate = llvm::CmpInst::getInversePredicate(predicate);
      auto *value = headerControl ? static_cast<llvm::Value *>(&index) : step;
      llvm::Value *bound = nullptr;
      if (comparison->getOperand(0) == value)
        bound = comparison->getOperand(1);
      else if (comparison->getOperand(1) == value) {
        bound = comparison->getOperand(0);
        predicate = llvm::CmpInst::getSwappedPredicate(predicate);
      }
      auto *success = branch->getSuccessor(continuesTrue ? 0 : 1);
      if (!bound || !available(bound, loop, dt) ||
          predicate !=
              (increasing ? llvm::CmpInst::ICMP_NE : llvm::CmpInst::ICMP_UGT) ||
          !dt.dominates(success, loop.getLoopLatch()))
        continue;
      stop = bound;
      controlSuccess = success;
      control = branch;
      break;
    }
    if (!control)
      continue;
    Plan plan{&loop, &index, start, stop, nullptr, increasing, *cost, {}};
    for (auto *block : loop.blocks()) {
      auto *branch = conditional(block);
      if (!branch || branch == control ||
          loop.contains(branch->getSuccessor(0)) ==
              loop.contains(branch->getSuccessor(1)))
        continue;
      auto *check = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
      if (!check || !dt.dominates(controlSuccess, check->getParent()))
        continue;
      auto relation = check->getPredicate();
      auto *value = check->getOperand(0);
      auto *length = check->getOperand(1);
      if (value != &index && value != step) {
        std::swap(value, length);
        relation = llvm::CmpInst::getSwappedPredicate(relation);
      }
      if ((value != &index && value != step) || !available(length, loop, dt) ||
          (relation != llvm::CmpInst::ICMP_ULT &&
           relation != llvm::CmpInst::ICMP_UGE) ||
          (plan.length && plan.length != length))
        continue;
      const bool result = relation == llvm::CmpInst::ICMP_ULT;
      if (!loop.contains(branch->getSuccessor(result ? 0 : 1)))
        continue;
      plan.length = length;
      plan.checks.emplace_back(check, result);
    }
    if (!plan.checks.empty())
      return plan;
  }
  return std::nullopt;
}

void apply(Plan &plan, llvm::LoopInfo &loops, llvm::DominatorTree &dt) {
  auto &loop = *plan.loop;
  auto *guard = loop.getLoopPreheader();
  auto [fast, slow, slowLoop] = cloneCheckedFallback(loop, loops, dt);
  llvm::IRBuilder<> builder(guard->getTerminator());
  llvm::DenseMap<llvm::Value *, llvm::Value *> frozen;
  for (auto *value : {plan.start, plan.stop, plan.length})
    if (!frozen.contains(value))
      frozen[value] = builder.CreateFreeze(value, "scan.bound");
  auto *start = frozen.lookup(plan.start);
  auto *stop = frozen.lookup(plan.stop);
  auto *length = frozen.lookup(plan.length);
  auto *domain = plan.increasing
                     ? builder.CreateICmpULT(start, stop, "scan.forward")
                     : builder.CreateICmpNE(
                           start, llvm::ConstantInt::get(start->getType(), 0),
                           "scan.nonzero");
  auto *fits = builder.CreateICmpULT(plan.increasing ? stop : start, length,
                                     "scan.fits");
  builder.CreateCondBr(builder.CreateAnd(domain, fits), fast, slow);
  guard->getTerminator()->eraseFromParent();
  // Freeze avoids observing poison on a newly introduced selector and makes
  // undef choose one consistent input for the proof and its entire fast loop.
  // The original checked clone uses the original inputs without substitution.
  for (auto *block : loop.blocks())
    for (auto &instruction : *block)
      for (auto &operand : instruction.operands())
        if (auto *replacement = frozen.lookup(operand.get()))
          operand.set(replacement);
  for (auto [check, result] : plan.checks)
    check->replaceAllUsesWith(
        llvm::ConstantInt::getBool(check->getContext(), result));
  markCheckedLoop(loop, marker);
  markCheckedLoop(*slowLoop, marker);
  dt.recalculate(*guard->getParent());
}

class CheckedRangePass : public llvm::PassInfoMixin<CheckedRangePass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses) {
    auto result = optimizeCheckedRanges(function, analyses);
    if (result.areAllPreserved())
      return result;
    llvm::FunctionPassManager cleanup;
    cleanup.addPass(llvm::SimplifyCFGPass());
    cleanup.addPass(llvm::InstCombinePass());
    // Removing proven check exits exposes loop-carried store-to-load reuse.
    // Let LLVM's ordinary alias analysis prove it; never attach noalias here.
    cleanup.addPass(llvm::GVNPass(
        llvm::GVNOptions().setLoadPRE(true).setLoadPRESplitBackedge(true)));
    cleanup.addPass(llvm::InstCombinePass());
    cleanup.addPass(llvm::LoopSimplifyPass());
    cleanup.addPass(llvm::LCSSAPass());
    cleanup.run(function, analyses);
    return llvm::PreservedAnalyses::none();
  }
};
} // namespace

llvm::PreservedAnalyses
optimizeCheckedRanges(llvm::Function &function,
                      llvm::FunctionAnalysisManager &analyses) {
  if (function.isDeclaration() || function.hasOptSize() ||
      function.hasFnAttribute(llvm::Attribute::OptimizeNone))
    return llvm::PreservedAnalyses::all();
  bool changed = false;
  unsigned remaining = 1024;
  for (unsigned iteration = 0; iteration < 32; ++iteration) {
    auto &loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto &dt = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    std::optional<Plan> plan;
    for (auto *loop : loops.getLoopsInPreorder())
      if ((plan = planFor(*loop, dt, remaining)))
        break;
    if (!plan)
      break;
    apply(*plan, loops, dt);
    remaining -= plan->cost;
    changed = true;
    analyses.invalidate(function, llvm::PreservedAnalyses::none());
  }
  return changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

void registerCheckedRangePass(llvm::PassBuilder &builder) {
  builder.registerVectorizerStartEPCallback(
      [](llvm::FunctionPassManager &pipeline, llvm::OptimizationLevel level) {
        if (level != llvm::OptimizationLevel::O2 &&
            level != llvm::OptimizationLevel::O3)
          return;
        pipeline.addPass(llvm::LoopSimplifyPass());
        pipeline.addPass(llvm::LCSSAPass());
        pipeline.addPass(CheckedRangePass());
      });
}
} // namespace lang::native_llvm
