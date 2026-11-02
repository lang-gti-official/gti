#include "../src/compiler/llvm/checked_offsets.h"
#include <iostream>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Shape {
  unsigned kind =
      0; // half, eighth*2, half+eighth, rebased half+eighth, >difference
  bool nuw = false;
  bool guard = true;
  bool stable = true;
  bool freezeDifference = false;
  bool signedGuard = false;
  bool mergedGuard = false;
  bool freezeEndpoints = false;
  bool swapped = false;
};
struct Fixture {
  llvm::Function *function;
  llvm::ExtractValueInst *overflow;
};
Fixture build(llvm::Module &module, const Shape &shape) {
  auto &context = module.getContext();
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(i8, {i8, i8}, false),
      llvm::GlobalValue::ExternalLinkage, "case", module);
  if (shape.stable)
    for (auto &arg : function->args())
      arg.addAttr(llvm::Attribute::NoUndef);
  auto *entry = llvm::BasicBlock::Create(context, "entry", function);
  auto *body = llvm::BasicBlock::Create(context, "body", function);
  auto *bad = llvm::BasicBlock::Create(context, "bad", function);
  auto *over = llvm::BasicBlock::Create(context, "overflow", function);
  auto *done = llvm::BasicBlock::Create(context, "done", function);
  llvm::IRBuilder<> b(entry);
  llvm::Value *low = function->getArg(0), *high = function->getArg(1);
  if (shape.freezeEndpoints) {
    low = b.CreateFreeze(low);
    high = b.CreateFreeze(high);
  }
  auto *event = llvm::Function::Create(
      llvm::FunctionType::get(b.getVoidTy(), {i8}, false),
      llvm::GlobalValue::ExternalLinkage, "event", module);
  b.CreateCall(event, {b.getInt8(1)});
  if (shape.guard) {
    auto *condition = shape.signedGuard
                          ? b.CreateICmpSLE(low, high)
                          : (shape.swapped ? b.CreateICmpUGE(high, low)
                                           : b.CreateICmpULE(low, high));
    b.CreateCondBr(condition, body, bad);
  } else
    b.CreateBr(body);
  b.SetInsertPoint(bad);
  b.CreateCall(event, {b.getInt8(2)});
  if (shape.mergedGuard)
    b.CreateBr(body);
  else
    b.CreateRet(b.getInt8(241));
  b.SetInsertPoint(body);
  llvm::Value *difference = b.CreateSub(high, low, "difference", shape.nuw);
  if (shape.freezeDifference)
    difference = b.CreateFreeze(difference);
  auto *half = b.CreateLShr(difference, b.getInt8(1));
  auto *eighth = b.CreateLShr(difference, b.getInt8(3));
  llvm::Value *offset = half;
  if (shape.kind == 1)
    offset = b.CreateMul(eighth, b.getInt8(2));
  if (shape.kind == 2)
    offset = b.CreateAdd(half, eighth);
  if (shape.kind == 4)
    offset = b.CreateAdd(difference, half);
  if (shape.kind == 5)
    offset = b.CreateShl(eighth, b.getInt8(1));
  if (shape.kind == 3)
    low = b.CreateExtractValue(
        b.CreateBinaryIntrinsic(llvm::Intrinsic::uadd_with_overflow, low, half),
        0);
  if (shape.kind == 3)
    offset = eighth;
  auto *sum = b.CreateBinaryIntrinsic(llvm::Intrinsic::uadd_with_overflow,
                                      shape.swapped ? offset : low,
                                      shape.swapped ? low : offset);
  auto *result = b.CreateExtractValue(sum, 0);
  auto *overflow =
      llvm::cast<llvm::ExtractValueInst>(b.CreateExtractValue(sum, 1));
  b.CreateCall(event, {b.getInt8(3)});
  b.CreateCondBr(overflow, over, done);
  b.SetInsertPoint(over);
  b.CreateCall(event, {b.getInt8(4)});
  b.CreateRet(b.getInt8(242));
  b.SetInsertPoint(done);
  b.CreateCall(event, {b.getInt8(5)});
  b.CreateRet(result);
  require(!llvm::verifyFunction(*function, &llvm::errs()), "invalid fixture");
  return {function, overflow};
}

struct Value {
  unsigned bits = 0;
  bool poison = false;
};
struct Outcome {
  unsigned result = 0;
  bool undefined = false;
  std::vector<unsigned> events;
  bool operator==(const Outcome &) const = default;
};
// Small independent modular execution oracle. Poison reaching a branch/return
// makes the original execution undefined; a defined original must match
// exactly.
Outcome execute(llvm::Function &function, unsigned low, unsigned high) {
  llvm::DenseMap<llvm::Value *, Value> values;
  llvm::DenseMap<llvm::Value *, std::pair<Value, Value>> aggregates;
  values[function.getArg(0)] = {low, false};
  values[function.getArg(1)] = {high, false};
  auto get = [&](llvm::Value *value) -> Value {
    if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(value))
      return {static_cast<unsigned>(c->getZExtValue()), false};
    auto found = values.find(value);
    require(found != values.end(), "unmodeled scalar");
    return found->second;
  };
  Outcome outcome;
  auto *block = &function.getEntryBlock();
  for (unsigned steps = 0; steps < 20; ++steps) {
    llvm::BasicBlock *next = nullptr;
    for (auto &instruction : *block) {
      if (auto *binary = llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
        auto a = get(binary->getOperand(0)), b = get(binary->getOperand(1));
        unsigned result = 0;
        bool overflow = false;
        switch (binary->getOpcode()) {
        case llvm::Instruction::Sub:
          result = (a.bits - b.bits) & 255;
          overflow = a.bits < b.bits;
          break;
        case llvm::Instruction::Add:
          result = a.bits + b.bits;
          overflow = result > 255;
          result &= 255;
          break;
        case llvm::Instruction::Mul:
          result = a.bits * b.bits;
          overflow = result > 255;
          result &= 255;
          break;
        case llvm::Instruction::LShr:
          result = a.bits >> b.bits;
          break;
        case llvm::Instruction::Shl:
          result = a.bits << b.bits;
          overflow = result > 255;
          result &= 255;
          break;
        default:
          throw std::runtime_error("unknown binary");
        }
        values[binary] = {result,
                          a.poison || b.poison ||
                              (binary->hasNoUnsignedWrap() && overflow)};
      } else if (auto *freeze =
                     llvm::dyn_cast<llvm::FreezeInst>(&instruction)) {
        auto v = get(freeze->getOperand(0));
        values[freeze] = {v.poison ? 255U : v.bits, false};
      } else if (auto *comparison =
                     llvm::dyn_cast<llvm::ICmpInst>(&instruction)) {
        auto a = get(comparison->getOperand(0)),
             b = get(comparison->getOperand(1));
        bool answer;
        switch (comparison->getPredicate()) {
        case llvm::ICmpInst::ICMP_ULE:
          answer = a.bits <= b.bits;
          break;
        case llvm::ICmpInst::ICMP_UGE:
          answer = a.bits >= b.bits;
          break;
        case llvm::ICmpInst::ICMP_SLE:
          answer = static_cast<int8_t>(a.bits) <= static_cast<int8_t>(b.bits);
          break;
        default:
          throw std::runtime_error("unknown comparison");
        }
        values[comparison] = {answer, a.poison || b.poison};
      } else if (auto *call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
        if (call->getCalledFunction()->getName() == "event") {
          outcome.events.push_back(get(call->getArgOperand(0)).bits);
          continue;
        }
        auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call);
        require(intrinsic && intrinsic->getIntrinsicID() ==
                                 llvm::Intrinsic::uadd_with_overflow,
                "unknown call");
        auto a = get(call->getArgOperand(0)), b = get(call->getArgOperand(1));
        aggregates[call] = {{(a.bits + b.bits) & 255, a.poison || b.poison},
                            {a.bits + b.bits > 255, a.poison || b.poison}};
      } else if (auto *extract =
                     llvm::dyn_cast<llvm::ExtractValueInst>(&instruction)) {
        auto found = aggregates.find(extract->getAggregateOperand());
        require(found != aggregates.end(), "unknown aggregate");
        values[extract] = *extract->idx_begin() == 0 ? found->second.first
                                                     : found->second.second;
      } else if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) {
        auto v = get(ret->getReturnValue());
        outcome.result = v.bits;
        outcome.undefined = v.poison;
        return outcome;
#if LLVM_VERSION_MAJOR >= 23
      } else if (auto *branch =
                     llvm::dyn_cast<llvm::CondBrInst>(&instruction)) {
        auto condition = get(branch->getCondition());
        if (condition.poison) {
          outcome.undefined = true;
          return outcome;
        }
        next = branch->getSuccessor(condition.bits ? 0 : 1);
        break;
      } else if (auto *branch =
                     llvm::dyn_cast<llvm::UncondBrInst>(&instruction)) {
        next = branch->getSuccessor();
        break;
#else
      } else if (auto *branch =
                     llvm::dyn_cast<llvm::BranchInst>(&instruction)) {
        if (!branch->isConditional()) {
          next = branch->getSuccessor(0);
          break;
        }
        auto condition = get(branch->getCondition());
        if (condition.poison) {
          outcome.undefined = true;
          return outcome;
        }
        next = branch->getSuccessor(condition.bits ? 0 : 1);
        break;
#endif
      } else
        throw std::runtime_error("unknown instruction");
    }
    require(next, "missing successor");
    block = next;
  }
  throw std::runtime_error("nonterminating fixture");
}
void check(Shape shape, bool admitted) {
  llvm::LLVMContext context;
  llvm::Module module("offsets", context);
  auto fixture = build(module, shape);
  std::vector<Outcome> before;
  before.reserve(65536);
  for (unsigned lo = 0; lo < 256; ++lo)
    for (unsigned hi = 0; hi < 256; ++hi)
      before.push_back(execute(*fixture.function, lo, hi));
  llvm::LoopAnalysisManager loops;
  llvm::FunctionAnalysisManager functions;
  llvm::CGSCCAnalysisManager cgscc;
  llvm::ModuleAnalysisManager modules;
  llvm::PassBuilder builder;
  builder.registerModuleAnalyses(modules);
  builder.registerCGSCCAnalyses(cgscc);
  builder.registerFunctionAnalyses(functions);
  builder.registerLoopAnalyses(loops);
  builder.crossRegisterProxies(loops, functions, cgscc, modules);
  auto preserved =
      lang::native_llvm::optimizeCheckedOffsets(*fixture.function, functions);
  require(!llvm::verifyFunction(*fixture.function, &llvm::errs()),
          "invalid transformed IR");
  require(fixture.overflow->use_empty() == admitted,
          "unexpected offset admission");
  require(preserved.areAllPreserved() != admitted,
          "incorrect preservation report");
  for (unsigned lo = 0; lo < 256; ++lo)
    for (unsigned hi = 0; hi < 256; ++hi) {
      auto after = execute(*fixture.function, lo, hi);
      const auto &original = before[lo * 256 + hi];
      if (!original.undefined)
        require(after == original,
                "offset refinement changed defined result/effects");
    }
}
} // namespace
int main() {
  try {
    for (unsigned kind = 0; kind < 6; ++kind) {
      Shape shape;
      shape.kind = kind;
      check(shape, kind != 4);
      shape.guard = false;
      shape.nuw = true;
      check(shape, kind != 4);
    }
    Shape shape;
    shape.guard = false;
    check(shape, false);
    shape = {};
    shape.signedGuard = true;
    check(shape, false);
    shape = {};
    shape.mergedGuard = true;
    check(shape, false);
    shape = {};
    shape.stable = false;
    check(shape, false);
    shape = {};
    shape.stable = false;
    shape.freezeEndpoints = true;
    check(shape, true);
    shape = {};
    shape.guard = false;
    shape.nuw = true;
    shape.freezeDifference = true;
    check(shape, false);
    shape = {};
    shape.guard = false;
    shape.nuw = true;
    shape.stable = false;
    check(shape, false);
    shape = {};
    shape.swapped = true;
    check(shape, true);
    std::cout << "checked offsets: 20 exhaustive 8-bit result/effect controls "
                 "passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
