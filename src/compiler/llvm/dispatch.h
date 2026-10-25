#pragma once

#include "abi.h"

namespace lang::native_llvm {

struct DispatchSlotPlan {
  HirFunctionInstanceId root = 0;
  HirFunctionInstanceId target = 0;
  HirClassInstanceId targetOwner = 0;
  std::int64_t receiverAdjustment = 0;
  bool pure = false;
};

struct DispatchViewPlan {
  HirClassInstanceId activeClass = 0;
  HirClassInstanceId viewClass = 0;
  std::vector<HirClassInstanceId> path;
  std::uint64_t offset = 0;
  unsigned dispatchIndex = 0;
  std::vector<DispatchSlotPlan> slots;
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

private:
  Session &session;
  std::map<HirClassInstanceId, std::vector<DispatchViewPlan>> views_;
  std::map<HirFunctionInstanceId, std::vector<HirFunctionInstanceId>> targets_;
};

} // namespace lang::native_llvm
