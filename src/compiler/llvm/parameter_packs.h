#pragma once

#include "gti/mir.h"

#include <optional>
#include <span>
#include <vector>

namespace lang::native_llvm {

struct ParameterLane final {
  HirBindingId binding = 0;
  SemanticType type = SemanticType::Unknown;
  std::optional<std::size_t> packElement;
  bool materialized = true;
};

struct ConcretePackParameter final {
  HirBindingId binding = 0;
  SemanticType type = SemanticType::Unknown;
};

struct ConcreteParameterPlan final {
  std::vector<ParameterLane> lanes;
  std::vector<ConcretePackParameter> packs;
};

// Concrete trailing packs retain source binding identity in MIR while the
// private native ABI transports one independently typed lane per element.
// Symbolic, nested and non-trailing packs have no native representation.
[[nodiscard]] std::vector<SemanticType>
flattenConcreteParameters(std::span<const SemanticType> parameters);

// Concrete instances currently retain either the source trailing TypePack or
// already-expanded ABI lanes. Expanded owners may retain the source binding
// list with one trailing binding for the whole pack. Resolve both verified
// forms through one exact plan; an empty trailing pack has no ABI lane. A
// per-lane binding vector with zero-valued continuations is not sealed MIR.
[[nodiscard]] std::optional<std::size_t>
concretePackPrefix(std::span<const SemanticType> parameters,
                   std::span<const HirBindingId> bindings,
                   HirBindingId packBinding, const SemanticType &packType);

// Build the exact physical parameter lanes from sealed MIR. Repeated place
// records with one PlaceKey are compatible aliases of the same logical formal;
// they never create another ABI lane. Divergent identities fail closed.
[[nodiscard]] std::optional<ConcreteParameterPlan>
planConcreteParameters(std::span<const SemanticType> parameters,
                       std::span<const HirBindingId> bindings,
                       const MirBody &body);

} // namespace lang::native_llvm
