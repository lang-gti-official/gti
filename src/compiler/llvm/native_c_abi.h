#pragma once

#include "layout.h"

namespace lang::native_llvm {

// Remaining System V registers, after preceding arguments and any hidden sret.
// Darwin allocation is left to LLVM's AArch64 calling-convention lowering.
struct NativeCRegisterBudget {
  unsigned integers = 6;
  unsigned vectors = 8;
};

struct NativeCAbiPiece {
  llvm::Type *type = nullptr;
  std::uint64_t offset = 0;
};

struct NativeCRecordTransport {
  // One result/coercion type. Expanded System V arguments use pieces instead.
  llvm::Type *coercion = nullptr;
  std::vector<NativeCAbiPiece> pieces;
  bool expand = false;
  // A pointer with byval(storage) and stack alignment, NOT an invisible ref.
  bool byValue = false;
  std::uint64_t stackAlignment = 1;
};

struct NativeCRecordAbi {
  TypeLayout storage;
  NativeCRecordTransport argument;
  NativeCRecordTransport result;
  NativeCRegisterBudget remaining;
};

// Private groundwork only; Abi::nativeC and aggregate admission remain closed.
// Accepts flat, natural LP64 C records, 1..16 bytes, with fixed-width integers,
// raw pointers, float or double fields. No bool/enums/arrays/nesting/unions,
// ownership, packing, over-alignment, variadic or non-C calling conventions.
// Plans and LLVM types belong to this Session. Callers must account for scalar
// arguments in the register budget; storage layout alone never grants
// admission.
[[nodiscard]] NativeCRecordAbi
classifyNativeCRecord(Session &session, const SemanticType &type,
                      NativeCRegisterBudget available);

} // namespace lang::native_llvm
