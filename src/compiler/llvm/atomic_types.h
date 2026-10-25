#pragma once

#include "gti/mir.h"

namespace lang::native_llvm {

// Representation predicates over one verified snapshot, not public names or
// permission/escape rules. All executable consumers still require admission.
[[nodiscard]] bool nativeAtomicScalar(const SemanticType &type);
[[nodiscard]] std::optional<SemanticType>
nativeAtomicElement(const MirProgram &program, const SemanticType &storage);
[[nodiscard]] bool nativeAtomicCall(const MirProgram &program,
                                    const MirBody &body,
                                    const MirInstruction &instruction);
[[nodiscard]] std::optional<SemanticType>
nativeAtomicConstructor(const MirProgram &program,
                        const MirConstructorInstance &constructor);

} // namespace lang::native_llvm
