#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>

namespace lang::mir_detail {

// Recognize the exact, single-use signed-minimum literal schedule shared by
// native emission and constant execution. Ordinary runtime negation is not
// interchangeable with this failure-free literal formation.
[[nodiscard]] const MirInstruction *
fusedSignedMinimumLiteral(const MirBody &body, const MirInstruction &negation);
[[nodiscard]] const MirInstruction *
fusedSignedMinimumNegation(const MirBody &body, const MirInstruction &carrier);

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  MirValueId value);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForPlace(const MirBody &body, MirPlaceId place, std::size_t depth);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForOperand(const MirBody &body, const MirOperand &operand,
                       std::size_t depth = 0);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId value, std::size_t depth);

} // namespace lang::mir_detail
