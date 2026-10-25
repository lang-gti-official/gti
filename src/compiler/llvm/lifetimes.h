#pragma once

#include "dispatch.h"
#include "failure.h"

#include <functional>
#include <map>

namespace lang::native_llvm {

// Object state is representation-only. The sealed recipe selects every cleanup
// body/subobject and its order; LLVM aggregates never acquire implicit cleanup.
class Lifetimes final {
public:
  using FindBody = std::function<llvm::Function *(MirBodyAddress)>;
  Lifetimes(Session &session, Failures &failures, Dispatch &dispatch,
            FindBody findBody);
  // These checks consume the same exact session and sealed records as emission,
  // but publish no helper, table, IR instruction, or runtime lifetime state.
  static void preflightCleanup(Session &session, Dispatch &dispatch,
                               const MirDropType &recipe,
                               bool replacement = false);
  [[nodiscard]] static const MirClassInstance &
  preflightGeneratedEmpty(Session &session, Dispatch &dispatch,
                          const MirBody &body,
                          const MirInstruction &instruction,
                          const MirConstructorInstance *constructor = nullptr);
  void active(llvm::IRBuilder<> &ir, llvm::Value *address,
              const SemanticType &type, bool value);
  void moved(llvm::IRBuilder<> &ir, llvm::Value *address,
             const SemanticType &type);
  void generatedEmpty(llvm::IRBuilder<> &ir, llvm::Value *address,
                      const MirBody &body, const MirInstruction &instruction,
                      const MirConstructorInstance *constructor = nullptr);
  [[nodiscard]] llvm::Function *cleanup(const MirDropType &recipe,
                                        bool replacement = false);

private:
  Session &session;
  Failures &failures;
  Dispatch &dispatch;
  FindBody findBody;
  std::vector<std::pair<MirDropType, llvm::Function *>> cleanups;
  std::map<llvm::Type *, llvm::Function *> moves;
  std::vector<std::pair<MirDropType, llvm::Function *>> observations;
  [[nodiscard]] llvm::Function *cleanupObservation(const MirDropType &recipe);
  [[nodiscard]] bool needsMoveState(const SemanticType &type) const;
  [[nodiscard]] llvm::Function *moveHelper(const SemanticType &type);
};

} // namespace lang::native_llvm
