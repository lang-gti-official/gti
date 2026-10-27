#pragma once

#include "memory_types.h"
#include "scalar.h"

namespace lang::native_llvm {

// Typed lifetime mechanisms only. Allocation ownership, initialized-slot sets,
// growth and reference-count policy remain in their existing source/MIR owners.
class Memory final {
public:
  using Cleanup = std::function<llvm::Function *(const MirDropType &)>;
  using Relocate =
      std::function<void(llvm::Value *, llvm::Value *, const SemanticType &)>;
  struct Result {
    bool handled = false;
    llvm::Value *value = nullptr;
    // A cleanup's existing private GTI status, never a second failure protocol.
    llvm::Value *status = nullptr;
  };
  struct SharedRelease {
    llvm::Value *last = nullptr;
    llvm::Value *allocation = nullptr;
    llvm::Value *block = nullptr;
  };

  Memory(Session &session, llvm::IRBuilder<> &ir, Cleanup cleanup,
         Relocate relocateObject, Scalars::Guard guard);
  [[nodiscard]] static bool handles(IntrinsicKind intrinsic);
  [[nodiscard]] llvm::Value *processCall(const MirFunctionInstance &target,
                                         std::span<llvm::Value *const> inputs);
  [[nodiscard]] llvm::Value *allocateObject(const MirInstruction &instruction);
  void releaseBytes(llvm::Value *address);
  [[nodiscard]] SharedRelease releaseShared(llvm::Value *handle);
  [[nodiscard]] Result emit(const MirInstruction &instruction,
                            std::span<llvm::Value *const> inputs,
                            llvm::Value *failureChannel, bool duringFailure);
  // The one typed relocation of a single live object: a byte transfer for a
  // bitwise-relocatable type, otherwise the verified structural relocation.
  // Either way the source is retired and must be refilled or abandoned, never
  // destroyed.
  void transfer(llvm::Value *destination, llvm::Value *source,
                const SemanticType &type);
  // Private storage for one object held during a restoring exchange.
  [[nodiscard]] llvm::Value *held(const SemanticType &type);

private:
  Session &session;
  llvm::IRBuilder<> &ir;
  Cleanup cleanup;
  Relocate relocateObject;
  Scalars::Guard guard;
  [[nodiscard]] llvm::Function *
  provider(ProcessMemoryRuntimeOperation operation);
  [[nodiscard]] llvm::Value *
  providerCall(ProcessMemoryRuntimeOperation operation,
               std::span<llvm::Value *const> inputs);
  [[nodiscard]] llvm::Value *allocateBytes(std::uint64_t size,
                                           std::uint64_t alignment);
  [[nodiscard]] llvm::Value *shared(const MirInstruction &instruction,
                                    std::span<llvm::Value *const> inputs);
  void relocate(const MirInstruction &instruction,
                std::span<llvm::Value *const> inputs);
};

} // namespace lang::native_llvm
