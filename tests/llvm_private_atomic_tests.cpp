#include "../src/compiler/llvm/private_atomics.h"
#include <iostream>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <stdexcept>

namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
enum class Shape {
  Direct,
  Wrapped,
  Lifetime,
  Shared,
  Escaped,
  NoCapture,
  PointerStore,
  AddressBits,
  Offset,
  Dynamic,
  MixedWidth,
  VolatileRmw,
  VolatileLoad,
  VolatileStore,
  AcquireRmw,
  ReleaseRmw,
  AcqRelRmw,
  SeqCstRmw,
  AcquireLoad,
  ReleaseStore,
  CompareExchange,
  SelectView,
  TwoCells,
  LargeGraph,
  Undersized,
  Scalable
};
struct Fixture {
  llvm::Function *function;
  llvm::AtomicRMWInst *update;
};
Fixture build(llvm::Module &module, Shape shape,
              llvm::AtomicRMWInst::BinOp operation) {
  auto &context = module.getContext();
  llvm::IRBuilder<> b(context);
  auto *function = llvm::Function::Create(
      llvm::FunctionType::get(
          b.getInt16Ty(), {b.getInt8Ty(), b.getInt8Ty(), b.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "test", module);
  auto *block = llvm::BasicBlock::Create(context, "entry", function);
  b.SetInsertPoint(block);
  auto *initial = function->getArg(0), *delta = function->getArg(1);
  llvm::Type *storage = b.getInt8Ty();
  if (shape == Shape::Wrapped)
    storage = llvm::StructType::get(
        context, llvm::ArrayRef<llvm::Type *>({b.getInt8Ty()}));
  if (shape == Shape::Offset || shape == Shape::MixedWidth)
    storage = llvm::ArrayType::get(b.getInt8Ty(), 2);
  if (shape == Shape::Undersized)
    storage = llvm::ArrayType::get(b.getInt8Ty(), 0);
  if (shape == Shape::Scalable)
    storage = llvm::VectorType::get(b.getInt8Ty(),
                                    llvm::ElementCount::getScalable(4));
  auto *allocation =
      b.CreateAlloca(storage, shape == Shape::Dynamic ? delta : nullptr);
  llvm::Value *pointer = allocation;
  if (shape == Shape::Wrapped)
    pointer = b.CreateStructGEP(storage, allocation, 0);
  if (shape == Shape::Offset)
    pointer = b.CreateInBoundsGEP(storage, allocation,
                                  {b.getInt32(0), b.getInt32(1)});
  if (shape == Shape::Shared)
    pointer = function->getArg(2);
  if (shape == Shape::SelectView)
    pointer = b.CreateSelect(b.CreateICmpEQ(initial, delta), allocation,
                             function->getArg(2));
  if (shape == Shape::Lifetime)
    b.CreateLifetimeStart(allocation);
  auto *store = b.CreateStore(initial, pointer);
  store->setAtomic(shape == Shape::ReleaseStore
                       ? llvm::AtomicOrdering::Release
                       : llvm::AtomicOrdering::Monotonic);
  store->setAlignment(llvm::Align(1));
  store->setVolatile(shape == Shape::VolatileStore);
  auto order = llvm::AtomicOrdering::Monotonic;
  if (shape == Shape::AcquireRmw)
    order = llvm::AtomicOrdering::Acquire;
  if (shape == Shape::ReleaseRmw)
    order = llvm::AtomicOrdering::Release;
  if (shape == Shape::AcqRelRmw)
    order = llvm::AtomicOrdering::AcquireRelease;
  if (shape == Shape::SeqCstRmw)
    order = llvm::AtomicOrdering::SequentiallyConsistent;
  auto *old =
      b.CreateAtomicRMW(operation, pointer, delta, llvm::Align(1), order);
  old->setVolatile(shape == Shape::VolatileRmw);
  // Place rejection uses after the RMW to verify transactional validation.
  if (shape == Shape::Escaped || shape == Shape::NoCapture) {
    auto *callee = llvm::Function::Create(
        llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy()}, false),
        llvm::GlobalValue::ExternalLinkage, "escape", module);
    if (shape == Shape::NoCapture) {
#if LLVM_VERSION_MAJOR >= 21
      callee->addParamAttr(0, llvm::Attribute::getWithCaptureInfo(
                                  context, llvm::CaptureInfo::none()));
#else
      callee->addParamAttr(0, llvm::Attribute::NoCapture);
#endif
    }
    b.CreateCall(callee, {pointer});
  }
  if (shape == Shape::PointerStore)
    b.CreateStore(pointer, function->getArg(2));
  if (shape == Shape::AddressBits)
    b.CreateStore(b.CreatePtrToInt(pointer, b.getInt64Ty()),
                  function->getArg(2));
  if (shape == Shape::MixedWidth)
    b.CreateStore(b.getInt16(3), pointer);
  if (shape == Shape::CompareExchange)
    b.CreateAtomicCmpXchg(pointer, initial, delta, llvm::Align(1),
                          llvm::AtomicOrdering::Monotonic,
                          llvm::AtomicOrdering::Monotonic);
  if (shape == Shape::LargeGraph)
    for (unsigned n = 0; n < 520; ++n)
      b.CreateLoad(b.getInt8Ty(), pointer);
  if (shape == Shape::TwoCells) {
    auto *shared =
        b.CreateAtomicRMW(operation, function->getArg(2), delta, llvm::Align(1),
                          llvm::AtomicOrdering::Monotonic);
    b.CreateStore(shared, function->getArg(2));
  }
  auto *value = b.CreateLoad(b.getInt8Ty(), pointer);
  value->setAtomic(shape == Shape::AcquireLoad
                       ? llvm::AtomicOrdering::Acquire
                       : llvm::AtomicOrdering::Monotonic);
  value->setAlignment(llvm::Align(1));
  value->setVolatile(shape == Shape::VolatileLoad);
  if (shape == Shape::Lifetime)
    b.CreateLifetimeEnd(allocation);
  b.CreateRet(
      b.CreateOr(b.CreateShl(b.CreateZExt(old, b.getInt16Ty()), b.getInt16(8)),
                 b.CreateZExt(value, b.getInt16Ty())));
  require(!llvm::verifyFunction(*function, &llvm::errs()), "invalid fixture");
  return {function, old};
}
unsigned updated(llvm::AtomicRMWInst::BinOp operation, unsigned old,
                 unsigned value) {
  switch (operation) {
  case llvm::AtomicRMWInst::Add:
    return (old + value) & 255;
  case llvm::AtomicRMWInst::Sub:
    return (old - value) & 255;
  case llvm::AtomicRMWInst::And:
    return old & value;
  case llvm::AtomicRMWInst::Or:
    return old | value;
  case llvm::AtomicRMWInst::Xor:
    return old ^ value;
  case llvm::AtomicRMWInst::Xchg:
    return value;
  default:
    throw std::runtime_error("unknown RMW");
  }
}
// Independent single-cell interpreter; the expected result below is computed
// directly from unsigned wrapping arithmetic, not from the lowering helper.
unsigned execute(llvm::Function &function, unsigned initial, unsigned delta) {
  llvm::DenseMap<llvm::Value *, uint64_t> values;
  values[function.getArg(0)] = initial;
  values[function.getArg(1)] = delta;
  uint64_t memory = 0;
  auto get = [&](llvm::Value *v) {
    if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(v))
      return c->getZExtValue();
    auto found = values.find(v);
    require(found != values.end(), "missing scalar");
    return found->second;
  };
  for (auto &instruction : function.getEntryBlock()) {
    if (llvm::isa<llvm::AllocaInst, llvm::GetElementPtrInst>(&instruction))
      continue;
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
      memory = get(store->getValueOperand());
    else if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
      values[load] = memory;
    else if (auto *rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction)) {
      values[rmw] = memory;
      memory = updated(rmw->getOperation(), memory, get(rmw->getValOperand()));
    } else if (auto *binary =
                   llvm::dyn_cast<llvm::BinaryOperator>(&instruction)) {
      uint64_t a = get(binary->getOperand(0)), b = get(binary->getOperand(1)),
               value = 0;
      switch (binary->getOpcode()) {
      case llvm::Instruction::Add:
        value = a + b;
        break;
      case llvm::Instruction::Sub:
        value = a - b;
        break;
      case llvm::Instruction::And:
        value = a & b;
        break;
      case llvm::Instruction::Or:
        value = a | b;
        break;
      case llvm::Instruction::Xor:
        value = a ^ b;
        break;
      case llvm::Instruction::Shl:
        value = a << b;
        break;
      default:
        throw std::runtime_error("unknown arithmetic");
      }
      unsigned bits = binary->getType()->getIntegerBitWidth();
      values[binary] = value & ((1ULL << bits) - 1);
    } else if (auto *cast = llvm::dyn_cast<llvm::ZExtInst>(&instruction))
      values[cast] = get(cast->getOperand(0));
    else if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction))
      return static_cast<unsigned>(get(ret->getReturnValue()));
    else if (auto *intrinsic =
                 llvm::dyn_cast<llvm::IntrinsicInst>(&instruction)) {
      require(intrinsic->isLifetimeStartOrEnd(), "unknown intrinsic");
    } else
      throw std::runtime_error("unknown instruction");
  }
  throw std::runtime_error("missing return");
}
unsigned countAtomics(llvm::Function &function) {
  unsigned count = 0;
  for (auto &block : function)
    for (auto &instruction : block)
      if (llvm::isa<llvm::AtomicRMWInst>(&instruction))
        ++count;
  return count;
}
void check(Shape shape, llvm::AtomicRMWInst::BinOp operation, bool accepted) {
  llvm::LLVMContext context;
  llvm::Module module("private", context);
  module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
  auto fixture = build(module, shape, operation);
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
      lang::native_llvm::optimizePrivateAtomics(*fixture.function, functions);
  require(preserved.areAllPreserved() != accepted, "unexpected admission");
  require(!llvm::verifyFunction(*fixture.function, &llvm::errs()),
          "invalid lowered IR");
  if (!accepted) {
    require(countAtomics(*fixture.function) == 1, "rejected graph changed");
    return;
  }
  require(countAtomics(*fixture.function) ==
              (shape == Shape::TwoCells ? 1U : 0U),
          "shared/private isolation failed");
  if (shape == Shape::TwoCells)
    return;
  for (unsigned a = 0; a < 256; ++a)
    for (unsigned b = 0; b < 256; ++b)
      require(execute(*fixture.function, a, b) ==
                  ((a << 8) | updated(operation, a, b)),
              "old/new mismatch after lowering");
  functions.invalidate(*fixture.function, preserved);
  llvm::FunctionPassManager clean;
  clean.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
  clean.run(*fixture.function, functions);
  for (auto &block : *fixture.function)
    for (auto &instruction : block)
      require(!llvm::isa<llvm::AllocaInst, llvm::LoadInst, llvm::StoreInst,
                         llvm::AtomicRMWInst>(&instruction),
              "private cell not promoted");
  for (unsigned a = 0; a < 256; ++a)
    for (unsigned b = 0; b < 256; ++b)
      require(execute(*fixture.function, a, b) ==
                  ((a << 8) | updated(operation, a, b)),
              "old/new mismatch after promotion");
}
} // namespace
int main() {
  try {
    for (auto operation : {llvm::AtomicRMWInst::Add, llvm::AtomicRMWInst::Sub,
                           llvm::AtomicRMWInst::And, llvm::AtomicRMWInst::Or,
                           llvm::AtomicRMWInst::Xor, llvm::AtomicRMWInst::Xchg})
      for (auto shape : {Shape::Direct, Shape::Wrapped, Shape::Lifetime})
        check(shape, operation, true);
    for (auto shape :
         {Shape::Shared,       Shape::Escaped,       Shape::NoCapture,
          Shape::PointerStore, Shape::AddressBits,   Shape::Offset,
          Shape::Dynamic,      Shape::MixedWidth,    Shape::VolatileRmw,
          Shape::VolatileLoad, Shape::VolatileStore, Shape::AcquireRmw,
          Shape::ReleaseRmw,   Shape::AcqRelRmw,     Shape::SeqCstRmw,
          Shape::AcquireLoad,  Shape::ReleaseStore,  Shape::CompareExchange,
          Shape::SelectView,   Shape::LargeGraph,    Shape::Undersized,
          Shape::Scalable})
      check(shape, llvm::AtomicRMWInst::Add, false);
    check(Shape::Direct, llvm::AtomicRMWInst::Nand, false);
    check(Shape::TwoCells, llvm::AtomicRMWInst::Add, true);
    for (auto level :
         {llvm::OptimizationLevel::O0, llvm::OptimizationLevel::O1,
          llvm::OptimizationLevel::O2, llvm::OptimizationLevel::O3}) {
      llvm::LLVMContext context;
      llvm::Module module("pipeline", context);
      module.setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
      auto fixture = build(module, Shape::Direct, llvm::AtomicRMWInst::Add);
      llvm::LoopAnalysisManager loops;
      llvm::FunctionAnalysisManager functions;
      llvm::CGSCCAnalysisManager cgscc;
      llvm::ModuleAnalysisManager modules;
      llvm::PassBuilder builder;
      lang::native_llvm::registerPrivateAtomicPass(builder);
      builder.registerModuleAnalyses(modules);
      builder.registerCGSCCAnalyses(cgscc);
      builder.registerFunctionAnalyses(functions);
      builder.registerLoopAnalyses(loops);
      builder.crossRegisterProxies(loops, functions, cgscc, modules);
      auto pipeline = level == llvm::OptimizationLevel::O0
                          ? builder.buildO0DefaultPipeline(level)
                          : builder.buildPerModuleDefaultPipeline(level);
      pipeline.run(module, modules);
      require(!llvm::verifyModule(module, &llvm::errs()),
              "invalid pipeline IR");
      require(countAtomics(*fixture.function) ==
                  (level == llvm::OptimizationLevel::O0 ||
                           level == llvm::OptimizationLevel::O1
                       ? 1U
                       : 0U),
              "private atomic optimization-level boundary changed");
    }
    std::cout << "private atomics: 18 exhaustive old/new controls and 24 "
                 "isolation controls plus O0-O3 pipeline controls passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
