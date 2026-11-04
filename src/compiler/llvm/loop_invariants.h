#pragma once
#include <cstddef>
namespace llvm {
class Module;
}
namespace lang::native_llvm {
// Prove bounded, conditionally invariant pointer PHIs by loop induction.
std::size_t foldInductivePointerLoops(llvm::Module &module);
} // namespace lang::native_llvm
