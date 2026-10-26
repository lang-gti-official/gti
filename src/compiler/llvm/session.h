#pragma once

#include "gti/llvm_admission.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lang::native_llvm {

class Layouts;
class Abi;
class DebugInfo;

// Private, invalidatable whole-program ABI, not an external GTI ABI promise.
inline constexpr unsigned executionAbiVersion = 2;

[[nodiscard]] std::unique_ptr<llvm::TargetMachine>
makeMachine(const TargetInfo &target, LlvmBackendOptions options);

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
  [[nodiscard]] DebugInfo *debugInfo() { return debugInfo_.get(); }
  [[nodiscard]] const llvm::DataLayout &dataLayout() const;
  // Initial native atomic profile: no widening and no libatomic dependency.
  // This is target legalization policy, not a public lock-free guarantee.
  [[nodiscard]] bool supportsNativeAtomic(const llvm::Function &function,
                                          unsigned bits,
                                          std::uint64_t alignment) const;
  [[nodiscard]] LlvmBackendOptions options() const { return options_; }
  [[nodiscard]] const std::vector<std::string> &diagnostics() const {
    return diagnostics_;
  }

  void verify() const;
  void requireDefinition(llvm::Function &function);
  // Only the ABI declaration factory supplies this exact source identity.
  // Unregistered/generated functions retain their existing linkage.
  void requireSourceDefinition(HirFunctionInstanceId id,
                               llvm::Function &function);
  [[nodiscard]] bool hasSourceDefinition(HirFunctionInstanceId id,
                                         const llvm::Function &function) const;
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
  std::unique_ptr<DebugInfo> debugInfo_;
  std::vector<std::pair<HirFunctionInstanceId, llvm::Function *>>
      sourceDefinitions_;
};

// Kept separate so target-contract mutations can be tested before any body
// lowering. All public GTI scalar facts must agree, not just pointer width.
void verifyTargetLayout(const TargetInfo &target,
                        const llvm::DataLayout &layout,
                        llvm::LLVMContext &context);

} // namespace lang::native_llvm
