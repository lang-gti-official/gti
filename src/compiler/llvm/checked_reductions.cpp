#include "checked_reductions.h"
#include "loop_versioning.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>

#include <algorithm>
#include <optional>

namespace lang::native_llvm {
namespace {

constexpr unsigned maximumFunctionGrowth = 1024;
constexpr unsigned maximumTransforms = 32;
constexpr llvm::StringLiteral versionedMarker = "gti.reduction.versioned";

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

// A unit-step traversal whose latch stops at equality visits every integer bit
// pattern at most once. Strict order comparisons may stop sooner, but never
// pass the equality target. This bound uses wrapping arithmetic,
// not nsw/nuw flags or an exit limit inferred from the checks being removed.
struct Traversal {
  llvm::Value *bound = nullptr;
  llvm::APInt initial;
  llvm::PHINode *index = nullptr;
  llvm::ICmpInst *comparison = nullptr;
  unsigned boundOperand = 0;
  bool comparesNext = false;
};

std::optional<Traversal> traversal(llvm::Loop &loop, llvm::DominatorTree &dt) {
  auto *latch = loop.getLoopLatch();
  auto *branch = latch ? conditional(latch) : nullptr;
  if (!branch)
    return std::nullopt;
  const bool trueBackedge = branch->getSuccessor(0) == loop.getHeader();
  const bool falseBackedge = branch->getSuccessor(1) == loop.getHeader();
  if (trueBackedge == falseBackedge ||
      loop.contains(branch->getSuccessor(trueBackedge ? 1 : 0)))
    return std::nullopt;
  auto *comparison = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
  if (!comparison)
    return std::nullopt;
  const auto continues =
      trueBackedge
          ? comparison->getPredicate()
          : llvm::CmpInst::getInversePredicate(comparison->getPredicate());
  if (continues != llvm::CmpInst::ICMP_NE &&
      continues != llvm::CmpInst::ICMP_ULT &&
      continues != llvm::CmpInst::ICMP_UGT &&
      continues != llvm::CmpInst::ICMP_SLT &&
      continues != llvm::CmpInst::ICMP_SGT)
    return std::nullopt;
  for (auto &phi : loop.getHeader()->phis()) {
    if (!phi.getType()->isIntegerTy() ||
        phi.getType()->getIntegerBitWidth() > 64 ||
        phi.getNumIncomingValues() != 2)
      continue;
    auto *initial = llvm::dyn_cast<llvm::ConstantInt>(
        phi.getIncomingValueForBlock(loop.getLoopPreheader()));
    auto *next = llvm::dyn_cast<llvm::BinaryOperator>(
        phi.getIncomingValueForBlock(latch));
    if (!initial || !next || next->getOpcode() != llvm::Instruction::Add)
      continue;
    auto *step = llvm::dyn_cast<llvm::ConstantInt>(next->getOperand(1));
    auto *base = next->getOperand(0);
    if (!step) {
      step = llvm::dyn_cast<llvm::ConstantInt>(next->getOperand(0));
      base = next->getOperand(1);
    }
    if (base != &phi || !step || !step->isOne())
      continue;
    for (unsigned operand = 0; operand != 2; ++operand) {
      auto *tested = comparison->getOperand(operand);
      auto *bound = comparison->getOperand(1 - operand);
      if ((tested != &phi && tested != next) || !loop.isLoopInvariant(bound))
        continue;
      if (auto *instruction = llvm::dyn_cast<llvm::Instruction>(bound))
        if (!dt.dominates(instruction,
                          loop.getLoopPreheader()->getTerminator()))
          continue;
      return Traversal{bound,      initial->getValue(), &phi,
                       comparison, 1 - operand,         tested == next};
    }
  }
  return std::nullopt;
}

struct Offset {
  uint64_t minimum = 0;
  uint64_t maximum = 0;
};

// Uniform value bounds from the operation's bit-level meaning alone. Do not
// consult nowrap flags, control-flow facts, assumptions, or the overflow exit
// this pass may remove. Even a loop-dependent input can have a fixed envelope
// after masking/remainder; no operand evaluation moves out of the loop.
struct IncrementBounds {
  llvm::DenseMap<llvm::Value *, Offset> bounds;
  llvm::SmallPtrSet<llvm::Value *, 32> visiting;
  unsigned visited = 0;

  std::optional<Offset> get(llvm::Value *value) {
    if (!value->getType()->isIntegerTy() ||
        value->getType()->getIntegerBitWidth() > 64)
      return std::nullopt;
    if (auto found = bounds.find(value); found != bounds.end())
      return found->second;
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value)) {
      const auto number = constant->getZExtValue();
      return bounds[value] = Offset{number, number};
    }
    if (++visited > 128 || !visiting.insert(value).second)
      return std::nullopt;
    std::optional<Offset> result;
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value)) {
      auto *constant = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));
      if (!constant && binary->getOpcode() == llvm::Instruction::And)
        constant = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(0));
      if (constant) {
        const auto amount = constant->getZExtValue();
        if (binary->getOpcode() == llvm::Instruction::And)
          result = Offset{0, amount};
        else if (binary->getOpcode() == llvm::Instruction::URem && amount)
          result = Offset{0, amount - 1};
        else if (binary->getOpcode() == llvm::Instruction::LShr &&
                 amount < value->getType()->getIntegerBitWidth())
          result = Offset{0, llvm::APInt::getMaxValue(
                                 value->getType()->getIntegerBitWidth())
                                 .lshr(amount)
                                 .getZExtValue()};
      }
      if (binary->getOpcode() == llvm::Instruction::Add) {
        auto left = get(binary->getOperand(0)),
             right = get(binary->getOperand(1));
        const auto maximum =
            llvm::APInt::getMaxValue(value->getType()->getIntegerBitWidth())
                .getZExtValue();
        if (left && right && left->maximum <= maximum - right->maximum)
          result = Offset{left->minimum + right->minimum,
                          left->maximum + right->maximum};
      }
    } else if (auto *cast = llvm::dyn_cast<llvm::ZExtInst>(value)) {
      result = Offset{
          0, llvm::APInt::getMaxValue(cast->getSrcTy()->getIntegerBitWidth())
                 .getZExtValue()};
      if (auto source = get(cast->getOperand(0)))
        result = source;
    } else {
      llvm::SmallVector<llvm::Value *, 4> alternatives;
      if (auto *select = llvm::dyn_cast<llvm::SelectInst>(value))
        alternatives = {select->getTrueValue(), select->getFalseValue()};
      else if (auto *phi = llvm::dyn_cast<llvm::PHINode>(value))
        for (auto &incoming : phi->incoming_values())
          alternatives.push_back(incoming);
      for (auto *alternative : alternatives) {
        auto bound = get(alternative);
        if (!bound) {
          result.reset();
          break;
        }
        result = result ? Offset{std::min(result->minimum, bound->minimum),
                                 std::max(result->maximum, bound->maximum)}
                        : bound;
      }
    }
    visiting.erase(value);
    if (result)
      bounds[value] = *result;
    return result;
  }
};

struct Recurrence {
  llvm::PHINode &root;
  llvm::Loop &loop;
  uint64_t limit;
  llvm::DenseMap<llvm::Value *, Offset> offsets;
  llvm::SmallPtrSet<llvm::Value *, 32> visiting;
  unsigned visited = 0;
  IncrementBounds increments;

  Recurrence(llvm::PHINode &root, llvm::Loop &loop, uint64_t limit)
      : root(root), loop(loop), limit(limit) {}

  std::optional<Offset> trace(llvm::Value *value) {
    if (auto found = offsets.find(value); found != offsets.end())
      return found->second;
    if (value == &root)
      return offsets[value] = Offset{};
    auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
    if (!instruction || !loop.contains(instruction) ||
        value->getType() != root.getType() || ++visited > 128 ||
        !visiting.insert(value).second)
      return std::nullopt;
    std::optional<Offset> result;
    llvm::Value *left = nullptr, *right = nullptr;
    if (auto *add = llvm::dyn_cast<llvm::BinaryOperator>(instruction);
        add && add->getOpcode() == llvm::Instruction::Add) {
      left = add->getOperand(0);
      right = add->getOperand(1);
    } else if (auto *lane = llvm::dyn_cast<llvm::ExtractValueInst>(instruction);
               lane && lane->getNumIndices() == 1 && *lane->idx_begin() == 0) {
      if (auto *add =
              llvm::dyn_cast<llvm::IntrinsicInst>(lane->getAggregateOperand());
          add &&
          (add->getIntrinsicID() == llvm::Intrinsic::uadd_with_overflow ||
           add->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow)) {
        left = add->getArgOperand(0);
        right = add->getArgOperand(1);
      }
    }
    if (left) {
      for (unsigned order = 0; order < 2 && !result; ++order) {
        auto amount = increments.get(right);
        if (amount && amount->maximum <= limit)
          if (auto previous = trace(left);
              previous && previous->maximum <= limit - amount->maximum)
            result = Offset{previous->minimum + amount->minimum,
                            previous->maximum + amount->maximum};
        std::swap(left, right);
      }
    } else {
      llvm::SmallVector<llvm::Value *, 4> alternatives;
      if (auto *phi = llvm::dyn_cast<llvm::PHINode>(instruction)) {
        for (auto &incoming : phi->incoming_values())
          alternatives.push_back(incoming);
      } else if (auto *select = llvm::dyn_cast<llvm::SelectInst>(instruction)) {
        alternatives = {select->getTrueValue(), select->getFalseValue()};
      }
      for (auto *alternative : alternatives) {
        auto offset = trace(alternative);
        if (!offset) {
          result.reset();
          break;
        }
        result = result ? Offset{std::min(result->minimum, offset->minimum),
                                 std::max(result->maximum, offset->maximum)}
                        : *offset;
      }
    }
    visiting.erase(value);
    if (result)
      offsets[value] = *result;
    return result;
  }
};

struct Plan {
  llvm::Loop *loop = nullptr;
  Traversal traversal;
  uint64_t maximumBackedges = 0;
  bool unconditional = false;
  unsigned cost = 0;
  llvm::SmallVector<std::pair<llvm::ICmpInst *, bool>, 4> comparisons;
  llvm::SmallVector<llvm::ExtractValueInst *, 4> overflowLanes;
};

std::optional<Plan> planFor(llvm::Loop &loop, llvm::DominatorTree &dt,
                            unsigned remainingGrowth) {
  if (!loop.getSubLoops().empty() || !loop.isLoopSimplifyForm() ||
      !loop.isLCSSAForm(dt) || hasCheckedLoopMarker(loop))
    return std::nullopt;
  auto cost = checkedLoopCloneCost(loop);
  auto induction = traversal(loop, dt);
  if (!cost || !induction || *cost > remainingGrowth)
    return std::nullopt;
  auto *latch = loop.getLoopLatch();
  for (auto &phi : loop.getHeader()->phis()) {
    if (&phi == induction->index || !phi.getType()->isIntegerTy() ||
        phi.getType()->getIntegerBitWidth() < 2 ||
        phi.getType()->getIntegerBitWidth() > 64 ||
        phi.getNumIncomingValues() != 2)
      continue;
    auto *initial = llvm::dyn_cast<llvm::ConstantInt>(
        phi.getIncomingValueForBlock(loop.getLoopPreheader()));
    if (!initial || initial->getValue().isNegative())
      continue;
    const auto width = phi.getType()->getIntegerBitWidth();
    const auto limit = llvm::APInt::getSignedMaxValue(width).getZExtValue();
    const auto start = initial->getZExtValue();
    Recurrence recurrence{phi, loop, limit};
    auto delta = recurrence.trace(phi.getIncomingValueForBlock(latch));
    if (!delta || delta->maximum == 0 || delta->maximum > limit - start)
      continue;
    const auto threshold = (limit - start) / delta->maximum - 1;
    auto maximumBackedges = threshold;
    bool unconditional = false;
    if (auto *bound = llvm::dyn_cast<llvm::ConstantInt>(induction->bound)) {
      auto backedges = bound->getValue() - induction->initial;
      if (induction->comparesNext)
        --backedges; // Deliberate modulo-width distance, including bound zero.
      if (backedges.getZExtValue() > threshold)
        continue;
      maximumBackedges = backedges.getZExtValue();
      unconditional = true;
    } else if (llvm::APInt::getMaxValue(
                   induction->bound->getType()->getIntegerBitWidth())
                   .getZExtValue() <= threshold) {
      maximumBackedges = llvm::APInt::getMaxValue(
                             induction->bound->getType()->getIntegerBitWidth())
                             .getZExtValue();
      unconditional = true;
    }
    // The division above guarantees this multiplication fits, without relying
    // on IR poison flags. Every intermediate belongs to the traced update DAG.
    const auto rootMaximum = start + maximumBackedges * delta->maximum;
    Plan plan{&loop, *induction, threshold, unconditional, *cost, {}, {}};
    // LLVM may sink a successful add into a PHI join while leaving each
    // predecessor's overflow intrinsic in place. Prove the exact operands of
    // each detector, rather than requiring its lane zero to be the recurrence.
    // Only replace lane-one observations INSIDE the fast loop: outside
    // aggregate PHIs may also carry slow-loop values.
    for (auto *block : loop.blocks())
      for (auto &instruction : *block) {
        auto *add = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
        if (!add ||
            (add->getIntrinsicID() != llvm::Intrinsic::uadd_with_overflow &&
             add->getIntrinsicID() != llvm::Intrinsic::sadd_with_overflow) ||
            add->getArgOperand(0)->getType() != phi.getType())
          continue;
        bool safe = false;
        for (unsigned order = 0; order < 2 && !safe; ++order) {
          auto offset = recurrence.offsets.find(add->getArgOperand(order));
          auto amount =
              recurrence.increments.get(add->getArgOperand(1 - order));
          safe =
              offset != recurrence.offsets.end() && amount &&
              amount->maximum <= limit &&
              offset->second.maximum <= limit - amount->maximum &&
              rootMaximum <= limit - amount->maximum - offset->second.maximum;
        }
        if (!safe)
          continue;
        for (auto *user : add->users())
          if (auto *lane = llvm::dyn_cast<llvm::ExtractValueInst>(user);
              lane && loop.contains(lane) && lane->getNumIndices() == 1 &&
              *lane->idx_begin() == 1)
            plan.overflowLanes.push_back(lane);
      }
    for (auto *block : loop.blocks()) {
      auto *branch = conditional(block);
      auto *comparison =
          branch ? llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition())
                 : nullptr;
      if (!comparison || block == latch ||
          (loop.contains(branch->getSuccessor(0)) &&
           loop.contains(branch->getSuccessor(1))))
        continue;
      auto *constant =
          llvm::dyn_cast<llvm::ConstantInt>(comparison->getOperand(1));
      auto *value = comparison->getOperand(0);
      auto predicate = comparison->getPredicate();
      if (!constant) {
        constant = llvm::dyn_cast<llvm::ConstantInt>(comparison->getOperand(0));
        value = comparison->getOperand(1);
        predicate = llvm::CmpInst::getSwappedPredicate(predicate);
      }
      const auto offset = recurrence.offsets.find(value);
      if (!constant || offset == recurrence.offsets.end())
        continue;
      const auto low = start + offset->second.minimum;
      const auto high = rootMaximum + offset->second.maximum;
      const llvm::ConstantRange range(llvm::APInt(width, low),
                                      llvm::APInt(width, high + 1));
      const llvm::ConstantRange bound(constant->getValue());
      if (range.icmp(predicate, bound))
        plan.comparisons.emplace_back(comparison, true);
      else if (range.icmp(llvm::CmpInst::getInversePredicate(predicate), bound))
        plan.comparisons.emplace_back(comparison, false);
    }
    if (!plan.comparisons.empty() || !plan.overflowLanes.empty())
      return plan;
  }
  return std::nullopt;
}

void apply(Plan &plan, llvm::LoopInfo &loops, llvm::DominatorTree &dt) {
  auto &loop = *plan.loop;
  auto *guard = loop.getLoopPreheader();
  llvm::IRBuilder<> entry(guard->getTerminator());
  llvm::Value *frozenBound = plan.traversal.bound;
  if (!llvm::isa<llvm::ConstantInt>(frozenBound))
    frozenBound = entry.CreateFreeze(frozenBound, "reduction.bound");
  if (!plan.unconditional) {
    auto [fast, slow, slowLoop] = cloneCheckedFallback(loop, loops, dt);
    llvm::IRBuilder<> builder(guard->getTerminator());
    // The fast latch uses the SAME frozen target. Undef must not choose a
    // different target at each comparison and evade the finite traversal.
    // Freeze also avoids a new poison observation on an earlier-exit path.
    // The first iteration is safe independently of this target.
    auto *bound = builder.CreateSub(
        frozenBound,
        llvm::ConstantInt::get(frozenBound->getType(), plan.traversal.initial),
        "reduction.distance");
    if (plan.traversal.comparesNext)
      bound =
          builder.CreateSub(bound, llvm::ConstantInt::get(bound->getType(), 1),
                            "reduction.backedges");
    const auto width = std::max(64U, bound->getType()->getIntegerBitWidth());
    auto *wide = builder.CreateZExtOrTrunc(bound, builder.getIntNTy(width));
    auto *safe = builder.CreateICmpULE(
        wide, llvm::ConstantInt::get(wide->getType(), plan.maximumBackedges),
        "reduction.fits");
    builder.CreateCondBr(safe, fast, slow);
    guard->getTerminator()->eraseFromParent();
    markCheckedLoop(*slowLoop, versionedMarker);
    dt.recalculate(*guard->getParent());
  }
  plan.traversal.comparison->setOperand(plan.traversal.boundOperand,
                                        frozenBound);
  markCheckedLoop(loop, versionedMarker);
  for (auto [comparison, value] : plan.comparisons)
    comparison->replaceAllUsesWith(
        llvm::ConstantInt::getBool(comparison->getContext(), value));
  for (auto *lane : plan.overflowLanes)
    lane->replaceAllUsesWith(llvm::ConstantInt::getFalse(lane->getContext()));
}

class CheckedReductionPass : public llvm::PassInfoMixin<CheckedReductionPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses) {
    auto result = optimizeCheckedReductions(function, analyses);
    if (result.areAllPreserved())
      return result;
    llvm::FunctionPassManager cleanup;
    cleanup.addPass(llvm::SimplifyCFGPass());
    cleanup.addPass(llvm::InstCombinePass());
    cleanup.addPass(llvm::LoopSimplifyPass());
    cleanup.addPass(llvm::LCSSAPass());
    cleanup.run(function, analyses);
    return llvm::PreservedAnalyses::none();
  }
};

} // namespace

llvm::PreservedAnalyses
optimizeCheckedReductions(llvm::Function &function,
                          llvm::FunctionAnalysisManager &analyses) {
  if (function.isDeclaration() || function.hasOptSize() ||
      function.hasFnAttribute(llvm::Attribute::OptimizeNone))
    return llvm::PreservedAnalyses::all();
  bool changed = false;
  unsigned remaining = maximumFunctionGrowth;
  for (unsigned iteration = 0; iteration < maximumTransforms; ++iteration) {
    auto &loops = analyses.getResult<llvm::LoopAnalysis>(function);
    auto &dt = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
    std::optional<Plan> plan;
    for (auto *loop : loops.getLoopsInPreorder())
      if ((plan = planFor(*loop, dt, remaining)))
        break;
    if (!plan)
      break;
    apply(*plan, loops, dt);
    if (!plan->unconditional)
      remaining -= plan->cost;
    changed = true;
    // Cloning and changed exits retire every cached loop/range/dominance fact.
    // Reacquire analyses for the next loop instead of reusing stale proofs.
    analyses.invalidate(function, llvm::PreservedAnalyses::none());
  }
  return changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

void registerCheckedReductionPass(llvm::PassBuilder &builder) {
  builder.registerVectorizerStartEPCallback(
      [](llvm::FunctionPassManager &pipeline, llvm::OptimizationLevel level) {
        if (level != llvm::OptimizationLevel::O2 &&
            level != llvm::OptimizationLevel::O3)
          return;
        pipeline.addPass(llvm::LoopSimplifyPass());
        pipeline.addPass(llvm::LCSSAPass());
        pipeline.addPass(CheckedReductionPass());
      });
}

} // namespace lang::native_llvm
