#include "gti/llvm_backend.h"

#include "gti/llvm_admission.h"
#include "gti/lowered_program.h"
#include "gti/target.h"
#include "llvm/session.h"
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

  const LlvmAdmissionPlan admission = planLlvmAdmission(*verified, options);
  if (const auto *unsupported = admission.firstUnsupported()) {
    if (!unsupported->source.source.empty()) {
      throw BackendUnsupportedError(admission.limitation(),
                                    unsupported->source);
    }
    throw BackendUnsupportedError(admission.limitation());
  }

  native_llvm::Session session(*verified, options);
  session.verify();
  throw BackendUnsupportedError(
      "executable LLVM lowering is not implemented yet");
}

} // namespace lang
