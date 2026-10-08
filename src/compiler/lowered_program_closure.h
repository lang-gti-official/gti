#pragma once

#include "gti/lowered_program.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang::lowered_detail {

template <typename Enum>
[[nodiscard]] constexpr std::size_t ordinal(Enum value) {
  return static_cast<std::size_t>(value);
}

struct DerivedGeneratedGraph {
  std::vector<LoweredGeneratedItem> items;
  std::vector<
      std::pair<MirBodyAddress, std::vector<LoweredGeneratedItemIdentity>>>
      bodyRoots;
  std::vector<std::pair<LoweredDeclarationId,
                        std::vector<LoweredGeneratedItemIdentity>>>
      declarationRoots;
};

[[nodiscard]] bool generatedLess(const LoweredGeneratedItemIdentity &left,
                                 const LoweredGeneratedItemIdentity &right);
[[nodiscard]] bool
containsGenerated(const std::vector<LoweredGeneratedItemIdentity> &items,
                  const LoweredGeneratedItemIdentity &identity);
[[nodiscard]] const LoweredGeneratedItem *
findGenerated(const std::vector<LoweredGeneratedItem> &items,
              const LoweredGeneratedItemIdentity &identity);
[[nodiscard]] const LoweredBody *
findBody(const std::vector<LoweredBody> &bodies, MirBodyAddress address);
[[nodiscard]] LoweredBody *findBody(std::vector<LoweredBody> &bodies,
                                    MirBodyAddress address);
[[nodiscard]] const LoweredDeclaration *
findDeclaration(const std::vector<LoweredDeclaration> &declarations,
                LoweredDeclarationId id);
[[nodiscard]] LoweredDeclaration *
findDeclaration(std::vector<LoweredDeclaration> &declarations,
                LoweredDeclarationId id);

// The construction seal: a 64-bit hash of the complete deterministic printed
// program. The text is hashed as it is printed and never materialized. Seals
// are compared only with seals of the same compiler, so the hash is not a
// stable external format.
[[nodiscard]] std::uint64_t constructionSeal(const LoweredProgram &program);
void addIssue(
    std::vector<LoweredProgramIssue> &issues, LoweredProgramIssueKind kind,
    std::string detail, std::optional<MirBodyAddress> body = std::nullopt,
    std::optional<LoweredDeclarationId> declaration = std::nullopt,
    std::optional<LoweredGeneratedItemIdentity> generated = std::nullopt);

[[nodiscard]] LoweredBodyRole bodyRole(const MirProgram &program,
                                       const LoweredBodyIdentity &identity);
[[nodiscard]] DerivedGeneratedGraph deriveGeneratedGraph(
    const MirProgram &program,
    const std::vector<LoweredDeclaration> &declarations,
    const std::vector<LoweredConstructorInstance> &constructorInstances);

} // namespace lang::lowered_detail
