#pragma once

#include "gti/mir_verifier.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace lang::mir_detail {

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
// The argument-relay call through which a text-carrying receiver narrows its
// own text field in `assign`, or null when `assign` is not that exact shape.
[[nodiscard]] const MirInstruction *
receiverTextReseatProducer(const MirBody &body, const MirInstruction &assign);
[[nodiscard]] bool receiverIsConsumed(const MirBody &body,
                                      const MirOperand &operand,
                                      MirValueUseKind expectedUseKind,
                                      MirInstructionId expectedUseInstruction,
                                      std::size_t expectedOperandIndex = 0);
[[nodiscard]] bool movedValueInto(const MirBody &body,
                                  const MirOperand &operand,
                                  MirInstructionId expectedInstruction,
                                  std::size_t expectedOperandIndex);
[[nodiscard]] bool freshOwnedValueInto(const MirBody &body,
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
