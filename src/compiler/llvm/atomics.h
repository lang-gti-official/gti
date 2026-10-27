#pragma once

#include "atomic_types.h"
#include "session.h"

#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

// The irreducible atomic operations only. Fetch arithmetic and retry policy
// remain source-defined; blocking and worker protocols have separate adapters.
class Atomics final {
public:
  Atomics(Session &session, llvm::IRBuilder<> &ir);
  [[nodiscard]] static bool handles(IntrinsicKind intrinsic);
  [[nodiscard]] llvm::Value *emit(const MirBody &body,
                                  const MirInstruction &instruction,
                                  std::span<llvm::Value *const> inputs);
  void initialize(const MirConstructorInstance &constructor,
                  llvm::Value *destination, llvm::Value *value);

private:
  Session &session;
  llvm::IRBuilder<> &ir;
  [[nodiscard]] llvm::Value *lane(llvm::Value *value,
                                  const SemanticType &element);
  [[nodiscard]] llvm::Value *value(llvm::Value *lane,
                                   const SemanticType &element);
};

} // namespace lang::native_llvm
