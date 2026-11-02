#include "../src/compiler/llvm/abi.h"
#include "../src/compiler/llvm/body.h"
#include "../src/compiler/llvm/loop_invariants.h"
#include "../src/compiler/llvm/placement.h"
#include "../src/compiler/llvm/scalar.h"
#include "gti/frontend.h"
#include "gti/lowered_program_builder.h"
#include "gti/optimizer.h"

#include <llvm/CodeGen/TargetSubtargetInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/MCSubtargetInfo.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace {
// LLVM releases differ on whether TargetMachine::getMCSubtargetInfo() returns
// a pointer or a reference (LLVM 20-21 return a pointer); accept either, as
// llvm_cost_target_probe does.
// A subtarget table entry's name: LLVM 23 exposes key(), while LLVM 20-21
// expose only the Key field.
template <class Entry> llvm::StringRef entryKey(const Entry &entry) {
  if constexpr (requires { entry.key(); })
    return llvm::StringRef(entry.key());
  else
    return llvm::StringRef(entry.Key);
}

template <class Machine>
const llvm::MCSubtargetInfo &machineSubtarget(const Machine &machine) {
  auto &&value = machine.getMCSubtargetInfo();
  if constexpr (std::is_pointer_v<std::remove_reference_t<decltype(value)>>) {
    if (value == nullptr)
      throw std::runtime_error("target machine has no MC subtarget");
    return *value;
  } else {
    return value;
  }
}
} // namespace

using namespace lang;
using namespace lang::native_llvm;

namespace {
void verifyFailureBranchHints(
    std::initializer_list<const llvm::Function *> functions) {
#if LLVM_VERSION_MAJOR >= 23
  using ConditionalBranch = llvm::CondBrInst;
#else
  using ConditionalBranch = llvm::BranchInst;
#endif
  unsigned checks = 0, calls = 0, drops = 0, ordinary = 0;
  for (const auto *function : functions) {
    if (!function)
      throw std::runtime_error("missing failure-likelihood observation root");
    for (const auto &block : *function) {
      const auto *branch =
          llvm::dyn_cast<ConditionalBranch>(block.getTerminator());
      if (!branch || branch->getNumSuccessors() != 2)
        continue;
      std::optional<unsigned> failure;
      bool detector = false;
      for (unsigned index = 0; index < 2; ++index) {
        const auto name = branch->getSuccessor(index)->getName().str();
        if (name.starts_with("check.failed")) {
          ++checks;
          detector = true;
          failure = index;
        } else if (name.starts_with("call.failed")) {
          ++calls;
          failure = index;
        } else if (name.starts_with("drop.failed")) {
          ++drops;
          failure = index;
        }
      }
      const auto *metadata = branch->getMetadata(llvm::LLVMContext::MD_prof);
      if (detector) {
        if (metadata)
          throw std::runtime_error(
              "direct detector acquired an explicit failure preference");
        continue;
      }
      if (!failure) {
        ++ordinary;
        if (metadata)
          throw std::runtime_error(
              "ordinary source/liveness branch acquired failure weights");
        continue;
      }
      if (!metadata || metadata->getNumOperands() != 3)
        throw std::runtime_error("failure edge lost its relative likelihood");
      const auto *tag = llvm::dyn_cast<llvm::MDString>(metadata->getOperand(0));
      const auto *failedWeight = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
          metadata->getOperand(1 + *failure));
      const auto *normalWeight = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
          metadata->getOperand(2 - *failure));
      if (!tag || tag->getString() != "branch_weights" || !failedWeight ||
          !normalWeight || failedWeight->isZero() ||
          normalWeight->getZExtValue() <= failedWeight->getZExtValue())
        throw std::runtime_error(
            "failure hint changed polarity or erased the rare continuation");
    }
  }
  if (!checks || !calls || !drops || !ordinary)
    throw std::runtime_error(
        "failure hint fixture lost a checked, call, cleanup or ordinary edge");
}

void verifyCheckedArithmeticReuse() {
  llvm::LLVMContext context;
  llvm::Module module("checked-result-reuse", context);
  auto *integer = llvm::Type::getInt64Ty(context);
  const llvm::Intrinsic::ID detectors[] = {
      llvm::Intrinsic::sadd_with_overflow, llvm::Intrinsic::uadd_with_overflow,
      llvm::Intrinsic::ssub_with_overflow, llvm::Intrinsic::usub_with_overflow,
      llvm::Intrinsic::smul_with_overflow, llvm::Intrinsic::umul_with_overflow};
  for (unsigned operation = 0; operation < 6; ++operation) {
    for (unsigned shape = 0; shape < 7; ++shape) {
      auto *function = llvm::Function::Create(
          llvm::FunctionType::get(
              integer,
              {integer, integer, integer, llvm::Type::getInt1Ty(context)},
              false),
          llvm::GlobalValue::ExternalLinkage,
          "reuse_" + std::to_string(operation) + "_" + std::to_string(shape),
          module);
      auto *entry = llvm::BasicBlock::Create(context, "entry", function);
      auto *success = llvm::BasicBlock::Create(context, "success", function);
      auto *failure = llvm::BasicBlock::Create(context, "failure", function);
      llvm::IRBuilder<> builder(entry);
      if (shape == 3) {
        // A second predecessor bypasses the detector: reusing its lane here
        // would introduce a use not dominated by its definition.
        auto *checked = llvm::BasicBlock::Create(context, "check", function);
        builder.CreateCondBr(function->getArg(3), checked, success);
        builder.SetInsertPoint(checked);
      }
      auto *pair = builder.CreateBinaryIntrinsic(
          detectors[operation], function->getArg(0), function->getArg(1));
      auto *overflow = builder.CreateExtractValue(pair, 1);
      auto *guard = builder.CreateCondBr(overflow, failure, success);
      builder.SetInsertPoint(failure);
      builder.CreateRet(llvm::ConstantInt::getSigned(integer, -1));
      builder.SetInsertPoint(success);
      llvm::Value *left = function->getArg(0), *right = function->getArg(1);
      if (shape == 1)
        std::swap(left, right);
      if (shape == 2)
        right = function->getArg(2);
      const bool signedValue = operation % 2 == 0;
      const unsigned group =
          shape == 4 ? (operation / 2 + 1) % 3 : operation / 2;
      if (shape == 6)
        builder.SetInsertPoint(llvm::cast<llvm::Instruction>(pair));
      llvm::Value *result = nullptr;
      if (group == 0)
        result =
            builder.CreateAdd(left, right, "result", shape != 5 && !signedValue,
                              shape != 5 && signedValue);
      if (group == 1)
        result =
            builder.CreateSub(left, right, "result", shape != 5 && !signedValue,
                              shape != 5 && signedValue);
      if (group == 2)
        result =
            builder.CreateMul(left, right, "result", shape != 5 && !signedValue,
                              shape != 5 && signedValue);
      builder.SetInsertPoint(success);
      auto *returned = builder.CreateRet(result);
      reuseCheckedArithmeticResults(module);
      const bool shouldReuse = shape == 0 || (shape == 1 && operation / 2 != 1);
      const auto *lane =
          llvm::dyn_cast<llvm::ExtractValueInst>(returned->getReturnValue());
      if (shouldReuse) {
        if (!lane || lane->getAggregateOperand() != pair ||
            lane->getNumIndices() != 1 || *lane->idx_begin() != 0)
          throw std::runtime_error("identical checked result was recomputed");
      } else if (!llvm::isa<llvm::BinaryOperator>(returned->getReturnValue())) {
        throw std::runtime_error("checked reuse crossed an operand, operation, "
                                 "dominance or wrapping boundary");
      }
      if (guard->getCondition() != overflow)
        throw std::runtime_error("checked reuse changed its live guard");
      if (llvm::verifyModule(module))
        throw std::runtime_error("checked reuse broke LLVM dominance");
    }
  }
  auto *chain = llvm::Function::Create(
      llvm::FunctionType::get(integer, {integer, integer, integer}, false),
      llvm::GlobalValue::ExternalLinkage, "nested_checked_chain", module);
  llvm::IRBuilder<> builder(llvm::BasicBlock::Create(context, "entry", chain));
  auto *first = builder.CreateBinaryIntrinsic(
      llvm::Intrinsic::uadd_with_overflow, chain->getArg(0), chain->getArg(1));
  auto *firstResult =
      builder.CreateAdd(chain->getArg(0), chain->getArg(1), "first", true);
  auto *second = builder.CreateBinaryIntrinsic(
      llvm::Intrinsic::umul_with_overflow, firstResult, chain->getArg(2));
  auto *secondResult =
      builder.CreateMul(firstResult, chain->getArg(2), "second", true);
  auto *returned = builder.CreateRet(secondResult);
  reuseCheckedArithmeticResults(module);
  const auto *firstLane = llvm::dyn_cast<llvm::ExtractValueInst>(
      llvm::cast<llvm::IntrinsicInst>(second)->getArgOperand(0));
  const auto *secondLane =
      llvm::dyn_cast<llvm::ExtractValueInst>(returned->getReturnValue());
  if (!firstLane || firstLane->getAggregateOperand() != first || !secondLane ||
      secondLane->getAggregateOperand() != second || llvm::verifyModule(module))
    throw std::runtime_error("checked reuse lost a dependent operand chain");
}

void verifyRecoveredRangeChecks() {
  llvm::LLVMContext context;
  llvm::Module module("recovered-range-checks", context);
  auto *integer = llvm::Type::getInt8Ty(context);
  struct Expectation {
    llvm::ReturnInst *returned;
    bool recover;
  };
  std::vector<Expectation> expectations;
  for (unsigned operation = 0; operation < 6; ++operation) {
    const bool isSigned = operation % 2 == 0;
    const unsigned group = operation / 2;
    const auto opcode = group == 0   ? llvm::Instruction::Add
                        : group == 1 ? llvm::Instruction::Sub
                                     : llvm::Instruction::Mul;
    for (int constant : {-127, -3, -1, 1, 3, 7, 127}) {
      const llvm::APInt rhs(8, constant, true);
      const auto valid = llvm::ConstantRange::makeExactNoWrapRegion(
          opcode, rhs,
          isSigned ? llvm::OverflowingBinaryOperator::NoSignedWrap
                   : llvm::OverflowingBinaryOperator::NoUnsignedWrap);
      if (valid.isFullSet() || valid.isEmptySet())
        continue;
      // Independent exhaustive arithmetic oracle, including both signed limits.
      for (unsigned raw = 0; raw < 256; ++raw) {
        const llvm::APInt value(8, raw);
        bool overflow;
        if (group == 0) {
          if (isSigned)
            static_cast<void>(value.sadd_ov(rhs, overflow));
          else
            static_cast<void>(value.uadd_ov(rhs, overflow));
        } else if (group == 1) {
          if (isSigned)
            static_cast<void>(value.ssub_ov(rhs, overflow));
          else
            static_cast<void>(value.usub_ov(rhs, overflow));
        } else {
          if (isSigned)
            static_cast<void>(value.smul_ov(rhs, overflow));
          else
            static_cast<void>(value.umul_ov(rhs, overflow));
        }
        if (valid.contains(value) == overflow)
          throw std::runtime_error(
              "range guard disagrees with arithmetic overflow");
      }
      for (bool successCondition : {false, true}) {
        for (unsigned shape = 0; shape < 9; ++shape) {
          auto *function = llvm::Function::Create(
              llvm::FunctionType::get(integer, {integer, integer}, false),
              llvm::GlobalValue::ExternalLinkage, "range", module);
          auto *entry = llvm::BasicBlock::Create(context, "entry", function);
          auto *success =
              llvm::BasicBlock::Create(context, "success", function);
          auto *failure =
              llvm::BasicBlock::Create(context, "failure", function);
          llvm::IRBuilder<> builder(entry);
          auto range = successCondition ? valid : valid.inverse();
          llvm::CmpInst::Predicate predicate;
          llvm::APInt bound(8, 0), offset(8, 0);
          range.getEquivalentICmp(predicate, bound, offset);
          if (shape == 8) {
            // Equivalent biased encoding exercises all arithmetic families,
            // including ranges LLVM normally spells as a single comparison.
            predicate = llvm::CmpInst::ICMP_ULT;
            offset = -range.getLower();
            bound = range.getUpper() - range.getLower();
          }
          if (shape == 1)
            bound += 1; // Nearly matching is insufficient.
          auto *base = function->getArg(shape == 2 ? 1 : 0);
          auto *tested =
              offset.isZero() && shape != 5
                  ? static_cast<llvm::Value *>(base)
                  : builder.CreateAdd(base,
                                      llvm::ConstantInt::get(integer, offset),
                                      "bias", false, shape == 5);
          auto *boundValue = llvm::ConstantInt::get(integer, bound);
          auto *condition =
              shape == 6 ? builder.CreateICmp(
                               llvm::CmpInst::getSwappedPredicate(predicate),
                               boundValue, tested)
                         : builder.CreateICmp(predicate, tested, boundValue);
          builder.CreateCondBr(condition, successCondition ? success : failure,
                               successCondition ? failure : success);
          builder.SetInsertPoint(failure);
          builder.CreateRet(llvm::ConstantInt::get(integer, 0));
          builder.SetInsertPoint(success);
          if (shape == 3)
            builder.SetInsertPoint(llvm::cast<llvm::Instruction>(condition));
          auto *constantValue = llvm::ConstantInt::get(integer, rhs);
          auto *result = llvm::cast<llvm::BinaryOperator>(builder.CreateBinOp(
              opcode,
              shape == 7 ? static_cast<llvm::Value *>(constantValue)
                         : function->getArg(0),
              shape == 7 ? static_cast<llvm::Value *>(function->getArg(0))
                         : constantValue));
          if (shape != 4) {
            result->setHasNoSignedWrap(isSigned);
            result->setHasNoUnsignedWrap(!isSigned);
          }
          builder.SetInsertPoint(success);
          auto *returned = builder.CreateRet(result);
          expectations.push_back(
              {returned,
               !offset.isZero() && (shape == 0 || shape == 6 || shape == 8 ||
                                    (shape == 7 && group != 1))});
        }
      }
    }
  }
  reuseCheckedArithmeticResults(module);
  for (const auto &expected : expectations) {
    const bool recovered =
        llvm::isa<llvm::ExtractValueInst>(expected.returned->getReturnValue());
    if (recovered != expected.recover)
      throw std::runtime_error(
          "range recovery changed an unmatched guard or missed an exact guard");
  }
  if (llvm::verifyModule(module))
    throw std::runtime_error("range recovery broke LLVM IR");
}

void verifyMergedCheckedArithmetic() {
  llvm::LLVMContext context;
  llvm::Module module("merged-checked-arithmetic", context);
  auto *integer = llvm::Type::getInt64Ty(context);
  for (unsigned shape = 0; shape < 4; ++shape) {
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(
            integer, {integer, integer, llvm::Type::getInt1Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage, "merge", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *left = llvm::BasicBlock::Create(context, "left", function);
    auto *right = llvm::BasicBlock::Create(context, "right", function);
    auto *join = llvm::BasicBlock::Create(context, "join", function);
    auto *failure = llvm::BasicBlock::Create(context, "failure", function);
    llvm::IRBuilder<> builder(entry);
    builder.CreateCondBr(function->getArg(2), left, right);
    builder.SetInsertPoint(left);
    auto *pair =
        builder.CreateBinaryIntrinsic(llvm::Intrinsic::uadd_with_overflow,
                                      function->getArg(0), function->getArg(1));
    auto *overflow = builder.CreateExtractValue(pair, 1);
    builder.CreateCondBr(overflow, failure, join);
    builder.SetInsertPoint(right);
    auto *condition = builder.CreateICmpEQ(
        function->getArg(0),
        llvm::ConstantInt::getSigned(integer, shape == 1 ? -2 : -1));
    builder.CreateCondBr(condition, failure, join);
    builder.SetInsertPoint(failure);
    builder.CreateRet(llvm::ConstantInt::get(integer, 0));
    builder.SetInsertPoint(join);
    auto *operand = builder.CreatePHI(integer, 2);
    operand->addIncoming(function->getArg(1), left);
    operand->addIncoming(llvm::ConstantInt::get(integer, 1), right);
    auto *result =
        builder.CreateAdd(operand, function->getArg(0), "value", shape != 2);
    if (shape == 3) { // Both operands may be merged.
      auto *other = builder.CreatePHI(integer, 2);
      other->moveBefore(*operand->getParent(), operand->getIterator());
      other->addIncoming(function->getArg(0), left);
      other->addIncoming(function->getArg(0), right);
      llvm::cast<llvm::Instruction>(result)->setOperand(1, other);
    }
    auto *returned = builder.CreateRet(result);
    reuseCheckedArithmeticResults(module);
    const bool shouldRecover = shape == 0 || shape == 3;
    if (llvm::isa<llvm::PHINode>(returned->getReturnValue()) != shouldRecover)
      throw std::runtime_error(
          "merge recovery failed complete-edge requirement");
#if LLVM_VERSION_MAJOR >= 23
    const auto *rightBranch =
        llvm::cast<llvm::CondBrInst>(right->getTerminator());
#else
    const auto *rightBranch =
        llvm::cast<llvm::BranchInst>(right->getTerminator());
#endif
    if (!shouldRecover && rightBranch->getCondition() != condition)
      throw std::runtime_error("partial merge recovery changed a guard");
  }
  if (llvm::verifyModule(module))
    throw std::runtime_error("merge recovery broke dominance");
}

void verifyRecoveryExecutionFrequency() {
  llvm::LLVMContext context;
  llvm::Module module("recovery-execution-frequency", context);
  auto *integer = llvm::Type::getInt64Ty(context);
  // A range check before another conditional, a repeatedly executed loop
  // condition, and an existing detector across that same conditional.
  for (unsigned shape = 0; shape < 4; ++shape) {
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(
            integer, {integer, llvm::Type::getInt1Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage, "frequency", module);
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *checked =
        shape == 1 ? llvm::BasicBlock::Create(context, "loop.check", function)
                   : entry;
    auto *middle = llvm::BasicBlock::Create(context, "conditional", function);
    auto *success = llvm::BasicBlock::Create(context, "success", function);
    auto *failure = llvm::BasicBlock::Create(context, "failure", function);
    llvm::IRBuilder<> builder(entry);
    if (shape == 1) {
      builder.CreateBr(checked);
      builder.SetInsertPoint(checked);
    }
    llvm::Value *condition;
    if (shape == 2) {
      auto *pair = builder.CreateBinaryIntrinsic(
          llvm::Intrinsic::uadd_with_overflow, function->getArg(0),
          llvm::ConstantInt::get(integer, 1));
      condition = builder.CreateExtractValue(pair, 1);
    } else {
      auto *biased =
          builder.CreateAdd(function->getArg(0),
                            llvm::ConstantInt::get(integer, 1), "range.bias");
      condition =
          builder.CreateICmpEQ(biased, llvm::ConstantInt::get(integer, 0));
    }
    llvm::Value *result = nullptr;
    if (shape == 3)
      result = builder.CreateAdd(function->getArg(0),
                                 llvm::ConstantInt::get(integer, 1),
                                 "same.block", true);
    auto *guard = builder.CreateCondBr(condition, failure, middle);
    builder.SetInsertPoint(middle);
    builder.CreateCondBr(function->getArg(1), success,
                         shape == 1 ? checked : failure);
    builder.SetInsertPoint(success);
    if (!result)
      result = builder.CreateAdd(function->getArg(0),
                                 llvm::ConstantInt::get(integer, 1),
                                 "rare.result", true);
    auto *returned = builder.CreateRet(result);
    builder.SetInsertPoint(failure);
    builder.CreateRet(llvm::ConstantInt::get(integer, 0));
    reuseCheckedArithmeticResults(module);
    const bool expectedRecovery = shape == 2 || shape == 3;
    if (llvm::isa<llvm::ExtractValueInst>(returned->getReturnValue()) !=
        expectedRecovery)
      throw std::runtime_error(
          "range recovery moved arithmetic across a conditional or loop");
    if (!expectedRecovery && guard->getCondition() != condition)
      throw std::runtime_error("rejected frequency recovery changed its guard");
  }

  // An incoming edge with a dominating range guard is still insufficient:
  // a later conditional can bypass the merged arithmetic entirely. Planning
  // must reject the whole merge without rewriting either incoming guard.
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(integer,
                              {integer, integer, llvm::Type::getInt1Ty(context),
                               llvm::Type::getInt1Ty(context)},
                              false),
      llvm::GlobalValue::ExternalLinkage, "conditional.merge.edge", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *left = llvm::BasicBlock::Create(context, "left", function);
  auto *right = llvm::BasicBlock::Create(context, "right", function);
  auto *edge = llvm::BasicBlock::Create(context, "conditional.edge", function);
  auto *join = llvm::BasicBlock::Create(context, "join", function);
  auto *failure = llvm::BasicBlock::Create(context, "failure", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(function->getArg(2), left, right);
  builder.SetInsertPoint(left);
  auto *pair =
      builder.CreateBinaryIntrinsic(llvm::Intrinsic::uadd_with_overflow,
                                    function->getArg(0), function->getArg(1));
  auto *overflow = builder.CreateExtractValue(pair, 1);
  builder.CreateCondBr(overflow, failure, join);
  builder.SetInsertPoint(right);
  auto *condition = builder.CreateICmpEQ(
      function->getArg(0), llvm::ConstantInt::getSigned(integer, -1));
  auto *guard = builder.CreateCondBr(condition, failure, edge);
  builder.SetInsertPoint(edge);
  builder.CreateCondBr(function->getArg(3), failure, join);
  builder.SetInsertPoint(join);
  auto *operand = builder.CreatePHI(integer, 2);
  operand->addIncoming(function->getArg(1), left);
  operand->addIncoming(llvm::ConstantInt::get(integer, 1), edge);
  auto *result = builder.CreateAdd(function->getArg(0), operand, "value", true);
  auto *returned = builder.CreateRet(result);
  builder.SetInsertPoint(failure);
  builder.CreateRet(llvm::ConstantInt::get(integer, 0));
  reuseCheckedArithmeticResults(module);
  if (returned->getReturnValue() != result ||
      guard->getCondition() != condition)
    throw std::runtime_error(
        "range recovery speculated on a conditional merge edge");
  if (llvm::verifyModule(module))
    throw std::runtime_error("frequency recovery broke LLVM IR");
}

llvm::Function *addIndependentLaneFixture(llvm::Module &module) {
  auto &context = module.getContext();
  llvm::IRBuilder<> builder(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(builder.getInt1Ty(),
                              {builder.getPtrTy(), builder.getInt8Ty()}, false),
      llvm::GlobalValue::ExternalLinkage, "independent_byte_lanes", module);
  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", function));
  llvm::Value *found = builder.getFalse();
  // No loop: only the SLP pass can combine these independent adjacent loads.
  // The fixture has no alignment, aliasing, no-wrap or fast-math assumptions.
  for (unsigned lane = 0; lane < 16; ++lane) {
    auto *address = builder.CreateGEP(builder.getInt8Ty(), function->getArg(0),
                                      builder.getInt64(lane));
    auto *value =
        builder.CreateAlignedLoad(builder.getInt8Ty(), address, llvm::Align(1));
    found = builder.CreateOr(found,
                             builder.CreateICmpEQ(value, function->getArg(1)));
  }
  builder.CreateRet(found);
  return function;
}

llvm::Function *addColdRegionFixture(llvm::Module &module) {
  auto &context = module.getContext();
  auto *integer = llvm::Type::getInt32Ty(context);
  auto *sink = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {integer}, false),
      llvm::GlobalValue::ExternalLinkage, "cold_region_observe", module);
  sink->addFnAttr(llvm::Attribute::Cold);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(integer, {integer}, false),
      llvm::GlobalValue::ExternalLinkage, "cold_region_client", module);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *cold = llvm::BasicBlock::Create(context, "rare", function);
  auto *hot = llvm::BasicBlock::Create(context, "ordinary", function);
  llvm::IRBuilder<> builder(entry);
  builder.CreateCondBr(
      builder.CreateICmpSLT(function->getArg(0),
                            llvm::ConstantInt::get(integer, 0)),
      cold, hot);
  builder.SetInsertPoint(hot);
  builder.CreateRet(function->getArg(0));
  builder.SetInsertPoint(cold);
  for (unsigned index = 0; index < 20; ++index)
    builder.CreateCall(
        sink, {builder.CreateAdd(function->getArg(0),
                                 llvm::ConstantInt::get(integer, index))});
  builder.CreateRet(llvm::ConstantInt::getSigned(integer, -1));
  return function;
}

void verifyCheckedSquareBounds() {
  llvm::LLVMContext context;
  llvm::Module module("checked-square-bounds", context);
  for (unsigned width : {8U, 16U, 32U, 64U}) {
    auto *integer = llvm::IntegerType::get(context, width);
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(integer, {integer}, false),
        llvm::GlobalValue::ExternalLinkage, "square", module);
    llvm::IRBuilder<> builder(
        llvm::BasicBlock::Create(context, "entry", function));
    auto *input = function->getArg(0);
    for (bool sign : {false, true}) {
      for (std::uint64_t factor : {1U, 3U, 5U}) {
        auto *weighted =
            builder.CreateMul(input, llvm::ConstantInt::get(integer, factor),
                              "weighted", !sign, sign);
        const auto bound = checkedSquareResultBound(weighted, input, sign);
        if (!bound)
          throw std::runtime_error(
              "checked square lost an exact weighted identity");
        if (width <= 16) {
          std::uint64_t largest = 0;
          const std::int64_t limit = std::int64_t{1}
                                     << (width - (sign ? 1 : 0));
          for (std::int64_t value = sign ? -limit : 0; value < limit; ++value) {
            const auto square = std::uint64_t(value * value) * factor;
            if (square < std::uint64_t(limit)) {
              largest = std::max(largest, square);
              if (square > *bound)
                throw std::runtime_error(
                    "checked square bound excludes a successful product");
            }
          }
          if (largest != *bound)
            throw std::runtime_error(
                "checked square bound is not the exact integer maximum");
        }
      }
    }
    if (width == 64 && (checkedSquareResultBound(input, input, true) !=
                            9223372030926249001ULL ||
                        checkedSquareResultBound(input, input, false) !=
                            18446744065119617025ULL))
      throw std::runtime_error(
          "64-bit square bound rounded through floating point");
    builder.CreateRet(input);
  }
  auto *integer = llvm::Type::getInt32Ty(context);
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                              {pointer, pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "load_boundaries", module);
  llvm::IRBuilder<> builder(
      llvm::BasicBlock::Create(context, "entry", function));
  auto *first = builder.CreateLoad(integer, function->getArg(0));
  auto *same = builder.CreateLoad(integer, function->getArg(0));
  auto *different = builder.CreateLoad(integer, function->getArg(1));
  if (!checkedSquareResultBound(first, same, true) ||
      checkedSquareResultBound(first, different, true))
    throw std::runtime_error(
        "checked square confused same and different storage");
  auto *wrapped = builder.CreateMul(first, builder.getInt32(3));
  if (checkedSquareResultBound(wrapped, same, true))
    throw std::runtime_error(
        "wrapped weighted product acquired a square bound");
  auto *weighted =
      builder.CreateMul(first, builder.getInt32(3), "weighted", false, true);
  builder.CreateStore(builder.getInt32(1), function->getArg(0));
  auto *changed = builder.CreateLoad(integer, function->getArg(0));
  if (checkedSquareResultBound(weighted, changed, true))
    throw std::runtime_error("checked square crossed an intervening store");
  auto *opaque = llvm::Function::Create(
      llvm::FunctionType::get(builder.getVoidTy(), {pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "opaque", module);
  builder.CreateCall(opaque, {function->getArg(0)});
  auto *afterCall = builder.CreateLoad(integer, function->getArg(0));
  if (checkedSquareResultBound(changed, afterCall, true))
    throw std::runtime_error("checked square crossed an opaque call");
  auto *volatileLoad = builder.CreateLoad(integer, function->getArg(0));
  volatileLoad->setVolatile(true);
  if (checkedSquareResultBound(afterCall, volatileLoad, true))
    throw std::runtime_error(
        "volatile reads acquired a repeated-value identity");
  builder.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
  auto *afterFence = builder.CreateLoad(integer, function->getArg(0));
  if (checkedSquareResultBound(afterCall, afterFence, true))
    throw std::runtime_error("checked square crossed synchronization");
  auto *continuation = llvm::BasicBlock::Create(context, "success", function);
  builder.CreateBr(continuation);
  builder.SetInsertPoint(continuation);
  auto *afterEdge = builder.CreateLoad(integer, function->getArg(0));
  if (!checkedSquareResultBound(afterFence, afterEdge, true))
    throw std::runtime_error(
        "unchanged single-predecessor path lost its identity");
  builder.CreateRetVoid();
  if (llvm::verifyModule(module))
    throw std::runtime_error("invalid checked-square structural fixture");
}

void verifySquareOffsetOptimization() {
  const auto frontend = Frontend().analyze("square-offset.gti", R"(
extern "C" { int square_input(); }
int square(int value) { return value * value + 7; }
int weighted(int value) { return (value * 3) * value + 7; }
int different(int left, int right) { return left * right + 7; }
int main() {
  int value = square_input();
  return square(value) + weighted(value) + different(value, value + 1);
}
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid square offset source");
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid square offset lowering");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified square offset lowering");
  for (const auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    const LlvmBackendOptions options{.optimization = level};
    const auto admission = planLlvmAdmission(*input, options);
    if (!admission.admitted())
      throw std::runtime_error(admission.limitation());
    Session session(*input, options);
    lowerScalarProgram(session, admission);
    std::map<std::string, llvm::Function *> observed;
    llvm::SmallVector<llvm::GlobalValue *> roots;
    for (const auto &instance : input->program().mir().functionInstances()) {
      const auto *declaration =
          input->program().findFunctionDeclaration(instance.declaration);
      if (declaration && (declaration->qualifiedName == "square" ||
                          declaration->qualifiedName == "weighted" ||
                          declaration->qualifiedName == "different")) {
        auto *function = session.abi().declareFunction(instance.id);
        observed.emplace(declaration->qualifiedName, function);
        roots.push_back(function);
      }
    }
    if (observed.size() != 3)
      throw std::runtime_error("missing square offset observation roots");
    llvm::appendToUsed(session.module(), roots);
    static_cast<void>(session.emitObject());
    for (const auto &[name, function] : observed) {
      unsigned additions = 0, conditionalBranches = 0;
      for (const auto &block : *function)
        for (const auto &instruction : block) {
          if (const auto *intrinsic =
                  llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
            additions += intrinsic->getIntrinsicID() ==
                         llvm::Intrinsic::sadd_with_overflow;
#if LLVM_VERSION_MAJOR >= 23
          conditionalBranches += llvm::isa<llvm::CondBrInst>(instruction);
#else
          if (const auto *branch =
                  llvm::dyn_cast<llvm::BranchInst>(&instruction))
            conditionalBranches += branch->isConditional();
#endif
        }
      // LLVM can rewrite add-with-overflow into a comparison with MAX-7.
      // Count the remaining failure decisions as well as the intrinsic so
      // that such a rewrite is not mistaken for eliminating the check.
      const bool unoptimized = level == LlvmCodegenOptimization::None;
      const bool valid =
          unoptimized ? additions > 0
          : name == "different"
              ? conditionalBranches >= 2
              : additions == 0 &&
                    conditionalBranches <= (name == "weighted" ? 2U : 1U);
      if (!valid) {
        function->print(llvm::errs());
        throw std::runtime_error(
            "square offset check disagrees with operand identity: " + name +
            " at optimization " + std::to_string(static_cast<int>(level)));
      }
    }
  }
}

void verifyBoundedRecursion() {
  llvm::LLVMContext context;
  llvm::Module module("bounded-recursion", context);
  auto *integer = llvm::Type::getInt64Ty(context);
  const auto make = [&](llvm::StringRef name, unsigned shape) {
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(integer, {integer}, false),
        llvm::GlobalValue::InternalLinkage, name, module);
    llvm::IRBuilder<> builder(
        llvm::BasicBlock::Create(context, "entry", function));
    auto *input = function->getArg(0);
    builder.CreateBinaryIntrinsic(llvm::Intrinsic::uadd_with_overflow, input,
                                  builder.getInt64(1));
    llvm::Value *argument = builder.CreateURem(input, builder.getInt64(251));
    if (shape == 1)
      argument = builder.CreateAnd(input, builder.getInt64(255));
    if (shape == 2)
      argument = builder.CreateZExt(
          builder.CreateTrunc(input, builder.getInt8Ty()), integer);
    if (shape == 3)
      argument = builder.CreateFreeze(argument);
    if (shape == 4)
      argument = input;
    if (shape == 5)
      argument = builder.CreateURem(input, builder.getInt64(0));
    if (shape == 6)
      argument = builder.CreateAnd(input, builder.getInt64(UINT64_MAX - 1));
    auto *first = builder.CreateCall(function, {argument});
    if (shape == 7)
      builder.CreateCall(function, {input});
    if (shape == 8)
      function->addFnAttr(llvm::Attribute::Convergent);
    if (shape == 9)
      new llvm::GlobalVariable(module, function->getType(), true,
                               llvm::GlobalValue::ExternalLinkage, function,
                               "escaped-recursive-address");
    builder.CreateRet(first);
    return function;
  };
  for (unsigned shape = 0; shape != 10; ++shape)
    make("recursive" + std::to_string(shape), shape);
  if (!specializeBoundedRecursion(module) || specializeBoundedRecursion(module))
    throw std::runtime_error(
        "recursive range specialization is not idempotent");
  for (unsigned shape = 0; shape != 10; ++shape) {
    auto *original = module.getFunction("recursive" + std::to_string(shape));
    auto *bounded = module.getFunction((original->getName() + ".bounded").str());
    if ((bounded != nullptr) != (shape < 3))
      throw std::runtime_error("recursive specialization crossed its boundary");
    if (!bounded)
      continue;
    const auto range = bounded->getArg(0)->getRange();
    const unsigned maximum = shape == 0 ? 250 : 255;
    if (!range || range->getUnsignedMax() != maximum ||
        original->getArg(0)->hasAttribute(llvm::Attribute::Range))
      throw std::runtime_error(
          "recursive specialization narrowed an open entry");
    for (auto *body : {original, bounded})
      for (auto &block : *body)
        for (auto &instruction : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
            if (!call->getCalledFunction()->isIntrinsic() &&
                call->getCalledFunction() != bounded)
              throw std::runtime_error("recursive edge missed bounded body");
            if (call->getIntrinsicID() == llvm::Intrinsic::assume)
              throw std::runtime_error(
                  "recursive range consumed a new poison value");
          }
  }
  if (llvm::verifyModule(module))
    throw std::runtime_error("recursive specialization produced invalid IR");
}

void verifyPrivateScalarResultPromotion() {
  llvm::LLVMContext context;
  llvm::Module module("private-scalar-results", context);
  auto *integer = llvm::Type::getInt64Ty(context);
  auto *boolean = llvm::Type::getInt1Ty(context);
  auto *pointer = llvm::PointerType::getUnqual(context);
  auto *signature =
      llvm::FunctionType::get(boolean, {pointer, integer, boolean}, false);
  auto fixture = [&](llvm::StringRef name, unsigned boundary) {
    auto *function = llvm::Function::Create(
        signature,
        boundary == 1 ? llvm::GlobalValue::ExternalLinkage
                      : llvm::GlobalValue::InternalLinkage,
        name, module);
    if (boundary != 2)
      function->addParamAttr(
          0, llvm::Attribute::get(context, "gti.scalar-result"));
    auto *entry = llvm::BasicBlock::Create(context, "entry", function);
    auto *success = llvm::BasicBlock::Create(context, "success", function);
    auto *failure = llvm::BasicBlock::Create(context, "failure", function);
    llvm::IRBuilder<> builder(entry);
    if (boundary == 3)
      builder.CreateLoad(integer, function->getArg(0));
    builder.CreateCondBr(function->getArg(2), success, failure);
    builder.SetInsertPoint(success);
    if (boundary != 4) {
      auto *store =
          builder.CreateStore(function->getArg(1), function->getArg(0));
      if (boundary == 5)
        store->setVolatile(true);
      if (boundary == 6)
        builder.CreateStore(builder.getInt64(3), function->getArg(0));
    }
    builder.CreateRet(builder.getTrue());
    builder.SetInsertPoint(failure);
    builder.CreateRet(builder.getFalse());
    auto *caller =
        llvm::Function::Create(signature, llvm::GlobalValue::ExternalLinkage,
                               name + ".caller", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", caller));
    auto *call = builder.CreateCall(
        function, {caller->getArg(0), caller->getArg(1), caller->getArg(2)});
    builder.CreateRet(call);
    if (boundary == 7)
      llvm::appendToUsed(module, {function});
  };
  fixture("eligible", 0);
  fixture("open", 1);
  fixture("unmarked", 2);
  fixture("reads_output", 3);
  fixture("missing_publication", 4);
  fixture("volatile_publication", 5);
  fixture("early_publication", 6);
  fixture("address_taken", 7);
  promotePrivateScalarResults(module);
  if (llvm::verifyModule(module))
    throw std::runtime_error(
        "private result promotion produced invalid LLVM IR");
  auto *promoted = module.getFunction("eligible");
  auto *type = llvm::dyn_cast<llvm::StructType>(promoted->getReturnType());
  if (!type || type->getNumElements() != 2 ||
      type->getElementType(0) != integer ||
      type->getElementType(1) != boolean || promoted->arg_size() != 2)
    throw std::runtime_error(
        "private scalar result did not move into registers");
  for (auto name :
       {"open", "unmarked", "reads_output", "missing_publication",
        "volatile_publication", "early_publication", "address_taken"})
    if (module.getFunction(name)->getFunctionType() != signature)
      throw std::runtime_error(
          "private result promotion crossed a rejection boundary");
  auto *caller = module.getFunction("eligible.caller");
  unsigned publications = 0;
  for (auto &block : *caller)
    for (auto &instruction : block)
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction)) {
        ++publications;
        auto *edge = block.getSinglePredecessor();
#if LLVM_VERSION_MAJOR >= 23
        auto *branch =
            edge ? llvm::dyn_cast<llvm::CondBrInst>(edge->getTerminator())
                 : nullptr;
#else
        auto *branch =
            edge ? llvm::dyn_cast<llvm::BranchInst>(edge->getTerminator())
                 : nullptr;
#endif
        auto *status =
            branch
                ? llvm::dyn_cast<llvm::ExtractValueInst>(branch->getCondition())
                : nullptr;
        if (!branch || branch->getSuccessor(0) != &block || !status ||
            status->getIndices() != llvm::ArrayRef<unsigned>{1} ||
            store->getPointerOperand() != caller->getArg(0))
          throw std::runtime_error(
              "scalar result published outside the successful call edge");
      }
  if (publications != 1)
    throw std::runtime_error(
        "scalar result publication was lost or duplicated");
}

void verifyDischargedCheckFacts() {
  const auto frontend = Frontend().analyze("discharged-check-facts.gti", R"(
extern "C" { int opaque_input(); }
int bounded(int value) {
  int reduced = value % 1000;
  return reduced * 17 + 3;
}

int unbounded(int value) { return value + 7; }
uint64_t bits(uint64_t value) { return value & uint64_t(255); }
int main() {
  int value = opaque_input();
  if (bits(uint64_t(7)) != uint64_t(7)) { return 1; }
  return bounded(value) + unbounded(value);
}
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid discharged-check fixture");
  for (const auto level : {OptimizationLevel::O0, OptimizationLevel::O3}) {
    const auto optimized = OptimizationPipeline().run(
        {.hir = frontend.hir, .mir = frontend.mir, .level = level});
    if (!optimized.valid())
      throw std::runtime_error("invalid discharged-check MIR");
    if (level == OptimizationLevel::O3 &&
        (optimized.report.transformations.empty() ||
         LoweredProgramBuilder()
             .build(frontend.program, frontend.semantics, frontend.hir,
                    optimized.sourceMir, optimized.mir, {}, TargetInfo::host())
             .valid()))
      throw std::runtime_error("discharged-check fixture must require "
                               "independently verified proofs");
    auto lowered = LoweredProgramBuilder().build(
        frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
        optimized.mir, optimized.report.transformations, TargetInfo::host());
    if (!lowered.valid())
      throw std::runtime_error("invalid discharged-check lowering");
    std::vector<LoweredProgramIssue> issues;
    const auto input = verifiedLoweredProgram(*lowered.program, issues);
    if (!input)
      throw std::runtime_error("unverified discharged-check lowering");
    Session session(*input, {});
    const auto admission = planLlvmAdmission(*input, {});
    if (!admission.admitted())
      throw std::runtime_error(admission.limitation());
    lowerScalarProgram(session, admission);
    unsigned bounded = 0, unbounded = 0, bits = 0;
    for (const auto &instance : input->program().mir().functionInstances()) {
      const auto *declaration =
          input->program().findFunctionDeclaration(instance.declaration);
      if (!declaration)
        continue;
      unsigned count = 0;
      for (const auto &block : *session.abi().declareFunction(instance.id))
        for (const auto &instruction : block)
          if (const auto *intrinsic =
                  llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
            count += intrinsic->getIntrinsicID() == llvm::Intrinsic::assume;
      if (declaration->qualifiedName == "bounded")
        bounded += count;
      if (declaration->qualifiedName == "unbounded")
        unbounded += count;
      if (declaration->qualifiedName == "bits")
        bits += count;
    }
    if ((level == OptimizationLevel::O0 ? bounded != 0 : bounded == 0) ||
        unbounded != 0 || bits != 0)
      throw std::runtime_error(
          "LLVM assumptions must require an explicit verified MIR discharge: " +
          std::to_string(bounded) + "/" + std::to_string(unbounded) + "/" +
          std::to_string(bits));
    session.verify();
    static_cast<void>(session.emitObject());
  }
}
} // namespace

void verifyLateInductiveDispatch() {
  const auto frontend = Frontend().analyze("inductive-dispatch.gti", R"(
extern "C" { uint64_t input(); void observe(uint64_t result); }
interface operation { int32_t evaluate(); };
class Square : public operation {
  int32_t operand;
public:
  Square(int32_t operand) : operand(operand) {}
  int32_t evaluate() override { return this.operand * this.operand; }
};
class Weighted : public operation {
  int32_t operand;
public:
  Weighted(int32_t operand) : operand(operand) {}
  int32_t evaluate() override { return 3 * this.operand * this.operand; }
};
uint64_t accumulate(operation& selected, int32_t count) {
  mut uint64_t result = 0;
  for (mut int32_t index = 0; index < count; index++) {
    result += uint64_t(selected.evaluate() + index % 7);
  }
  return result;
}
int main() {
  uint64_t count = input();
  mut uint64_t result = 0;
  for (mut uint64_t index = 0; index < count; index++) {
    Square first{int32_t(index % uint64_t(16)) + 1};
    Weighted second{int32_t(index % uint64_t(7)) + 1};
    result += accumulate(first, 1000);
    result += accumulate(second, 1000);
  }
  observe(result);
  return 0;
}
)");
  if (!frontend.canGenerateCode())
    throw std::runtime_error("invalid inductive dispatch source");
  const auto optimized = OptimizationPipeline().run(
      {frontend.hir, frontend.mir, OptimizationLevel::O3});
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, optimized.sourceMir,
      optimized.mir, optimized.report.transformations, TargetInfo::host());
  if (!optimized.valid() || !lowered.valid())
    throw std::runtime_error("invalid inductive dispatch MIR");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified inductive dispatch source");
  HierarchyStorageScope hierarchy(input->program().mir());
  const LlvmBackendOptions options{.optimization =
                                       LlvmCodegenOptimization::Aggressive};
  const auto admission = planLlvmAdmission(*input, options);
  if (!admission.admitted())
    throw std::runtime_error(admission.limitation());
  Session session(*input, options);
  lowerScalarProgram(session, admission);
  static_cast<void>(session.emitObject());
  // Cold-region outlining exposes new inlining and loop PHIs. The final
  // production pipeline must already have handled those late opportunities.
  if (foldInductivePointerLoops(session.module()))
    throw std::runtime_error("inductive loop proof ran before late inlining");
}

int main() {
  verifyLateInductiveDispatch();
  verifyCheckedArithmeticReuse();
  verifyRecoveredRangeChecks();
  verifyMergedCheckedArithmetic();
  verifyRecoveryExecutionFrequency();
  verifyPrivateScalarResultPromotion();
  verifyBoundedRecursion();
  verifyCheckedSquareBounds();
  verifySquareOffsetOptimization();
  verifyDischargedCheckFacts();
  FrontendOptions frontendOptions;
  frontendOptions.requireEntryPoint = true;
  const auto frontend = Frontend(frontendOptions).analyze("pipeline.gti", R"(
int promote(int input) { mut int local = input; return local; }
int checked(int left, int right) { return left + right; }
int checked_sub(int left, int right) { return left - right; }
int checked_mul(int left, int right) { return left * right; }
int checked_neg(int value) { return -value; }
uint64_t checked_uadd(uint64_t left, uint64_t right) { return left + right; }
uint64_t checked_usub(uint64_t left, uint64_t right) { return left - right; }
uint64_t checked_umul(uint64_t left, uint64_t right) { return left * right; }
double strict(double left, double right, double addend) { return left * right + addend; }
double divide(double left, double right) { return left / right; }
int step(int value, int bias) { return (value * 17 + bias) % 65521; }
int drive(int seed, int rounds) {
  mut int value = seed;
  for (mut int round = 0; round < rounds; round++) { value = step(value, round % 127); }
  return value;
}
void retire(int value) { auto checked = value + 1; }
struct Observe {
  Observe() = default;
  ~Observe() { retire(2); }
};
int guarded(int value) { Observe cleanup{}; return checked(value, 1); }
struct Inert { int tag = 0; Inert() = default; };
int passive(int value) { Inert local{}; return checked(value, 1); }
int main() {
  if (guarded(1) != 2 || passive(1) != 2) { return 4; }
  if (promote(7) != 7 || checked(1, 2) != 3) { return 1; }
  if (checked_sub(7, 2) != 5 || checked_mul(3, 2) != 6 || checked_neg(7) != -7) { return 5; }
  if (checked_uadd(uint64_t(1), uint64_t(2)) != uint64_t(3) ||
      checked_usub(uint64_t(7), uint64_t(2)) != uint64_t(5) ||
      checked_umul(uint64_t(3), uint64_t(2)) != uint64_t(6)) { return 6; }
  if (drive(1, 3) != 4932) { return 3; }
  if (strict(2.0d, 3.0d, 4.0d) != 10.0d || divide(4.0d, 2.0d) != 2.0d) { return 2; }
  return 0;
}
)");
  if (!frontend.canGenerateCode()) {
    for (const auto &diagnostic : frontend.diagnostics)
      std::cerr << diagnostic.message << '\n';
    throw std::runtime_error("invalid pipeline source");
  }
  auto lowered = LoweredProgramBuilder().build(
      frontend.program, frontend.semantics, frontend.hir, frontend.mir,
      frontend.mir, {}, TargetInfo::host());
  if (!lowered.valid())
    throw std::runtime_error("invalid lowered pipeline source");
  std::vector<LoweredProgramIssue> issues;
  const auto input = verifiedLoweredProgram(*lowered.program, issues);
  if (!input)
    throw std::runtime_error("unverified pipeline source");
  unsigned levels = 0;
  for (auto level :
       {LlvmCodegenOptimization::None, LlvmCodegenOptimization::Less,
        LlvmCodegenOptimization::Default,
        LlvmCodegenOptimization::Aggressive}) {
    const LlvmBackendOptions options{.optimization = level};
    const auto admission = planLlvmAdmission(*input, options);
    if (!admission.admitted())
      throw std::runtime_error(admission.limitation());
    Session session(*input, options);
    lowerScalarProgram(session, admission);
    const auto *coldClient = addColdRegionFixture(session.module());
    const auto *independentLanes = addIndependentLaneFixture(session.module());
    llvm::Function *promote = nullptr, *checked = nullptr, *strict = nullptr,
                   *step = nullptr, *drive = nullptr, *guarded = nullptr,
                   *passive = nullptr;
    std::string privateName, entryName;
    for (const auto &function : input->program().mir().functionInstances()) {
      const auto *declaration =
          input->program().findFunctionDeclaration(function.declaration);
      if (!declaration)
        continue;
      if (declaration->qualifiedName == "promote")
        promote = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "checked")
        checked = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "strict")
        strict = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "step")
        step = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "drive")
        drive = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "guarded")
        guarded = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "passive")
        passive = session.abi().declareFunction(function.id);
      if (declaration->qualifiedName == "divide")
        privateName =
            session.abi().declareFunction(function.id)->getName().str();
      if (function.entryKind != ProgramEntryKind::None)
        entryName = session.abi().declareFunction(function.id)->getName().str();
    }
    if (!promote || !checked || !strict || !step || !drive)
      throw std::runtime_error("missing pipeline function");
    verifyFailureBranchHints({checked, drive, guarded, passive});
    // Keep these unknown-input observation roots available after optimization.
    llvm::appendToUsed(session.module(), {promote, checked, strict, drive});
    const auto allocas = [](const llvm::Function &function) {
      unsigned count = 0;
      for (const auto &block : function)
        for (const auto &row : block)
          count += llvm::isa<llvm::AllocaInst>(row);
      return count;
    };
    if (allocas(*promote) == 0)
      throw std::runtime_error("pipeline fixture lost its source storage");
    // The detector itself must remain non-poisoning. Only the value on the
    // successful continuation carries the ordinary arithmetic no-wrap fact.
    struct CheckedOperation {
      const char *name;
      llvm::Intrinsic::ID detector;
      unsigned opcode;
      bool signedValue;
    };
    const CheckedOperation operations[] = {
        {"checked", llvm::Intrinsic::sadd_with_overflow, llvm::Instruction::Add,
         true},
        {"checked_sub", llvm::Intrinsic::ssub_with_overflow,
         llvm::Instruction::Sub, true},
        {"checked_mul", llvm::Intrinsic::smul_with_overflow,
         llvm::Instruction::Mul, true},
        {"checked_neg", llvm::Intrinsic::ssub_with_overflow,
         llvm::Instruction::Sub, true},
        {"checked_uadd", llvm::Intrinsic::uadd_with_overflow,
         llvm::Instruction::Add, false},
        {"checked_usub", llvm::Intrinsic::usub_with_overflow,
         llvm::Instruction::Sub, false},
        {"checked_umul", llvm::Intrinsic::umul_with_overflow,
         llvm::Instruction::Mul, false},
    };
    for (const auto &operation : operations) {
      llvm::Function *body = nullptr;
      for (const auto &instance : input->program().mir().functionInstances()) {
        const auto *declaration =
            input->program().findFunctionDeclaration(instance.declaration);
        if (declaration && declaration->qualifiedName == operation.name)
          body = session.abi().declareFunction(instance.id);
      }
      if (!body)
        throw std::runtime_error("missing checked-operation control");
      const llvm::DominatorTree dominators(*body);
      unsigned detectors = 0, checkedResults = 0;
      for (const auto &block : *body)
        for (const auto &instruction : block) {
          if (const auto *intrinsic =
                  llvm::dyn_cast<llvm::IntrinsicInst>(&instruction))
            detectors += intrinsic->getIntrinsicID() == operation.detector;
          if (const auto *binary =
                  llvm::dyn_cast<llvm::BinaryOperator>(&instruction);
              binary && binary->getOpcode() == operation.opcode &&
              (binary->hasNoSignedWrap() || binary->hasNoUnsignedWrap())) {
            ++checkedResults;
            if (binary->hasNoSignedWrap() != operation.signedValue ||
                binary->hasNoUnsignedWrap() == operation.signedValue)
              throw std::runtime_error(
                  "checked result uses the wrong integer domain");
#if LLVM_VERSION_MAJOR >= 23
            const auto *branch =
                llvm::dyn_cast<llvm::CondBrInst>(block.getTerminator());
#else
            const auto *branch =
                llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
#endif
            if (!branch || branch->getNumSuccessors() != 2)
              throw std::runtime_error(
                  "checked result must share its independent detector block");
            const auto *overflow =
                llvm::dyn_cast<llvm::ExtractValueInst>(branch->getCondition());
            const auto *detector = overflow
                                       ? llvm::dyn_cast<llvm::IntrinsicInst>(
                                             overflow->getAggregateOperand())
                                       : nullptr;
            if (!detector || detector->getIntrinsicID() != operation.detector ||
                overflow->getNumIndices() != 1 || *overflow->idx_begin() != 1 ||
                detector->getArgOperand(0) != binary->getOperand(0) ||
                detector->getArgOperand(1) != binary->getOperand(1))
              throw std::runtime_error(
                  "guard must test independent overflow of original operands");
            for (const auto *user : binary->users()) {
              const auto *use = llvm::dyn_cast<llvm::Instruction>(user);
              if (!use || !dominators.dominates(branch->getSuccessor(1),
                                                use->getParent()))
                throw std::runtime_error(
                    "only guard success may consume the no-wrap result");
            }
          }
        }
      if (detectors != 1 || checkedResults != 1)
        throw std::runtime_error(
            "checked arithmetic lost detector/result split");
    }
#if !defined(GTI_LLVM_C04_NATIVE_UNWIND)
    const auto failureBuffers = [&](const llvm::Function &function) {
      unsigned count = 0;
      for (const auto &block : function)
        for (const auto &row : block)
          if (const auto *allocation = llvm::dyn_cast<llvm::AllocaInst>(&row))
            count += allocation->getAllocatedType() ==
                     session.abi().failureChannel();
      return count;
    };
    if (!guarded || !passive || failureBuffers(*step) != 0 ||
        failureBuffers(*drive) != 0 || failureBuffers(*guarded) == 0)
      throw std::runtime_error(
          "failure destination reuse crossed its cleanup boundary");
    // An inert drop may still have an unused scratch destination before
    // LLVM folds its helper, but it must not establish a cleanup context.
    for (const auto &block : *passive)
      for (const auto &row : block)
        if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&row))
          if (const auto *callee = call->getCalledFunction())
            if (callee->getName() == "gti_rt_failure_cleanup_enter_v1" ||
                callee->getName() == "gti_rt_failure_cleanup_leave_v1")
              throw std::runtime_error(
                  "inert binding retirement entered a cleanup context");
#endif
    const auto floating = [&] {
      unsigned multiply = 0, addition = 0;
      for (const auto &block : *strict)
        for (const auto &row : block) {
          if (const auto *operation =
                  llvm::dyn_cast<llvm::FPMathOperator>(&row))
            if (operation->getFastMathFlags().any())
              throw std::runtime_error("unverified fast math");
          multiply += row.getOpcode() == llvm::Instruction::FMul;
          addition += row.getOpcode() == llvm::Instruction::FAdd;
          if (const auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&row))
            if (call->getIntrinsicID() == llvm::Intrinsic::fma ||
                call->getIntrinsicID() == llvm::Intrinsic::fmuladd)
              throw std::runtime_error("implicit floating contraction");
        }
      if (!multiply || !addition)
        throw std::runtime_error("strict multiply/add was lost");
    };
    floating();
    session.verify();
    const auto artifact = session.emitObject();
    bool vectorComparison = false;
    for (const auto &block : *independentLanes)
      for (const auto &instruction : block)
        if (llvm::isa<llvm::ICmpInst>(instruction) &&
            instruction.getType()->isVectorTy())
          vectorComparison = true;
    if ((level == LlvmCodegenOptimization::Default ||
         level == LlvmCodegenOptimization::Aggressive) &&
        !vectorComparison)
      throw std::runtime_error("independent adjacent byte operations were not "
                               "SLP-vectorized at O2/O3");
    unsigned directColdCalls = 0, allColdCalls = 0;
    for (const auto &function : session.module())
      for (const auto &block : function)
        for (const auto &instruction : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction))
            if (const auto *callee = call->getCalledFunction();
                callee && callee->getName() == "cold_region_observe") {
              ++allColdCalls;
              directColdCalls += &function == coldClient;
            }
    const bool outlines = level == LlvmCodegenOptimization::Default ||
                          level == LlvmCodegenOptimization::Aggressive;
    if (allColdCalls != 20 || directColdCalls != (outlines ? 0U : 20U))
      throw std::runtime_error(
          "cold outlining must move the cold region only at O2/O3 and retain "
          "every observable call");
    // Every emitted body is tuned for the target's selected processor while
    // its instruction features stay at the machine baseline. LLVM records
    // tuning knobs (fusion pairs, interleave factor, zero-cycle moves) in the
    // same feature set, so the only bits tuning may add are the tuning
    // processor's TuneImplies closure; no instruction extension, such as
    // LSE atomics, may appear.
    const auto machine = makeMachine(TargetInfo::host(), options);
    const auto &baseline = machineSubtarget(*machine);
    const auto host = TargetInfo::host();
    const llvm::StringRef requestedTune(tuneCpu(host));
    const auto closure = [&](llvm::FeatureBitset bits) {
      for (bool grew = true; grew;) {
        grew = false;
        for (const auto &feature : baseline.getAllProcessorFeatures())
          if (bits[feature.Value]) {
            const auto implied = bits | feature.Implies.getAsBitset();
            grew |= implied != bits;
            bits = implied;
          }
      }
      return bits;
    };
    // LLVM may resolve the requested name through a processor alias (Apple
    // M1 tunes as the A14 core), so judge the closure of what it selected.
    const auto tuningOf = [&](llvm::StringRef processorName) {
      for (const auto &processor : baseline.getAllProcessorDescriptions())
        if (entryKey(processor) == processorName)
          return closure(processor.TuneImplies.getAsBitset());
      throw std::runtime_error("unknown tuning processor " +
                               processorName.str());
    };
    std::optional<unsigned> atomics;
    for (const auto &feature : baseline.getAllProcessorFeatures())
      if (entryKey(feature) == "lse")
        atomics = feature.Value;
    unsigned tuned = 0;
    for (const auto &function : session.module()) {
      if (function.isDeclaration())
        continue;
      const auto *effective = machine->getSubtargetImpl(function);
      const auto added =
          effective->getFeatureBits() & ~baseline.getFeatureBits();
      const auto attribute = function.getFnAttribute("tune-cpu");
      // LLVM's late machine outliner copies already-selected machine
      // instructions into a helper represented here by a synthetic ret-void
      // shell. That shell has no IR to tune and LLVM does not copy tune-cpu.
      // Keep the instruction-feature checks for it; every real IR body still
      // requires the requested tuning, including IR-level cold outlines.
      const bool machineShell =
          function.getName().starts_with("OUTLINED_FUNCTION_") &&
          !function.getMetadata("gti.source") && function.arg_empty() &&
          function.getReturnType()->isVoidTy() && function.hasLocalLinkage() &&
          function.hasFnAttribute(llvm::Attribute::MinSize) &&
          function.hasFnAttribute(llvm::Attribute::OptimizeForSize) &&
          function.size() == 1 && function.front().size() == 1 &&
          llvm::isa<llvm::ReturnInst>(function.front().front());
      if ((!machineShell &&
           (requestedTune.empty()
                ? attribute.isValid() ||
                      effective->getTuneCPU() != baseline.getCPU()
                : !attribute.isStringAttribute() ||
                      attribute.getValueAsString() != requestedTune ||
                      effective->getTuneCPU() == baseline.getCPU())) ||
          effective->getCPU() != baseline.getCPU() ||
          (added & ~tuningOf(effective->getTuneCPU())).any() ||
          (atomics && effective->getFeatureBits()[*atomics] !=
                          baseline.getFeatureBits()[*atomics]))
        throw std::runtime_error("emitted body " + function.getName().str() +
                                 " lost its tuning or gained an instruction "
                                 "feature");
      tuned += !machineShell;
    }
    if (tuned == 0 || (host.os == "macos" && host.arch == "arm64" &&
                       (requestedTune != "apple-m1" || !atomics ||
                        baseline.getFeatureBits()[*atomics])))
      throw std::runtime_error(
          "macOS arm64 is not tuned for Apple M1 on the LSE-free baseline");
    const auto *privateFunction = session.module().getFunction(privateName);
    const auto *entryFunction = session.module().getFunction(entryName);
    if (privateName.empty() || entryName.empty() ||
        (privateFunction && !privateFunction->hasInternalLinkage()) ||
        !entryFunction || !entryFunction->hasExternalLinkage())
      throw std::runtime_error(
          "real GTI private/hosted visibility policy failed");
    if (artifact.kind != BackendArtifactKind::Object ||
        artifact.contents.empty())
      throw std::runtime_error("pipeline did not emit a real object");
    session.verify();
    floating();
    if (level == LlvmCodegenOptimization::None ? allocas(*promote) == 0
                                               : allocas(*promote) != 0)
      throw std::runtime_error("selected O0/O1/O2/O3 pipeline did not "
                               "preserve/promote storage as expected");
    bool overflow = false;
    for (const auto &block : *checked)
      for (const auto &row : block)
        if (const auto *call = llvm::dyn_cast<llvm::IntrinsicInst>(&row))
          overflow |=
              call->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow;
    if (!overflow)
      throw std::runtime_error(
          "unknown-input checked addition lost overflow detection");
    // A checked operation originates its record through one cold helper, so
    // the record's fixed-size fill never inflates the checked body. A body
    // whose failure chain only propagates neither enters nor leaves the
    // incoming-cleanup context, which nothing inside that chain can observe.
    // Together they leave a two-check scalar helper small enough to inline
    // into its loop at O2/O3.
    const auto *originator =
        session.module().getFunction("__gti_failure_originate_v1");
    if (!originator || originator->isDeclaration() ||
        !originator->hasFnAttribute(llvm::Attribute::Cold) ||
        !originator->hasFnAttribute(llvm::Attribute::NoInline) ||
        !originator->hasInternalLinkage())
      throw std::runtime_error("failure origination is not one cold helper");
    const auto *recordType = session.abi().failureRecord();
    for (const auto &function : session.module())
      for (const auto &block : function)
        for (const auto &row : block)
          if (const auto *write = llvm::dyn_cast<llvm::StoreInst>(&row);
              write && write->getValueOperand()->getType() == recordType &&
              &function != originator)
            throw std::runtime_error("a body stores a whole failure record "
                                     "outside the cold originator");
    unsigned originations = 0, stepCalls = 0;
    for (const auto &function : session.module()) {
      // Outlining may move the propagation-only tail into a private cold
      // helper. It still belongs to this checked source call chain.
      if (&function != drive &&
          !function.getName().starts_with(drive->getName().str() + ".cold."))
        continue;
      for (const auto &block : function)
        for (const auto &row : block)
          if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&row)) {
            const auto *callee = call->getCalledFunction();
            originations += callee == originator;
            stepCalls += callee == step;
            if (callee &&
                (callee->getName() == "gti_rt_failure_cleanup_enter_v1" ||
                 callee->getName() == "gti_rt_failure_cleanup_leave_v1"))
              throw std::runtime_error("a propagate-only failure chain entered "
                                       "the incoming-cleanup context");
          }
    }
    const bool inlines = level == LlvmCodegenOptimization::Default ||
                         level == LlvmCodegenOptimization::Aggressive;
    if (inlines && (stepCalls != 0 || originations == 0))
      throw std::runtime_error(
          "a two-check scalar helper was not inlined into its loop");
    Session invalid(*input, options);
    auto *malformed = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(invalid.context()),
                                false),
        llvm::GlobalValue::ExternalLinkage, "unterminated", invalid.module());
    llvm::BasicBlock::Create(invalid.context(), "entry", malformed);
    bool rejected = false;
    try {
      static_cast<void>(invalid.emitObject());
    } catch (const std::logic_error &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error(
          "malformed IR escaped before the selected pipeline");
    ++levels;
  }
  std::cout << levels
            << " real pipelines: private/hosted visibility, storage promotion, "
               "strict FP, checked "
               "overflow, cold failure origination, processor tuning and "
               "rejection passed\n";
}
