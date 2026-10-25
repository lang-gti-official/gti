#pragma once

#include "abi.h"
#include <functional>
#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

// Guards branch to the MIR-selected failure continuation before an operation
// that could produce LLVM poison/UB. The body emitter owns that continuation.
class Scalars final {
public:
  using Guard = std::function<void(llvm::Value *, DefinedFailureOutcome)>;
  Scalars(Session &session, llvm::IRBuilder<> &ir, Guard guard);
  [[nodiscard]] llvm::Value *literal(const Literal &literal,
                                     const SemanticType &type);
  [[nodiscard]] llvm::Constant *constant(const ConstantValue &value,
                                         const SemanticType &type);
  [[nodiscard]] llvm::Value *convert(llvm::Value *value,
                                     const SemanticType &from,
                                     const SemanticType &to,
                                     bool checked = true);
  [[nodiscard]] llvm::Value *compute(const MirInstruction &instruction,
                                     std::span<llvm::Value *const> inputs);
  [[nodiscard]] SemanticType domain(const SemanticType &type) const;
  [[nodiscard]] SemanticType common(const SemanticType &left,
                                    const SemanticType &right) const;

private:
  Session &session;
  llvm::IRBuilder<> &ir;
  Guard guard;
  [[nodiscard]] bool signedType(const SemanticType &type) const;
  [[nodiscard]] SemanticType promoted(const SemanticType &type) const;
  [[nodiscard]] llvm::Value *arithmetic(MirOperation operation,
                                        std::span<llvm::Value *const> inputs,
                                        std::span<const SemanticType> types,
                                        const SemanticType &result);
};

} // namespace lang::native_llvm
