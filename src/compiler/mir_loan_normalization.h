#pragma once

#include "gti/mir.h"

#include <stdexcept>

namespace lang {

// Loan-bookkeeping normalization for the RFC 0004 erasure gate.
//
// RFC 0004 moves borrow checking into MIR: MIR derives where each loan ends
// from liveness instead of taking the end points semantics chose. The owner's
// acceptance gate requires a lifetime-free program to keep producing the same
// MIR and the same machine code as the reference compiler, with loan
// bookkeeping the only permitted difference. This normalizer erases exactly
// that bookkeeping, so the remaining MIR text can be compared byte for byte.
//
// It changes exactly two things and nothing else:
//
//   1. Every non-tombstone EndBorrow instruction is removed. EndBorrow is the
//      only MIR representation of a loan end.
//   2. Instruction IDs are renumbered densely per body (1..n, in block order,
//      then instruction order within a block), because removing an EndBorrow
//      shifts every later counter-allocated ID. Every field holding an
//      instruction ID is rewritten through the same per-body map:
//        MirInstruction::{id, replacementPair}
//        MirTerminator::invokeInstruction
//        MirValue::definition
//        MirValueUse::instruction (MirBody::valueUses)
//        MirFailureRecord::producerInstruction
//        MirRangeEndpointParking::{beginCall, endCall, endReceiver, park,
//                                  restore, failureRestores}
//        MirSourceDebugTables::{localEnds, valueEvents}[].point.instruction
//        MirThreadTaskAdapter::spawnInstruction (through sourceBody)
//        MirConstructorDispatchState::afterBaseInitialization
//        MirProgramInitializationStep::storageInitialization (Module body)
//        MirHostedStartupOperation::instruction (HostedStartup body)
//      MirTransformRecord and MirScalarPromotionRewrite also hold instruction
//      IDs, but they are optimizer evidence outside MirProgram and are never
//      serialized with it.
//
// MIR-local loan IDs, loan rows, loan operands, drops, lifecycle events,
// cleanup boundaries, values, places, calls, control flow and effect
// summaries are untouched, so a moved Drop or cleanup still differs.
//
// The normalizer fails closed rather than hide a difference. It throws
// MirLoanNormalizationError when
//   - an EndBorrow carries anything beyond its loan and its own source
//     attribution (id, kind, loan, hirValue, hirStatement, callSite,
//     constructorInitializer, hostedStartupOperation, and, only when hirValue
//     is set, the unsafeOperation and ownership event lowering copies from
//     that HIR value onto every record attributed to it): for example a
//     lifecycle event, a full-expression or cleanup-boundary end, an operand
//     or a result, any of which would make its position a non-loan fact;
//   - any instruction-ID field refers to a removed EndBorrow, so the end
//     position would leak into a non-loan field;
//   - any nonzero instruction-ID field names no instruction of its body, or a
//     body repeats an instruction ID.
// A field holding an instruction ID that is missing from the list above keeps
// its original value, so it shows up as a difference whenever EndBorrow
// placement shifts IDs; it is never silently made equal.
//
// The result is an inspection artifact for `--emit-mir --mir-normalize-loans`
// only. It is not verified MIR and must never reach a verifier, optimizer or
// backend. Changing what this normalizes, including the EndBorrow attribution
// allowlist or the field list above, requires owner review: see "RFC 0004
// erasure gate" in docs/architecture/verification.md.
class MirLoanNormalizationError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[nodiscard]] MirProgram normalizeLoanBookkeeping(MirProgram program);

// Borrow-dependency normalization for the RFC 0004 signature-contract gate.
//
// The owner decided that borrow contracts come only from signatures: an
// unannotated borrowed result borrows from every borrow-carrying input, where
// main infers a narrower set from the body. Call-result dependency metadata
// therefore legitimately widens. This mode erases exactly that metadata so the
// rest of MIR still compares byte for byte; the widening itself is checked
// separately for monotonicity from `--emit-borrow-dependencies`
// (gti/driver/borrow_dependencies.h). It is applied after
// normalizeLoanBookkeeping and clears only:
//
//   MirLoan, every kind: parent, storedDependency, additionalDependencies and
//     referenceSelection->transferredFrom (the loans and places a loan
//     depends on). For CallResult and Stored loans, whose primary dependency
//     call lowering selects from the callee contract, also source,
//     designatedFields and conditions (that primary dependency's place, field
//     path and alternative conditions). Other loans keep their source: it is
//     the place their own Borrow names. Loan id, kind, access, carriers,
//     storedField, lifecycleBound, twoPhase, entry, escapes and the rest of
//     referenceSelection stay compared.
//   MirValue::borrowDependencies: the set's contents; whether a set is
//     recorded at all stays compared.
//   Call, Construct and CallBody instructions: borrowOrigin, borrowArgument,
//     borrowArguments, borrowFields, borrowAccess, borrowPlace,
//     restoresReferenceResult and restoredBorrowFields. Closure computes keep
//     their capture borrow origin.
//   Borrow instructions: the `loan` of each MirOperandKind::Loan operand,
//     which names the parent loan, i.e. the dependency edge in instruction
//     form. The operand's kind, place, value and type stay compared, as do
//     the Borrow's own result loan and every Loan operand of other
//     instructions (call inputs, calls, initializations).
//   MirFunctionInstance: explicitReturnBorrowOrigin, returnBorrowOrigin,
//     returnBorrowParameter, returnBorrowAccess, returnBorrowParameters,
//     returnBorrowFields, the contents of completeReturnBorrow (its presence
//     stays compared), returnBorrowPlace, restoresReferenceResult and
//     restoredBorrowFields.
//   MirConstructorInstance: borrowOrigin, borrowParameter, borrowAccess,
//     borrowParameters and the contents of completeBorrow.
//   MirLambdaInstance: returnBorrowOrigin, returnBorrowParameter and
//     returnBorrowAccess (not serialized today).
//
// Instructions, places, values, calls, moves, control flow, drops, lifecycle
// events, text-relay contracts and effect summaries are untouched. A widened
// dependency that creates or removes a loan row still shifts loan IDs and so
// still differs. It throws MirLoanNormalizationError when a Borrow's parent
// operand names no loan of its body. Extending this list requires owner
// review, like the loan-bookkeeping normalizer above.
[[nodiscard]] MirProgram normalizeBorrowDependencies(MirProgram program);

} // namespace lang
