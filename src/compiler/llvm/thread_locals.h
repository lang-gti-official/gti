#pragma once

#include "failure.h"
#include "lifetimes.h"
#include "scalar.h"

namespace lang::native_llvm {

// One target-native slot/state/registration per exact sealed TLS identity.
// No host thread-local constructors or representation destructors are used.
class ThreadLocals final {
public:
  using FindBody = std::function<llvm::Function *(MirBodyAddress)>;
  using BodyAbi = std::function<FunctionAbi(MirBodyAddress)>;
  ThreadLocals(Session &, Failures &, Lifetimes &, FindBody, BodyAbi);
  void allocate(std::map<SymbolId, llvm::GlobalVariable *> &globals);
  void ensure(llvm::IRBuilder<> &, SymbolId, llvm::Value *failure,
              Scalars::Guard, std::function<void()> propagate);

private:
  struct Slot {
    llvm::GlobalVariable *storage = nullptr;
    llvm::GlobalVariable *state = nullptr;
    llvm::GlobalVariable *registration = nullptr;
    llvm::Function *cleanup = nullptr;
  };
  Session &session;
  Failures &failures;
  Lifetimes &lifetimes;
  FindBody findBody;
  BodyAbi bodyAbi;
  std::map<SymbolId, Slot> slots;
  llvm::StructType *registrationType;
  llvm::Function *cleanup(const MirThreadLocalStorage &, Slot &);
};

} // namespace lang::native_llvm
