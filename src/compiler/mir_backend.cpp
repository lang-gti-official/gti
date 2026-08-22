#include "gti/mir_backend.h"

#include "gti/lowered_program.h"
#include "gti/mir_printer.h"

#include <stdexcept>

namespace lang {

BackendArtifact MirBackend::generate(const LoweredProgram &program) {
  if (!verifyLoweredProgram(program).empty()) {
    throw std::logic_error(
        "MIR backend refuses to serialize an invalid lowered program");
  }
  return {.kind = BackendArtifactKind::Source,
          .contents = MirPrinter().print(program.mir()),
          .extension = ".mir"};
}

} // namespace lang
