#include "checked_float.h"

#include <algorithm>
#include <cmath>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Passes/PassBuilder.h>
#include <optional>

namespace lang::native_llvm {
namespace {
using namespace llvm;
using namespace llvm::PatternMatch;

#if LLVM_VERSION_MAJOR >= 23
using ConditionalBranch = CondBrInst;
#else
using ConditionalBranch = BranchInst;
#endif

// An outward bound on the magnitude of a *finite* binary32 value. Look through
// direct calls only when every use is known. The visit budget also cuts cycles.
std::optional<double> magnitude(Value *value, unsigned &budget) {
  if (!budget || !value->getType()->isFloatTy())
    return std::nullopt;
  --budget;
  if (auto *constant = dyn_cast<ConstantFP>(value)) {
    const double v = constant->getValueAPF().convertToDouble();
    if (std::isfinite(v))
      return std::abs(v);
  }
  if (auto *cast = dyn_cast<CastInst>(value)) {
    const unsigned op = cast->getOpcode();
    if ((op == Instruction::SIToFP || op == Instruction::UIToFP) &&
        cast->getSrcTy()->isIntegerTy() &&
        cast->getSrcTy()->getIntegerBitWidth() <= 32) {
      // Includes rounding the largest positive integer up to a power of two.
      return std::ldexp(1.0, cast->getSrcTy()->getIntegerBitWidth() -
                                 (op == Instruction::SIToFP ? 1 : 0));
    }
  }
  Value *factor = nullptr;
  ConstantFP *scale = nullptr;
  if (match(value, m_c_FMul(m_Value(factor), m_ConstantFP(scale)))) {
    const double s = std::abs(scale->getValueAPF().convertToDouble());
    int exponent = 0;
    if (s <= 1.0 && std::frexp(s, &exponent) == 0.5) {
      if (const auto bound = magnitude(factor, budget)) {
        // Rounding subnormal products cannot justify a bound smaller than one.
        return std::max(1.0, *bound * s);
      }
    }
  }
  if (auto *arg = dyn_cast<Argument>(value)) {
    auto *function = arg->getParent();
    if (!function->hasLocalLinkage() || function->use_empty())
      return std::nullopt;
    double maximum = 0;
    for (Use &use : function->uses()) {
      auto *call = dyn_cast<CallBase>(use.getUser());
      if (!call || !call->isCallee(&use) ||
          call->getFunctionType() != function->getFunctionType())
        return std::nullopt;
      const auto bound =
          magnitude(call->getArgOperand(arg->getArgNo()), budget);
      if (!bound)
        return std::nullopt;
      maximum = std::max(maximum, *bound);
    }
    return maximum;
  }
  return std::nullopt;
}

// A binary32 that truncates into i32 also floors into i32. At INT32_MIN
// binary32 has no fractional values between it and the next smaller integer.
bool floorArms(FCmpInst &test, DominatorTree &dt) {
  if (test.getPredicate() != FCmpInst::FCMP_OLT || !test.hasOneUse())
    return false;
  auto *x = test.getOperand(0);
  if (!x->getType()->isFloatTy())
    return false;
  Value *integer = nullptr;
  if (!match(test.getOperand(1), m_SIToFP(m_Value(integer))) ||
      !integer->getType()->isIntegerTy(32) ||
      !match(integer, m_FPToSI(m_Specific(x))))
    return false;
  auto *branch = dyn_cast<ConditionalBranch>(*test.user_begin());
  if (!branch || branch->getCondition() != &test ||
      branch->getSuccessor(0) == branch->getSuccessor(1))
    return false;
  BasicBlockEdge fractional(branch->getParent(), branch->getSuccessor(0));
  BasicBlockEdge integral(branch->getParent(), branch->getSuccessor(1));
  SmallVector<Use *, 4> identities;
  SmallVector<Instruction *, 2> decrements;
  SmallVector<ICmpInst *, 2> impossible;
  for (Use &use : integer->uses()) {
    auto *inst = dyn_cast<Instruction>(use.getUser());
    if (!inst)
      continue;
    if (dt.dominates(integral, use)) {
      identities.push_back(&use);
    } else if (dt.dominates(fractional, use)) {
      if (match(inst, m_Add(m_Specific(integer), m_AllOnes())) ||
          match(inst, m_Sub(m_Specific(integer), m_One())))
        decrements.push_back(inst);
      if (auto *cmp = dyn_cast<ICmpInst>(inst);
          cmp && cmp->getPredicate() == ICmpInst::ICMP_EQ &&
          match(cmp->getOperand(1), m_SignMask()) &&
          cmp->getOperand(0) == integer)
        impossible.push_back(cmp);
    }
  }
  if (identities.empty() || decrements.empty())
    return false;
  // Only substitute values on their proven arms. Other side effects, failure
  // paths, and every use outside the two dominated regions stay in place.
  IRBuilder<> builder(&test);
  auto *rounded = builder.CreateUnaryIntrinsic(Intrinsic::floor, x);
  auto *floor = builder.CreateFPToSI(rounded, integer->getType());
  for (auto *use : identities)
    use->set(floor);
  for (auto *decrement : decrements)
    decrement->replaceAllUsesWith(floor);
  for (auto *cmp : impossible)
    cmp->replaceAllUsesWith(builder.getFalse());
  return true;
}

bool validConversionGuard(FCmpInst &cmp) {
  Value *x = cmp.getOperand(0);
  auto *limit = dyn_cast<ConstantFP>(cmp.getOperand(1));
  if (!x->getType()->isFloatTy() || !limit ||
      std::abs(limit->getValueAPF().convertToDouble()) != 0x1p31)
    return false;
  if (auto *intrinsic = dyn_cast<IntrinsicInst>(x);
      intrinsic && intrinsic->getIntrinsicID() == Intrinsic::trunc)
    x = intrinsic->getArgOperand(0);
  unsigned budget = 128;
  auto bound = magnitude(x, budget);
  if (!bound || *bound > 0x1p30)
    return false;
  // Every allowed value, also after truncation, is strictly on the same
  // side of this limit as zero. NaN and infinity were excluded by the proof.
  const bool result = FCmpInst::compare(APFloat(0.0f), limit->getValueAPF(),
                                        cmp.getPredicate());
  cmp.replaceAllUsesWith(ConstantInt::getBool(cmp.getContext(), result));
  return true;
}

// The greatest float below 2^31 is 2147483520. Consequently, no
// defined f32 -> i32 conversion can equal INT32_MAX. A poison conversion
// can be refined at this comparison; no assumption or new use is introduced.
class FloatIntegerRangePass : public PassInfoMixin<FloatIntegerRangePass> {
public:
  PreservedAnalyses run(Function &function, FunctionAnalysisManager &) {
    bool changed = false;
    for (BasicBlock &block : function)
      for (Instruction &inst : block) {
        auto *cmp = dyn_cast<ICmpInst>(&inst);
        if (!cmp || cmp->use_empty() || !cmp->isEquality() ||
            !cmp->getOperand(0)->getType()->isIntegerTy(32))
          continue;
        Value *input = nullptr;
        if (!match(cmp->getOperand(0), m_FPToSI(m_Value(input))) ||
            !input->getType()->isFloatTy() ||
            !match(cmp->getOperand(1), m_MaxSignedValue()))
          continue;
        cmp->replaceAllUsesWith(ConstantInt::getBool(
            cmp->getContext(), cmp->getPredicate() == ICmpInst::ICMP_NE));
        changed = true;
      }
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};
} // namespace

void registerFloatIntegerRangePass(llvm::PassBuilder &builder) {
  builder.registerScalarOptimizerLateEPCallback(
      [](llvm::FunctionPassManager &pm, llvm::OptimizationLevel level) {
        if (level == llvm::OptimizationLevel::O2 ||
            level == llvm::OptimizationLevel::O3)
          pm.addPass(FloatIntegerRangePass());
      });
}

bool simplifyCheckedFloors(llvm::Module &module) {
  bool changed = false;
  for (Function &function : module) {
    if (function.isDeclaration())
      continue;
    std::optional<DominatorTree> dt;
    for (BasicBlock &block : function)
      for (Instruction &inst : block) {
        auto *cmp = dyn_cast<FCmpInst>(&inst);
        if (!cmp || cmp->use_empty())
          continue;
        if (cmp->getPredicate() == FCmpInst::FCMP_OLT &&
            isa<SIToFPInst>(cmp->getOperand(1))) {
          if (!dt)
            dt.emplace(function);
          changed |= floorArms(*cmp, *dt);
        }
        changed |= validConversionGuard(*cmp);
      }
  }
  return changed;
}
} // namespace lang::native_llvm
