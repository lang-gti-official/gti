#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>

namespace lang::mir_detail {

// Concrete complete object representation, independent of any lifetime action.
[[nodiscard]] bool completeObjectType(const MirProgram &program,
                                      const SemanticType &type);

// The generated same-type copy/move family has no source constructor target.
// Allocation and staged input ownership still have distinct MIR identities.
[[nodiscard]] bool
generatedUniqueConstruction(const MirInstruction &instruction);

// Runtime representation queries have no runtime input and cannot acquire
// ownership or borrow authority merely by naming a type.
[[nodiscard]] bool objectLayoutQuery(const MirProgram &program,
                                     const MirInstruction &instruction);
// Exact raw-address lifetime end. Cleanup/failure recipes are independently
// derived by the program verifier; this query checks the operation's shape.
[[nodiscard]] bool objectDestroy(const MirProgram &program,
                                 const MirInstruction &instruction);
[[nodiscard]] bool objectConstruct(const MirProgram &program,
                                   const MirInstruction &instruction);
[[nodiscard]] bool objectRelocate(const MirProgram &program,
                                  const MirInstruction &instruction);

// Inert, const-preserving typed/erased address conversion; never a borrow or
// an assertion that the pointee is initialized, aligned, live or writable.
[[nodiscard]] bool rawPointerConversion(const MirInstruction &instruction);

// The runtime address comes from operand zero; the checked lifetime and
// access authority come from the explicit witness in operand one.
[[nodiscard]] const MirLoan *
rawBorrowResultLoan(const MirBody &body, const MirInstruction &instruction);

// Recognize the exact, single-use signed-minimum literal schedule shared by
// native emission and constant execution. Ordinary runtime negation is not
// interchangeable with this failure-free literal formation.
[[nodiscard]] const MirInstruction *
fusedSignedMinimumLiteral(const MirBody &body, const MirInstruction &negation);
[[nodiscard]] const MirInstruction *
fusedSignedMinimumNegation(const MirBody &body, const MirInstruction &carrier);

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  MirValueId value);
// Recover a cleanup-bound carrier's original publication after subsequent
// moves changed its current carrier list. This proves identity, not liveness;
// ordinary ownership/loan dataflow still checks availability at each use.
[[nodiscard]] bool bindingReceivedStoredLoan(const MirBody &body,
                                             HirBindingId binding,
                                             MirLoanId dependency,
                                             std::size_t depth = 0);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForPlace(const MirBody &body, MirPlaceId place, std::size_t depth);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForOperand(const MirBody &body, const MirOperand &operand,
                       std::size_t depth = 0);
[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId value, std::size_t depth);

} // namespace lang::mir_detail
