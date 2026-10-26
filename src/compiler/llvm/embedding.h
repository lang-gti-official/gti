#pragma once

#include "session.h"

namespace lang::native_llvm {
class Failures;
[[nodiscard]] bool infallibleEmbeddingContract(const LoweredProgram &program);
void emitEmbedding(Session &session, Failures &failures,
                   llvm::Function &initialize);
} // namespace lang::native_llvm
