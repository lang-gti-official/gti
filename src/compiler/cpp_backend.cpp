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
  // The retained C++ representation names types, aliases and namespace storage
  // by qualified spelling. Module-distinct homonyms require the LLVM backend;
  // reject them before native compilation rather than emitting duplicate C++
  // types. Class definition families and exact specializations still share
  // their primary's representation name and remain part of the existing C++
  // support contract.
  struct RepresentationName {
    LoweredDeclarationKind kind;
    std::size_t identity;
    const LoweredDeclaration *declaration;
  };
  std::unordered_map<std::string, RepresentationName> representationNames;
  for (const auto &declaration : verified->program().declarations()) {
    std::string name;
    std::size_t identity = 0;
    if (declaration.kind == LoweredDeclarationKind::Namespace) {
      for (const auto &part : declaration.namespaceScope)
        name += part + "::";
      name += declaration.name;
      // Namespace contributions compose by qualified spelling across modules.
      identity = 0;
    } else if (const auto *type =
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
    } else if (const auto *alias = std::get_if<LoweredTypeAliasDeclaration>(
                   &declaration.payload);
               alias && declaration.ownerClass == 0) {
      name = alias->qualifiedName;
      identity = declaration.id;
    } else if (std::holds_alternative<LoweredNamespaceAliasDeclaration>(
                   declaration.payload)) {
      for (const auto &part : declaration.namespaceScope)
        name += part + "::";
      name += declaration.name;
      identity = declaration.id;
    } else if (const auto *storage =
                   std::get_if<LoweredStorageDeclaration>(&declaration.payload);
               storage && storage->ownerClass == 0 &&
               !storage->internalLinkage && storage->externalSymbol.empty()) {
      for (const auto &part : declaration.namespaceScope)
        name += part + "::";
      name += declaration.name;
      identity = storage->symbol;
    } else {
      continue;
    }
    const auto [found, inserted] = representationNames.emplace(
        name, RepresentationName{declaration.kind, identity, &declaration});
    if (inserted || (found->second.kind == declaration.kind &&
                     found->second.identity == identity))
      continue;
    // C++ permits repeated identical alias declarations. Preserve that already
    // representable case rather than rejecting every distinct semantic alias.
    if (const auto *alias =
            std::get_if<LoweredTypeAliasDeclaration>(&declaration.payload)) {
      const auto *previous = std::get_if<LoweredTypeAliasDeclaration>(
          &found->second.declaration->payload);
      if (previous && previous->type == alias->type)
        continue;
    }
    if (const auto *alias = std::get_if<LoweredNamespaceAliasDeclaration>(
            &declaration.payload)) {
      const auto *previous = std::get_if<LoweredNamespaceAliasDeclaration>(
          &found->second.declaration->payload);
      if (previous && previous->target == alias->target)
        continue;
    }
    throw BackendUnsupportedError(
        "distinct declarations with the same qualified name '" + name +
            "' require the LLVM backend",
        declaration.source);
  }
  if (verified->program().mir().requiresNativeObjectLayout())
    throw BackendUnsupportedError(
        "general sizeof/alignof queries require the LLVM backend's "
        "native GTI object representation");
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
    for (const auto &constant : body->programConstantSubstitutions)
      if (std::holds_alternative<ConstantAggregateValue>(constant.constant))
        throw BackendUnsupportedError(
            "aggregate constant publication requires the LLVM backend",
            row.identity.source);
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
