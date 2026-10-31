#pragma once
#include <llvm/IR/PassManager.h>
namespace llvm {
class PassBuilder;
}
namespace lang::native_llvm {
llvm::PreservedAnalyses optimizeCheckedRanges(llvm::Function &,
                                              llvm::FunctionAnalysisManager &);
void registerCheckedRangePass(llvm::PassBuilder &);
} // namespace lang::native_llvm
