#pragma once

#include "gti/mir_verifier.h"

#include <span>

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
// A text payload of a payload enum whose type has no borrowed state. A value
// of that enum may carry the payload's read-only text owner dependency in a
// Stored loan designating the payload (L-OWN-02).
[[nodiscard]] bool directTextPayload(const MirProgram &program,
                                     const SemanticType &type,
                                     SymbolId payload);
// A direct text field of the class `type`, or a text payload of one of its
// payload-enum fields: `{field}` or `{field, payload}`.
[[nodiscard]] bool directTextPath(const MirProgram &program,
                                  const SemanticType &type,
                                  std::span<const SymbolId> path);
// The formal that alone initializes the text field path `path` of the
// constructor's class, when that formal reaches no other text initializer.
[[nodiscard]] std::optional<std::size_t>
dependentTextFieldFormal(const MirProgram &program,
                         const MirConstructorInstance &constructor,
                         std::span<const SymbolId> path);
// The text field path at which the constructor alone stores the text field
// `field` of its class formal `parameter`, which a construction may re-root
// its argument's owner dependency at.
[[nodiscard]] std::optional<std::vector<SymbolId>>
dependentTextFieldRelay(const MirProgram &program,
                        const MirConstructorInstance &constructor,
                        std::size_t parameter, SymbolId field);
} // namespace lang::mir_detail
