#pragma once

#include "abi.h"

#include <functional>
#include <llvm/IR/IRBuilder.h>
#include <span>

namespace lang::native_llvm {

class Failures;
class Lifetimes;

// The trusted managed-thread handle capability. A void handle owns exactly
// one runtime join obligation (its native state pointer). A result-bearing
// handle additionally retains one successfully joined result context and its
// take/abandon callbacks until the single take. Representation only: join,
// transfer and cleanup policy remain the verified MIR schedule's.
struct ThreadHandleShape final {
  bool resultBearing = false;
  SemanticType result = SemanticType::Void;
};

[[nodiscard]] std::optional<ThreadHandleShape>
nativeThreadHandle(const MirProgram &program, const SemanticType &type);

// One exact zero-argument `void()` or `Result()` entry selected by verified
// MIR, with once invocation and contained worker failure. Task and result
// value representation is checked separately by the storage witnesses.
[[nodiscard]] bool nativeThreadTaskAdapter(const MirProgram &program,
                                           const MirThreadTaskAdapter &adapter);

[[nodiscard]] bool isNativeThreadIntrinsic(IntrinsicKind intrinsic);
// Exact staged managed-thread operation over sealed MIR facts and trusted
// capability identities. No public library spelling is consulted.
[[nodiscard]] bool nativeThreadCall(const MirProgram &program,
                                    MirBodyAddress address, const MirBody &body,
                                    const MirInstruction &instruction);

// Runtime C records exchanged with `runtime_thread.h`. They are private
// lowering types matching the versioned C declarations exactly.
struct ThreadRuntimeTypes final {
  llvm::StructType *spawnResult = nullptr;   // gti_thread_spawn_result_v1
  llvm::StructType *joinResult = nullptr;    // gti_thread_join_result_v2
  llvm::StructType *valueJoin = nullptr;     // gti_thread_value_join_result_v2
  llvm::StructType *scopeResult = nullptr;   // gti_thread_scope_result_v1
  llvm::StructType *scopedTask = nullptr;    // gti_thread_scoped_task_v1
  llvm::StructType *failureView = nullptr;   // gti_thread_failure_view_v1
};
[[nodiscard]] ThreadRuntimeTypes threadRuntimeTypes(Session &session);

// Managed task adapters and operations. Every generated callback contains
// GTI failure before returning to the non-unwinding C runtime: a body failure
// is published to the runtime-installed worker sink, never thrown.
class ThreadTasks final {
public:
  using FindBody = std::function<llvm::Function *(MirBodyAddress)>;
  using FindAbi = std::function<FunctionAbi(MirBodyAddress)>;

  ThreadTasks(Session &session, Failures &failures, Lifetimes &lifetimes,
              FindBody findBody, FindAbi findAbi);

  [[nodiscard]] static bool handles(IntrinsicKind intrinsic) {
    return isNativeThreadIntrinsic(intrinsic);
  }

  // Facts supplied by the owning body for one exact staged operation.
  struct Stage {
    MirBodyAddress address;
    const MirBody &body;
    const MirInstruction &instruction;
    std::span<llvm::Value *const> inputs;
    // The exact Invoke failure channel for operations with a TaskJoin edge.
    llvm::Value *failure = nullptr;
    // Takes the verified MIR failure edge after `failure` is populated.
    std::function<void()> failed;
    // Final storage for an object-valued result.
    std::function<llvm::Value *()> destination;
    // Moves a consumed value into fresh storage using the body's typed
    // value transport (native identities are rebuilt, never byte-copied).
    std::function<void(llvm::Value *, llvm::Value *, const SemanticType &)>
        store;
    // Loads a value-form result from its storage.
    std::function<llvm::Value *(llvm::Value *, const SemanticType &)> load;
  };
  [[nodiscard]] llvm::Value *emit(llvm::IRBuilder<> &ir, const Stage &stage);

private:
  struct Adapter final {
    const MirThreadTaskAdapter *facts = nullptr;
    llvm::StructType *box = nullptr;
    llvm::Function *entry = nullptr;
    llvm::Function *destroy = nullptr;       // void task
    llvm::Function *complete = nullptr;      // value task
    llvm::Function *abandonTask = nullptr;   // value task
    llvm::Function *take = nullptr;          // value task
    llvm::Function *abandonResult = nullptr; // value task
  };
  Session &session;
  Failures &failures;
  Lifetimes &lifetimes;
  FindBody findBody;
  FindAbi findAbi;
  ThreadRuntimeTypes runtime;
  std::map<MirThreadTaskAdapterId, Adapter> adapters;
  llvm::Function *capture_ = nullptr;
  llvm::StructType *captureBuffer_ = nullptr;

  [[nodiscard]] const Adapter &adapter(MirThreadTaskAdapterId id);
  void emitEntry(Adapter &adapter);
  void emitValueCallbacks(Adapter &adapter);
  void emitDestroy(Adapter &adapter);
  [[nodiscard]] llvm::Function *runtimeService(const char *name,
                                               llvm::Type *result,
                                               llvm::ArrayRef<llvm::Type *> parameters,
                                               llvm::Type *sret = nullptr);
  // Task/result cleanup at a task boundary, following the runtime's task
  // cleanup state like the worker entry boundary. Returns i1: true when a
  // terminal emergency abandoned the remaining task state (no box release).
  [[nodiscard]] llvm::Value *cleanupTaskValue(llvm::IRBuilder<> &ir,
                                              const SemanticType &type,
                                              llvm::Value *address,
                                              llvm::Value *liveFlag = nullptr);
  [[nodiscard]] bool fallibleCleanup(const SemanticType &type) const;
  // A caller-thread region that may synchronously run task/result cleanup.
  // On a managed worker its outcome reaches the worker sink and execution
  // continues; elsewhere a captured failure is terminal after the region.
  [[nodiscard]] llvm::Value *
  callerContained(llvm::IRBuilder<> &ir,
                  const std::function<llvm::Value *()> &region);
  [[nodiscard]] llvm::Function *captureCallback();
  [[nodiscard]] llvm::StructType *captureBuffer();
  void freeBox(llvm::IRBuilder<> &ir, llvm::Value *box);
  void require(llvm::IRBuilder<> &ir, llvm::Value *condition);
  void storeValue(llvm::IRBuilder<> &ir, llvm::Value *destination,
                  llvm::Value *source, const SemanticType &type);

  [[nodiscard]] llvm::Value *spawn(llvm::IRBuilder<> &ir, const Stage &stage);
  [[nodiscard]] llvm::Value *join(llvm::IRBuilder<> &ir, const Stage &stage,
                                  bool automatic);
  [[nodiscard]] llvm::Value *take(llvm::IRBuilder<> &ir, const Stage &stage);
  [[nodiscard]] llvm::Value *scope(llvm::IRBuilder<> &ir, const Stage &stage);
  void workerFailure(llvm::IRBuilder<> &ir, const Stage &stage,
                     llvm::Value *join);
};

} // namespace lang::native_llvm
