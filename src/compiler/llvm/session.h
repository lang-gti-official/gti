#pragma once

#include "gti/llvm_admission.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>
#include <string>
#include <vector>

namespace lang::native_llvm {

class Layouts;
class Abi;

// Private, invalidatable whole-program ABI, not an external GTI ABI promise.
inline constexpr unsigned executionAbiVersion = 2;

class Session final {
public:
  explicit Session(VerifiedLoweredProgram program, LlvmBackendOptions options);
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  ~Session();

  [[nodiscard]] const LoweredProgram &program() const;
  [[nodiscard]] llvm::LLVMContext &context() { return context_; }
  [[nodiscard]] llvm::Module &module() { return module_; }
  [[nodiscard]] Layouts &layouts() { return *layouts_; }
  [[nodiscard]] Abi &abi() { return *abi_; }
  [[nodiscard]] const llvm::DataLayout &dataLayout() const;
  [[nodiscard]] LlvmBackendOptions options() const { return options_; }
  [[nodiscard]] const std::vector<std::string> &diagnostics() const {
    return diagnostics_;
  }

  void verify() const;
  void requireDefinition(llvm::Function &function);
  // Rejects unresolved GTI definitions, then verifies before and after the
  // selected LLVM pipeline. Never returns partial object bytes on failure.
  [[nodiscard]] BackendArtifact emitObject();

private:
  VerifiedLoweredProgram input_;
  LlvmBackendOptions options_;
  std::vector<std::string> diagnostics_;
  std::vector<llvm::Function *> requiredDefinitions_;
  bool diagnosticError_ = false;
  bool emissionStarted_ = false;
  llvm::LLVMContext context_;
  std::unique_ptr<llvm::TargetMachine> machine_;
  llvm::Module module_;
  std::unique_ptr<Layouts> layouts_;
  std::unique_ptr<Abi> abi_;
};

// Kept separate so target-contract mutations can be tested before any body
// lowering. All public GTI scalar facts must agree, not just pointer width.
void verifyTargetLayout(const TargetInfo &target,
                        const llvm::DataLayout &layout,
                        llvm::LLVMContext &context);

} // namespace lang::native_llvm
