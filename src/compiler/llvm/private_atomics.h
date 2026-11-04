#pragma once
#include <llvm/IR/PassManager.h>
namespace llvm {
class PassBuilder;
}
namespace lang::native_llvm {
llvm::PreservedAnalyses optimizePrivateAtomics(llvm::Function &,
                                               llvm::FunctionAnalysisManager &);
void registerPrivateAtomicPass(llvm::PassBuilder &);
} // namespace lang::native_llvm
