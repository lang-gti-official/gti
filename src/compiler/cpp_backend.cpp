#include "gti/cpp_backend.h"
#include "gti/support.h"

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
  const std::optional<VerifiedLoweredProgram> verified = [&] {
    const PhaseTimeScope timeScope("backend-verify-lowered-program");
    return verifiedLoweredProgram(std::move(program), issues);
  }();
  if (!verified) {
    throw std::logic_error("C++ backend requires a verified lowered program: " +
                           issues.front().detail);
  }
  // New native atomic operations deliberately remain LLVM-only. Reject their
  // exact executable identities before representation planning, rather than
  // misreporting a valid program as a C++ emitter invariant failure.
  for (const auto &row : verified->program().bodies()) {
    if (row.role == LoweredBodyRole::DataOnly ||
        row.role == LoweredBodyRole::AbiDeclaration)
      continue;
    const auto *body = findMirBody(verified->program().mir(), row.identity.address);
    if (!body) continue;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        if (!instruction.tombstone &&
            (instruction.intrinsic == IntrinsicKind::AtomicFetchAddWrapping ||
             instruction.intrinsic == IntrinsicKind::AtomicFetchSubWrapping))
          throw BackendUnsupportedError(
              "wrapping atomic read-modify-write operations require the LLVM backend",
              row.identity.source);
  }
  const PhaseTimeScope timeScope("backend-emit");
  return {.kind = BackendArtifactKind::Source,
          .contents = CppEmitter(*verified, standard, debugInfo).emit(),
          .extension = ".cpp"};
}

} // namespace lang
