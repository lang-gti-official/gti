#pragma once

#include "failure.h"

#include <functional>
#include <map>

namespace lang::native_llvm {

// Object state is representation-only. The sealed recipe selects every cleanup
// body/subobject and its order; LLVM aggregates never acquire implicit cleanup.
class Lifetimes final {
public:
  using FindBody = std::function<llvm::Function *(MirBodyAddress)>;
  Lifetimes(Session &session, Failures &failures, FindBody findBody);
  void active(llvm::IRBuilder<> &ir, llvm::Value *address,
              const SemanticType &type, bool value);
  void moved(llvm::IRBuilder<> &ir, llvm::Value *address,
             const SemanticType &type);
  // Native identity cannot be transported by an aggregate byte copy.
  [[nodiscard]] bool transferMutexes(llvm::IRBuilder<> &ir,
                                     llvm::Value *destination,
                                     llvm::Value *source,
                                     const SemanticType &type);
  [[nodiscard]] llvm::Function *cleanup(const MirDropType &recipe);

private:
  Session &session;
  Failures &failures;
  FindBody findBody;
  std::vector<std::pair<MirDropType, llvm::Function *>> cleanups;
  std::vector<std::pair<MirDropType, llvm::Function *>> buildingCleanups;
  std::map<llvm::Type *, llvm::Function *> moves;
  std::map<llvm::Type *, llvm::Function *> mutexTransfers;
  [[nodiscard]] bool containsMutex(const SemanticType &type) const;
  [[nodiscard]] llvm::Function *mutexTransfer(const SemanticType &type);
  [[nodiscard]] bool needsMoveState(const SemanticType &type) const;
  [[nodiscard]] llvm::Function *moveHelper(const SemanticType &type);
};

} // namespace lang::native_llvm
