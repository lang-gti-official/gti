#pragma once

#include "gti/mir.h"

#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

[[nodiscard]] llvm::Value *
hostedReturnStatus(llvm::IRBuilder<> &ir,
                   const MirHostedStartupOperation &operation,
                   llvm::Value *result);

} // namespace lang::native_llvm
