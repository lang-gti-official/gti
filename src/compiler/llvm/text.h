#pragma once

#include "session.h"
#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

[[nodiscard]] const std::string *wholeCStringLiteral(const MirBody &body,
                                                     const MirOperand &operand);

// Counted byte equality never scans for NUL and never touches empty data.
[[nodiscard]] llvm::Value *equalText(Session &session, llvm::IRBuilder<> &ir,
                                     llvm::Value *left, llvm::Value *right);

} // namespace lang::native_llvm
