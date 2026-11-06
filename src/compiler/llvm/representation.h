#pragma once

#include "../mir_lifetimes.h"
#include "gti/lowered_program.h"

namespace lang::native_llvm {

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
