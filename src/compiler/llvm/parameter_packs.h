#pragma once

#include "gti/semantic_core.h"

#include <span>
#include <vector>

namespace lang::native_llvm {

// Concrete trailing packs retain source binding identity in MIR while the
// private native ABI transports one independently typed lane per element.
// Symbolic, nested and non-trailing packs have no native representation.
[[nodiscard]] std::vector<SemanticType>
flattenConcreteParameters(std::span<const SemanticType> parameters);

} // namespace lang::native_llvm
