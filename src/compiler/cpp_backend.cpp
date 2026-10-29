#include "gti/cpp_backend.h"
#include "gti/support.h"

#include "gti/cpp_emitter.h"
#include "gti/lowered_program.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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
  // The retained C++ representation names nominal declarations by qualified
  // spelling. Module-distinct homonyms require the LLVM backend; reject them
  // before native compilation rather than emitting duplicate C++ types. Class
  // definition families and exact specializations still share their primary's
  // representation name and remain part of the existing C++ support contract.
  std::unordered_map<std::string,
                     std::pair<LoweredDeclarationKind, std::size_t>>
      nominalNames;
  for (const auto &declaration : verified->program().declarations()) {
    std::string name;
    std::size_t identity = 0;
    if (const auto *type =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      name = type->qualifiedName;
      identity = type->id;
      std::unordered_set<ClassId> visited;
      for (const auto *current = type;;) {
        if (!visited.insert(identity).second)
          throw std::logic_error(
              "cyclic class primary identity in lowered program");
        const ClassId primary = current->exactSpecializationPrimary
                                    ? current->exactSpecializationPrimary
                                    : current->definitionFamilyPrimary;
        if (!primary)
          break;
        identity = primary;
        current = verified->program().findClassDeclaration(identity);
        if (!current)
          throw std::logic_error(
              "missing class primary identity in lowered program");
      }
    } else if (const auto *type =
                   std::get_if<LoweredEnumDeclaration>(&declaration.payload)) {
      name = type->qualifiedName;
      identity = type->id;
    } else {
      continue;
    }
    const auto key = std::pair(declaration.kind, identity);
    const auto [found, inserted] = nominalNames.emplace(name, key);
    if (!inserted && found->second != key)
      throw BackendUnsupportedError(
          "distinct nominal declarations with the same qualified name '" +
              name + "' require the LLVM backend",
          declaration.source);
  }
  // New native atomic operations deliberately remain LLVM-only. Reject their
  // exact executable identities before representation planning, rather than
  // misreporting a valid program as a C++ emitter invariant failure.
  for (const auto &row : verified->program().bodies()) {
    if (row.role == LoweredBodyRole::DataOnly ||
        row.role == LoweredBodyRole::AbiDeclaration)
      continue;
    const auto *body =
        findMirBody(verified->program().mir(), row.identity.address);
    if (!body)
      continue;
    for (const auto &block : body->blocks)
      for (const auto &instruction : block.instructions)
        if (!instruction.tombstone &&
            (instruction.intrinsic == IntrinsicKind::AtomicFetchAddWrapping ||
             instruction.intrinsic == IntrinsicKind::AtomicFetchSubWrapping))
          throw BackendUnsupportedError("wrapping atomic read-modify-write "
                                        "operations require the LLVM backend",
                                        row.identity.source);
  }
  const PhaseTimeScope timeScope("backend-emit");
  return {.kind = BackendArtifactKind::Source,
          .contents = CppEmitter(*verified, standard, debugInfo).emit(),
          .extension = ".cpp"};
}

} // namespace lang
