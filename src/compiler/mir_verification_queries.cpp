#include "mir_verification_queries.h"

#include <algorithm>
#include <climits>

namespace lang::mir_detail {

bool rawPointerConversion(const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute ||
      instruction.operation != MirOperation::Convert ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.operands.size() != 1 || !instruction.result ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.intrinsic != IntrinsicKind::None || instruction.loan ||
      instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      !instruction.lifecycle.empty() || instruction.ownership ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty())
    return false;
  const auto &source = instruction.operands.front().type;
  const auto &target = instruction.info.type;
  return source.kind == SemanticType::RawPointer &&
         target.kind == SemanticType::RawPointer &&
         source.arguments.size() == 1 && target.arguments.size() == 1 &&
         source.arguments.front().kind != SemanticType::RawPointer &&
         target.arguments.front().kind != SemanticType::RawPointer &&
         (source.pointerAccess == AccessMode::Mutable ||
          target.pointerAccess == AccessMode::ReadOnly) &&
         (source.arguments.front() == target.arguments.front() ||
          source.arguments.front() == SemanticType::Void ||
          target.arguments.front() == SemanticType::Void);
}

namespace {

[[nodiscard]] std::optional<std::uint64_t>
signedMinimumMagnitude(const SemanticType &type) {
  switch (type.kind) {
  case SemanticType::Int8:
    return std::uint64_t{1} << 7U;
  case SemanticType::Int16:
    return std::uint64_t{1} << 15U;
  case SemanticType::Int32:
    return std::uint64_t{1} << 31U;
  case SemanticType::Int64:
    return std::uint64_t{1} << 63U;
  default:
    return std::nullopt;
  }
}

[[nodiscard]] bool integerLiteralCarrierCanRepresent(const SemanticType &type,
                                                     std::uint64_t magnitude) {
  switch (type.kind) {
  case SemanticType::Int8:
    return magnitude <= static_cast<std::uint64_t>(INT8_MAX);
  case SemanticType::Int16:
    return magnitude <= static_cast<std::uint64_t>(INT16_MAX);
  case SemanticType::Int32:
    return magnitude <= static_cast<std::uint64_t>(INT32_MAX);
  case SemanticType::Int64:
    return magnitude <= static_cast<std::uint64_t>(INT64_MAX);
  case SemanticType::UInt8:
    return magnitude <= UINT8_MAX;
  case SemanticType::UInt16:
    return magnitude <= UINT16_MAX;
  case SemanticType::UInt32:
    return magnitude <= UINT32_MAX;
  case SemanticType::UInt64:
    return true;
  default:
    return false;
  }
}

} // namespace

// GTI parses the signed minimum as a positive magnitude followed by unary
// negation. The magnitude is intentionally one past the positive range, so it
// has no independently spellable value of the result type. Fuse the exact
// one-use source chain. Parentheses can retain an explicit MIR Identity
// between the magnitude and negation; those identities must be exact,
// failure-free, same-type, and single-use. Program-constant substitution may
// retain the result type on the magnitude, while a contextually typed direct
// literal may use a wider integer carrier that can represent the positive
// magnitude.
[[nodiscard]] const MirInstruction *
fusedSignedMinimumLiteral(const MirBody &body, const MirInstruction &negation) {
  if (negation.kind != MirInstructionKind::Compute ||
      negation.operation != MirOperation::Negate || !negation.result ||
      negation.operands.size() != 1 ||
      negation.operands.front().kind != MirOperandKind::Value ||
      !negation.localFailureSites.empty() || !negation.definedFailure.empty()) {
    return nullptr;
  }
  const MirInstruction *consumer = &negation;
  const MirValue *operand = body.findValue(negation.operands.front().value);
  const MirInstruction *literal = nullptr;
  while (operand != nullptr) {
    const MirInstruction *producer = definitionFor(body, operand->id);
    if (producer == nullptr || !producer->result ||
        *producer->result != operand->id) {
      return nullptr;
    }
    const std::vector<MirValueUse> &uses = body.usesOf(operand->id);
    if (uses.size() != 1 ||
        uses.front().kind != MirValueUseKind::InstructionOperand ||
        uses.front().instruction != consumer->id ||
        uses.front().operandIndex != 0) {
      return nullptr;
    }
    if (producer->operation != MirOperation::Identity) {
      literal = producer;
      break;
    }
    if (producer->kind != MirInstructionKind::Compute ||
        producer->operands.size() != 1 ||
        producer->operands.front().kind != MirOperandKind::Value ||
        producer->info.type != producer->operands.front().type ||
        !producer->localFailureSites.empty() ||
        !producer->definedFailure.empty()) {
      return nullptr;
    }
    consumer = producer;
    operand = body.findValue(producer->operands.front().value);
  }
  const std::optional<std::uint64_t> minimum =
      signedMinimumMagnitude(negation.info.type);
  const auto *magnitude = literal != nullptr && literal->literal
                              ? std::get_if<std::uint64_t>(&*literal->literal)
                              : nullptr;
  if (literal == nullptr || literal->kind != MirInstructionKind::Compute ||
      literal->operation != MirOperation::Literal || !literal->result ||
      operand == nullptr || *literal->result != operand->id ||
      literal->info.type != negation.operands.front().type ||
      !literal->localFailureSites.empty() || !literal->definedFailure.empty() ||
      !minimum || magnitude == nullptr || *magnitude != *minimum ||
      (literal->info.type != negation.info.type &&
       !integerLiteralCarrierCanRepresent(literal->info.type, *magnitude))) {
    return nullptr;
  }
  return literal;
}

[[nodiscard]] const MirInstruction *
fusedSignedMinimumNegation(const MirBody &body, const MirInstruction &carrier) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &candidate : block.instructions) {
      const MirInstruction *literal =
          fusedSignedMinimumLiteral(body, candidate);
      if (literal == nullptr) {
        continue;
      }
      const MirValue *operand =
          body.findValue(candidate.operands.front().value);
      while (operand != nullptr) {
        const MirInstruction *producer = definitionFor(body, operand->id);
        if (producer == nullptr) {
          break;
        }
        if (producer == &carrier) {
          return &candidate;
        }
        if (producer == literal ||
            producer->operation != MirOperation::Identity ||
            producer->operands.size() != 1 ||
            producer->operands.front().kind != MirOperandKind::Value) {
          break;
        }
        operand = body.findValue(producer->operands.front().value);
      }
    }
  }
  return nullptr;
}

const MirInstruction *definitionFor(const MirBody &body, MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  const MirBlock *block =
      value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
  if (block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const MirInstruction &instruction) {
                     return instruction.id == value->definition;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

std::optional<MirPlaceId> borrowSourceForPlace(const MirBody &body,
                                               MirPlaceId placeId,
                                               std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  if (place->root == MirPlaceRootKind::Loan) {
    const MirLoan *loan = body.findLoan(place->loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  if (place->root == MirPlaceRootKind::Value) {
    const std::optional<MirPlaceId> source =
        borrowSourceForValue(body, place->value, depth + 1);
    return source ? source : std::optional<MirPlaceId>{placeId};
  }
  if (place->root == MirPlaceRootKind::Binding) {
    const MirLoan *carrier = nullptr;
    for (const MirLoan &loan : body.loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    place->binding) == loan.carriers.end()) {
        continue;
      }
      if (carrier != nullptr && carrier->source != loan.source) {
        return std::nullopt;
      }
      carrier = &loan;
    }
    if (carrier != nullptr) {
      return carrier->source;
    }
  }
  return placeId;
}

std::optional<MirPlaceId> borrowSourceForOperand(const MirBody &body,
                                                 const MirOperand &operand,
                                                 std::size_t depth) {
  switch (operand.kind) {
  case MirOperandKind::Loan: {
    const MirLoan *loan = body.findLoan(operand.loan);
    return loan == nullptr ? std::nullopt
                           : std::optional<MirPlaceId>{loan->source};
  }
  case MirOperandKind::Address:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
    return borrowSourceForPlace(body, operand.place, depth + 1);
  case MirOperandKind::Value:
    return borrowSourceForValue(body, operand.value, depth + 1);
  case MirOperandKind::Constant:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<MirPlaceId> borrowSourceForValue(const MirBody &body,
                                               MirValueId valueId,
                                               std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size()) {
    return std::nullopt;
  }
  const MirInstruction *definition = definitionFor(body, valueId);
  if (definition == nullptr) {
    return std::nullopt;
  }
  if (definition->loan) {
    const MirLoan *loan = body.findLoan(*definition->loan);
    if (loan != nullptr) {
      return loan->source;
    }
  }
  if (definition->operands.size() == 1) {
    return borrowSourceForOperand(body, definition->operands.front(),
                                  depth + 1);
  }
  return std::nullopt;
}

const MirLoan *rawBorrowResultLoan(const MirBody &body,
                                   const MirInstruction &instruction) {
  const bool mutableBorrow =
      instruction.intrinsic == IntrinsicKind::RawBorrowMut;
  if ((!mutableBorrow && instruction.intrinsic != IntrinsicKind::RawBorrow) ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.functionTarget || instruction.receiver ||
      instruction.destination || instruction.operands.size() != 2 ||
      instruction.parameterTypes.size() != 2 ||
      instruction.unsafeOperation != UnsafeOperationKind::RawDereference ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.lifecycle.empty() || !instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument != 1 || !instruction.borrowArguments.empty() ||
      !instruction.borrowFields.empty() || instruction.borrowPlace)
    return nullptr;
  const auto access =
      mutableBorrow ? AccessMode::Mutable : AccessMode::ReadOnly;
  const auto &pointer = instruction.operands[0];
  const auto &witness = instruction.operands[1];
  const auto *address = pointer.kind == MirOperandKind::Value
                            ? body.findValue(pointer.value)
                            : nullptr;
  const auto source = borrowSourceForOperand(body, witness);
  const auto *loan = body.findLoan(*instruction.loan);
  const auto *parent = witness.kind == MirOperandKind::Loan
                           ? body.findLoan(witness.loan)
                           : nullptr;
  if (!address || address->info.type != pointer.type ||
      pointer.type.kind != SemanticType::RawPointer ||
      pointer.type.arguments.size() != 1 ||
      pointer.type.arguments.front() == SemanticType::Void ||
      (mutableBorrow && pointer.type.pointerAccess != AccessMode::Mutable) ||
      instruction.parameterTypes[0] != pointer.type ||
      instruction.parameterTypes[1].kind != SemanticType::Reference ||
      instruction.parameterTypes[1].arguments.size() != 1 ||
      instruction.parameterTypes[1].referenceAccess != access ||
      witness.type != instruction.parameterTypes[1] ||
      instruction.info.type != pointer.type.arguments.front() ||
      instruction.info.category != ValueCategory::Place ||
      instruction.info.access != access || instruction.borrowAccess != access ||
      !source || !loan || loan->kind != MirLoanKind::CallResult ||
      loan->source != *source || loan->access != access ||
      loan->producedBy != instruction.hirValue || loan->entry ||
      loan->escapes || loan->lifecycleBound ||
      (witness.kind == MirOperandKind::Loan &&
       (!parent || loan->parent != witness.loan ||
        (mutableBorrow && parent->access != AccessMode::Mutable))))
    return nullptr;
  return loan;
}

} // namespace lang::mir_detail
