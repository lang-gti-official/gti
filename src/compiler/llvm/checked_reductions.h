#pragma once

#include <llvm/IR/PassManager.h>

namespace llvm {
class PassBuilder;
}

namespace lang::native_llvm {

// Late, representation-local proof for bounded conditional integer reductions.
// The guarded clone keeps the original checked loop as its fallback.
llvm::PreservedAnalyses
optimizeCheckedReductions(llvm::Function &function,
                          llvm::FunctionAnalysisManager &analyses);

void registerCheckedReductionPass(llvm::PassBuilder &builder);

} // namespace lang::native_llvm
