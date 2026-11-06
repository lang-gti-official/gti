#pragma once

#include "../mir_lifetimes.h"
#include "gti/lowered_program.h"

#include <stdexcept>

namespace lang::native_llvm {

// Descriptor classification uses an exact universal-to-actual view, without
// erasure. These return canonical descriptors only; consumers of stored child
// types must instantiate those recipes for the actual type separately.
[[nodiscard]] inline const MirClassInstance *exactClassInstance(
    const MirProgram &program, const SemanticType &type) {
  const auto view = mirClassLifetimeView(program, type);
  return view ? view->instance : nullptr;
}
[[nodiscard]] inline const MirPayloadEnum *exactPayloadEnum(
    const MirProgram &program, const SemanticType &type) {
  const auto view = mirEnumLifetimeView(program, type);
  return view ? view->payload : nullptr;
}
[[nodiscard]] inline const MirIntegralEnum *exactIntegralEnum(
    const MirProgram &program, const SemanticType &type) {
  const auto view = mirEnumLifetimeView(program, type);
  return view ? view->integral : nullptr;
}
[[nodiscard]] inline std::optional<SemanticType> exactPayloadElementType(
    const MirProgram &program, const SemanticType &type,
    std::size_t variant, std::size_t index) {
  const auto view = mirEnumLifetimeView(program, type);
  if (!view || !view->payload)
    return std::nullopt;
  for (const auto &alternative : view->payload->variants)
    if (alternative.variantIndex == variant && index < alternative.payloadTypes.size())
      return view->instantiate(alternative.payloadTypes[index]);
  return std::nullopt;
}

// The recipe must come from the selected canonical descriptor. This keeps
// recursive storage checks/emission in the actual region scope; it does not
// establish formation or permission independently of the verified program.
[[nodiscard]] inline SemanticType instantiateStorageType(
    const MirProgram &program, const SemanticType &owner, const SemanticType &recipe) {
  std::optional<SemanticType> actual;
  if (owner.kind == SemanticType::Class) {
    if (const auto view = mirClassLifetimeView(program, owner))
      actual = view->instantiate(recipe);
  } else if (owner.kind == SemanticType::Enum) {
    if (const auto view = mirEnumLifetimeView(program, owner))
      actual = view->instantiate(recipe);
  }
  if (!actual)
    throw std::logic_error("LLVM storage recipe lost its exact actual nominal view");
  return *actual;
}

// Representation queries require the owning proof for the unchanged semantic
// program. Erased keys cannot be converted back to a SemanticType or used as
// outlives/permission evidence.
class Representations final {
public:
  class TypeKey final {
  public:
    friend bool operator==(const TypeKey &, const TypeKey &) = default;
  private:
    explicit TypeKey(SemanticType type) : type_(eraseTypeLifetimes(std::move(type))) {}
    SemanticType type_;
    friend class Representations;
  };

  explicit Representations(VerifiedLoweredProgram program)
      : input_(std::move(program)) {}

  [[nodiscard]] TypeKey key(const SemanticType &type) const { return TypeKey(type); }
  [[nodiscard]] bool sameType(const SemanticType &left,
                              const SemanticType &right) const {
    return key(left) == key(right);
  }
  [[nodiscard]] bool sameTypes(std::span<const SemanticType> left,
                               std::span<const SemanticType> right) const {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
      if (!sameType(left[index], right[index])) return false;
    return true;
  }
  [[nodiscard]] const MirClassInstance *classInstance(const SemanticType &type) const {
    const auto view = mirClassLifetimeView(input_.program().mir(), type);
    return view ? view->instance : nullptr;
  }
  [[nodiscard]] const MirPayloadEnum *payloadEnum(const SemanticType &type) const {
    const auto view = mirEnumLifetimeView(input_.program().mir(), type);
    return view ? view->payload : nullptr;
  }
  [[nodiscard]] const MirIntegralEnum *integralEnum(const SemanticType &type) const {
    const auto view = mirEnumLifetimeView(input_.program().mir(), type);
    return view ? view->integral : nullptr;
  }

private:
  VerifiedLoweredProgram input_;
};

} // namespace lang::native_llvm
