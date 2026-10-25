#pragma once

#include "layout.h"

#include <llvm/IR/IRBuilder.h>

namespace lang::native_llvm {

[[nodiscard]] inline llvm::Value *payloadTagAddress(llvm::IRBuilder<> &ir,
                                                    const PayloadLayout &layout,
                                                    llvm::Value *address) {
  return ir.CreateStructGEP(layout.storage, address, layout.tagIndex);
}

[[nodiscard]] inline llvm::Value *
payloadFieldAddress(llvm::IRBuilder<> &ir, const PayloadLayout &layout,
                    llvm::Value *address, std::size_t variant,
                    std::size_t field) {
  auto *storage =
      ir.CreateStructGEP(layout.storage, address, layout.payloadIndex);
  return ir.CreateStructGEP(layout.variants.at(variant), storage,
                            static_cast<unsigned>(field));
}

} // namespace lang::native_llvm
