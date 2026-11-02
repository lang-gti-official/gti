#include "../src/compiler/llvm/checked_reductions.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lang::native_llvm;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Scalar {
  llvm::APInt bits;
  bool poison = false;
};
struct Outcome {
  uint64_t result;
  std::vector<uint64_t> events;
  bool operator==(const Outcome &) const = default;
};

// Deliberately small execution oracle for the IRBuilder fixtures below. It
// models modular integer arithmetic, poison on flagged overflow, simultaneous
// PHI updates and executed side effects. Unknown instructions fail the test;
// this is not a compiler evaluator or an alternative source-language semantics.
Outcome execute(llvm::Function &function, unsigned bound, unsigned pattern,
                bool early = false, bool poisonedBound = false,
                int frozenBound = -1) {
  llvm::DenseMap<const llvm::Value *, Scalar> values;
  llvm::DenseMap<const llvm::Value *, std::pair<Scalar, Scalar>> pairs;
  for (auto &argument : function.args()) {
    unsigned input = argument.getArgNo() == 0   ? bound
                     : argument.getArgNo() == 1 ? pattern
                                                : unsigned(early);
    values[&argument] = {
        llvm::APInt(argument.getType()->getIntegerBitWidth(), input),
        poisonedBound && argument.getArgNo() == 0};
  }
  const auto get = [&](const llvm::Value *value) -> Scalar {
    if (auto *constant = llvm::dyn_cast<llvm::ConstantInt>(value))
      return {constant->getValue(), false};
    if (llvm::isa<llvm::PoisonValue>(value))
      return {llvm::APInt(value->getType()->getIntegerBitWidth(), 0), true};
    auto found = values.find(value);
    require(found != values.end(), "oracle read an unavailable SSA value");
    return found->second;
  };
  Outcome outcome{};
  llvm::BasicBlock *block = &function.getEntryBlock(), *predecessor = nullptr;
  for (unsigned visits = 0; visits < 200000; ++visits) {
    std::vector<std::pair<const llvm::PHINode *, Scalar>> incoming;
    std::vector<std::pair<const llvm::PHINode *, std::pair<Scalar, Scalar>>>
        incomingPairs;
    for (auto &phi : block->phis()) {
      require(predecessor != nullptr, "entry PHI unsupported");
      if (phi.getType()->isStructTy()) {
        auto found = pairs.find(phi.getIncomingValueForBlock(predecessor));
        require(found != pairs.end(), "oracle read an unavailable aggregate");
        incomingPairs.emplace_back(&phi, found->second);
        continue;
      }
      incoming.emplace_back(&phi,
                            get(phi.getIncomingValueForBlock(predecessor)));
    }
    for (const auto &[phi, value] : incoming)
      values[phi] = value;
    for (const auto &[phi, value] : incomingPairs)
      pairs[phi] = value;
    llvm::BasicBlock *next = nullptr;
    for (auto &instruction : *block) {
      if (llvm::isa<llvm::PHINode>(instruction))
        continue;
      if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
        auto left = get(binary->getOperand(0)),
             right = get(binary->getOperand(1));
        bool signedOverflow = false, unsignedOverflow = false;
        llvm::APInt result(left.bits.getBitWidth(), 0);
        switch (binary->getOpcode()) {
        case llvm::Instruction::Add:
          result = left.bits.uadd_ov(right.bits, unsignedOverflow);
          static_cast<void>(left.bits.sadd_ov(right.bits, signedOverflow));
          break;
        case llvm::Instruction::Sub:
          result = left.bits.usub_ov(right.bits, unsignedOverflow);
          static_cast<void>(left.bits.ssub_ov(right.bits, signedOverflow));
          break;
        case llvm::Instruction::And:
          result = left.bits & right.bits;
          break;
        case llvm::Instruction::Or:
          result = left.bits | right.bits;
          break;
        case llvm::Instruction::Xor:
          result = left.bits ^ right.bits;
          break;
        case llvm::Instruction::URem:
          require(!right.bits.isZero(), "oracle remainder by zero");
          result = left.bits.urem(right.bits);
          break;
        case llvm::Instruction::LShr:
          require(right.bits.ult(left.bits.getBitWidth()),
                  "oracle invalid shift");
          result = left.bits.lshr(right.bits);
          break;
        default:
          throw std::runtime_error("unknown oracle binary operation");
        }
        values[binary] = {
            result, left.poison || right.poison ||
                        (binary->hasNoSignedWrap() && signedOverflow) ||
                        (binary->hasNoUnsignedWrap() && unsignedOverflow)};
      } else if (auto *lane =
                     llvm::dyn_cast<llvm::ExtractValueInst>(&instruction)) {
        require(lane->getNumIndices() == 1 && *lane->idx_begin() <= 1 &&
                    pairs.contains(lane->getAggregateOperand()),
                "unknown oracle aggregate extraction");
        const auto &pair = pairs.find(lane->getAggregateOperand())->second;
        values[lane] = *lane->idx_begin() ? pair.second : pair.first;
      } else if (auto *compare = llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
        auto a = get(compare->getOperand(0)), b = get(compare->getOperand(1));
        bool result;
        switch (compare->getPredicate()) {
        case llvm::CmpInst::ICMP_EQ:
          result = a.bits == b.bits;
          break;
        case llvm::CmpInst::ICMP_NE:
          result = a.bits != b.bits;
          break;
        case llvm::CmpInst::ICMP_ULT:
          result = a.bits.ult(b.bits);
          break;
        case llvm::CmpInst::ICMP_ULE:
          result = a.bits.ule(b.bits);
          break;
        case llvm::CmpInst::ICMP_UGT:
          result = a.bits.ugt(b.bits);
          break;
        case llvm::CmpInst::ICMP_UGE:
          result = a.bits.uge(b.bits);
          break;
        case llvm::CmpInst::ICMP_SLT:
          result = a.bits.slt(b.bits);
          break;
        case llvm::CmpInst::ICMP_SLE:
          result = a.bits.sle(b.bits);
          break;
        case llvm::CmpInst::ICMP_SGT:
          result = a.bits.sgt(b.bits);
          break;
        case llvm::CmpInst::ICMP_SGE:
          result = a.bits.sge(b.bits);
          break;
        default:
          throw std::runtime_error("unknown oracle comparison");
        }
        values[compare] = {llvm::APInt(1, result), a.poison || b.poison};
      } else if (auto *select =
                     llvm::dyn_cast<llvm::SelectInst>(&instruction)) {
        auto condition = get(select->getCondition());
        auto selected = get(condition.bits.isZero() ? select->getFalseValue()
                                                    : select->getTrueValue());
        selected.poison |= condition.poison;
        values[select] = selected;
      } else if (auto *freeze =
                     llvm::dyn_cast<llvm::FreezeInst>(&instruction)) {
        auto value = get(freeze->getOperand(0));
        if (value.poison || frozenBound >= 0)
          value.bits =
              llvm::APInt(value.bits.getBitWidth(),
                          frozenBound >= 0 ? unsigned(frozenBound) : bound);
        value.poison = false;
        values[freeze] = value;
      } else if (auto *cast = llvm::dyn_cast<llvm::CastInst>(&instruction)) {
        auto value = get(cast->getOperand(0));
        unsigned width = cast->getType()->getIntegerBitWidth();
        require(cast->getOpcode() == llvm::Instruction::ZExt ||
                    cast->getOpcode() == llvm::Instruction::Trunc,
                "unsupported oracle integer cast");
        value.bits = value.bits.zextOrTrunc(width);
        values[cast] = value;
      } else if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
        auto *callee = call->getCalledFunction();
        require(callee != nullptr, "indirect oracle call");
        if (callee->getIntrinsicID() == llvm::Intrinsic::uadd_with_overflow ||
            callee->getIntrinsicID() == llvm::Intrinsic::sadd_with_overflow) {
          auto left = get(call->getArgOperand(0)),
               right = get(call->getArgOperand(1));
          bool overflow = false;
          auto result =
              callee->getIntrinsicID() == llvm::Intrinsic::uadd_with_overflow
                  ? left.bits.uadd_ov(right.bits, overflow)
                  : left.bits.sadd_ov(right.bits, overflow);
          pairs[call] = {
              {result, left.poison || right.poison},
              {llvm::APInt(1, overflow), left.poison || right.poison}};
        } else if (callee->getName() == "record") {
          auto value = get(call->getArgOperand(0));
          require(!value.poison, "poison effect operand");
          outcome.events.push_back(value.bits.getZExtValue());
        } else if (callee->getName() == "gate") {
          auto index = get(call->getArgOperand(0));
          auto seed = get(call->getArgOperand(1));
          bool selected = seed.bits == 0   ? true
                          : seed.bits == 1 ? false
                                           : index.bits.getZExtValue() % 3 !=
                                                 seed.bits.getZExtValue() % 3;
          values[call] = {llvm::APInt(1, selected),
                          index.poison || seed.poison};
        } else {
          throw std::runtime_error("unknown oracle call");
        }
      } else if (
#if LLVM_VERSION_MAJOR >= 23
          llvm::isa<llvm::CondBrInst, llvm::UncondBrInst>(instruction)
#else
          llvm::isa<llvm::BranchInst>(instruction)
#endif
      ) {
        unsigned successor = 0;
        if (instruction.getNumSuccessors() == 2) {
          auto condition = get(instruction.getOperand(0));
          require(!condition.poison, "transformed branch consumed poison");
          successor = condition.bits.isZero() ? 1 : 0;
        }
        next = instruction.getSuccessor(successor);
        break;
      } else if (auto *returned =
                     llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
        auto value = get(returned->getReturnValue());
        require(!value.poison, "poison return");
        outcome.result = value.bits.getZExtValue();
        return outcome;
      } else {
        throw std::runtime_error("unknown oracle instruction");
      }
    }
    require(next != nullptr, "oracle block has no successor");
    predecessor = block;
    block = next;
  }
  throw std::runtime_error("oracle execution budget exceeded");
}

struct Shape {
  unsigned indexWidth = 8, accumulatorWidth = 8;
  int initial = 0, step = 1;
  bool currentIndex = false, conditional = true, signedArithmetic = true;
  bool multipleUpdates = false, selectUpdate = false, earlyExit = false;
  bool dependentLatch = false, variableInitial = false, variableStep = false;
  bool subtract = false, flaggedIndex = false, sideAddition = false;
  bool convergent = false, oversized = false, equalityExit = false;
  bool noDuplicate = false, swappedCompare = false;
  bool sharedExit = false, nestedOuter = false;
  int constantBound = -1;
  int indexInitial = 0;
  unsigned boundedStep = 0;
  bool intrinsicAdd = false;
  bool externalOverflow = false;
  bool separateDetector = false;
  llvm::CmpInst::Predicate latchPredicate = llvm::CmpInst::ICMP_NE;
};

llvm::Function *makeLoop(llvm::Module &module, const Shape &shape,
                         llvm::StringRef name) {
  auto &context = module.getContext();
  auto *indexType = llvm::IntegerType::get(context, shape.indexWidth);
  auto *accType = llvm::IntegerType::get(context, shape.accumulatorWidth);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(
          i32, {indexType, accType, llvm::Type::getInt1Ty(context)}, false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  auto record = module.getOrInsertFunction(
      "record",
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i32}, false));
  llvm::cast<llvm::Function>(record.getCallee())->setDoesNotThrow();
  auto gate = module.getOrInsertFunction(
      "gate", llvm::FunctionType::get(llvm::Type::getInt1Ty(context),
                                      {indexType, accType}, false));
  llvm::cast<llvm::Function>(gate.getCallee())->setDoesNotThrow();
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *header = llvm::BasicBlock::Create(context, "header", function);
  auto *choose = llvm::BasicBlock::Create(context, "choose", function);
  auto *check = llvm::BasicBlock::Create(context, "check", function);
  auto *update = llvm::BasicBlock::Create(context, "update", function);
  auto *latch = llvm::BasicBlock::Create(context, "latch", function);
  auto *done = llvm::BasicBlock::Create(context, "done", function);
  auto *failed = llvm::BasicBlock::Create(context, "failed", function);
  auto *early = shape.earlyExit
                    ? llvm::BasicBlock::Create(context, "early", function)
                    : nullptr;
  llvm::IRBuilder<> builder(entry);
  builder.CreateBr(header);
  builder.SetInsertPoint(header);
  auto *index = builder.CreatePHI(indexType, 2, "index");
  auto *accumulator = builder.CreatePHI(accType, 2, "accumulator");
  index->addIncoming(llvm::ConstantInt::get(indexType, shape.indexInitial),
                     entry);
  accumulator->addIncoming(
      shape.variableInitial
          ? static_cast<llvm::Value *>(function->getArg(1))
          : llvm::ConstantInt::getSigned(accType, shape.initial),
      entry);
  auto *event = builder.CreateCall(record, {llvm::ConstantInt::get(i32, 1)});
  if (shape.convergent)
    event->setConvergent();
  if (shape.noDuplicate)
    event->addFnAttr(llvm::Attribute::NoDuplicate);
  if (shape.oversized)
    for (unsigned i = 0; i < 4096; ++i)
      builder.CreateCall(record, {llvm::ConstantInt::get(i32, i)});
  if (shape.earlyExit)
    builder.CreateCondBr(function->getArg(2), early, choose);
  else
    builder.CreateBr(choose);
  builder.SetInsertPoint(choose);
  auto *selected = builder.CreateCall(gate, {index, function->getArg(1)});
  if (shape.conditional && !shape.selectUpdate)
    builder.CreateCondBr(selected, check, latch);
  else
    builder.CreateBr(check);
  builder.SetInsertPoint(check);
  unsigned delta = unsigned(shape.step) * (shape.multipleUpdates ? 2 : 1);
  llvm::APInt maximum =
      shape.signedArithmetic
          ? llvm::APInt::getSignedMaxValue(shape.accumulatorWidth)
          : llvm::APInt::getMaxValue(shape.accumulatorWidth);
  llvm::Value *increment =
      shape.variableStep ? static_cast<llvm::Value *>(function->getArg(1))
                         : llvm::ConstantInt::getSigned(accType, shape.step);
  if (shape.boundedStep == 1)
    increment = builder.CreateAnd(function->getArg(1),
                                  llvm::ConstantInt::get(accType, 3));
  if (shape.boundedStep == 2)
    increment = builder.CreateURem(function->getArg(1),
                                   llvm::ConstantInt::get(accType, 5));
  if (shape.boundedStep == 3)
    increment = builder.CreateLShr(
        function->getArg(1),
        llvm::ConstantInt::get(accType, shape.accumulatorWidth - 2));
  if (shape.boundedStep == 4)
    increment = builder.CreateZExt(
        builder.CreateTrunc(function->getArg(1), builder.getIntNTy(2)),
        accType);
  if (shape.boundedStep == 5)
    increment =
        builder.CreateSelect(selected, llvm::ConstantInt::get(accType, 3),
                             llvm::ConstantInt::get(accType, 7));
  // The guard and the value use the same checked operation, including the
  // zero-result lane; this catches accidentally dropping a failure without
  // proving the corresponding intermediate addition.
  llvm::Value *checkedResult = nullptr;
  llvm::Value *checkedPair = nullptr;
  llvm::Value *condition = nullptr;
  if (shape.intrinsicAdd) {
    auto *pair = builder.CreateBinaryIntrinsic(
        shape.signedArithmetic ? llvm::Intrinsic::sadd_with_overflow
                               : llvm::Intrinsic::uadd_with_overflow,
        accumulator, increment);
    checkedResult = builder.CreateExtractValue(pair, 0);
    checkedPair = pair;
    condition = builder.CreateExtractValue(pair, 1, "overflow.guard");
    if (shape.separateDetector)
      checkedResult = nullptr;
  } else {
    condition = builder.CreateICmpUGT(
        accumulator, llvm::ConstantInt::get(accType, maximum - delta),
        "overflow.guard");
  }
  builder.CreateCondBr(condition, failed, update);
  builder.SetInsertPoint(update);
  llvm::Value *nextAccumulator =
      checkedResult    ? checkedResult
      : shape.subtract ? builder.CreateSub(accumulator, increment, "decrease")
                       : builder.CreateAdd(accumulator, increment, "increase");
  if (shape.multipleUpdates)
    nextAccumulator =
        builder.CreateAdd(nextAccumulator, increment, "increase.again");
  if (shape.sideAddition) {
    auto *side = builder.CreateAdd(
        accumulator, llvm::ConstantInt::get(accType, 100), "unrelated.add");
    auto *sideGuard = builder.CreateICmpUGT(
        side, llvm::ConstantInt::get(accType, 126), "side.guard");
    builder.CreateCall(record, {builder.CreateZExt(sideGuard, i32)});
  }
  if (shape.selectUpdate)
    nextAccumulator = builder.CreateSelect(selected, nextAccumulator,
                                           accumulator, "optional.update");
  builder.CreateBr(latch);
  builder.SetInsertPoint(latch);
  auto *carried = builder.CreatePHI(accType, 2, "carried");
  carried->addIncoming(nextAccumulator, update);
  if (shape.conditional && !shape.selectUpdate)
    carried->addIncoming(accumulator, choose);
  auto *nextIndex =
      builder.CreateAdd(index, llvm::ConstantInt::get(indexType, 1),
                        "index.next", shape.flaggedIndex, false);
  llvm::Value *bound =
      shape.constantBound >= 0
          ? llvm::ConstantInt::get(indexType, shape.constantBound)
          : static_cast<llvm::Value *>(function->getArg(0));
  llvm::Value *tested =
      shape.currentIndex ? static_cast<llvm::Value *>(index) : nextIndex;
  if (shape.dependentLatch)
    tested = builder.CreateZExtOrTrunc(carried, indexType);
  if (shape.swappedCompare)
    std::swap(tested, bound);
  if (shape.equalityExit)
    builder.CreateCondBr(builder.CreateICmpEQ(tested, bound), done, header);
  else
    builder.CreateCondBr(
        builder.CreateICmp(shape.latchPredicate, tested, bound), header, done);
  index->addIncoming(nextIndex, latch);
  accumulator->addIncoming(carried, latch);
  const auto finish = [&](llvm::BasicBlock *block, llvm::BasicBlock *from,
                          llvm::Value *value, unsigned tag) {
    builder.SetInsertPoint(block);
    auto *exitValue = builder.CreatePHI(accType, 1, "exit.value");
    exitValue->addIncoming(value, from);
    llvm::Value *exportedStatus = nullptr;
    if (shape.externalOverflow) {
      require(checkedPair && !shape.conditional && !shape.earlyExit,
              "aggregate fixture requires a dominating checked addition");
      auto *aggregate =
          builder.CreatePHI(checkedPair->getType(), 1, "exit.aggregate");
      aggregate->addIncoming(checkedPair, from);
      exportedStatus =
          builder.CreateExtractValue(aggregate, 1, "exported.overflow");
    }
    builder.CreateCall(record, {llvm::ConstantInt::get(i32, tag)});
    auto *result = builder.CreateAdd(builder.CreateZExt(exitValue, i32),
                                     llvm::ConstantInt::get(i32, tag << 16));
    if (exportedStatus)
      result = builder.CreateAdd(result,
                                 builder.CreateSelect(exportedStatus,
                                                      builder.getInt32(1 << 15),
                                                      builder.getInt32(0)));
    builder.CreateRet(result);
  };
  finish(done, latch, carried, 2);
  finish(failed, check, accumulator, 3);
  if (shape.earlyExit)
    finish(early, header, accumulator, 4);
  if (shape.sharedExit) {
    // Distinct exiting blocks feed one dedicated LCSSA join. Cloning must
    // append BOTH incoming edges and remap their different values correctly.
    check->getTerminator()->setSuccessor(0, done);
    llvm::cast<llvm::PHINode>(&done->front())->addIncoming(accumulator, check);
    failed->eraseFromParent();
  }
  if (shape.nestedOuter) {
    // Retain the original entry as the inner preheader; it is now inside a
    // three-iteration outer loop. Inner versioning must update parent-loop
    // membership and the outer SSA/LCSSA users without cloning outer effects.
    auto *outerEntry =
        llvm::BasicBlock::Create(context, "outer.entry", function, entry);
    auto *outerHeader =
        llvm::BasicBlock::Create(context, "outer.header", function, entry);
    auto *outerLatch =
        llvm::BasicBlock::Create(context, "outer.latch", function);
    auto *outerDone = llvm::BasicBlock::Create(context, "outer.done", function);
    builder.SetInsertPoint(outerEntry);
    builder.CreateBr(outerHeader);
    builder.SetInsertPoint(outerHeader);
    auto *outerIndex = builder.CreatePHI(i32, 2, "outer.index");
    outerIndex->addIncoming(builder.getInt32(0), outerEntry);
    builder.CreateBr(entry);
    auto *oldReturn = llvm::cast<llvm::ReturnInst>(done->getTerminator());
    auto *result = oldReturn->getReturnValue();
    oldReturn->eraseFromParent();
    builder.SetInsertPoint(done);
    builder.CreateBr(outerLatch);
    builder.SetInsertPoint(outerLatch);
    auto *outerNext = builder.CreateAdd(outerIndex, builder.getInt32(1));
    outerIndex->addIncoming(outerNext, outerLatch);
    builder.CreateCondBr(builder.CreateICmpNE(outerNext, builder.getInt32(3)),
                         outerHeader, outerDone);
    builder.SetInsertPoint(outerDone);
    auto *outerResult = builder.CreatePHI(i32, 1, "outer.result");
    outerResult->addIncoming(result, outerLatch);
    builder.CreateRet(outerResult);
  }
  require(!llvm::verifyFunction(*function, &llvm::errs()), "invalid test loop");
  return function;
}

std::string printed(llvm::Function &function) {
  std::string output;
  llvm::raw_string_ostream stream(output);
  function.print(stream);
  return output;
}
void optimize(llvm::Function &function) {
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerLoopAnalyses(loops);
  builder.registerFunctionAnalyses(functions);
  builder.registerCGSCCAnalyses(cgscc);
  builder.registerModuleAnalyses(modules);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  auto preserved = optimizeCheckedReductions(function, functions);
  functions.invalidate(function, preserved);
  require(!llvm::verifyFunction(function, &llvm::errs()),
          "reduction pass produced invalid IR");
  const auto once = printed(function);
  preserved = optimizeCheckedReductions(function, functions);
  functions.invalidate(function, preserved);
  require(printed(function) == once,
          "reduction pass repeatedly versions a loop");
}

void differential(const Shape &shape, bool expectChange) {
  llvm::LLVMContext context;
  llvm::Module module("checked-reduction", context);
  auto *before = makeLoop(module, shape, "reference");
  auto *after = makeLoop(module, shape, "optimized");
  const auto original = printed(*after);
  optimize(*after);
  require((printed(*after) != original) == expectChange,
          "unexpected reduction admission decision");
  // A version selector must constrain the same chosen value used by the
  // optimized latch. An undef argument can otherwise choose a small selector
  // value and a different target at each latch, invalidating the trip proof.
  // Here the oracle gives the raw bound a conflicting value while every freeze
  // consistently chooses a short traversal; the optimized path must behave as
  // that refinement. For a nonzero initial index, target==initial instead
  // means a complete modular traversal and correctly selects the fallback.
  if (expectChange && shape.constantBound < 0) {
    const auto target = llvm::APInt(64, shape.indexInitial + 3)
                            .zextOrTrunc(shape.indexWidth)
                            .getZExtValue();
    for (unsigned pattern : {0U, 1U, 2U, 3U, 127U, 255U})
      require(execute(*before, target, pattern) ==
                  execute(*after, 200, pattern, false, false, target),
              "optimized latch did not use the frozen traversal target");
  }
  for (unsigned bound = shape.indexWidth > 8 ? 1 : 0; bound < 256; ++bound)
    for (unsigned pattern : {0U, 1U, 2U, 3U, 127U, 255U})
      require(execute(*before, bound, pattern) ==
                  execute(*after, bound, pattern),
              "reduction changed result, failure, or executed cleanup trace");
  if (shape.earlyExit)
    for (unsigned frozen = 0; frozen < 256; ++frozen)
      require(execute(*before, frozen, 0, true, true) ==
                  execute(*after, frozen, 0, true, true),
              "poison bound changed a defined pre-latch early exit");
}
} // namespace

int main() {
  try {
    {
      llvm::LLVMContext context;
      llvm::Module module("oracle-anchors", context);
      auto *function = makeLoop(module, {}, "anchors");
      auto success = execute(*function, 7, 0);
      require(success.result == (2U << 16) + 7 && success.events.size() == 8 &&
                  success.events.back() == 2,
              "oracle success anchor failed");
      auto overflow = execute(*function, 200, 0);
      require(overflow.result == (3U << 16) + 127 &&
                  overflow.events.size() == 129 && overflow.events.back() == 3,
              "oracle overflow cleanup anchor failed");
    }
    differential({}, true);
    Shape unconditional;
    unconditional.conditional = false;
    differential(unconditional, true);
    Shape swapped;
    swapped.swappedCompare = true;
    differential(swapped, true);
    Shape flagged;
    flagged.currentIndex = true;
    flagged.flaggedIndex = true;
    differential(flagged, true);
    Shape wraps;
    wraps.constantBound = 0;
    differential(wraps, false);
    Shape equality;
    equality.equalityExit = true;
    differential(equality, true);
    Shape side;
    side.sideAddition = true;
    differential(side, true);
    Shape current;
    current.currentIndex = true;
    differential(current, true);
    Shape constant;
    constant.constantBound = 63;
    differential(constant, true);
    Shape conditional;
    conditional.selectUpdate = true;
    differential(conditional, true);
    Shape multiple;
    multiple.multipleUpdates = true;
    multiple.step = 3;
    differential(multiple, true);
    Shape initial;
    initial.initial = 120;
    differential(initial, true);
    Shape unsignedCount;
    unsignedCount.signedArithmetic = false;
    differential(unsignedCount, true);
    Shape early;
    early.earlyExit = true;
    differential(early, true);
    Shape wide;
    wide.accumulatorWidth = 16;
    differential(wide, true);
    Shape wideIndex;
    wideIndex.indexWidth = 16;
    differential(wideIndex, true);
    for (unsigned kind = 1; kind <= 5; ++kind) {
      Shape dynamic;
      dynamic.boundedStep = kind;
      dynamic.intrinsicAdd = true;
      dynamic.conditional = false;
      differential(dynamic, true);
      dynamic.signedArithmetic = false;
      dynamic.conditional = true;
      dynamic.earlyExit = true;
      differential(dynamic, true);
      dynamic.indexInitial = 7;
      differential(dynamic, true);
      dynamic.multipleUpdates = true;
      differential(dynamic, true);
      dynamic.separateDetector = true;
      differential(dynamic, true);
    }
    Shape aggregate;
    aggregate.intrinsicAdd = true;
    aggregate.boundedStep = 2;
    aggregate.conditional = false;
    aggregate.externalOverflow = true;
    differential(aggregate, true);
    aggregate.signedArithmetic = false;
    differential(aggregate, true);
    for (auto predicate : {llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_SLT,
                           llvm::CmpInst::ICMP_UGT, llvm::CmpInst::ICMP_SGT})
      for (int initial : {0, 2, 127, 255}) {
        Shape ordered;
        ordered.latchPredicate = predicate;
        ordered.indexInitial = initial;
        ordered.intrinsicAdd = true;
        ordered.boundedStep = 2;
        ordered.separateDetector = true;
        differential(ordered, true);
      }
    for (int initial : {1, 7, 127, 255}) {
      Shape started;
      started.indexInitial = initial;
      differential(started, true);
      started.currentIndex = true;
      differential(started, true);
    }
    for (unsigned kind = 0; kind < 11; ++kind) {
      Shape negative;
      if (kind == 0)
        negative.dependentLatch = true;
      if (kind == 1)
        negative.initial = -1;
      if (kind == 2)
        negative.variableInitial = true;
      if (kind == 3)
        negative.variableStep = true;
      if (kind == 4)
        negative.subtract = true;
      if (kind == 5)
        negative.convergent = true;
      if (kind == 6)
        negative.oversized = true;
      if (kind == 7)
        negative.step = 0;
      if (kind == 8)
        negative.step = 128;
      if (kind == 9)
        negative.initial = 127;
      if (kind == 10)
        negative.noDuplicate = true;
      llvm::LLVMContext context;
      llvm::Module module("rejected-reduction", context);
      auto *function = makeLoop(module, negative, "negative");
      const auto original = printed(*function);
      optimize(*function);
      require(printed(*function) == original,
              "unsupported reduction was changed");
    }
    std::cout << "checked reduction proof and differential execution passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
