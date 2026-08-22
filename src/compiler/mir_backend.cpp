#include "gti/mir_backend.h"

#include "gti/lowered_program.h"
#include "gti/mir_printer.h"

#include <stdexcept>

namespace lang {

BackendArtifact MirBackend::generate(const BackendInput &input) {
  if (input.loweredProgram == nullptr) {
    throw std::logic_error(
        "MIR backend requires the compiler-owned lowered program");
  }
  // This is the first independent contract client: it deliberately does not
  // consult any transitional Program, semantic, HIR, or optimizer fields.
  if (!verifyLoweredProgram(*input.loweredProgram).empty()) {
    throw std::logic_error(
        "MIR backend refuses to serialize an invalid lowered program");
  }
  return {.kind = BackendArtifactKind::Source,
          .contents = MirPrinter().print(input.loweredProgram->mir()),
          .extension = ".mir"};
}

} // namespace lang
