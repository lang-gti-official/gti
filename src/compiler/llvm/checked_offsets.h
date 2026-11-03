#pragma once
#include <llvm/IR/PassManager.h>
namespace llvm {
class PassBuilder;
}
namespace lang::native_llvm {
llvm::PreservedAnalyses optimizeCheckedOffsets(llvm::Function &,
                                               llvm::FunctionAnalysisManager &);
void registerCheckedOffsetPass(llvm::PassBuilder &);
} // namespace lang::native_llvm
