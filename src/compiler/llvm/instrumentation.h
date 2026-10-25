#pragma once

#include <llvm/IR/PassManager.h>

#include <cstddef>

namespace llvm {
class Module;
}

namespace lang::native_llvm {

// Diagnostic instrumentation, not new GTI execution semantics. UndefinedMemory
// checks LLVM load/store null/alignment obligations only, not all Clang UBSan.
enum class InstrumentationMode {
  None,
  Address,
  UndefinedMemory,
  AddressAndUndefinedMemory,
};

[[nodiscard]] bool instrumentsAddress(InstrumentationMode mode);
[[nodiscard]] bool instrumentsUndefinedMemory(InstrumentationMode mode);
// Null/alignment checks precede optimization, before LLVM can exploit invalid
// memory accesses. Returns the number of original load/store sites covered.
std::size_t instrumentUndefinedMemory(llvm::Module &module);
// Run after the selected optimization pipeline, before verification/codegen.
void instrumentAddress(llvm::Module &module,
                       llvm::ModuleAnalysisManager &analyses);

} // namespace lang::native_llvm
