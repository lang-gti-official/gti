#pragma once

#include "parameter_packs.h"
#include "scalar.h"

#include <map>
#include <optional>
#include <span>
#include <tuple>

namespace lang::native_llvm {

// Physical addresses are separate from the owner places protected by loans.
// All queries execute at the MIR stage that selects/uses the address; dynamic
// projections must never be hoisted to entry or cached across mutations.
class Places final {
public:
  Places(Session &session, llvm::IRBuilder<> &ir, const MirBody &body,
         std::vector<llvm::Value *> &values, Scalars::Guard guard,
         const MirLambdaInstance *lambda = nullptr,
         std::span<const ConcretePackParameter> packs = {});
  void allocate(const std::map<SymbolId, llvm::GlobalVariable *> &globals);
  [[nodiscard]] llvm::Value *address(MirPlaceId id);
  [[nodiscard]] llvm::Value *loan(MirLoanId id);
  void publish(MirLoanId id, llvm::Value *address);
  void parameter(const ParameterLane &lane, llvm::Value *argument,
                 bool indirect);
  void receiver(llvm::Value *address, const SemanticType &type);
  [[nodiscard]] llvm::Value *temporary(const SemanticType &type);
  [[nodiscard]] llvm::Value *load(llvm::Value *address,
                                  const SemanticType &type);
  void store(llvm::Value *value, llvm::Value *address,
             const SemanticType &type);
  [[nodiscard]] llvm::Value *selected(const MirOperand &operand);
  [[nodiscard]] llvm::Value *index(llvm::Value *address,
                                   const SemanticType &type,
                                   llvm::Value *offset, bool checked);
  [[nodiscard]] llvm::Value *arrayValue(llvm::Value *value,
                                        const SemanticType &type);
  [[nodiscard]] llvm::Value *advance(llvm::Value *address,
                                     const SemanticType &pointee,
                                     llvm::Value *offset, bool signedOffset,
                                     bool subtract);
  [[nodiscard]] llvm::Value *difference(llvm::Value *left, llvm::Value *right,
                                        const SemanticType &pointee);

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
  const MirLambdaInstance *lambda;
  std::map<Key, Root> roots;
  std::map<HirBindingId, SemanticType> packParameters;
  std::map<std::pair<HirBindingId, std::size_t>, Root> packElements;
  std::vector<llvm::Value *> loans;
  std::vector<SemanticType> loanTypes;
  llvm::Value *receiverAddress = nullptr;
  SemanticType receiverType = SemanticType::Unknown;
  [[nodiscard]] static Key key(const MirPlace &place);
  [[nodiscard]] std::optional<std::size_t>
  packElement(const MirPlace &place) const;
  [[nodiscard]] Root root(const MirPlace &place);
  [[nodiscard]] llvm::Value *slot(llvm::Type *type);
};

} // namespace lang::native_llvm
