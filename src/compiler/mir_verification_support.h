#pragma once

#include "gti/mir_editor.h"
#include "gti/mir_verifier.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lang::mir_detail {

// Whole-program verification depends only on program content, and callers
// often hold a program they have just verified. These entry points skip only
// that repeated check; every other step, and so every result, is unchanged.
//
// rewriteMirDeadInstruction for a program that already passed
// verifyMirProgram.
[[nodiscard]] std::optional<MirDeadInstructionRewrite>
rewriteVerifiedMirDeadInstruction(MirProgram &program, MirBodyAddress body,
                                  MirInstructionId instruction);
// verifyMirOptimizationCoherence where `sourceVerified` or
// `optimizedVerified` records that the program already passed
// verifyMirProgram.
[[nodiscard]] MirVerificationResult verifyVerifiedMirOptimizationCoherence(
    const MirProgram &source, const MirProgram &optimized,
    const std::vector<MirTransformRecord> &records, bool sourceVerified,
    bool optimizedVerified);

struct CanonicalPlace {
  MirPlaceRootKind root = MirPlaceRootKind::Value;
  HirBindingId binding = 0;
  SymbolId symbol = 0;
  MirTemporaryId temporary = 0;
  MirValueId value = 0;
  std::vector<MirPlaceProjection> projections;
  MirLoanId throughLoan = 0;
  bool ambiguous = false;
};

[[nodiscard]] MirVerificationResult
verificationFailure(const MirBody &body, std::size_t owner, std::string message,
                    MirBlockId block = 0, MirInstructionId instruction = 0);
void appendVerification(MirVerificationResult &destination,
                        MirVerificationResult source);

[[nodiscard]] std::vector<MirBlockId>
terminatorSuccessors(const MirTerminator &terminator);
[[nodiscard]] bool constantMatchesType(const ConstantValue &constant,
                                       const SemanticType &type,
                                       const MirProgram *program = nullptr);
// The bounded conversions represented by a CallInput, after semantics selects
// the parameter. Pack lowering must preserve this same input contract.
[[nodiscard]] bool validCallInputType(const SemanticType &source,
                                      const SemanticType &parameter,
                                      HirCallInputKind kind);

[[nodiscard]] const MirInstruction *linearDefinition(const MirBody &body,
                                                     MirValueId value);
[[nodiscard]] bool receiverIsConsumed(const MirBody &body,
                                      const MirOperand &operand,
                                      MirValueUseKind expectedUseKind,
                                      MirInstructionId expectedUseInstruction,
                                      std::size_t expectedOperandIndex = 0);
[[nodiscard]] bool movedValueInto(const MirBody &body,
                                  const MirOperand &operand,
                                  MirInstructionId expectedInstruction,
                                  std::size_t expectedOperandIndex);
[[nodiscard]] bool freshOwnedValueInto(const MirProgram &program,
                                       const MirBody &body,
                                       const MirOperand &operand,
                                       MirInstructionId expectedInstruction,
                                       std::size_t expectedOperandIndex);

[[nodiscard]] CanonicalPlace canonicalBorrowOrigin(
    const MirBody &body, MirPlaceId place,
    const std::unordered_map<HirBindingId, std::vector<MirLoanId>>
        &bindingLoans,
    bool physicalCarrier = false);
[[nodiscard]] bool sameCanonicalPlace(const CanonicalPlace &left,
                                      const CanonicalPlace &right);
[[nodiscard]] bool sameGlobalBorrowPlace(const BorrowOriginPlace &expected,
                                         const CanonicalPlace &actual);

} // namespace lang::mir_detail
