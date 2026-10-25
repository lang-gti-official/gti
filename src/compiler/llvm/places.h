#pragma once

#include "scalar.h"

#include <map>
#include <tuple>

namespace lang::native_llvm {

// Physical addresses are separate from the owner places protected by loans.
// All queries execute at the MIR stage that selects/uses the address; dynamic
// projections must never be hoisted to entry or cached across mutations.
class Places final {
public:
  Places(Session &session, llvm::IRBuilder<> &ir, const MirBody &body,
         std::vector<llvm::Value *> &values, Scalars::Guard guard);
  void allocate(const std::map<SymbolId, llvm::GlobalVariable *> &globals);
  [[nodiscard]] llvm::Value *address(MirPlaceId id);
  [[nodiscard]] llvm::Value *loan(MirLoanId id);
  void publish(MirLoanId id, llvm::Value *address);
  void parameter(HirBindingId binding, llvm::Value *argument, bool indirect);
  [[nodiscard]] llvm::Value *load(llvm::Value *address,
                                  const SemanticType &type);
  void store(llvm::Value *value, llvm::Value *address,
             const SemanticType &type);
  [[nodiscard]] llvm::Value *selected(const MirOperand &operand);
  [[nodiscard]] llvm::Value *index(llvm::Value *address,
                                   const SemanticType &type,
                                   llvm::Value *offset, bool checked);
  [[nodiscard]] llvm::Value *arrayValue(llvm::Value *value);

private:
  using Key = std::tuple<MirPlaceRootKind, std::size_t, std::size_t>;
  struct Root {
    llvm::Value *address;
    SemanticType type;
  };
  Session &session;
  llvm::IRBuilder<> &ir;
  const MirBody &body;
  std::vector<llvm::Value *> &values;
  Scalars::Guard guard;
  std::map<Key, Root> roots;
  std::vector<llvm::Value *> loans;
  std::vector<SemanticType> loanTypes;
  [[nodiscard]] static Key key(const MirPlace &place);
  [[nodiscard]] Root root(const MirPlace &place);
  [[nodiscard]] llvm::Value *slot(llvm::Type *type);
};

} // namespace lang::native_llvm
