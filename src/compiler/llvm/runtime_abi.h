#pragma once

#include "abi.h"

namespace lang::native_llvm {

enum class RuntimeService {
  BeginThreadLocals,
  DrainThreadLocals,
  TerminateFailure,
  TerminateCleanupFailure,
  AbortAllocation,
  ValidateFailure,
  CapturePanic,
};

// Only real C symbols with explicitly modeled signatures enter the module.
// C++ inline/template services must acquire their own status/callback ABI;
// their names must never be guessed or declared with the private GTI ABI.
[[nodiscard]] llvm::Function *declareRuntimeService(Session &session,
                                                    RuntimeService service);

} // namespace lang::native_llvm
