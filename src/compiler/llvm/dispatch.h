#pragma once

#include "abi.h"

#include <llvm/IR/IRBuilder.h>

#include <tuple>

namespace lang::native_llvm {

struct DispatchSlotPlan {
  HirFunctionInstanceId root = 0;
  HirFunctionInstanceId target = 0;
  HirClassInstanceId targetOwner = 0;
  std::int64_t receiverAdjustment = 0;
  bool pure = false;
  friend bool operator==(const DispatchSlotPlan &,
                         const DispatchSlotPlan &) = default;
};

struct DispatchViewPlan {
  HirClassInstanceId activeClass = 0;
  HirClassInstanceId viewClass = 0;
  std::vector<HirClassInstanceId> path;
  std::uint64_t offset = 0;
  DispatchLocation location;
  HirClassInstanceId profileOwner = 0;
  std::vector<DispatchSlotPlan> slots;
};

struct VirtualCallPlan {
  HirFunctionInstanceId root = 0;
  HirClassInstanceId sourceClass = 0;
  HirClassInstanceId rootOwner = 0;
  DispatchLocation location;
  std::size_t slot = 0;
  VirtualRootAbi abi;
  friend bool operator==(const VirtualCallPlan &,
                         const VirtualCallPlan &) = default;
};

struct LoadedVirtualCall {
  llvm::Value *callee = nullptr;
  llvm::Value *receiver = nullptr;
};

// Native representation of already-selected virtual targets. This planner
// never resolves names, signatures or final overrides. All state is scoped
// to the immutable input and LLVM context of one emission session.
class Dispatch final {
public:
  Dispatch(Session &session, const LlvmAdmissionPlan &admission);
  Dispatch(Session &, const LlvmAdmissionPlan &&) = delete;
  [[nodiscard]] bool boundTo(const Session &selected) const {
    return &session == &selected;
  }
  [[nodiscard]] const std::vector<DispatchViewPlan> &
  views(HirClassInstanceId activeClass);
  [[nodiscard]] VirtualRootAbi rootAbi(HirFunctionInstanceId root);
  [[nodiscard]] const std::vector<HirFunctionInstanceId> &
  slotProfile(HirClassInstanceId view);
  // Validate the entire active state without declaring functions, demanding
  // source definitions, or publishing adapters/tables into the module.
  void preflightState(HirClassInstanceId activeClass);
  // Entry receivers are the physical dispatch-pointer owner, not the logical
  // source view. Public native polymorphism remains independently gated.
  [[nodiscard]] llvm::Function *adjustment(HirClassInstanceId activeClass,
                                           HirClassInstanceId sourceView,
                                           HirFunctionInstanceId root);
  [[nodiscard]] llvm::GlobalVariable *table(HirClassInstanceId activeClass,
                                            HirClassInstanceId sourceView);
  void install(llvm::IRBuilder<> &ir, llvm::Value *completeObject,
               HirClassInstanceId activeClass);
  // Recover the installed active state, which is not necessarily the complete
  // allocation during construction or destruction. Requires a live view.
  [[nodiscard]] llvm::Value *activeObject(llvm::IRBuilder<> &ir,
                                          llvm::Value *view,
                                          const SemanticType &viewType);
  [[nodiscard]] VirtualCallPlan planCall(const MirInstruction &instruction);
  [[nodiscard]] LoadedVirtualCall loadCall(llvm::IRBuilder<> &ir,
                                           const MirInstruction &instruction,
                                           const VirtualCallPlan &plan,
                                           llvm::Value *evaluatedReceiver);
  [[nodiscard]] llvm::Value *directReceiver(llvm::IRBuilder<> &ir,
                                            const MirInstruction &instruction,
                                            llvm::Value *evaluatedReceiver);

private:
  // Only the owning structural test defines this access shim. Production
  // cannot construct an unbound publisher or substitute a caller-made set.
  friend struct DispatchStructuralTestAccess;
  explicit Dispatch(Session &session);
  void preflightSourceBodies(HirClassInstanceId activeClass) const;
  [[nodiscard]] llvm::StructType *tableType(std::size_t slots) const;
  struct AdjustmentPlan {
    DispatchSlotPlan selected;
    HirClassInstanceId physicalOwner = 0;
    VirtualAdjustmentAbi abi;
    TypeLayout result;
    std::vector<unsigned> channels;
    bool directResult = false;
  };
  [[nodiscard]] AdjustmentPlan planAdjustment(HirClassInstanceId activeClass,
                                              HirClassInstanceId sourceView,
                                              HirFunctionInstanceId root);
  Session &session;
  const LlvmAdmissionPlan *admission = nullptr;
  std::map<HirClassInstanceId, std::vector<DispatchViewPlan>> views_;
  std::map<HirFunctionInstanceId, std::vector<HirFunctionInstanceId>> targets_;
  std::map<HirClassInstanceId, std::vector<HirFunctionInstanceId>> profiles_;
  using AdjustmentKey =
      std::tuple<HirClassInstanceId, HirClassInstanceId, HirClassInstanceId,
                 HirFunctionInstanceId, HirFunctionInstanceId, std::int64_t>;
  using TableKey =
      std::tuple<HirClassInstanceId, std::vector<unsigned>, unsigned>;
  std::map<AdjustmentKey, llvm::Function *> adjustments_;
  std::map<TableKey, llvm::GlobalVariable *> tables_;
};

} // namespace lang::native_llvm
