#pragma once

#include "lifetimes.h"

namespace lang::native_llvm {

// Private inline storage mechanisms. Source policy chooses inline versus boxed
// ownership; verified MIR owns payload eligibility, loans and publication.
class PolymorphicStorage final {
public:
  PolymorphicStorage(Session &session, Lifetimes &lifetimes,
                     Dispatch &dispatch);
  [[nodiscard]] static bool handles(IntrinsicKind intrinsic);
  void preflight(const MirBody &body, const MirInstruction &instruction);
  [[nodiscard]] llvm::Value *emit(llvm::IRBuilder<> &ir, const MirBody &body,
                                  const MirInstruction &instruction,
                                  std::span<llvm::Value *const> inputs);
  // Callers must already own a fresh destination and the source move. These
  // representation helpers do not authorize a GTI construction or transfer.
  void initialize(llvm::IRBuilder<> &ir, const SemanticType &type,
                  llvm::Value *destination);
  void relocate(llvm::IRBuilder<> &ir, const SemanticType &type,
                llvm::Value *destination, llvm::Value *source);

private:
  struct Descriptor {
    SemanticType model;
    SemanticType interface;
    llvm::GlobalVariable *value;
  };
  Session &session;
  Lifetimes &lifetimes;
  Dispatch &dispatch;
  std::vector<Descriptor> descriptors;
  [[nodiscard]] llvm::StructType *descriptorType();
  [[nodiscard]] llvm::GlobalVariable *descriptor(const SemanticType &slot,
                                                 const SemanticType &model);
  [[nodiscard]] llvm::Value *hasValue(llvm::IRBuilder<> &ir,
                                      const PolymorphicStorageLayout &layout,
                                      llvm::Value *address);
  void guard(llvm::IRBuilder<> &ir, llvm::Value *condition);
};

} // namespace lang::native_llvm
