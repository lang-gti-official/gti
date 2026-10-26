#pragma once

#include "session.h"
#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

// Debug-only consumer of sealed source identities, MIR places and Layouts.
// It never adds execution attributes, changes storage, or consults source ASTs.
class DebugInfo final {
public:
  explicit DebugInfo(Session &session);
  ~DebugInfo();
  void begin(MirBodyAddress address, llvm::Function &function);
  void location(llvm::IRBuilder<> &ir, MirBodyAddress address, MirBlockId block,
                MirInstructionId instruction);
  void location(llvm::IRBuilder<> &ir, MirBlockId block,
                MirInstructionId instruction);
  void local(llvm::IRBuilder<> &ir, const MirPlace &place, llvm::Value *storage,
             unsigned parameter = 0);
  void endLocal(llvm::IRBuilder<> &ir, const MirPlace &place);
  void finalize();

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang::native_llvm
