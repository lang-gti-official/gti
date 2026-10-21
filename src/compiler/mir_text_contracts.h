#pragma once

#include "gti/mir_verifier.h"

namespace lang::mir_detail {
[[nodiscard]] MirVerificationResult
verifyOwnerlessTextContracts(const MirProgram &program);
// Shape query used only after the complete program's text-storage invariants
// have been independently verified. This does not establish shell lifetime.
[[nodiscard]] bool isOwnerlessTextFieldProjection(const MirProgram &program,
                                                  const MirBody &body,
                                                  const MirPlace &place);
// A direct text field of a class whose type has no borrowed state. A value of
// that class may still carry the field's read-only text owner dependency in a
// Stored loan designating the field (L-OWN-02).
[[nodiscard]] bool directTextField(const MirProgram &program,
                                   const SemanticType &type, SymbolId field);
// The formal that alone initializes the direct text field `field` of the
// constructor's class, when that formal reaches no other text initializer.
[[nodiscard]] std::optional<std::size_t>
dependentTextFieldFormal(const MirProgram &program,
                         const MirConstructorInstance &constructor,
                         SymbolId field);
} // namespace lang::mir_detail
