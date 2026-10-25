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

int main() {
  for (const auto invalid : {-1, 4, 99}) {
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
    for (auto mode : {InstrumentationMode::None, InstrumentationMode::Address,
                      InstrumentationMode::UndefinedMemory,
                      InstrumentationMode::AddressAndUndefinedMemory}) {
      auto module = memory(context, triple);
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
      if (llvm::verifyModule(*module, &llvm::errs()))
        throw std::runtime_error("instrumentation produced invalid LLVM IR");
      if (mode == InstrumentationMode::None && text(*module) != before)
        throw std::runtime_error("disabled diagnostic mode changed the module");
      if (bool(module->getFunction("__asan_init")) != instrumentsAddress(mode) ||
          bool(module->getFunction("__ubsan_handle_type_mismatch_v1_abort")) !=
              instrumentsUndefinedMemory(mode))
        throw std::runtime_error("diagnostic runtime dependency leaked across modes");
      ++controls;
    }
  }
  std::cout << controls << " LLVM mode/layout/handler controls plus rejection preflight passed\n";
}
