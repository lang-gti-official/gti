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
  // The exact generated copy of a scalar/record hierarchy into fresh storage:
  // logical fields only, then the destination's own dispatch state. Callers
  // must already own MIR authority for the copy and its fresh destination.
  void generatedCopy(llvm::IRBuilder<> &ir, llvm::Value *destination,
                     llvm::Value *source, const SemanticType &type);
  // Representation mechanism for an already-authorized generated relocation
  // into fresh storage. This is not source move/copy admission. Inert records
  // and arrays rebuild destination dispatch state without source callbacks;
  // payload enums relocate only their active variant and retire the source tag.
  static void preflightInertRelocation(Session &session, Dispatch &dispatch,
                                       const SemanticType &type);
  static void preflightGeneratedRelocation(Session &session, Dispatch &dispatch,
                                           const SemanticType &type);
  [[nodiscard]] llvm::FunctionType *relocationType() const;
  [[nodiscard]] llvm::Function *inertRelocation(const SemanticType &type);
  // Transfer cleanup permission and owner handles without running their
  // cleanup. Callers must already possess MIR authority for relocation to fresh
  // storage.
  [[nodiscard]] llvm::Function *generatedRelocation(const SemanticType &type);
  void active(llvm::IRBuilder<> &ir, llvm::Value *address,
              const SemanticType &type, bool value);
  void moved(llvm::IRBuilder<> &ir, llvm::Value *address,
             const SemanticType &type);
  // Native identity cannot be transported by an aggregate byte copy.
  [[nodiscard]] bool transferMutexes(llvm::IRBuilder<> &ir,
                                     llvm::Value *destination,
                                     llvm::Value *source,
                                     const SemanticType &type);
  void prepareProgramStorage(
      const std::map<SymbolId, llvm::GlobalVariable *> &globals);
  void publishProgramStorage(llvm::IRBuilder<> &ir,
                             MirInstructionId instruction);
  void retireProgramStorage(llvm::IRBuilder<> &ir);
  void generatedEmpty(llvm::IRBuilder<> &ir, llvm::Value *address,
                      const MirBody &body, const MirInstruction &instruction,
                      const MirConstructorInstance *constructor = nullptr);
  [[nodiscard]] llvm::Function *cleanup(const MirDropType &recipe,
                                        bool replacement = false);
  // The complete-object cleanup a dynamically owned active class publishes
  // in its dispatch descriptors. It never releases the allocation itself.
  [[nodiscard]] llvm::Function *completeCleanup(HirClassInstanceId activeClass);
  // The exact complete recipe of a concrete polymorphic active class, or none
  // when its complete object has no observable cleanup.
  [[nodiscard]] static std::optional<MirDropType>
  completeCleanupRecipe(Session &session, HirClassInstanceId activeClass);

private:
  Session &session;
  Failures &failures;
  Dispatch &dispatch;
  FindBody findBody;
  std::vector<std::pair<MirDropType, llvm::Function *>> cleanups;
  llvm::Function *inertCleanup = nullptr;
  std::map<llvm::Type *, llvm::Function *> moves;
  std::map<llvm::Type *, llvm::Function *> mutexTransfers;
  struct ProgramStorage {
    MirInstructionId initialization = 0;
    llvm::GlobalVariable *storage = nullptr;
    llvm::GlobalVariable *live = nullptr;
    MirDropType recipe;
  };
  std::vector<ProgramStorage> programStorage;
  llvm::Function *programRetirement = nullptr;
  [[nodiscard]] bool containsMutex(const SemanticType &type) const;
  [[nodiscard]] llvm::Function *mutexTransfer(const SemanticType &type);
  std::vector<std::pair<SemanticType, llvm::Function *>> relocations;
  std::vector<std::pair<MirDropType, llvm::Function *>> observations;
  [[nodiscard]] llvm::Function *cleanupObservation(const MirDropType &recipe);
  [[nodiscard]] bool needsMoveState(const SemanticType &type) const;
  [[nodiscard]] llvm::Function *moveHelper(const SemanticType &type);
};

} // namespace lang::native_llvm
