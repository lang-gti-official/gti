#pragma once

#include "gti/mir.h"

#include <llvm/IR/IRBuilder.h>
#include <optional>

namespace lang::native_llvm {

[[nodiscard]] llvm::Value *
hostedReturnStatus(llvm::IRBuilder<> &ir,
                   const MirHostedStartupOperation &operation,
                   llvm::Value *result);

[[nodiscard]] std::optional<Literal>
hostedSourceLiteral(const MirInstruction &instruction);

} // namespace lang::native_llvm
