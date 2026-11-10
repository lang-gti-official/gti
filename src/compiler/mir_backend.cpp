#include "gti/mir_backend.h"

#include "gti/lowered_program.h"
#include "gti/mir_printer.h"
#include "mir_loan_normalization.h"

#include <stdexcept>

namespace lang {

BackendArtifact MirBackend::generate(LoweredProgram program) {
  std::vector<LoweredProgramIssue> issues;
  const std::optional<VerifiedLoweredProgram> verified =
      verifiedLoweredProgram(std::move(program), issues);
  if (!verified) {
    throw std::logic_error(
        "MIR backend refuses to serialize an invalid lowered program");
  }
  const MirProgram &mir = verified->program().mir();
  std::string contents;
  if (options.normalizeBorrowDependencies)
    contents = MirPrinter().print(
        normalizeBorrowDependencies(normalizeLoanBookkeeping(mir)));
  else if (options.normalizeLoanBookkeeping)
    contents = MirPrinter().print(normalizeLoanBookkeeping(mir));
  else
    contents = MirPrinter().print(mir);
  return {.kind = BackendArtifactKind::Source,
          .contents = std::move(contents),
          .extension = ".mir"};
}

} // namespace lang
