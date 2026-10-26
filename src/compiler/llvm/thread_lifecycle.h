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

} // namespace lang::native_llvm
