#pragma once

#include "session.h"
#include <llvm/IR/IRBuilder.h>
#include <span>

namespace lang::native_llvm {

// Debug-only consumer of sealed source identities, MIR places and Layouts.
// It never adds execution attributes, changes storage, or consults source ASTs.
class DebugInfo final {
public:
  explicit DebugInfo(Session &session);
  ~DebugInfo();
  // Only the existing Body function factory registers exact lifecycle bodies.
  // This records its result; it does not plan a signature or require a body.
  void registerLifecycleBody(MirBodyAddress address, llvm::Function &function);
  void registerLambdaBody(MirBodyAddress address, llvm::Function &function);
  // The startup factory supplies a generated identity, never a user scope.
  void registerHostedStartupBody(MirBodyAddress address,
                                 llvm::Function &function);
  void begin(MirBodyAddress address, llvm::Function &function);
  void location(llvm::IRBuilder<> &ir, MirBodyAddress address, MirBlockId block,
                MirInstructionId instruction);
  void location(llvm::IRBuilder<> &ir, MirBlockId block,
                MirInstructionId instruction);
  void local(llvm::IRBuilder<> &ir, const MirPlace &place, llvm::Value *storage,
             unsigned parameter = 0);
  // Binds the factory's existing physical argument to its exact source pack.
  void parameter(llvm::IRBuilder<> &ir, llvm::Argument &argument);
  // Consumes exact sealed half-open ranges at MIR emission boundaries.
  void point(llvm::IRBuilder<> &ir, MirDebugPoint point,
             std::span<llvm::Value *const> values = {});
  void endLocal(llvm::IRBuilder<> &ir, const MirPlace &place);
  void finalize();
  void prepareCodegen();

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang::native_llvm
