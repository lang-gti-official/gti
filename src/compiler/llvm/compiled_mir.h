#pragma once
#include "session.h"

namespace lang::native_llvm {
// Make verified imported bodies visible at direct calls before LLVM optimizes
// the consumer. Address-taken functions retain their canonical native symbol.
void emitCompiledMir(Session &session);
} // namespace lang::native_llvm
