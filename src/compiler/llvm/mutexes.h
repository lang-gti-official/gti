#pragma once

#include "layout.h"

#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

// Representation-only predicates over sealed capability identities. Public
// admission additionally needs the native shell's lifecycle schedule.
[[nodiscard]] std::optional<SemanticType>
nativeMutexElement(const MirProgram &program, const SemanticType &type,
                   CompilerCapabilityTypeKind kind);
[[nodiscard]] std::optional<SemanticType>
nativeMutexConstructor(const MirProgram &program,
                       const MirConstructorInstance &constructor);
[[nodiscard]] bool nativeMutexCall(const MirProgram &program,
                                   const MirBody &body,
                                   const MirInstruction &instruction);

class Mutexes final {
public:
  Mutexes(Session &session, llvm::IRBuilder<> &ir) : session(session), ir(ir) {}
  [[nodiscard]] static bool handles(IntrinsicKind intrinsic);
  [[nodiscard]] llvm::Value *emit(const MirBody &body,
                                  const MirInstruction &instruction,
                                  std::span<llvm::Value *const> inputs);
  void initializeShell(llvm::Value *address, const SemanticType &type);
  void destroyShell(llvm::Value *address, const SemanticType &type);
  void initializeToken(llvm::Value *address, const SemanticType &type);
  void moveToken(llvm::Value *destination, llvm::Value *source,
                 const SemanticType &type);

private:
  Session &session;
  llvm::IRBuilder<> &ir;
  void require(llvm::Value *condition);
};

} // namespace lang::native_llvm
