#include "hosted.h"

#include <stdexcept>

namespace lang::native_llvm {

llvm::Value *hostedReturnStatus(llvm::IRBuilder<> &ir,
                                const MirHostedStartupOperation &operation,
                                llvm::Value *result) {
  if (operation.kind != MirHostedStartupOperationKind::ReturnEntry ||
      !operation.terminator ||
      operation.failureBehavior != MirHostedStartupFailureBehavior::None ||
      !result || !result->getType()->isIntegerTy(32))
    throw std::logic_error(
        "LLVM hosted return requires an exact Int32 ReturnEntry operation");
  // Unsigned comparison also rejects negative GTI results, without letting
  // host exit-status truncation turn a nonzero result into success.
  auto *portable = ir.CreateICmpULE(result, ir.getInt32(255));
  return ir.CreateSelect(portable, result, ir.getInt32(1));
}

} // namespace lang::native_llvm
