#pragma once

#include "session.h"
#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

// Counted byte equality never scans for NUL and never touches empty data.
[[nodiscard]] llvm::Value *equalText(Session &session, llvm::IRBuilder<> &ir,
                                     llvm::Value *left, llvm::Value *right);

} // namespace lang::native_llvm
