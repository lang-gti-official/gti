#include "gti/optimizer.h"

#include <utility>

namespace lang {

OptimizedProgram OptimizationPipeline::run(OptimizationRequest request) const {
  OptimizedProgram result{.mir = std::move(request.mir)};
  result.report.verificationEnabled = request.options.verifyMir;

  // Stage A intentionally owns and verifies an unchanged MIR snapshot. The
  // request fields become pass context when the first transforming pass lands.
  (void)request.hir;
  (void)request.level;
  (void)request.target;
  if (!request.options.verifyMir) {
    return result;
  }

  result.report.inputVerification = verifyMirProgram(result.mir);
  if (!result.report.inputVerification.valid()) {
    return result;
  }
  // No pass can mutate MIR in the identity pipeline, so the verified input is
  // also the verified output. A changed pass must replace this with a fresh
  // post-pass verification.
  result.report.outputVerification = result.report.inputVerification;
  return result;
}

} // namespace lang
