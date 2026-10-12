#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <optional>

namespace lang::mir_detail {

// Concrete complete object representation, independent of any lifetime action.
[[nodiscard]] bool completeObjectType(const MirProgram &program,
                                      const SemanticType &type);

// Trivial generated default and same-type copy/move construction have no
// source constructor target. The program verifier checks the class's exact
// special-member policy, including the absence of a required default schedule.
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
// Unsafe raw text assertions retain a checked witness without assigning
// ownership or validity to the address itself. The exact ordered input
// stages and resulting loan source are shared by verification and emission.
[[nodiscard]] const MirLoan *
textAddressResultLoan(const MirBody &body, const MirInstruction &instruction);
// A distinct loan-rooted address retains either descendant provenance or a
// parallel read-only loan over the identical source/dependencies. Check typed
// call publication too; this proves identity and access, never liveness or
// permission to use a suspended loan.
[[nodiscard]] bool referenceAddressRetainsLoan(const MirBody &body,
                                               const MirPlace &address,
                                               const MirLoan &retained);

// A stable, checked field projection through an entry reference parameter is
// still caller-owned. This establishes owner containment, not the referent
// address or permission to escape; call publication and loan flow prove those.
[[nodiscard]] bool referenceParameterContainsPlace(const MirBody &body,
                                                   const MirLoan &parameter,
                                                   MirPlaceId place);

// Checked expected-reference extraction retains the external owner's identity,
// not the carrier's storage. Ordinary loan flow separately proves availability.
[[nodiscard]] bool
fallibleReferenceOwnerTransport(const MirBody &body,
                                const MirInstruction &instruction,
                                const MirLoan &extracted);

// Publish an exact runtime alias of an entry-reference dependency. A unique,
// dominating initializer chain must establish its typed address, and the return
// expression must identify the selected alias. Shared by verification and
// backend admission; carrier membership alone never proves the referent.
[[nodiscard]] bool
referenceParameterReturnPublication(const MirBody &body, const MirBlock &block,
                                    const MirInstruction &instruction);

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
