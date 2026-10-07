#include "gti/cpp_backend.h"

#include "gti/cpp_emitter.h"
#include "gti/lowered_program.h"

#include <stdexcept>

namespace lang {

CppBackend::CppBackend(CppStandard standard, bool debugInfo)
    : standard(standard), debugInfo(debugInfo) {}

std::string_view CppBackend::name() const { return "cpp"; }

BackendArtifact CppBackend::generate(const LoweredProgram &program) {
  const std::vector<LoweredProgramIssue> issues = verifyLoweredProgram(program);
  if (!issues.empty()) {
    throw std::logic_error("C++ backend requires a verified lowered program: " +
                           issues.front().detail);
  }
  return {.kind = BackendArtifactKind::Source,
          .contents = CppEmitter(program, standard, debugInfo).emit(),
          .extension = ".cpp"};
}

} // namespace lang
