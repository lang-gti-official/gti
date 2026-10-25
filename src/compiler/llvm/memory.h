#pragma once

#include "lifetimes.h"
#include "scalar.h"

namespace lang::native_llvm {

// Typed lifetime mechanisms only. Allocation ownership, initialized-slot sets,
// growth and reference-count policy remain in their existing source/MIR owners.
class Memory final {
public:
  struct Result {
    bool handled = false;
    llvm::Value *value = nullptr;
    // A cleanup's existing private GTI status, never a second failure protocol.
    llvm::Value *status = nullptr;
  };

  Memory(Session &session, llvm::IRBuilder<> &ir, Lifetimes &lifetimes,
         Scalars::Guard guard);
  [[nodiscard]] Result emit(const MirInstruction &instruction,
                            std::span<llvm::Value *const> inputs,
                            llvm::Value *failureChannel, bool duringFailure);

private:
  Session &session;
  llvm::IRBuilder<> &ir;
  Lifetimes &lifetimes;
  Scalars::Guard guard;
  void relocate(const MirInstruction &instruction,
                std::span<llvm::Value *const> inputs);
};

} // namespace lang::native_llvm
