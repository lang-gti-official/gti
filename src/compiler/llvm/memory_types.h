#pragma once

#include "gti/mir.h"

namespace lang::native_llvm {

// Exact sealed capability/operation predicates shared by layout, admission and
// emission. A represented storage type does not itself begin an object
// lifetime.
[[nodiscard]] bool
uninitializedStorageRepresentation(const MirProgram &program,
                                   const MirClassInstance &instance);
[[nodiscard]] bool
nativeObjectConstructionShape(const MirProgram &program,
                              const MirInstruction &instruction);
[[nodiscard]] bool
nativeProcessMemoryBinding(const MirProgram &program,
                           const MirFunctionInstance &function);

} // namespace lang::native_llvm
