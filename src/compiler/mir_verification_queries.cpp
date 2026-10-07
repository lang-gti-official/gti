#include "mir_verification_queries.h"
#include "gti/mir_effects.h"

#include <algorithm>
#include <climits>

namespace lang::mir_detail {

bool bindingReceivedStoredLoan(const MirBody &body, HirBindingId binding,
                               MirLoanId dependency, std::size_t depth) {
  const auto *loan = body.findLoan(dependency);
  if (depth > body.places.size() || loan == nullptr ||
      loan->kind != MirLoanKind::Stored || !loan->lifecycleBound)
    return false;
  const MirInstruction *publication = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      const auto *place = instruction.destination
                              ? body.findPlace(*instruction.destination)
                              : nullptr;
      if (instruction.kind != MirInstructionKind::Initialize ||
          place == nullptr || place->root != MirPlaceRootKind::Binding ||
          place->binding != binding || !place->projections.empty())
        continue;
      if (publication != nullptr)
        return false;
      publication = &instruction;
    }
  if (publication == nullptr || publication->operands.size() != 1 ||
      publication->operands.front().kind != MirOperandKind::Value)
    return false;
  const auto *producer =
      definitionFor(body, publication->operands.front().value);
  const auto *destination = body.findPlace(*publication->destination);
  if (producer == nullptr || destination == nullptr ||
      producer->info.type != destination->type ||
      !destination->traits.containsBorrowedState)
    return false;
  if (producer->loan == dependency)
    return true;
  if (producer->kind != MirInstructionKind::Move ||
      producer->operands.size() != 1 ||
      producer->operands.front().kind != MirOperandKind::Move)
    return false;
  const auto *previous = body.findPlace(producer->operands.front().place);
  return previous != nullptr && previous->root == MirPlaceRootKind::Binding &&
         previous->projections.empty() && previous->type == destination->type &&
         bindingReceivedStoredLoan(body, previous->binding, dependency,
                                   depth + 1);
}

bool completeObjectType(const MirProgram &program,
                        const SemanticType &selected) {
  const auto eligible = [&](const auto &self, const SemanticType &type,
                            std::size_t depth) -> bool {
    if (depth > 128 || type.arrayLengthParameterId != 0)
      return false;
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::Bool:
    case SemanticType::Char:
      return type == SemanticType(type.kind) ||
             (type.nativeCScalar != NativeCScalarKind::None &&
              type == SemanticType::nativeCScalarType(type.nativeCScalar,
                                                      program.target()));
    case SemanticType::RawPointer:
      return type.arguments.size() == 1 &&
             type.arguments.front().kind != SemanticType::TypeParameter &&
             type.arguments.front().kind != SemanticType::Unknown;
    case SemanticType::Enum:
      return program.findIntegralEnum(type) != nullptr;
    case SemanticType::Array:
      return type.arguments.size() == 1 && type.arrayLength > 0 &&
             self(self, type.arguments.front(), depth + 1);
    case SemanticType::Class: {
      const MirClassInstance *owner = nullptr;
      for (const MirClassInstance &candidate : program.classInstances()) {
        if (candidate.type == type) {
          if (owner != nullptr)
            return false;
          owner = &candidate;
        }
      }
      return owner != nullptr && owner->kind != ClassKind::Interface &&
             !owner->abstract && !owner->cOpaqueHandle &&
             owner->compilerCapability == CompilerCapabilityTypeKind::None;
    }
    default:
      return false;
    }
  };
  return eligible(eligible, selected, 0);
}

bool objectRelocate(const MirProgram &program,
                    const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectRelocate ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.operands.size() != 2 ||
      instruction.parameterTypes.size() != 2)
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      !mirTypeRelocationIsDefinedFailureFree(program,
                                             pointer.arguments.front()) ||
      instruction.parameterTypes[1] != pointer ||
      !std::all_of(instruction.operands.begin(), instruction.operands.end(),
                   [&](const MirOperand &operand) {
                     return operand.kind == MirOperandKind::Value &&
                            operand.type == pointer;
                   }))
    return false;
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = IntrinsicKind::ObjectRelocate,
                             .info = instruction.info};
  return instruction == exact;
}

bool objectDestroy(const MirProgram &program,
                   const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectDestroy ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.result || instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1)
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.operands.front().type != pointer ||
      !instruction.definedFailure.localOrigins.empty() ||
      (instruction.definedFailure.propagation != FailurePropagationKind::None &&
       instruction.definedFailure.propagation !=
           FailurePropagationKind::Destructor))
    return false;
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = IntrinsicKind::ObjectDestroy,
                             .definedFailure = instruction.definedFailure,
                             .elementCleanup = instruction.elementCleanup,
                             .info = instruction.info};
  return instruction == exact;
}

bool objectLayoutQuery(const MirProgram &program,
                       const MirInstruction &instruction) {
  if ((instruction.intrinsic != IntrinsicKind::ObjectSize &&
       instruction.intrinsic != IntrinsicKind::ObjectAlignment) ||
      !instruction.objectLayoutType ||
      instruction.kind != MirInstructionKind::Call || !instruction.result ||
      instruction.info.type != SemanticType::UInt64 ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::UInt64))
    return false;
  // All unlisted fields must remain neutral, including future ownership,
  // failure and invocation metadata. Provenance IDs do not grant authority.
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .result = instruction.result,
                             .intrinsic = instruction.intrinsic,
                             .objectLayoutType = instruction.objectLayoutType,
                             .info = instruction.info};
  if (instruction != exact)
    return false;
  return completeObjectType(program, *instruction.objectLayoutType);
}

bool objectConstruct(const MirProgram &program,
                     const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::ObjectConstruct ||
      instruction.unsafeOperation != UnsafeOperationKind::ObjectLifetime ||
      instruction.result || instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.info.traits != semanticTraits(SemanticType::Void) ||
      instruction.info.updateArithmeticType != SemanticType::Unknown ||
      instruction.parameterTypes.empty() ||
      instruction.parameterTypes.size() != instruction.operands.size())
    return false;
  const auto &pointer = instruction.parameterTypes.front();
  if (pointer.kind != SemanticType::RawPointer ||
      pointer.pointerAccess != AccessMode::Mutable ||
      pointer.arguments.size() != 1 ||
      !completeObjectType(program, pointer.arguments.front()) ||
      !instruction.definedFailure.localOrigins.empty() ||
      (instruction.definedFailure.propagation != FailurePropagationKind::None &&
       instruction.definedFailure.propagation !=
           FailurePropagationKind::Constructor))
    return false;
  // Call-input/lifecycle verification proves every prepared parameter and its
  // handoff; the program verifier binds constructor identity/signature/failure.
  for (std::size_t index = 0; index < instruction.operands.size(); ++index)
    if (instruction.operands[index].kind != MirOperandKind::Value ||
        instruction.operands[index].type != instruction.parameterTypes[index])
      return false;
  const auto &element = pointer.arguments.front();
  if (element.kind == SemanticType::Class) {
    for (const auto &owner : program.classInstances()) {
      if (owner.type != element)
        continue;
      if (owner.traits.containsBorrowedState)
        return false;
      if (!instruction.constructorTarget) {
        const bool transfer =
            instruction.parameterTypes.size() == 2 &&
            instruction.parameterTypes[1] == element &&
            ((instruction.constructorKind == ConstructorKind::Copy &&
              owner.traits.copyable) ||
             (instruction.constructorKind == ConstructorKind::Move &&
              owner.traits.movable));
        const bool defaultConstruction =
            instruction.parameterTypes.size() == 1 &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            owner.defaultConstructor == SpecialMemberStatus::Generated &&
            !owner.generatedDefaultRequiresSchedule;
        if (!transfer && !defaultConstruction)
          return false;
      }
    }
  } else if (instruction.constructorTarget || !instruction.lifecycle.empty() ||
             instruction.constructorKind != ConstructorKind::Ordinary ||
             !instruction.definedFailure.empty() ||
             instruction.parameterTypes.size() > 2 ||
             (instruction.parameterTypes.size() == 2 &&
              instruction.parameterTypes[1] != element)) {
    return false;
  }
  const MirInstruction exact{.id = instruction.id,
                             .kind = MirInstructionKind::Call,
                             .hirValue = instruction.hirValue,
                             .hirStatement = instruction.hirStatement,
                             .callSite = instruction.callSite,
                             .unsafeOperation =
                                 UnsafeOperationKind::ObjectLifetime,
                             .operands = instruction.operands,
                             .parameterTypes = instruction.parameterTypes,
                             .intrinsic = IntrinsicKind::ObjectConstruct,
                             .definedFailure = instruction.definedFailure,
                             .constructorTarget = instruction.constructorTarget,
                             .constructorKind = instruction.constructorKind,
                             .info = instruction.info,
                             .lifecycle = instruction.lifecycle};
  return instruction == exact;
}

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
