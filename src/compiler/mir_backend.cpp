#include "gti/mir_backend.h"

#include "gti/mir.h"
#include "gti/mir_printer.h"

#include <stdexcept>

namespace lang {

BackendArtifact MirBackend::generate(const BackendInput &input) {
  // The reusable compilation boundary verifies the optimized program before
  // any backend runs, but this backend is also a public library surface, so
  // it re-checks rather than serializing an invalid program as if it were
  // authoritative.
  if (!verifyMirProgram(input.mir).valid()) {
    throw std::logic_error(
        "MIR backend refuses to serialize an invalid MIR program");
  }
  return {.kind = BackendArtifactKind::Source,
          .contents = MirPrinter().print(input.mir),
          .extension = ".mir"};
}

} // namespace lang
