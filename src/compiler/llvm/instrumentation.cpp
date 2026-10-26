#include "instrumentation.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Instrumentation/AddressSanitizer.h>
#include <llvm/Transforms/Instrumentation/ThreadSanitizer.h>

#include <bit>
#include <stdexcept>
#include <vector>

namespace lang::native_llvm {

bool instrumentsAddress(InstrumentationMode mode) {
  switch (mode) {
  case InstrumentationMode::None:
  case InstrumentationMode::UndefinedMemory:
  case InstrumentationMode::Thread:
    return false;
  case InstrumentationMode::Address:
  case InstrumentationMode::AddressAndUndefinedMemory:
    return true;
  }
  throw std::invalid_argument("invalid LLVM instrumentation mode");
}

bool instrumentsUndefinedMemory(InstrumentationMode mode) {
  static_cast<void>(instrumentsAddress(mode));
  return mode == InstrumentationMode::UndefinedMemory ||
         mode == InstrumentationMode::AddressAndUndefinedMemory;
}

bool instrumentsThread(InstrumentationMode mode) {
  static_cast<void>(instrumentsAddress(mode));
  return mode == InstrumentationMode::Thread;
}

std::size_t instrumentUndefinedMemory(llvm::Module &module) {
  auto &context = module.getContext();
  const auto &layout = module.getDataLayout();
  if (layout.isDefault() || layout.isNonIntegralAddressSpace(0))
    throw std::logic_error(
        "LLVM memory instrumentation needs an exact native layout");
  auto *pointer = llvm::PointerType::get(context, 0);
  auto *integer =
      llvm::IntegerType::get(context, layout.getPointerSizeInBits(0));
  auto *i8 = llvm::Type::getInt8Ty(context);
  auto *i16 = llvm::Type::getInt16Ty(context);
  auto *i32 = llvm::Type::getInt32Ty(context);
  auto *source = llvm::StructType::get(context, {pointer, i32, i32});
  auto *dataType = llvm::StructType::get(context, {source, pointer, i8, i8});
  std::vector<llvm::Instruction *> accesses;
  for (auto &function : module)
    if (!function.isDeclaration())
      for (auto &block : function)
        for (auto &instruction : block)
          if (llvm::isa<llvm::LoadInst, llvm::StoreInst>(instruction)) {
            const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
            const auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            const auto *address =
                load ? load->getPointerOperand() : store->getPointerOperand();
            if (address->getType()->getPointerAddressSpace() != 0)
              throw std::logic_error("LLVM memory instrumentation requires "
                                     "native address space zero");
            accesses.push_back(&instruction);
          }
  if (accesses.empty())
    return 0;
  auto *handlerType = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                              {pointer, integer}, false);
  constexpr const char *handlerName = "__ubsan_handle_type_mismatch_v1_abort";
  if (const auto *existing = module.getNamedValue(handlerName)) {
    const auto *function = llvm::dyn_cast<llvm::Function>(existing);
    if (!function || function->getFunctionType() != handlerType ||
        function->getCallingConv() != llvm::CallingConv::C ||
        function->getAddressSpace() != 0)
      throw std::logic_error("conflicting LLVM undefined-memory handler ABI");
  }
  auto handler = module.getOrInsertFunction(handlerName, handlerType);
  for (auto *access : accesses) {
    auto *load = llvm::dyn_cast<llvm::LoadInst>(access);
    auto *store = llvm::dyn_cast<llvm::StoreInst>(access);
    auto *address =
        load ? load->getPointerOperand() : store->getPointerOperand();
    const auto alignment = load ? load->getAlign() : store->getAlign();
    llvm::IRBuilder<> ir(access);
    ir.SetCurrentDebugLocation(access->getDebugLoc());
    auto *bits = ir.CreatePtrToInt(address, integer);
    auto *notNull = ir.CreateICmpNE(bits, llvm::ConstantInt::get(integer, 0));
    auto *aligned = ir.CreateICmpEQ(
        ir.CreateAnd(bits,
                     llvm::ConstantInt::get(integer, alignment.value() - 1)),
        llvm::ConstantInt::get(integer, 0));
    auto *valid = ir.CreateAnd(notNull, aligned);
    auto *original = access->getParent();
    auto *continuation = original->splitBasicBlock(access, "ubsan.memory.ok");
    original->getTerminator()->eraseFromParent();
    auto *bad = llvm::BasicBlock::Create(context, "ubsan.memory.bad",
                                         original->getParent(), continuation);
    ir.SetInsertPoint(original);
    ir.CreateCondBr(valid, continuation, bad);
    ir.SetInsertPoint(bad);
    auto *name =
        llvm::ConstantDataArray::getString(context, "'GTI LLVM memory'");
    auto *descriptorType =
        llvm::StructType::get(context, {i16, i16, name->getType()});
    auto *descriptor = new llvm::GlobalVariable(
        module, descriptorType, true, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(descriptorType,
                                  {ir.getInt16(0xffff), ir.getInt16(0), name}),
        "gti.ubsan.type");
    llvm::Constant *filename = llvm::ConstantPointerNull::get(pointer);
    unsigned line = 0, column = 0;
    if (const auto location = access->getDebugLoc()) {
      filename =
          ir.CreateGlobalString(location->getFilename(), "gti.ubsan.file");
      line = location.getLine();
      column = location.getCol();
    }
    auto *location = llvm::ConstantStruct::get(
        source, {filename, ir.getInt32(line), ir.getInt32(column)});
    // compiler-rt mutates SourceLocation.Column when acquiring a diagnostic.
    // TypeMismatchData must therefore be writable, unlike its type descriptor.
    auto *data = new llvm::GlobalVariable(
        module, dataType, false, llvm::GlobalValue::PrivateLinkage,
        llvm::ConstantStruct::get(
            dataType, {location, descriptor,
                       ir.getInt8(std::countr_zero(alignment.value())),
                       ir.getInt8(load ? 0 : 1)}),
        "gti.ubsan.memory");
    ir.CreateCall(handler, {data, bits});
    ir.CreateUnreachable();
  }
  return accesses.size();
}

void instrumentAddress(llvm::Module &module,
                       llvm::ModuleAnalysisManager &analyses) {
  for (const auto &value : module.global_values())
    if (value.getName().starts_with("__tsan_"))
      throw std::logic_error(
          "LLVM address and thread instrumentation are incompatible");
  for (const auto &function : module)
    if (function.hasFnAttribute(llvm::Attribute::SanitizeThread))
      throw std::logic_error(
          "LLVM address and thread instrumentation are incompatible");
  for (auto &function : module)
    if (!function.isDeclaration())
      function.addFnAttr(llvm::Attribute::SanitizeAddress);
  llvm::AddressSanitizerOptions options;
  options.Recover = false;
  options.UseAfterScope = false;
  analyses.invalidate(module, llvm::PreservedAnalyses::none());
  llvm::ModulePassManager pipeline;
  pipeline.addPass(llvm::AddressSanitizerPass(options));
  pipeline.run(module, analyses);
}

void instrumentThread(llvm::Module &module,
                      llvm::ModuleAnalysisManager &analyses) {
  const auto &layout = module.getDataLayout();
  const llvm::Triple triple(module.getTargetTriple());
  if (layout.isDefault() || layout.isNonIntegralAddressSpace(0) ||
      layout.getPointerSizeInBits(0) != 64 ||
      !(triple.isOSDarwin() || triple.isOSLinux()) ||
      !(triple.getArch() == llvm::Triple::aarch64 ||
        triple.getArch() == llvm::Triple::x86_64))
    throw std::logic_error(
        "LLVM thread instrumentation needs a supported native layout");
  // A fresh diagnostic pipeline owns these runtime names. Reject collisions or
  // repeated instrumentation before changing attributes or inserting callbacks.
  for (const auto &value : module.global_values())
    if (value.getName().starts_with("__tsan_"))
      throw std::logic_error("conflicting LLVM thread instrumentation runtime");
  for (const auto &function : module) {
    if (function.hasFnAttribute(llvm::Attribute::SanitizeAddress) ||
        function.hasFnAttribute(llvm::Attribute::SanitizeThread) ||
        function.hasFnAttribute(
            llvm::Attribute::DisableSanitizerInstrumentation) ||
        function.hasFnAttribute(llvm::Attribute::Naked) ||
        function.hasFnAttribute("sanitize_thread_no_checking_at_run_time"))
      throw std::logic_error(
          "conflicting LLVM thread instrumentation attributes");
    for (const auto &block : function)
      for (const auto &instruction : block) {
        const llvm::Value *address = nullptr;
        if (const auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
          address = load->getPointerOperand();
        else if (const auto *store =
                     llvm::dyn_cast<llvm::StoreInst>(&instruction))
          address = store->getPointerOperand();
        else if (const auto *exchange =
                     llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction))
          address = exchange->getPointerOperand();
        else if (const auto *compare =
                     llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction))
          address = compare->getPointerOperand();
        if (address && address->getType()->getPointerAddressSpace() != 0)
          throw std::logic_error(
              "LLVM thread instrumentation requires native address space zero");
      }
  }
  for (auto &function : module)
    if (!function.isDeclaration())
      function.addFnAttr(llvm::Attribute::SanitizeThread);
  analyses.invalidate(module, llvm::PreservedAnalyses::none());
  llvm::ModulePassManager pipeline;
  pipeline.addPass(llvm::ModuleThreadSanitizerPass());
  pipeline.addPass(
      llvm::createModuleToFunctionPassAdaptor(llvm::ThreadSanitizerPass()));
  pipeline.run(module, analyses);
}

} // namespace lang::native_llvm
