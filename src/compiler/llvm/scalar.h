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
  [[nodiscard]] llvm::Value *mutate(const MirInstruction &instruction,
                                    const SemanticType &target,
                                    std::span<llvm::Value *const> inputs);
  [[nodiscard]] SemanticType domain(const SemanticType &type) const;
  [[nodiscard]] SemanticType common(const SemanticType &left,
                                    const SemanticType &right) const;

private:
  Session &session;
  llvm::IRBuilder<> &ir;
  Guard guard;
  [[nodiscard]] llvm::Constant *constantStorage(
      const TypeLayout &layout,
      std::span<const std::pair<std::uint64_t, llvm::Constant *>> fragments);
  [[nodiscard]] bool signedType(const SemanticType &type) const;
  [[nodiscard]] SemanticType promoted(const SemanticType &type) const;
  [[nodiscard]] llvm::Value *arithmetic(MirOperation operation,
                                        std::span<llvm::Value *const> inputs,
                                        std::span<const SemanticType> types,
                                        const SemanticType &result);
};

// The canonical LLVM constant for a sign-magnitude GTI integer at the type's
// width. ConstantInt::get requires its uint64_t payload to fit the type unless
// truncation is explicit, and release LLVM builds do not check that contract:
// a negative value's 64-bit two's complement keeps its high bits, prints as
// the intended value, and miscompiles once optimizers compare it.
[[nodiscard]] llvm::ConstantInt *integerConstant(llvm::IntegerType *type,
                                                 bool negative,
                                                 std::uint64_t magnitude);

} // namespace lang::native_llvm
