#include "gti/mir_backend.h"

#include "gti/lowered_program.h"
#include "gti/mir_printer.h"

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
  return {.kind = BackendArtifactKind::Source,
          .contents = MirPrinter().print(verified->program().mir()),
          .extension = ".mir"};
}

} // namespace lang
