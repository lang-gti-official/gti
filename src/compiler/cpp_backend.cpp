#include "gti/cpp_backend.h"

#include "gti/cpp_emitter.h"
#include "gti/lowered_program.h"

#include <stdexcept>
#include <utility>

namespace lang {

CppBackend::CppBackend(CppStandard standard, bool debugInfo)
    : standard(standard), debugInfo(debugInfo) {}

std::string_view CppBackend::name() const { return "cpp"; }

BackendArtifact CppBackend::generate(LoweredProgram program) {
  std::vector<LoweredProgramIssue> issues;
  const std::optional<VerifiedLoweredProgram> verified =
      verifiedLoweredProgram(std::move(program), issues);
  if (!verified) {
    throw std::logic_error("C++ backend requires a verified lowered program: " +
                           issues.front().detail);
  }
  return {.kind = BackendArtifactKind::Source,
          .contents = CppEmitter(*verified, standard, debugInfo).emit(),
          .extension = ".cpp"};
}

} // namespace lang
