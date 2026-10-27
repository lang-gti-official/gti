#pragma once

#include "gti/mir.h"

namespace lang::native_llvm {

[[nodiscard]] bool nativeCThreadLifecycleBinding(const MirProgram &,
                                                 const MirFunctionInstance &);
[[nodiscard]] bool nativeCThreadLifecycleCall(const MirProgram &,
                                              const MirBody &,
                                              const MirInstruction &);
[[nodiscard]] bool
nativeCThreadLifecycleOperation(const MirProgram &,
                                const MirHostedStartupOperation &);
// Exact trusted manual C-thread services beyond startup lifecycle: create,
// join, once invocation, and manual mutex/condition operations. These are the
// versioned `cthread.h` C declarations selected by semantic declaration
// identity; they execute through the ordinary native C call ABI.
[[nodiscard]] bool nativeCThreadOperationBinding(const MirProgram &,
                                                 const MirFunctionInstance &);
// The synchronization record semantics attaches to a call of the operation.
[[nodiscard]] SynchronizationOperationKind
cThreadOperationSynchronization(CThreadRuntimeOperation operation);

} // namespace lang::native_llvm
