#pragma once

#include "abi.h"
#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

class Failures final {
public:
  explicit Failures(Session &session);
  [[nodiscard]] llvm::Constant *artifact() const { return artifact_; }
  [[nodiscard]] llvm::Value *record(llvm::IRBuilder<> &ir,
                                    llvm::Value *channel);
  [[nodiscard]] llvm::Value *origin(llvm::IRBuilder<> &ir,
                                    llvm::Value *channel);
  void copyChannel(llvm::IRBuilder<> &ir, llvm::Value *destination,
                   llvm::Value *source);
  void originate(llvm::IRBuilder<> &ir, llvm::Value *channel,
                 const MirInstruction &instruction,
                 DefinedFailureOutcome outcome,
                 llvm::Value *callerSite = nullptr,
                 llvm::Value *callerArtifact = nullptr,
                 llvm::Value *message = nullptr);
  void terminate(llvm::IRBuilder<> &ir, llvm::Value *channel);
  void terminateCleanup(llvm::IRBuilder<> &ir, llvm::Value *primary,
                        llvm::Value *secondary);
  [[nodiscard]] llvm::Value *enterCleanup(llvm::IRBuilder<> &ir,
                                          llvm::Value *primary);
  void leaveCleanup(llvm::IRBuilder<> &ir, llvm::Value *token);
  void checkIncomingCleanup(llvm::IRBuilder<> &ir, llvm::Value *secondary);

private:
  [[nodiscard]] llvm::Function *cleanupTerminator();
  Session &session;
  llvm::Constant *artifact_ = nullptr;
  llvm::Function *cleanupTerminator_ = nullptr;
};

} // namespace lang::native_llvm
