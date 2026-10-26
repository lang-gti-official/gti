#pragma once

#include "layout.h"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>

namespace lang::native_llvm {

enum class AbiChannel {
  ResultDestination,
  FailureDestination,
  Receiver,
  CallerSite,
  CallerArtifact,
  Argument,
};
enum class AbiExtension { None, Signed, Unsigned };
struct AbiParameter {
  AbiChannel channel = AbiChannel::Argument;
  std::size_t sourceIndex = 0;
  bool indirect = false;
  AbiExtension extension = AbiExtension::None;
};
struct NativeCMemoryParameter {
  unsigned index = 0;
  llvm::Type *storage = nullptr;
  std::uint64_t alignment = 1;
  bool result = false;
  bool byValue = false;
};
struct FunctionAbi {
  llvm::FunctionType *type = nullptr;
  std::vector<AbiParameter> parameters;
  AbiExtension resultExtension = AbiExtension::None;
  bool statusResult = false;
  std::optional<unsigned> resultDestination;
  std::optional<unsigned> failureDestination;
  // Empty for every private GTI signature; these are native C attributes only.
  std::vector<NativeCMemoryParameter> nativeMemory;
  void applyTo(llvm::Function &function) const;
  void applyTo(llvm::CallBase &call) const;
};

// One planner is used for definitions, calls and adapters. No foreign aggregate
// classification is inferred from a record's field offsets.
class Abi final {
public:
  [[nodiscard]] FunctionAbi gti(const SemanticType &result,
                                std::span<const SemanticType> parameters,
                                bool mayFail, bool receiver = false,
                                bool tracksCaller = false);
  [[nodiscard]] FunctionAbi nativeC(const SemanticType &result,
                                    std::span<const SemanticType> parameters);
  [[nodiscard]] llvm::StructType *failureRecord() const {
    return failureRecord_;
  }
  [[nodiscard]] llvm::StructType *failureChannel() const {
    return failureChannel_;
  }
  [[nodiscard]] llvm::StructType *textView() const { return textView_; }
  [[nodiscard]] llvm::StructType *failureArtifact() const {
    return failureArtifact_;
  }
  [[nodiscard]] llvm::StructType *failureSite() const { return failureSite_; }
  [[nodiscard]] llvm::StructType *failureOutcome() const {
    return failureOutcome_;
  }
  [[nodiscard]] llvm::StructType *failureEmergency() const {
    return failureEmergency_;
  }
  [[nodiscard]] llvm::Function *declareFunction(HirFunctionInstanceId id);
  void declareFunctions(std::span<const HirFunctionInstanceId> ids);

private:
  friend class Session;
  Abi(Session &session, Layouts &layouts);
  Abi(const Abi &) = delete;
  Abi &operator=(const Abi &) = delete;
  Session &session;
  Layouts &layouts;
  llvm::StructType *textView_;
  llvm::StructType *failureRecord_;
  llvm::StructType *failureArtifact_;
  llvm::StructType *failureChannel_;
  llvm::StructType *failureSite_;
  llvm::StructType *failureOutcome_;
  llvm::StructType *failureEmergency_;
};

} // namespace lang::native_llvm
