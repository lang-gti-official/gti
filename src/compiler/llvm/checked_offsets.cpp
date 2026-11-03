#include "checked_offsets.h"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>

#include <optional>
#include <vector>

namespace lang::native_llvm {
namespace {
#if LLVM_VERSION_MAJOR >= 23
using ConditionalBranch = llvm::CondBrInst;
#else
using ConditionalBranch = llvm::BranchInst;
#endif

// A fixed denominator bounds nonnegative dyadic multiples of one difference.
// 128-bit arithmetic leaves room for bounded coefficients without host
// overflow.
const llvm::APInt unit(128, 1ULL << 63);

class OffsetBound {
  llvm::Value *difference_;
  llvm::Value *low_;
  unsigned budget_ = 128;

  std::optional<llvm::APInt> bounded(llvm::APInt value) {
    if (value.ugt(unit))
      return std::nullopt;
    return value;
  }

public:
  OffsetBound(llvm::Value *difference, llvm::Value *low)
      : difference_(difference), low_(low) {}

  std::optional<llvm::APInt> offset(llvm::Value *value, unsigned depth = 0) {
    if (depth > 24 || budget_ == 0)
      return std::nullopt;
    --budget_;
    if (value == difference_)
      return unit;
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
      return constant->isZero()
                 ? std::optional<llvm::APInt>(llvm::APInt(128, 0))
                 : std::nullopt;
    auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
    if (!binary)
      return std::nullopt;
    auto *right = llvm::dyn_cast<llvm::ConstantInt>(binary->getOperand(1));
    if (binary->getOpcode() == llvm::Instruction::LShr && right &&
        right->getValue().ult(value->getType()->getIntegerBitWidth()) &&
        right->getValue().ule(63)) {
      auto bound = offset(binary->getOperand(0), depth + 1);
      if (!bound)
        return std::nullopt;
      unsigned shift = static_cast<unsigned>(right->getZExtValue());
      // Round upward: a prior sum need not have a coefficient divisible by 2^k.
      return bounded(
          (*bound + llvm::APInt(128, (1ULL << shift) - 1)).lshr(shift));
    }
    if (binary->getOpcode() == llvm::Instruction::Add) {
      auto left = offset(binary->getOperand(0), depth + 1);
      auto other = offset(binary->getOperand(1), depth + 1);
      if (left && other)
        return bounded(*left + *other);
    }
    if (binary->getOpcode() == llvm::Instruction::Mul && right &&
        right->getValue().getActiveBits() <= 63) {
      auto left = offset(binary->getOperand(0), depth + 1);
      if (left)
        return bounded(*left * right->getValue().zextOrTrunc(128));
    }
    if (binary->getOpcode() == llvm::Instruction::Shl && right &&
        right->getValue().ult(value->getType()->getIntegerBitWidth()) &&
        right->getValue().ule(63)) {
      auto left = offset(binary->getOperand(0), depth + 1);
      if (left)
        return bounded(left->shl(static_cast<unsigned>(right->getZExtValue())));
    }
    return std::nullopt;
  }

  std::optional<llvm::APInt> rebased(llvm::Value *value, unsigned depth = 0) {
    if (depth > 24 || budget_ == 0)
      return std::nullopt;
    --budget_;
    if (value == low_)
      return llvm::APInt(128, 0);
    llvm::Value *left = nullptr, *right = nullptr;
    if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(value);
        binary && binary->getOpcode() == llvm::Instruction::Add) {
      left = binary->getOperand(0);
      right = binary->getOperand(1);
    } else if (auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(value);
               extract && extract->getNumIndices() == 1 &&
               *extract->idx_begin() == 0) {
      auto *intrinsic =
          llvm::dyn_cast<llvm::IntrinsicInst>(extract->getAggregateOperand());
      if (intrinsic &&
          intrinsic->getIntrinsicID() == llvm::Intrinsic::uadd_with_overflow) {
        left = intrinsic->getArgOperand(0);
        right = intrinsic->getArgOperand(1);
      }
    }
    if (!left)
      return std::nullopt;
    for (unsigned swapped = 0; swapped != 2; ++swapped) {
      auto base = rebased(swapped ? right : left, depth + 1);
      auto amount = offset(swapped ? left : right, depth + 1);
      if (base && amount)
        return bounded(*base + *amount);
    }
    return std::nullopt;
  }

  bool sum(llvm::Value *left, llvm::Value *right) {
    for (unsigned swapped = 0; swapped != 2; ++swapped) {
      auto base = rebased(swapped ? right : left);
      auto amount = offset(swapped ? left : right);
      if (base && amount && (*base + *amount).ule(unit))
        return true;
    }
    return false;
  }
};

// Only an independently established unsigned comparison supplies low<=high.
// This path does not use subtraction flags or the candidate's overflow status;
// the separate nuw/refinement path below has its own poison obligations.
// Edge dominance handles shared exits.
bool ordered(llvm::Value *low, llvm::Value *high, llvm::Instruction &at,
             llvm::DominatorTree &dt) {
  if (!llvm::isGuaranteedNotToBeUndefOrPoison(low, nullptr, &at, &dt) ||
      !llvm::isGuaranteedNotToBeUndefOrPoison(high, nullptr, &at, &dt))
    return false;
  for (auto *user : low->users()) {
    auto *comparison = llvm::dyn_cast<llvm::ICmpInst>(user);
    if (!comparison)
      continue;
    auto predicate = comparison->getPredicate();
    if (comparison->getOperand(0) == high && comparison->getOperand(1) == low)
      predicate = llvm::ICmpInst::getSwappedPredicate(predicate);
    else if (comparison->getOperand(0) != low ||
             comparison->getOperand(1) != high)
      continue;
    for (auto *use : comparison->users()) {
      auto *branch = llvm::dyn_cast<ConditionalBranch>(use);
#if LLVM_VERSION_MAJOR < 23
      if (branch && !branch->isConditional())
        continue;
#endif
      if (!branch || branch->getCondition() != comparison ||
          branch->getSuccessor(0) == branch->getSuccessor(1))
        continue;
      unsigned success;
      if (predicate == llvm::ICmpInst::ICMP_ULE ||
          predicate == llvm::ICmpInst::ICMP_ULT)
        success = 0;
      else if (predicate == llvm::ICmpInst::ICMP_UGT ||
               predicate == llvm::ICmpInst::ICMP_UGE)
        success = 1;
      else
        continue;
      if (dt.dominates(llvm::BasicBlockEdge(branch->getParent(),
                                            branch->getSuccessor(success)),
                       at.getParent()))
        return true;
    }
  }
  return false;
}

class CheckedOffsetPass : public llvm::PassInfoMixin<CheckedOffsetPass> {
public:
  llvm::PreservedAnalyses run(llvm::Function &function,
                              llvm::FunctionAnalysisManager &analyses) {
    auto result = optimizeCheckedOffsets(function, analyses);
    if (result.areAllPreserved())
      return result;
    analyses.invalidate(function, result);
    llvm::FunctionPassManager cleanup;
    cleanup.addPass(llvm::InstCombinePass());
    cleanup.addPass(llvm::SimplifyCFGPass());
    cleanup.run(function, analyses);
    return llvm::PreservedAnalyses::none();
  }
};
} // namespace

llvm::PreservedAnalyses
optimizeCheckedOffsets(llvm::Function &function,
                       llvm::FunctionAnalysisManager &analyses) {
  if (function.isDeclaration() ||
      function.hasFnAttribute(llvm::Attribute::OptimizeNone))
    return llvm::PreservedAnalyses::all();
  auto &dt = analyses.getResult<llvm::DominatorTreeAnalysis>(function);
  std::vector<llvm::BinaryOperator *> differences;
  std::vector<llvm::IntrinsicInst *> additions;
  for (auto &block : function)
    for (auto &instruction : block) {
      if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction);
          binary && binary->getOpcode() == llvm::Instruction::Sub &&
          binary->getType()->isIntegerTy())
        differences.push_back(binary);
      if (auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
          intrinsic &&
          intrinsic->getIntrinsicID() == llvm::Intrinsic::uadd_with_overflow &&
          intrinsic->getArgOperand(0)->getType()->isIntegerTy())
        additions.push_back(intrinsic);
    }
  unsigned budget = 4096;
  bool changed = false;
  for (auto *addition : additions) {
    for (auto *difference : differences) {
      if (budget == 0)
        return changed ? llvm::PreservedAnalyses::none()
                       : llvm::PreservedAnalyses::all();
      --budget;
      if (difference->getType() != addition->getArgOperand(0)->getType() ||
          !dt.dominates(difference, addition))
        continue;
      auto *low = difference->getOperand(1);
      auto *high = difference->getOperand(0);
      // A pre-existing sub nuw is an LLVM representation contract, not an
      // inferred source fact. If high<low, its poison propagates through this
      // restricted DAG into the original overflow observation. Replacing that
      // poison with false is refinement. No freeze/select/PHI may launder a
      // poison difference into a defined offset, and endpoints must be stable.
      OffsetBound bounds(difference, low);
      if (!bounds.sum(addition->getArgOperand(0), addition->getArgOperand(1)) ||
          !(ordered(low, high, *addition, dt) ||
            (difference->hasNoUnsignedWrap() &&
             llvm::isGuaranteedNotToBeUndefOrPoison(low, nullptr, addition,
                                                    &dt) &&
             llvm::isGuaranteedNotToBeUndefOrPoison(high, nullptr, addition,
                                                    &dt))))
        continue;
      for (auto *user : addition->users())
        if (auto *extract = llvm::dyn_cast<llvm::ExtractValueInst>(user);
            extract && extract->getNumIndices() == 1 &&
            *extract->idx_begin() == 1 && !extract->use_empty()) {
          extract->replaceAllUsesWith(
              llvm::ConstantInt::getFalse(function.getContext()));
          changed = true;
        }
      break;
    }
  }
  return changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

void registerCheckedOffsetPass(llvm::PassBuilder &builder) {
  builder.registerVectorizerStartEPCallback(
      [](llvm::FunctionPassManager &pipeline, llvm::OptimizationLevel level) {
        if (level != llvm::OptimizationLevel::O2 &&
            level != llvm::OptimizationLevel::O3)
          return;
        pipeline.addPass(CheckedOffsetPass());
      });
}
} // namespace lang::native_llvm
