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
  explicit Dispatch(Session &session);
  [[nodiscard]] const std::vector<DispatchViewPlan> &
  views(HirClassInstanceId activeClass);
  [[nodiscard]] VirtualRootAbi rootAbi(HirFunctionInstanceId root);
  [[nodiscard]] const std::vector<HirFunctionInstanceId> &
  slotProfile(HirClassInstanceId view);
  // Entry receivers are the physical dispatch-pointer owner, not the logical
  // source view. These helpers are not wired to production admission yet.
  [[nodiscard]] llvm::Function *adjustment(HirClassInstanceId activeClass,
                                           HirClassInstanceId sourceView,
                                           HirFunctionInstanceId root);
  [[nodiscard]] llvm::GlobalVariable *table(HirClassInstanceId activeClass,
                                            HirClassInstanceId sourceView);
  void install(llvm::IRBuilder<> &ir, llvm::Value *completeObject,
               HirClassInstanceId activeClass);
  [[nodiscard]] VirtualCallPlan planCall(const MirInstruction &instruction);
  [[nodiscard]] LoadedVirtualCall loadCall(llvm::IRBuilder<> &ir,
                                           const MirInstruction &instruction,
                                           const VirtualCallPlan &plan,
                                           llvm::Value *evaluatedReceiver);
  [[nodiscard]] llvm::Value *directReceiver(llvm::IRBuilder<> &ir,
                                            const MirInstruction &instruction,
                                            llvm::Value *evaluatedReceiver);

private:
  Session &session;
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
