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

// Bounded checked additions in non-reducing loops (fills, stores, keys).
// Run after the reduction/range passes so those stronger plans get priority.
llvm::PreservedAnalyses
optimizeCheckedInvariantAdds(llvm::Function &function,
                             llvm::FunctionAnalysisManager &analyses);
void registerCheckedInvariantAddPass(llvm::PassBuilder &builder);

void registerCheckedReductionPass(llvm::PassBuilder &builder);

} // namespace lang::native_llvm
