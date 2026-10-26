#pragma once

#include "dispatch.h"
#include "failure.h"
#include "placement.h"

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
  [[nodiscard]] bool boundTo(const Session &selected) const {
    return &session == &selected;
  }
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
  [[nodiscard]] static FreshHierarchyTransfer
  preflightFreshTransfer(Session &session, Dispatch &dispatch,
                         const MirBody &body,
                         const MirInstruction &instruction);
  void freshTransfer(llvm::IRBuilder<> &ir, llvm::Value *destination,
                     llvm::Value *source, const MirBody &body,
                     const MirInstruction &instruction);
  // Representation mechanism for an already-authorized generated relocation
  // into fresh storage. This is not source move/copy admission. Inert records
  // and arrays rebuild destination dispatch state without source callbacks;
  // payload enums relocate only their active variant and retire the source tag.
  static void preflightInertRelocation(Session &session, Dispatch &dispatch,
                                       const SemanticType &type);
  [[nodiscard]] llvm::FunctionType *inertRelocationType() const;
  [[nodiscard]] llvm::Function *inertRelocation(const SemanticType &type);
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
  std::vector<std::pair<SemanticType, llvm::Function *>> inertRelocations;
  std::vector<std::pair<MirDropType, llvm::Function *>> observations;
  [[nodiscard]] llvm::Function *cleanupObservation(const MirDropType &recipe);
  [[nodiscard]] bool needsMoveState(const SemanticType &type) const;
  [[nodiscard]] llvm::Function *moveHelper(const SemanticType &type);
};

} // namespace lang::native_llvm
