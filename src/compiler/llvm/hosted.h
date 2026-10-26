#pragma once

#include "gti/mir.h"

#include <array>
#include <llvm/IR/IRBuilder.h>
#include <optional>

namespace lang::native_llvm {

class Session;
struct FunctionAbi;

[[nodiscard]] std::optional<std::array<SemanticType, 2>>
hostedNativeParameters(const TargetInfo &target);

[[nodiscard]] FunctionAbi hostedEntryAbi(Session &session);

[[nodiscard]] const MirHostedStartupOperation *
hostedArgumentOperation(const MirProgram &program, const MirBody &body,
                        const MirBlock &block,
                        const MirInstruction &instruction);

[[nodiscard]] llvm::Value *hostedCountOutOfRange(llvm::IRBuilder<> &ir,
                                                 llvm::Value *count);

[[nodiscard]] llvm::Value *hostedArgumentView(Session &session,
                                              llvm::IRBuilder<> &ir,
                                              llvm::Value *arguments,
                                              llvm::Value *index);

[[nodiscard]] llvm::Value *
hostedReturnStatus(llvm::IRBuilder<> &ir,
                   const MirHostedStartupOperation &operation,
                   llvm::Value *result);

[[nodiscard]] std::optional<Literal>
hostedSourceLiteral(const MirInstruction &instruction);

} // namespace lang::native_llvm
