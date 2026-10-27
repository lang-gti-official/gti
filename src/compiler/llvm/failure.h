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
  // Terminal two-record emergency. Under worker containment a native managed
  // worker instead publishes the envelope and requests frame abandonment, so
  // the emitted call returns there; the caller must then follow `abandoning`.
  // Without containment the call never returns and this emits `unreachable`.
  void terminateCleanup(llvm::IRBuilder<> &ir, llvm::Value *primary,
                        llvm::Value *secondary);
  [[nodiscard]] llvm::Value *enterCleanup(llvm::IRBuilder<> &ir,
                                          llvm::Value *primary);
  // Enter with a borrowed record/artifact pair not held in a channel.
  [[nodiscard]] llvm::Value *enterCleanupRecord(llvm::IRBuilder<> &ir,
                                                llvm::Value *record,
                                                llvm::Value *artifact);
  void leaveCleanup(llvm::IRBuilder<> &ir, llvm::Value *token);
  void checkIncomingCleanup(llvm::IRBuilder<> &ir, llvm::Value *secondary);

  // Managed-worker containment exists only in programs with verified managed
  // task adapters. The inherited cleanup context is then generated
  // thread-local state, and a cleanup emergency on a native managed worker is
  // published to the runtime worker sink while every generated frame up to the
  // task boundary returns failure status without further cleanup. No C++
  // exception ever crosses a generated frame.
  [[nodiscard]] bool workerContainment() const { return containment_; }
  // i1: a worker emergency was published and frames must be abandoned.
  [[nodiscard]] llvm::Value *abandoning(llvm::IRBuilder<> &ir);
  void clearAbandoning(llvm::IRBuilder<> &ir);
  // Entry adapters mark their dedicated runtime-created worker thread.
  void markWorker(llvm::IRBuilder<> &ir);
  // i1: the current thread is a native managed worker; constant false in a
  // program without managed task adapters.
  [[nodiscard]] llvm::Value *onWorker(llvm::IRBuilder<> &ir);
  // Suspend/restore worker routing around a terminal native callback.
  [[nodiscard]] llvm::Value *suspendWorker(llvm::IRBuilder<> &ir);
  void restoreWorker(llvm::IRBuilder<> &ir, llvm::Value *previous);
  // Copy the ordinary record in `channel` to the current runtime sink.
  void publishOrdinary(llvm::IRBuilder<> &ir, llvm::Value *channel);

private:
  [[nodiscard]] llvm::Function *cleanupTerminator();
  [[nodiscard]] llvm::Function *emergency();
  [[nodiscard]] llvm::GlobalVariable *threadState(const char *name,
                                                  llvm::Type *type);
  [[nodiscard]] llvm::Value *slot(llvm::IRBuilder<> &ir,
                                  llvm::GlobalVariable *global);
  [[nodiscard]] llvm::StructType *failureView();
  Session &session;
  llvm::Constant *artifact_ = nullptr;
  llvm::Function *cleanupTerminator_ = nullptr;
  llvm::Function *emergency_ = nullptr;
  bool containment_ = false;
  llvm::GlobalVariable *incomingRecord_ = nullptr;
  llvm::GlobalVariable *incomingArtifact_ = nullptr;
  llvm::GlobalVariable *worker_ = nullptr;
  llvm::GlobalVariable *abandon_ = nullptr;
  llvm::StructType *failureView_ = nullptr;
};

} // namespace lang::native_llvm
