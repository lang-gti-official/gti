#include "../src/compiler/llvm/checked_ranges.h"

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
using Inputs = std::array<unsigned, 5>;
Outcome execute(llvm::Function &function, Inputs inputs,
                bool poisonInputs = false,
                const Inputs *freezeChoices = nullptr) {
  llvm::DenseMap<const llvm::Value *, Scalar> values;
  for (auto &argument : function.args()) {
    unsigned index = argument.getArgNo();
    values[&argument] = {
        llvm::APInt(argument.getType()->getIntegerBitWidth(), inputs[index]),
        poisonInputs && index < 3};
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
    for (auto &phi : block->phis()) {
      require(predecessor != nullptr, "entry PHI unsupported");
      incoming.emplace_back(&phi,
                            get(phi.getIncomingValueForBlock(predecessor)));
    }
    for (const auto &[phi, value] : incoming)
      values[phi] = value;
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
        default:
          throw std::runtime_error("unknown oracle binary operation");
        }
        values[binary] = {
            result, left.poison || right.poison ||
                        (binary->hasNoSignedWrap() && signedOverflow) ||
                        (binary->hasNoUnsignedWrap() && unsignedOverflow)};
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
        if (auto *argument =
                llvm::dyn_cast<llvm::Argument>(freeze->getOperand(0))) {
          unsigned index = argument->getArgNo();
          if (value.poison || freezeChoices)
            value.bits = llvm::APInt(value.bits.getBitWidth(),
                                     freezeChoices ? (*freezeChoices)[index]
                                                   : inputs[index]);
        } else {
          require(!value.poison && !freezeChoices,
                  "oracle needs an explicit freeze choice for derived scalar");
        }
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
        if (callee->getName() == "record") {
          auto value = get(call->getArgOperand(0));
          require(!value.poison, "poison effect operand");
          outcome.events.push_back(value.bits.getZExtValue());
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
  bool reverse = false, emptyEntry = false, sharedFailure = false;
  bool swappedCheck = false, invertedCheck = false, equalityExit = false;
  bool comparePhi = false, nonStrictReverse = false, signedLatch = false;
  bool variableStride = false, variableLength = false;
  bool noDuplicate = false, oversized = false, nonLCSSA = false;
  bool forwardHeader = false, misplacedControl = false;
};

llvm::Function *makeLoop(llvm::Module &module, const Shape &shape,
                         llvm::StringRef name) {
  auto &context = module.getContext();
  auto *coordinate = llvm::IntegerType::get(context, 4);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(i32,
                              {coordinate, coordinate, coordinate, i32,
                               llvm::Type::getInt1Ty(context)},
                              false),
      llvm::GlobalValue::ExternalLinkage, name, module);
  auto record = module.getOrInsertFunction(
      "record",
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {i32}, false));
  llvm::cast<llvm::Function>(record.getCallee())->setDoesNotThrow();
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *preheader = llvm::BasicBlock::Create(context, "preheader", function);
  auto *header = llvm::BasicBlock::Create(context, "header", function);
  auto *observed = llvm::BasicBlock::Create(context, "observed", function);
  auto *check = llvm::BasicBlock::Create(context, "check", function);
  auto *adjacent = llvm::BasicBlock::Create(context, "adjacent", function);
  auto *latch = llvm::BasicBlock::Create(context, "latch", function);
  auto *done = llvm::BasicBlock::Create(context, "done", function);
  auto *failed = llvm::BasicBlock::Create(context, "failed", function);
  auto *failedAdjacent =
      llvm::BasicBlock::Create(context, "failed.adjacent", function);
  auto *early = llvm::BasicBlock::Create(context, "early", function);
  auto *entryEarly =
      shape.forwardHeader
          ? llvm::BasicBlock::Create(context, "entry.early", function)
          : nullptr;
  auto *headerDone =
      shape.forwardHeader
          ? llvm::BasicBlock::Create(context, "header.done", function)
          : nullptr;
  auto *bodyControl =
      shape.misplacedControl
          ? llvm::BasicBlock::Create(context, "body.control", function)
          : nullptr;
  auto *empty = shape.emptyEntry
                    ? llvm::BasicBlock::Create(context, "empty", function)
                    : nullptr;
  llvm::IRBuilder<> b(entry);
  if (shape.forwardHeader)
    b.CreateCondBr(function->getArg(4), entryEarly, preheader);
  else if (empty)
    b.CreateCondBr(b.CreateICmpEQ(function->getArg(0), function->getArg(1)),
                   empty, preheader);
  else
    b.CreateBr(preheader);
  b.SetInsertPoint(preheader);
  b.CreateBr(header);
  b.SetInsertPoint(header);
  auto *index = b.CreatePHI(coordinate, 2, "index");
  index->addIncoming(function->getArg(0), preheader);
  llvm::Value *headerNext =
      shape.forwardHeader
          ? b.CreateAdd(index, llvm::ConstantInt::get(coordinate, 1),
                        "header.next")
          : nullptr;
  if (shape.forwardHeader && !shape.misplacedControl)
    b.CreateCondBr(b.CreateICmpNE(index, function->getArg(1)), observed,
                   headerDone);
  else
    b.CreateCondBr(function->getArg(4), early, observed);
  b.SetInsertPoint(observed);
  auto *event = b.CreateCall(record, {b.CreateZExt(index, i32)});
  if (shape.noDuplicate)
    event->addFnAttr(llvm::Attribute::NoDuplicate);
  if (shape.oversized)
    for (unsigned i = 0; i < 2048; ++i)
      b.CreateCall(record, {b.getInt32(i)});
  b.CreateCondBr(b.CreateICmpEQ(b.CreateZExt(index, i32), function->getArg(3)),
                 early, check);
  b.SetInsertPoint(check);
  auto bounds = [&](llvm::Value *value, llvm::BasicBlock *success,
                    llvm::BasicBlock *failure) {
    llvm::Value *length = shape.variableLength
                              ? static_cast<llvm::Value *>(index)
                              : function->getArg(2);
    auto predicate =
        shape.invertedCheck ? llvm::CmpInst::ICMP_UGE : llvm::CmpInst::ICMP_ULT;
    if (shape.swappedCheck) {
      std::swap(value, length);
      predicate = llvm::CmpInst::getSwappedPredicate(predicate);
    }
    b.CreateCondBr(b.CreateICmp(predicate, value, length),
                   shape.invertedCheck ? failure : success,
                   shape.invertedCheck ? success : failure);
  };
  bounds(index, bodyControl ? bodyControl : adjacent, failed);
  if (bodyControl) {
    // This stop condition cannot justify removing the earlier bounds check.
    b.SetInsertPoint(bodyControl);
    b.CreateCondBr(b.CreateICmpNE(index, function->getArg(1)), adjacent,
                   headerDone);
  }
  b.SetInsertPoint(adjacent);
  llvm::Value *step = shape.variableStride
                          ? static_cast<llvm::Value *>(function->getArg(1))
                          : llvm::ConstantInt::get(coordinate, 1);
  auto *next = headerNext      ? headerNext
               : shape.reverse ? b.CreateSub(index, step, "next")
                               : b.CreateAdd(index, step, "next");
  bounds(next, latch, failedAdjacent);
  b.SetInsertPoint(latch);
  llvm::Value *tested = shape.comparePhi ? index : next;
  if (shape.forwardHeader) {
    b.CreateCondBr(b.CreateICmpNE(b.CreateZExt(next, i32), function->getArg(3)),
                   header, done);
  } else if (shape.reverse) {
    auto predicate = shape.nonStrictReverse ? llvm::CmpInst::ICMP_UGE
                                            : llvm::CmpInst::ICMP_UGT;
    if (shape.signedLatch)
      predicate = llvm::CmpInst::ICMP_SGT;
    b.CreateCondBr(b.CreateICmp(predicate, tested, function->getArg(1)), header,
                   done);
  } else if (shape.equalityExit) {
    b.CreateCondBr(b.CreateICmpEQ(tested, function->getArg(1)), done, header);
  } else {
    b.CreateCondBr(b.CreateICmpNE(tested, function->getArg(1)), header, done);
  }
  index->addIncoming(next, latch);
  auto finish = [&](llvm::BasicBlock *block, llvm::BasicBlock *from,
                    llvm::Value *value, unsigned tag) {
    b.SetInsertPoint(block);
    auto *result = b.CreatePHI(coordinate, 1, "exit.index");
    result->addIncoming(value, from);
    b.CreateCall(record, {b.getInt32(tag + 256)});
    b.CreateRet(b.CreateAdd(b.CreateZExt(result, i32), b.getInt32(tag * 256)));
  };
  finish(done, latch, next, 2);
  if (headerDone)
    finish(headerDone, bodyControl ? bodyControl : header, index, 2);
  finish(failed, check, index, 3);
  finish(failedAdjacent, adjacent, next, 4);
  b.SetInsertPoint(early);
  b.CreateCall(record, {b.getInt32(9 + 256)});
  b.CreateRet(b.getInt32(9 * 256));
  if (empty) {
    b.SetInsertPoint(empty);
    b.CreateRet(b.getInt32(8 * 256));
  }
  if (entryEarly) {
    b.SetInsertPoint(entryEarly);
    b.CreateCall(record, {b.getInt32(9 + 256)});
    b.CreateRet(b.getInt32(9 * 256));
  }
  if (shape.sharedFailure) {
    adjacent->getTerminator()->setSuccessor(shape.invertedCheck ? 0 : 1,
                                            failed);
    llvm::cast<llvm::PHINode>(&failed->front())->addIncoming(next, adjacent);
    failedAdjacent->eraseFromParent();
  }
  if (shape.nonLCSSA) {
    // Valid SSA, but the shared cloning helper requires exit PHIs to repair
    // uses reached from its fallback. The public optimization API must reject.
    auto *exitValue = llvm::cast<llvm::PHINode>(&done->front());
    exitValue->replaceAllUsesWith(exitValue->getIncomingValue(0));
    exitValue->eraseFromParent();
  }
  require(!llvm::verifyFunction(*function, &llvm::errs()),
          "invalid range fixture");
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
  auto preserved = optimizeCheckedRanges(function, functions);
  functions.invalidate(function, preserved);
  require(!llvm::verifyFunction(function, &llvm::errs()),
          "range pass produced invalid IR");
  auto once = printed(function);
  preserved = optimizeCheckedRanges(function, functions);
  functions.invalidate(function, preserved);
  require(once == printed(function), "range loop repeatedly cloned");
}
void differential(const Shape &shape, const char *name) {
  llvm::LLVMContext context;
  llvm::Module module("range-differential", context);
  auto *before = makeLoop(module, shape, "before");
  auto *after = makeLoop(module, shape, "after");
  llvm::DominatorTree dominance(*after);
  llvm::LoopInfo loops(dominance);
  for (auto *loop : loops.getLoopsInPreorder())
    if (!loop->isLoopSimplifyForm() || !loop->isLCSSAForm(dominance))
      throw std::runtime_error(
          std::string("positive fixture is not canonical: ") + name);
  auto original = printed(*after);
  optimize(*after);
  if (original == printed(*after))
    throw std::runtime_error(
        std::string("supported range loop was not optimized: ") + name);
  for (unsigned start = 0; start < 16; ++start)
    for (unsigned stop = 0; stop < 16; ++stop)
      for (unsigned length = 0; length < 16; ++length)
        for (unsigned earlyAt : {32U, 0U, 7U}) {
          Inputs input{start, stop, length, earlyAt, 0};
          if (!(execute(*before, input) == execute(*after, input)))
            throw std::runtime_error(
                std::string("range result/failure/effects mismatch: ") + name +
                " start=" + std::to_string(start) + " stop=" +
                std::to_string(stop) + " length=" + std::to_string(length) +
                " early=" + std::to_string(earlyAt));
        }
  // Poison must remain unobserved when the original exits before checks.
  if (!shape.emptyEntry)
    for (unsigned arbitrary = 0; arbitrary < 16; ++arbitrary) {
      Inputs input{arbitrary, arbitrary, arbitrary, 32, 1};
      require(execute(*before, input, true) == execute(*after, input, true),
              "range selector observed poison on a defined early exit");
    }
  // Freeze one consistent undef refinement in the FAST path, including its
  // initial PHI and latch target, instead of allowing unrelated uses to vary.
  Inputs choices =
      shape.reverse ? Inputs{7, 2, 12, 32, 0} : Inputs{2, 7, 12, 32, 0};
  Inputs conflicting{14, 0, 1, 32, 0};
  require(execute(*before, choices) ==
              execute(*after, conflicting, false, &choices),
          "fast range loop did not preserve selector freeze choices");
}
} // namespace

int main() {
  try {
    {
      llvm::LLVMContext context;
      llvm::Module module("range-oracle-anchors", context);
      auto *forward = makeLoop(module, {}, "forward");
      require(execute(*forward, {2, 5, 8, 32, 0}) ==
                  Outcome{517, {2, 3, 4, 258}},
              "oracle forward anchor failed");
      require(execute(*forward, {2, 5, 3, 32, 0}) == Outcome{1027, {2, 260}},
              "oracle adjacent failure anchor failed");
      require(execute(*forward, {4, 5, 3, 32, 0}) == Outcome{772, {4, 259}},
              "oracle first failure anchor failed");
      Shape header;
      header.forwardHeader = true;
      auto *headerLoop = makeLoop(module, header, "header.control");
      require(execute(*headerLoop, {7, 7, 4, 32, 0}) == Outcome{519, {258}},
              "oracle empty header exit anchor failed");
      header.misplacedControl = true;
      auto *lateControl = makeLoop(module, header, "late.control");
      require(execute(*lateControl, {7, 7, 4, 32, 0}) == Outcome{775, {7, 259}},
              "oracle pre-control failure ordering anchor failed");
      Shape reverse;
      reverse.reverse = true;
      auto *backward = makeLoop(module, reverse, "reverse");
      require(execute(*backward, {5, 2, 8, 32, 0}) ==
                  Outcome{514, {5, 4, 3, 258}},
              "oracle reverse anchor failed");
      require(execute(*backward, {0, 0, 8, 32, 0}) == Outcome{1039, {0, 260}},
              "oracle reverse wrap failure anchor failed");
    }
    differential({}, "forward-next");
    Shape header;
    header.forwardHeader = true;
    differential(header, "header");
    Shape reverse;
    reverse.reverse = true;
    differential(reverse, "reverse");
    Shape empty;
    empty.emptyEntry = true;
    differential(empty, "empty");
    Shape shared;
    shared.sharedFailure = true;
    differential(shared, "shared");
    Shape swapped;
    swapped.swappedCheck = true;
    differential(swapped, "swapped");
    Shape inverted;
    inverted.invertedCheck = true;
    differential(inverted, "inverted");
    Shape equality;
    equality.equalityExit = true;
    differential(equality, "equality");
    for (unsigned kind = 0; kind < 9; ++kind) {
      Shape shape;
      if (kind == 0)
        shape.variableStride = true;
      if (kind == 1)
        shape.variableLength = true;
      if (kind == 2)
        shape.noDuplicate = true;
      if (kind == 3)
        shape.oversized = true;
      if (kind == 4)
        shape.comparePhi = true;
      if (kind == 5) {
        shape.reverse = true;
        shape.nonStrictReverse = true;
      }
      if (kind == 6) {
        shape.reverse = true;
        shape.signedLatch = true;
      }
      if (kind == 7)
        shape.nonLCSSA = true;
      if (kind == 8) {
        shape.forwardHeader = true;
        shape.misplacedControl = true;
      }
      llvm::LLVMContext context;
      llvm::Module module("rejected-range", context);
      auto *function = makeLoop(module, shape, "negative");
      auto before = printed(*function);
      optimize(*function);
      if (before != printed(*function))
        throw std::runtime_error("unsupported range loop transformed: kind=" +
                                 std::to_string(kind));
    }
    std::cout << "checked range differential execution passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
