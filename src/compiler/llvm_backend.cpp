#include "gti/llvm_backend.h"

#include "gti/lowered_program.h"
#include "gti/target.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#if __has_include(<llvm/TargetParser/Host.h>)
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#else
#include <llvm/ADT/Triple.h>
#include <llvm/Support/Host.h>
#endif

#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lang {
namespace {

bool sameNativeTarget(const TargetInfo &left, const TargetInfo &right) {
  return left.os == right.os && left.vendor == right.vendor &&
         left.arch == right.arch && left.dataLayout == right.dataLayout;
}

void initializeNativeLlvmTarget() {
  static std::once_flag once;
  static std::string failure;
  std::call_once(once, [] {
    if (llvm::InitializeNativeTarget()) {
      failure = "LLVM could not initialize the native target";
      return;
    }
    if (llvm::InitializeNativeTargetAsmPrinter()) {
      failure = "LLVM could not initialize the native assembly printer";
      return;
    }
    std::string lookupFailure;
    const llvm::Triple hostTriple(llvm::sys::getDefaultTargetTriple());
    if (llvm::TargetRegistry::lookupTarget(hostTriple, lookupFailure) ==
        nullptr) {
      failure = "LLVM could not resolve the native target: " + lookupFailure;
    }
  });
  if (!failure.empty()) {
    throw std::runtime_error(failure);
  }
}

} // namespace

LlvmBackend::LlvmBackend(LlvmBackendOptions options) : options(options) {}

std::string_view LlvmBackend::name() const { return "llvm"; }

BackendArtifact LlvmBackend::generate(LoweredProgram program) {
  std::vector<LoweredProgramIssue> issues;
  std::optional<VerifiedLoweredProgram> verified =
      verifiedLoweredProgram(std::move(program), issues);
  if (!verified) {
    throw std::logic_error(
        "LLVM backend requires a verified lowered program: " +
        issues.front().detail);
  }

  if (!sameNativeTarget(verified->program().target(), TargetInfo::host())) {
    throw BackendUnsupportedError(
        "the LLVM backend foundation currently accepts only the host target");
  }

  initializeNativeLlvmTarget();
  // Constructing an empty context proves that the backend owns a usable LLVM
  // IR environment without manufacturing a program or lowering GTI meaning.
  llvm::LLVMContext context;
  static_cast<void>(context);
  static_cast<void>(options);
  throw BackendUnsupportedError(
      "executable LLVM lowering is not implemented yet");
}

} // namespace lang
