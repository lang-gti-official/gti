#pragma once
namespace llvm {
class Module;
class PassBuilder;
} // namespace llvm
namespace lang::native_llvm {
void registerFloatIntegerRangePass(llvm::PassBuilder &builder);
bool simplifyCheckedFloors(llvm::Module &module);
} // namespace lang::native_llvm
