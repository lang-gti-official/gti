#pragma once

#include "layout.h"
#include <llvm/IR/IRBuilder.h>
#include <span>

namespace lang::native_llvm {

// The MIR verifier owns capability identity, lane shape and loan permissions.
// Addresses and array storage layouts come from the ordinary place machinery.
void emitSimd(llvm::IRBuilder<> &ir, const MirInstruction &instruction,
              std::span<llvm::Value *const> addresses,
              std::span<const TypeLayout> arrays);

} // namespace lang::native_llvm
