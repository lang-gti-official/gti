#pragma once

#include "abi.h"

#include <llvm/IR/IRBuilder.h>

#include <map>

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
  bool indirect = false;
  std::uint64_t stackAlignment = 1;
};

struct NativeCRecordAbi {
  TypeLayout storage;
  NativeCRecordTransport argument;
  NativeCRecordTransport result;
  NativeCRegisterBudget remaining;
};

// Natural passive C records, including sealed nested records/unions and fixed
// arrays. Storage, arguments and results remain distinct. Plans belong to the
// Session; this query alone never grants executable admission.
[[nodiscard]] NativeCRecordAbi
classifyNativeCRecord(Session &session, const SemanticType &type,
                      NativeCRegisterBudget available);

struct NativeCValueAbi {
  SemanticType type = SemanticType::Unknown;
  TypeLayout storage;
  NativeCRecordTransport transport;
  bool aggregate = false;
  unsigned firstParameter = 0;
};

struct NativeCFunctionAbi {
  FunctionAbi signature;
  NativeCValueAbi result;
  std::vector<NativeCValueAbi> arguments;
};

[[nodiscard]] NativeCFunctionAbi
planNativeCFunction(Session &session, const SemanticType &result,
                    std::span<const SemanticType> parameters);

// Arguments are already staged MIR body values, not reconstructed source
// expressions. Aggregate results are written to their final destination.
[[nodiscard]] llvm::Value *
emitNativeCCall(Session &session, llvm::IRBuilder<> &ir,
                const NativeCFunctionAbi &plan, llvm::Value *callee,
                std::span<llvm::Value *const> arguments,
                llvm::Value *resultDestination = nullptr);

void declareNativeCStorage(Session &session,
                           std::map<SymbolId, llvm::GlobalVariable *> &globals);

} // namespace lang::native_llvm
