#include "../src/compiler/llvm/instrumentation.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <stdexcept>

using namespace lang::native_llvm;

// LLVM transformation/ABI controls, not generated-GTI execution evidence.
std::unique_ptr<llvm::Module> memory(llvm::LLVMContext &context,
                                    const char *triple, unsigned addressSpace = 0) {
  auto module = std::make_unique<llvm::Module>("instrumentation-control", context);
#if LLVM_VERSION_MAJOR >= 21
  module->setTargetTriple(llvm::Triple(triple));
#else
  module->setTargetTriple(triple);
#endif
  module->setDataLayout("e-p:64:64-i64:64-n8:16:32:64-S128");
  auto *integer = llvm::Type::getInt32Ty(context);
  auto *pointer = llvm::PointerType::get(context, addressSpace);
  auto *read = llvm::Function::Create(
      llvm::FunctionType::get(integer, {pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "read_control", *module);
  llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", read));
  ir.CreateRet(ir.CreateAlignedLoad(integer, read->getArg(0), llvm::Align(4)));
  auto *write = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(context), {pointer, integer}, false),
      llvm::GlobalValue::ExternalLinkage, "write_control", *module);
  ir.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", write));
  ir.CreateAlignedStore(write->getArg(1), write->getArg(0), llvm::Align(4));
  ir.CreateRetVoid();
  return module;
}

std::string text(const llvm::Module &module) {
  std::string value;
  llvm::raw_string_ostream stream(value);
  module.print(stream, nullptr);
  return value;
}

void rejectsUnchanged(llvm::Module &module) {
  const auto before = text(module);
  bool rejected = false;
  try {
    static_cast<void>(instrumentUndefinedMemory(module));
  } catch (const std::logic_error &) {
    rejected = true;
  }
  if (!rejected || text(module) != before)
    throw std::runtime_error("invalid instrumentation input was accepted or partially changed");
}

void rejectsThreadUnchanged(llvm::Module &module) {
  const auto before = text(module);
  llvm::ModuleAnalysisManager analyses;
  bool rejected = false;
  try {
    instrumentThread(module, analyses);
  } catch (const std::logic_error &) {
    rejected = true;
  }
  if (!rejected || text(module) != before)
    throw std::runtime_error("invalid thread instrumentation partially changed the module");
}

void atomicControls(llvm::Module &module) {
  auto &context = module.getContext();
  for (unsigned bits : {8, 16, 32, 64}) {
    auto *integer = llvm::IntegerType::get(context, bits);
    auto *function = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                {llvm::PointerType::get(context, 0), integer}, false),
        llvm::GlobalValue::ExternalLinkage, "atomic_control_" + std::to_string(bits), module);
    llvm::IRBuilder<> ir(llvm::BasicBlock::Create(context, "entry", function));
    auto *address = function->getArg(0);
    auto *value = function->getArg(1);
    auto *store = ir.CreateAlignedStore(value, address, llvm::Align(bits / 8));
    store->setAtomic(llvm::AtomicOrdering::Release);
    auto *load = ir.CreateAlignedLoad(integer, address, llvm::Align(bits / 8));
    load->setAtomic(llvm::AtomicOrdering::Acquire);
    ir.CreateAtomicRMW(llvm::AtomicRMWInst::Add, address, value, llvm::Align(bits / 8),
                        llvm::AtomicOrdering::Monotonic);
    ir.CreateAtomicCmpXchg(address, value, load, llvm::Align(bits / 8),
                           llvm::AtomicOrdering::AcquireRelease,
                           llvm::AtomicOrdering::Monotonic);
    ir.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
    ir.CreateRetVoid();
  }
}

int main() {
  for (const auto invalid : {-1, 5, 99}) {
    bool rejected = false;
    try {
      static_cast<void>(instrumentsAddress(static_cast<InstrumentationMode>(invalid)));
    } catch (const std::invalid_argument &) {
      rejected = true;
    }
    if (!rejected)
      throw std::runtime_error("invalid diagnostic mode accepted");
  }
  unsigned controls = 0;
  for (const char *triple : {"aarch64-apple-macosx", "x86_64-unknown-linux-gnu"}) {
    llvm::LLVMContext context;
    auto empty = std::make_unique<llvm::Module>("empty", context);
    empty->setDataLayout("e-p:64:64");
    const auto unchanged = text(*empty);
    if (instrumentUndefinedMemory(*empty) != 0 || text(*empty) != unchanged)
      throw std::runtime_error("empty module acquired a runtime dependency");
    auto noLayout = memory(context, triple);
    noLayout->setDataLayout("");
    rejectsUnchanged(*noLayout);
    auto foreignSpace = memory(context, triple, 1);
    rejectsUnchanged(*foreignSpace);
    auto conflict = memory(context, triple);
    llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
        llvm::GlobalValue::ExternalLinkage,
        "__ubsan_handle_type_mismatch_v1_abort", *conflict);
    rejectsUnchanged(*conflict);
    auto convention = memory(context, triple);
    auto *handler = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                {llvm::PointerType::get(context, 0),
                                 llvm::Type::getInt64Ty(context)}, false),
        llvm::GlobalValue::ExternalLinkage,
        "__ubsan_handle_type_mismatch_v1_abort", *convention);
    handler->setCallingConv(llvm::CallingConv::Fast);
    rejectsUnchanged(*convention);
    rejectsThreadUnchanged(*noLayout);
    rejectsThreadUnchanged(*foreignSpace);
    auto narrow = memory(context, triple);
    narrow->setDataLayout("e-p:32:32");
    rejectsThreadUnchanged(*narrow);
    auto runtimeConflict = memory(context, triple);
    llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(context), false),
        llvm::GlobalValue::ExternalLinkage, "__tsan_init", *runtimeConflict);
    rejectsThreadUnchanged(*runtimeConflict);
    for (auto attribute : {llvm::Attribute::SanitizeAddress,
                           llvm::Attribute::SanitizeThread,
                           llvm::Attribute::DisableSanitizerInstrumentation}) {
      auto attributed = memory(context, triple);
      attributed->getFunction("read_control")->addFnAttr(attribute);
      rejectsThreadUnchanged(*attributed);
    }
    for (auto mode : {InstrumentationMode::None, InstrumentationMode::Address,
                      InstrumentationMode::UndefinedMemory,
                      InstrumentationMode::AddressAndUndefinedMemory,
                      InstrumentationMode::Thread}) {
      auto module = memory(context, triple);
      if (instrumentsThread(mode))
        atomicControls(*module);
      const auto before = text(*module);
      if (instrumentsUndefinedMemory(mode)) {
        if (instrumentUndefinedMemory(*module) != 2)
          throw std::runtime_error("memory access control count changed");
        const auto *data = module->getGlobalVariable("gti.ubsan.memory", true);
        const auto *type = module->getGlobalVariable("gti.ubsan.type", true);
        if (!data || data->isConstant() || !type || !type->isConstant())
          throw std::runtime_error("compiler-rt data mutability contract changed");
      }
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
      if (instrumentsAddress(mode))
        instrumentAddress(*module, modules);
      if (instrumentsThread(mode))
        instrumentThread(*module, modules);
      if (llvm::verifyModule(*module, &llvm::errs()))
        throw std::runtime_error("instrumentation produced invalid LLVM IR");
      if (mode == InstrumentationMode::None && text(*module) != before)
        throw std::runtime_error("disabled diagnostic mode changed the module");
      if (bool(module->getFunction("__asan_init")) != instrumentsAddress(mode) ||
          bool(module->getFunction("__ubsan_handle_type_mismatch_v1_abort")) !=
              instrumentsUndefinedMemory(mode) ||
          bool(module->getFunction("__tsan_init")) != instrumentsThread(mode))
        throw std::runtime_error("diagnostic runtime dependency leaked across modes");
      if (instrumentsThread(mode)) {
        for (unsigned bits : {8, 16, 32, 64})
          for (const char *operation : {"_load", "_store", "_fetch_add", "_compare_exchange_val"})
            if (!module->getFunction("__tsan_atomic" + std::to_string(bits) + operation))
              throw std::runtime_error("thread instrumentation missed an atomic operation");
        if (!module->getFunction("__tsan_read4") || !module->getFunction("__tsan_write4") ||
            !module->getFunction("__tsan_atomic_thread_fence"))
          throw std::runtime_error("thread instrumentation missed memory or fence controls");
        rejectsThreadUnchanged(*module);
      }
      ++controls;
    }
  }
  std::cout << controls << " LLVM mode/layout/handler controls plus rejection preflight passed\n";
}
