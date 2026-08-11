#include "gti/mir.h"

#include <algorithm>
#include <iterator>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] MirVerificationResult
failure(const MirBody &body, std::size_t owner, std::string message,
        MirBlockId block = 0, MirInstructionId instruction = 0) {
  MirVerificationResult result;
  result.errors.push_back({.bodyKind = body.kind,
                           .owner = owner,
                           .block = block,
                           .instruction = instruction,
                           .message = std::move(message)});
  return result;
}

[[nodiscard]] std::vector<bool> reachableBlocks(const MirBody &body) {
  std::vector<bool> reachable(body.blocks.size(), false);
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return reachable;
  }

  std::queue<MirBlockId> pending;
  pending.push(body.entry);
  while (!pending.empty()) {
    const MirBlockId id = pending.front();
    pending.pop();
    if (id == 0 || id > body.blocks.size() || reachable[id - 1]) {
      continue;
    }

    reachable[id - 1] = true;
    const MirTerminator &terminator = body.blocks[id - 1].terminator;
    const auto enqueue = [&](MirBlockId target) {
      if (target != 0) {
        pending.push(target);
      }
    };
    switch (terminator.kind) {
    case MirTerminatorKind::Goto:
      enqueue(terminator.target);
      break;
    case MirTerminatorKind::Branch:
      enqueue(terminator.target);
      enqueue(terminator.elseTarget);
      break;
    case MirTerminatorKind::Switch:
      enqueue(terminator.target);
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        enqueue(target.target);
      }
      break;
    case MirTerminatorKind::None:
    case MirTerminatorKind::Return:
    case MirTerminatorKind::Unreachable:
    case MirTerminatorKind::Exit:
      break;
    }
  }
  return reachable;
}

void append(MirVerificationResult &destination, MirVerificationResult source) {
  destination.errors.insert(destination.errors.end(),
                            std::make_move_iterator(source.errors.begin()),
                            std::make_move_iterator(source.errors.end()));
}

using MirLoanState = std::vector<bool>;

[[nodiscard]] std::vector<MirBlockId>
successors(const MirTerminator &terminator) {
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    return {terminator.target};
  case MirTerminatorKind::Branch:
    return {terminator.target, terminator.elseTarget};
  case MirTerminatorKind::Switch: {
    std::vector<MirBlockId> result{terminator.target};
    result.reserve(terminator.switchTargets.size() + 1);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      result.push_back(target.target);
    }
    return result;
  }
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
    return {};
  }
  return {};
}

[[nodiscard]] MirVerificationResult verifyMirLoanFlow(const MirBody &body,
                                                      std::size_t owner) {
  if (body.loans.empty()) {
    return {};
  }

  std::vector<std::size_t> producerCounts(body.loans.size(), 0);
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  for (const MirLoan &loan : body.loans) {
    for (const HirBindingId carrier : loan.carriers) {
      bindingLoans[carrier].push_back(loan.id);
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!instruction.loan ||
          instruction.kind == MirInstructionKind::EndBorrow) {
        continue;
      }
      if (instruction.kind != MirInstructionKind::Borrow &&
          instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct) {
        return failure(body, owner,
                       "only borrow, call, and construct instructions may "
                       "produce a loan",
                       block.id, instruction.id);
      }
      ++producerCounts[*instruction.loan - 1];
    }
  }
  for (std::size_t index = 0; index < producerCounts.size(); ++index) {
    const MirLoan &loan = body.loans[index];
    const std::size_t expected = loan.entry ? 0 : 1;
    if (producerCounts[index] != expected) {
      return failure(body, owner,
                     "loan " + std::to_string(index + 1) +
                         (loan.entry ? " is an entry loan and must not have a "
                                       "producing instruction"
                                     : " must have exactly one producing "
                                       "instruction"));
    }
  }

  std::vector<std::optional<MirLoanState>> blockEntries(body.blocks.size());
  MirLoanState entryState(body.loans.size(), false);
  for (const MirLoan &loan : body.loans) {
    if (loan.entry) {
      entryState[loan.id - 1] = true;
    }
  }
  blockEntries[body.entry - 1] = std::move(entryState);
  std::queue<MirBlockId> pending;
  pending.push(body.entry);

  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock &block = body.blocks[blockId - 1];
    MirLoanState active = *blockEntries[blockId - 1];

    const auto requireActive =
        [&](MirLoanId loan, const char *context,
            MirInstructionId instruction =
                0) -> std::optional<MirVerificationResult> {
      if (!active[loan - 1]) {
        return failure(body, owner,
                       "loan " + std::to_string(loan) +
                           " is used after its borrow has ended in " + context,
                       block.id, instruction);
      }
      return std::nullopt;
    };
    const auto checkPlace = [&](MirPlaceId placeId, const char *context,
                                MirInstructionId instruction =
                                    0) -> std::optional<MirVerificationResult> {
      const MirPlace &place = *body.findPlace(placeId);
      if (place.root == MirPlaceRootKind::Loan) {
        if (auto invalid = requireActive(place.loan, context, instruction)) {
          return invalid;
        }
      }
      if (place.root == MirPlaceRootKind::Binding) {
        const auto found = bindingLoans.find(place.binding);
        if (found != bindingLoans.end()) {
          for (const MirLoanId loan : found->second) {
            if (auto invalid = requireActive(loan, context, instruction)) {
              return invalid;
            }
          }
        }
      }
      return std::nullopt;
    };
    const auto checkOperand =
        [&](const MirOperand &operand, const char *context,
            MirInstructionId instruction =
                0) -> std::optional<MirVerificationResult> {
      switch (operand.kind) {
      case MirOperandKind::Loan:
        return requireActive(operand.loan, context, instruction);
      case MirOperandKind::Copy:
      case MirOperandKind::Move:
      case MirOperandKind::Address:
      case MirOperandKind::BorrowRead:
      case MirOperandKind::BorrowWrite:
        return checkPlace(operand.place, context, instruction);
      case MirOperandKind::Value:
      case MirOperandKind::Constant:
        return std::nullopt;
      }
      return std::nullopt;
    };

    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::EndBorrow) {
        const MirLoanId loan = *instruction.loan;
        if (!active[loan - 1]) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is ended while it is inactive",
                         block.id, instruction.id);
        }
        active[loan - 1] = false;
        continue;
      }

      if (instruction.receiver) {
        if (auto invalid = checkOperand(*instruction.receiver, "a receiver",
                                        instruction.id)) {
          return *invalid;
        }
      }
      for (const MirOperand &operand : instruction.operands) {
        if (auto invalid = checkOperand(operand, "an instruction operand",
                                        instruction.id)) {
          return *invalid;
        }
      }
      if (instruction.destination &&
          instruction.kind != MirInstructionKind::Drop) {
        if (auto invalid =
                checkPlace(*instruction.destination,
                           "an instruction destination", instruction.id)) {
          return *invalid;
        }
      }

      if (instruction.loan) {
        const MirLoanId loan = *instruction.loan;
        if (active[loan - 1]) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is produced while it is already active",
                         block.id, instruction.id);
        }
        active[loan - 1] = true;
      }
    }

    if (block.terminator.value) {
      if (auto invalid =
              checkOperand(*block.terminator.value, "a terminator")) {
        return *invalid;
      }
    }
    if (block.terminator.returnLoan) {
      if (auto invalid = requireActive(*block.terminator.returnLoan,
                                       "a return dependency")) {
        return *invalid;
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Return ||
        block.terminator.kind == MirTerminatorKind::Exit) {
      for (const MirLoan &loan : body.loans) {
        if (!active[loan.id - 1]) {
          continue;
        }
        if (!loan.escapes) {
          return failure(body, owner,
                         "non-escaping loan " + std::to_string(loan.id) +
                             " remains active at a normal body exit",
                         block.id);
        }
        if (loan.kind != MirLoanKind::Stored &&
            (!block.terminator.returnLoan ||
             *block.terminator.returnLoan != loan.id)) {
          return failure(body, owner,
                         "escaping loan " + std::to_string(loan.id) +
                             " remains active on a return edge that does not "
                             "return it",
                         block.id);
        }
      }
    }

    for (const MirBlockId successor : successors(block.terminator)) {
      std::optional<MirLoanState> &entry = blockEntries[successor - 1];
      if (!entry) {
        entry = active;
        pending.push(successor);
        continue;
      }
      if (*entry == active) {
        continue;
      }
      const auto mismatch =
          std::mismatch(entry->begin(), entry->end(), active.begin());
      const MirLoanId loan = static_cast<MirLoanId>(std::distance(
                                 entry->begin(), mismatch.first)) +
                             1;
      return failure(body, owner,
                     "loan " + std::to_string(loan) +
                         " has inconsistent active state at CFG join",
                     successor);
    }
  }
  return {};
}

} // namespace

void rebuildMirReachability(MirBody &body) {
  const std::vector<bool> reachable = reachableBlocks(body);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    body.blocks[index].reachable = reachable[index];
  }
}

bool rebuildMirValueUses(MirBody &body) {
  body.valueUses.assign(body.values.size(), {});
  bool valid = true;
  const auto addUse = [&](MirValueUse use) {
    if (use.value == 0 || use.value > body.valueUses.size()) {
      valid = false;
      return;
    }
    body.valueUses[use.value - 1].push_back(std::move(use));
  };

  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value) {
      addUse({.value = place.value,
              .kind = MirValueUseKind::PlaceRoot,
              .place = place.id});
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if (projection.kind == MirProjectionKind::Index ||
          projection.kind == MirProjectionKind::RawIndex) {
        addUse({.value = projection.index,
                .kind = MirValueUseKind::PlaceIndex,
                .place = place.id});
      }
    }
  }

  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value) {
        addUse({.value = instruction.receiver->value,
                .kind = MirValueUseKind::InstructionReceiver,
                .block = block.id,
                .instruction = instruction.id});
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind == MirOperandKind::Value) {
          addUse({.value = instruction.operands[index].value,
                  .kind = MirValueUseKind::InstructionOperand,
                  .block = block.id,
                  .instruction = instruction.id,
                  .operandIndex = index});
        }
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value) {
      addUse({.value = block.terminator.value->value,
              .kind = MirValueUseKind::Terminator,
              .block = block.id});
    }
  }
  return valid;
}

MirVerificationResult verifyMirBody(const MirBody &body, std::size_t owner) {
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return failure(body, owner, "entry block is outside the body");
  }

  const auto validPlace = [&](MirPlaceId id) {
    return body.findPlace(id) != nullptr;
  };
  const auto validLoan = [&](MirLoanId id) {
    return body.findLoan(id) != nullptr;
  };
  const auto validValue = [&](MirValueId id) {
    return body.findValue(id) != nullptr;
  };
  const auto validOperand = [&](const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value: {
      const MirValue *value = body.findValue(operand.value);
      return value != nullptr && operand.type == value->info.type;
    }
    case MirOperandKind::Constant:
      return operand.literal.has_value();
    case MirOperandKind::Address:
      return validPlace(operand.place);
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      return validPlace(operand.place);
    case MirOperandKind::Loan:
      return validLoan(operand.loan);
    }
    return false;
  };
  const auto isBinaryOperation = [](MirOperation operation) {
    switch (operation) {
    case MirOperation::Comma:
    case MirOperation::Add:
    case MirOperation::Subtract:
    case MirOperation::Multiply:
    case MirOperation::Divide:
    case MirOperation::Remainder:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::ShiftLeft:
    case MirOperation::ShiftRight:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Index:
    case MirOperation::PointerAdd:
    case MirOperation::PointerSubtract:
    case MirOperation::PointerDifference:
      return true;
    case MirOperation::None:
    case MirOperation::Literal:
    case MirOperation::EnumConstant:
    case MirOperation::Aggregate:
    case MirOperation::Identity:
    case MirOperation::Convert:
    case MirOperation::ExpectedHasValue:
    case MirOperation::Closure:
    case MirOperation::PackExpansion:
    case MirOperation::Unexpected:
    case MirOperation::AddressOf:
    case MirOperation::Positive:
    case MirOperation::Negate:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
    case MirOperation::Assign:
    case MirOperation::AddAssign:
    case MirOperation::SubtractAssign:
    case MirOperation::MultiplyAssign:
    case MirOperation::DivideAssign:
    case MirOperation::RemainderAssign:
    case MirOperation::BitwiseAndAssign:
    case MirOperation::BitwiseOrAssign:
    case MirOperation::BitwiseXorAssign:
    case MirOperation::ShiftLeftAssign:
    case MirOperation::ShiftRightAssign:
    case MirOperation::PreIncrement:
    case MirOperation::PreDecrement:
    case MirOperation::PostIncrement:
    case MirOperation::PostDecrement:
    case MirOperation::Count:
      return false;
    }
    return false;
  };
  const auto isUnaryOperation = [](MirOperation operation) {
    switch (operation) {
    case MirOperation::Identity:
    case MirOperation::Convert:
    case MirOperation::ExpectedHasValue:
    case MirOperation::Unexpected:
    case MirOperation::Positive:
    case MirOperation::Negate:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
    case MirOperation::AddressOf:
      return true;
    default:
      return false;
    }
  };
  const auto isIntegerType = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
      return true;
    default:
      return false;
    }
  };
  const auto validRawPointer = [](const SemanticType &type) {
    return type.kind == SemanticType::RawPointer &&
           type.arguments.size() == 1 &&
           type.arguments.front() != SemanticType::Void;
  };
  const auto validRawMemberCallReceiver =
      [&](const MirInstruction &instruction) {
        if (instruction.kind != MirInstructionKind::Call ||
            instruction.unsafeOperation != UnsafeOperationKind::RawMember) {
          return true;
        }
        if (!instruction.rawMemoryAccess || !instruction.receiver ||
            instruction.receiver->place == 0 ||
            instruction.receiver->type.kind != SemanticType::Class) {
          return false;
        }
        const MirPlace *place = body.findPlace(instruction.receiver->place);
        if (place == nullptr || place->root != MirPlaceRootKind::Value ||
            place->projections.size() != 1 ||
            place->projections.front().kind !=
                MirProjectionKind::RawDereference ||
            place->type != instruction.receiver->type) {
          return false;
        }
        const MirValue *pointer = body.findValue(place->value);
        return pointer != nullptr && validRawPointer(pointer->info.type) &&
               pointer->info.type.arguments.front() == place->type &&
               pointer->info.type.pointerAccess == place->access &&
               (instruction.receiver->kind != MirOperandKind::BorrowWrite ||
                place->access == AccessMode::Mutable);
      };
  const auto validCompute = [&](const MirInstruction &instruction) {
    if (!instruction.result || instruction.operation == MirOperation::None ||
        instruction.operation == MirOperation::Count) {
      return false;
    }
    const MirValue *result = body.findValue(*instruction.result);
    if (result == nullptr) {
      return false;
    }
    const SemanticType &resultType = result->info.type;
    if (instruction.operation == MirOperation::AddressOf) {
      if (instruction.operands.size() != 1 ||
          instruction.operands.front().kind != MirOperandKind::Address ||
          resultType.kind != SemanticType::RawPointer ||
          resultType.arguments.size() != 1 ||
          resultType.arguments.front() != instruction.operands.front().type) {
        return false;
      }
      const MirPlace *place =
          body.findPlace(instruction.operands.front().place);
      return place != nullptr && resultType.pointerAccess == place->access;
    }
    if (instruction.operation == MirOperation::PointerAdd) {
      if (instruction.operands.size() != 2 || !validRawPointer(resultType)) {
        return false;
      }
      const SemanticType &left = instruction.operands[0].type;
      const SemanticType &right = instruction.operands[1].type;
      return (left == resultType && isIntegerType(right)) ||
             (isIntegerType(left) && right == resultType);
    }
    if (instruction.operation == MirOperation::PointerSubtract) {
      return instruction.operands.size() == 2 && validRawPointer(resultType) &&
             instruction.operands[0].type == resultType &&
             isIntegerType(instruction.operands[1].type);
    }
    if (instruction.operation == MirOperation::PointerDifference) {
      return instruction.operands.size() == 2 &&
             resultType == SemanticType::Int64 &&
             validRawPointer(instruction.operands[0].type) &&
             instruction.operands[0].type == instruction.operands[1].type;
    }
    if (isBinaryOperation(instruction.operation)) {
      return instruction.operands.size() == 2;
    }
    if (isUnaryOperation(instruction.operation)) {
      return instruction.operands.size() == 1;
    }
    switch (instruction.operation) {
    case MirOperation::Literal:
      return instruction.operands.empty() && instruction.literal.has_value();
    case MirOperation::EnumConstant:
      return instruction.operands.empty() && instruction.enumOwner &&
             *instruction.enumOwner != 0 && instruction.enumValue;
    case MirOperation::Aggregate:
      return true;
    case MirOperation::Closure:
      return instruction.lambdaTarget && *instruction.lambdaTarget != 0;
    case MirOperation::PackExpansion:
      return instruction.operands.empty();
    default:
      return false;
    }
  };
  const auto validInstructionShape = [&](const MirInstruction &instruction) {
    const bool hasResult = instruction.result.has_value();
    const bool noOperation = instruction.operation == MirOperation::None;
    switch (instruction.kind) {
    case MirInstructionKind::Compute:
      return !instruction.destination && !instruction.receiver &&
             !instruction.loan && !instruction.functionTarget &&
             !instruction.constructorTarget &&
             (instruction.operation == MirOperation::Closure ||
              !instruction.lambdaTarget) &&
             validCompute(instruction);
    case MirInstructionKind::Load:
      return noOperation && hasResult && !instruction.destination &&
             instruction.operands.size() == 1 &&
             instruction.operands.front().kind == MirOperandKind::Copy;
    case MirInstructionKind::Initialize:
      return noOperation && !hasResult && instruction.destination &&
             instruction.operands.size() <= 1;
    case MirInstructionKind::Assign:
      return hasResult && instruction.destination &&
             instruction.operands.size() == 1 &&
             (instruction.operation == MirOperation::Assign ||
              instruction.operation == MirOperation::AddAssign ||
              instruction.operation == MirOperation::SubtractAssign ||
              instruction.operation == MirOperation::MultiplyAssign ||
              instruction.operation == MirOperation::DivideAssign ||
              instruction.operation == MirOperation::RemainderAssign ||
              instruction.operation == MirOperation::BitwiseAndAssign ||
              instruction.operation == MirOperation::BitwiseOrAssign ||
              instruction.operation == MirOperation::BitwiseXorAssign ||
              instruction.operation == MirOperation::ShiftLeftAssign ||
              instruction.operation == MirOperation::ShiftRightAssign);
    case MirInstructionKind::Modify:
      return hasResult && instruction.destination &&
             instruction.operands.empty() &&
             (instruction.operation == MirOperation::PreIncrement ||
              instruction.operation == MirOperation::PreDecrement ||
              instruction.operation == MirOperation::PostIncrement ||
              instruction.operation == MirOperation::PostDecrement);
    case MirInstructionKind::Move:
      return noOperation && hasResult && !instruction.destination &&
             instruction.operands.size() == 1 &&
             instruction.operands.front().kind == MirOperandKind::Move;
    case MirInstructionKind::Borrow:
      return noOperation && !hasResult && instruction.loan &&
             instruction.operands.size() == 1 &&
             (instruction.operands.front().kind == MirOperandKind::BorrowRead ||
              instruction.operands.front().kind == MirOperandKind::BorrowWrite);
    case MirInstructionKind::Call:
      return noOperation &&
             hasResult == (instruction.info.type.kind != SemanticType::Void) &&
             (!instruction.constructorTarget ||
              ((instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
                instruction.intrinsic == IntrinsicKind::StorageConstruct) &&
               !instruction.functionTarget && !instruction.lambdaTarget)) &&
             std::all_of(instruction.nonEscapingArguments.begin(),
                         instruction.nonEscapingArguments.end(),
                         [&](std::size_t index) {
                           return index < instruction.operands.size();
                         }) &&
             (instruction.dispatch != CallDispatch::Virtual ||
              (instruction.functionTarget && instruction.receiver &&
               instruction.dispatchOwner.kind == SemanticType::Class)) &&
             validRawMemberCallReceiver(instruction);
    case MirInstructionKind::Construct:
      return noOperation && hasResult &&
             instruction.info.type.kind == SemanticType::Class &&
             instruction.intrinsic == IntrinsicKind::None &&
             !instruction.functionTarget && !instruction.lambdaTarget &&
             (instruction.constructorKind == ConstructorKind::Ordinary ||
              (!instruction.constructorTarget &&
               instruction.operands.size() == 1));
    case MirInstructionKind::Drop:
      return noOperation && !hasResult && instruction.destination &&
             instruction.operands.empty();
    case MirInstructionKind::EndBorrow:
      return noOperation && !hasResult && instruction.loan &&
             instruction.operands.empty();
    case MirInstructionKind::Count:
      return false;
    }
    return false;
  };

  std::size_t expectedUseCount = 0;
  for (std::size_t index = 0; index < body.places.size(); ++index) {
    const MirPlace &place = body.places[index];
    if (place.id != index + 1 ||
        (place.root == MirPlaceRootKind::Binding && place.binding == 0) ||
        (place.root == MirPlaceRootKind::Symbol && place.symbol == 0) ||
        (place.root == MirPlaceRootKind::Temporary && place.temporary == 0) ||
        (place.root == MirPlaceRootKind::Value && !validValue(place.value)) ||
        (place.root == MirPlaceRootKind::Loan && !validLoan(place.loan))) {
      return failure(body, owner,
                     "place " + std::to_string(place.id) +
                         " has an invalid identity or root");
    }
    expectedUseCount += place.root == MirPlaceRootKind::Value ? 1 : 0;
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Field &&
           projection.field == 0) ||
          ((projection.kind == MirProjectionKind::Index ||
            projection.kind == MirProjectionKind::RawIndex) &&
           !validValue(projection.index))) {
        return failure(body, owner,
                       "place " + std::to_string(place.id) +
                           " has an invalid projection");
      }
      if (projection.kind == MirProjectionKind::RawDereference ||
          projection.kind == MirProjectionKind::RawIndex) {
        const bool firstProjection = &projection == &place.projections.front();
        const MirValue *root = place.root == MirPlaceRootKind::Value
                                   ? body.findValue(place.value)
                                   : nullptr;
        if (!firstProjection || root == nullptr ||
            !validRawPointer(root->info.type)) {
          return failure(body, owner,
                         "place " + std::to_string(place.id) +
                             " has a raw projection without a valid raw "
                             "pointer root");
        }
        if (projection.kind == MirProjectionKind::RawIndex) {
          const MirValue *indexValue = body.findValue(projection.index);
          if (indexValue == nullptr || !isIntegerType(indexValue->info.type)) {
            return failure(body, owner,
                           "place " + std::to_string(place.id) +
                               " has a non-integer raw pointer index");
          }
        }
      }
      expectedUseCount += projection.kind == MirProjectionKind::Index ||
                                  projection.kind == MirProjectionKind::RawIndex
                              ? 1
                              : 0;
    }
  }

  std::unordered_set<SemanticLoanId> semanticLoans;
  for (std::size_t index = 0; index < body.loans.size(); ++index) {
    const MirLoan &loan = body.loans[index];
    if (loan.id != index + 1 || !validPlace(loan.source)) {
      return failure(body, owner,
                     "loan " + std::to_string(loan.id) +
                         " has an invalid identity or source place");
    }
    std::unordered_set<HirBindingId> carriers;
    for (const HirBindingId carrier : loan.carriers) {
      if (carrier == 0 || !carriers.insert(carrier).second) {
        return failure(body, owner,
                       "loan " + std::to_string(loan.id) +
                           " has an invalid or duplicate carrier binding");
      }
    }
    if (loan.semanticLoan != 0 &&
        !semanticLoans.insert(loan.semanticLoan).second) {
      return failure(body, owner,
                     "semantic loan " + std::to_string(loan.semanticLoan) +
                         " is represented by more than one MIR loan");
    }
    if (loan.entry) {
      const MirPlace *source = body.findPlace(loan.source);
      if (loan.producedBy != 0 || source == nullptr ||
          source->root != MirPlaceRootKind::Binding ||
          std::find(loan.carriers.begin(), loan.carriers.end(),
                    source->binding) == loan.carriers.end()) {
        return failure(body, owner,
                       "entry loan " + std::to_string(loan.id) +
                           " must originate at a formal parameter binding "
                           "that remains one of its carriers");
      }
    }
    if (loan.kind == MirLoanKind::Parameter && !loan.entry) {
      return failure(body, owner,
                     "parameter loan " + std::to_string(loan.id) +
                         " is not active at body entry");
    }
  }

  if (body.valueUses.size() != body.values.size()) {
    return failure(body, owner,
                   "value-use index size does not match the value table");
  }
  std::vector<std::size_t> definitionCounts(body.values.size(), 0);
  for (std::size_t index = 0; index < body.values.size(); ++index) {
    const MirValue &value = body.values[index];
    if (value.id != index + 1 || value.sourceValue == 0 ||
        value.definitionBlock == 0 || value.definition == 0) {
      return failure(body, owner,
                     "value " + std::to_string(value.id) +
                         " has an invalid identity, provenance, or definition");
    }
    for (const MirValueUse &use : body.valueUses[index]) {
      if (use.value != value.id) {
        return failure(body, owner,
                       "value-use index is stored under the wrong value");
      }
    }
  }

  std::unordered_set<MirInstructionId> instructionIds;
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const MirBlock &block = body.blocks[index];
    if (block.id != index + 1) {
      return failure(body, owner,
                     "block identity does not match stored block order");
    }
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.id == 0 ||
          !instructionIds.insert(instruction.id).second ||
          instruction.intrinsic == IntrinsicKind::Count ||
          (instruction.destination && !validPlace(*instruction.destination)) ||
          (instruction.receiver && !validOperand(*instruction.receiver)) ||
          (instruction.loan && !validLoan(*instruction.loan)) ||
          (instruction.result && !validValue(*instruction.result)) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return !validOperand(operand);
                      }) ||
          !validInstructionShape(instruction)) {
        return failure(body, owner,
                       "instruction has an invalid shape or reference",
                       block.id, instruction.id);
      }
      if (instruction.result) {
        const MirValue &result = body.values[*instruction.result - 1];
        if (result.definitionBlock != block.id ||
            result.definition != instruction.id ||
            result.info.type != instruction.info.type) {
          return failure(
              body, owner,
              "instruction result does not match its value definition",
              block.id, instruction.id);
        }
        ++definitionCounts[*instruction.result - 1];
      }
      expectedUseCount += static_cast<std::size_t>(instruction.receiver &&
                                                   instruction.receiver->kind ==
                                                       MirOperandKind::Value);
      expectedUseCount += static_cast<std::size_t>(std::count_if(
          instruction.operands.begin(), instruction.operands.end(),
          [](const MirOperand &operand) {
            return operand.kind == MirOperandKind::Value;
          }));
    }

    if (block.terminator.kind == MirTerminatorKind::None) {
      return failure(body, owner, "block has no terminator", block.id);
    }
    const auto validTarget = [&](MirBlockId target) {
      return target > 0 && target <= body.blocks.size();
    };
    if (block.terminator.kind == MirTerminatorKind::Goto &&
        !validTarget(block.terminator.target)) {
      return failure(body, owner, "goto target is outside the body", block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Branch &&
        (!validTarget(block.terminator.target) ||
         !validTarget(block.terminator.elseTarget) || !block.terminator.value ||
         !validOperand(*block.terminator.value) ||
         block.terminator.value->type != SemanticType::Bool)) {
      return failure(body, owner, "branch condition or target is invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Switch &&
        (!block.terminator.value || !validOperand(*block.terminator.value) ||
         !validTarget(block.terminator.target) ||
         std::any_of(block.terminator.switchTargets.begin(),
                     block.terminator.switchTargets.end(),
                     [&](const MirSwitchTarget &target) {
                       return !validTarget(target.target);
                     }))) {
      return failure(body, owner, "switch value or target is invalid",
                     block.id);
    }
    if ((block.terminator.returnLoan &&
         block.terminator.kind != MirTerminatorKind::Return) ||
        (block.terminator.returnLoan &&
         !validLoan(*block.terminator.returnLoan))) {
      return failure(body, owner, "return dependency loan is invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Return) {
      if (block.terminator.value && !validOperand(*block.terminator.value)) {
        return failure(body, owner, "return operand is invalid", block.id);
      }
      if (block.terminator.returnLoan) {
        const MirLoan &loan = *body.findLoan(*block.terminator.returnLoan);
        if (loan.kind != MirLoanKind::Return || !loan.escapes) {
          return failure(body, owner,
                         "return dependency loan is not marked as escaping",
                         block.id);
        }
      }
    }
    expectedUseCount += static_cast<std::size_t>(block.terminator.value &&
                                                 block.terminator.value->kind ==
                                                     MirOperandKind::Value);
  }

  if (std::any_of(definitionCounts.begin(), definitionCounts.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "every value must have exactly one instruction definition");
  }

  const std::vector<bool> expectedReachability = reachableBlocks(body);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (body.blocks[index].reachable != expectedReachability[index]) {
      return failure(body, owner, "block reachability index is stale",
                     body.blocks[index].id);
    }
  }

  std::size_t indexedUseCount = 0;
  for (const std::vector<MirValueUse> &uses : body.valueUses) {
    for (const MirValueUse &use : uses) {
      const MirBlock *block = body.findBlock(use.block);
      const MirPlace *place = body.findPlace(use.place);
      const MirInstruction *instruction = nullptr;
      if (block != nullptr && use.instruction != 0) {
        const auto found =
            std::find_if(block->instructions.begin(), block->instructions.end(),
                         [&](const MirInstruction &candidate) {
                           return candidate.id == use.instruction;
                         });
        instruction = found == block->instructions.end() ? nullptr : &*found;
      }
      switch (use.kind) {
      case MirValueUseKind::InstructionOperand:
        if (instruction == nullptr ||
            use.operandIndex >= instruction->operands.size() ||
            instruction->operands[use.operandIndex].kind !=
                MirOperandKind::Value ||
            instruction->operands[use.operandIndex].value != use.value) {
          return failure(body, owner,
                         "indexed instruction operand use does not match MIR");
        }
        break;
      case MirValueUseKind::InstructionReceiver:
        if (instruction == nullptr || !instruction->receiver ||
            instruction->receiver->kind != MirOperandKind::Value ||
            instruction->receiver->value != use.value) {
          return failure(body, owner,
                         "indexed receiver use does not match MIR");
        }
        break;
      case MirValueUseKind::Terminator:
        if (block == nullptr || !block->terminator.value ||
            block->terminator.value->kind != MirOperandKind::Value ||
            block->terminator.value->value != use.value) {
          return failure(body, owner,
                         "indexed terminator use does not match MIR");
        }
        break;
      case MirValueUseKind::PlaceRoot:
        if (place == nullptr || place->root != MirPlaceRootKind::Value ||
            place->value != use.value) {
          return failure(body, owner,
                         "indexed place-root use does not match MIR");
        }
        break;
      case MirValueUseKind::PlaceIndex:
        if (place == nullptr ||
            std::none_of(
                place->projections.begin(), place->projections.end(),
                [&](const MirPlaceProjection &projection) {
                  return (projection.kind == MirProjectionKind::Index ||
                          projection.kind == MirProjectionKind::RawIndex) &&
                         projection.index == use.value;
                })) {
          return failure(body, owner,
                         "indexed place-projection use does not match MIR");
        }
        break;
      }
    }
    indexedUseCount += uses.size();
  }
  if (indexedUseCount != expectedUseCount) {
    return failure(body, owner,
                   "value-use index count does not match MIR operands");
  }
  return verifyMirLoanFlow(body, owner);
}

namespace {

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  MirValueId valueId) {
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

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId valueId,
                     std::size_t depth);

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForPlace(const MirBody &body, MirPlaceId placeId,
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

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForOperand(const MirBody &body, const MirOperand &operand,
                       std::size_t depth = 0) {
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

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId valueId,
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

[[nodiscard]] MirVerificationResult
verifyMirBorrowProducers(const MirProgram &program, const MirBody &body,
                         std::size_t owner) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct) {
        if (instruction.borrowOrigin != BorrowOriginKind::None) {
          return failure(body, owner,
                         "only call and construct instructions may carry a "
                         "borrow-result origin",
                         block.id, instruction.id);
        }
        continue;
      }

      const MirFunctionInstance *target = nullptr;
      if (instruction.functionTarget) {
        target = program.findFunctionInstance(*instruction.functionTarget);
        if (target == nullptr) {
          return failure(body, owner,
                         "call borrow contract references an invalid function "
                         "target",
                         block.id, instruction.id);
        }
        if (instruction.borrowOrigin != target->returnBorrowOrigin ||
            instruction.borrowArgument != target->returnBorrowParameter ||
            instruction.borrowAccess != target->returnBorrowAccess) {
          return failure(body, owner,
                         "call-result borrow origin does not match the target "
                         "function summary",
                         block.id, instruction.id);
        }
      }
      const MirConstructorInstance *constructorTarget = nullptr;
      if (instruction.constructorTarget) {
        constructorTarget =
            program.findConstructorInstance(*instruction.constructorTarget);
        if (constructorTarget == nullptr) {
          return failure(body, owner,
                         "construct borrow contract references an invalid "
                         "constructor target",
                         block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Construct &&
            (instruction.borrowOrigin != constructorTarget->borrowOrigin ||
             instruction.borrowArgument != constructorTarget->borrowParameter ||
             instruction.borrowAccess != constructorTarget->borrowAccess)) {
          return failure(body, owner,
                         "construct-result borrow origin does not match the "
                         "target constructor summary",
                         block.id, instruction.id);
        }
      }

      if (instruction.borrowOrigin == BorrowOriginKind::None) {
        if (instruction.loan) {
          return failure(body, owner,
                         "call or construct produces a loan without a borrow "
                         "origin",
                         block.id, instruction.id);
        }
        continue;
      }
      if (!instruction.loan) {
        return failure(body, owner,
                       "owner-dependent call or construct is missing its "
                       "result loan",
                       block.id, instruction.id);
      }
      const MirLoan *loan = body.findLoan(*instruction.loan);
      if (loan == nullptr || loan->access != instruction.borrowAccess) {
        return failure(body, owner,
                       "call-result loan access does not match its borrow "
                       "origin",
                       block.id, instruction.id);
      }

      std::optional<MirPlaceId> expectedSource;
      if (instruction.borrowOrigin == BorrowOriginKind::Receiver) {
        if (instruction.kind != MirInstructionKind::Call ||
            !instruction.receiver) {
          return failure(body, owner,
                         "receiver-dependent result has no call receiver",
                         block.id, instruction.id);
        }
        expectedSource = borrowSourceForOperand(body, *instruction.receiver);
      } else if (instruction.borrowOrigin == BorrowOriginKind::Argument) {
        if (instruction.borrowArgument >= instruction.operands.size()) {
          return failure(body, owner,
                         "owner-dependent argument index is outside the call "
                         "operands",
                         block.id, instruction.id);
        }
        expectedSource = borrowSourceForOperand(
            body, instruction.operands[instruction.borrowArgument]);
      }
      if (!expectedSource || *expectedSource != loan->source) {
        const std::string expected =
            expectedSource ? std::to_string(*expectedSource) : "unresolved";
        return failure(body, owner,
                       "call-result loan does not preserve the selected "
                       "receiver or argument source identity (expected " +
                           expected + ", found " +
                           std::to_string(loan->source) + ")",
                       block.id, instruction.id);
      }
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirFunctionBorrowSummary(const MirProgram &program,
                               const MirFunctionInstance &instance) {
  const MirBody &body = instance.body;
  const auto invalidSummary = [&](std::string message) {
    return failure(body, instance.id, std::move(message));
  };
  switch (instance.returnBorrowOrigin) {
  case BorrowOriginKind::None:
    if (instance.returnBorrowParameter != 0) {
      return invalidSummary(
          "function without a return dependency has a nonzero origin index");
    }
    break;
  case BorrowOriginKind::Receiver:
    if (!instance.owner || instance.staticMember ||
        *instance.owner > program.classInstances().size() ||
        instance.returnBorrowParameter != 0) {
      return invalidSummary(
          "receiver return dependency requires a non-static class method");
    }
    break;
  case BorrowOriginKind::Argument:
    if (instance.returnBorrowParameter >= instance.parameterTypes.size()) {
      return invalidSummary(
          "return dependency argument index is outside the formal parameters");
    }
    if ((!instance.owner || instance.staticMember) &&
        instance.returnBorrowAccess != AccessMode::ReadOnly) {
      return invalidSummary(
          "ordinary free or static return dependency must be read-only");
    }
    break;
  }

  std::unordered_set<MirLoanId> returnedLoans;
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind != MirTerminatorKind::Return) {
      continue;
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::None) {
      if (block.terminator.returnLoan) {
        return failure(body, instance.id,
                       "return escapes a loan but the function has no return "
                       "dependency summary",
                       block.id);
      }
      continue;
    }
    if (!block.terminator.returnLoan) {
      return failure(body, instance.id,
                     "owner-dependent return is missing its escaping loan",
                     block.id);
    }
    const MirLoan *loan = body.findLoan(*block.terminator.returnLoan);
    if (loan == nullptr || loan->kind != MirLoanKind::Return ||
        !loan->escapes || loan->access != instance.returnBorrowAccess) {
      return failure(body, instance.id,
                     "owner-dependent return loan does not match the function "
                     "summary",
                     block.id);
    }
    const std::optional<MirPlaceId> canonicalSource =
        borrowSourceForPlace(body, loan->source, 0);
    const MirPlace *source =
        canonicalSource ? body.findPlace(*canonicalSource) : nullptr;
    if (source == nullptr) {
      return failure(body, instance.id,
                     "owner-dependent return loan has no source identity",
                     block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Receiver &&
        source->root != MirPlaceRootKind::This) {
      return failure(body, instance.id,
                     "receiver-dependent return does not originate from this",
                     block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Argument &&
        (instance.returnBorrowParameter >= instance.parameterBindings.size() ||
         source->root != MirPlaceRootKind::Binding ||
         source->binding !=
             instance.parameterBindings[instance.returnBorrowParameter])) {
      return failure(body, instance.id,
                     "argument-dependent return does not originate from the "
                     "summarized formal parameter",
                     block.id);
    }
    returnedLoans.insert(loan->id);
  }
  for (const MirLoan &loan : body.loans) {
    if (loan.kind == MirLoanKind::Return && loan.escapes &&
        !returnedLoans.contains(loan.id)) {
      return invalidSummary(
          "escaping return loan is not attached to a return terminator");
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirConstructorBorrowSummary(const MirConstructorInstance &instance) {
  const MirBody &body = instance.body;
  const auto invalidSummary = [&](std::string message) {
    return failure(body, instance.id, std::move(message));
  };
  if (instance.parameterBindings.size() != instance.parameterTypes.size()) {
    return invalidSummary(
        "constructor parameter bindings do not match its formal parameters");
  }
  switch (instance.borrowOrigin) {
  case BorrowOriginKind::None:
    if (instance.borrowParameter != 0) {
      return invalidSummary(
          "constructor without a borrow dependency has a nonzero origin "
          "index");
    }
    break;
  case BorrowOriginKind::Receiver:
    return invalidSummary(
        "constructor result cannot depend on a receiver lifetime");
  case BorrowOriginKind::Argument:
    if (instance.borrowParameter >= instance.parameterTypes.size()) {
      return invalidSummary(
          "constructor borrow dependency is outside the formal parameters");
    }
    if (instance.parameterBindings[instance.borrowParameter] == 0 ||
        instance.parameterTypes[instance.borrowParameter].kind !=
            SemanticType::Reference ||
        instance.parameterTypes[instance.borrowParameter].referenceAccess !=
            instance.borrowAccess) {
      return invalidSummary(
          "constructor borrow dependency does not identify an exact "
          "reference formal parameter");
    }
    break;
  }

  std::unordered_set<MirLoanId> initializerLoans;
  std::size_t storedInitializers = 0;
  for (const MirConstructorInitializer &initializer : instance.initializers) {
    if (!initializer.storesReference) {
      continue;
    }
    ++storedInitializers;
    if (instance.borrowOrigin != BorrowOriginKind::Argument ||
        initializer.field == 0 || initializer.arguments.size() != 1 ||
        initializer.borrowAccess != instance.borrowAccess) {
      return invalidSummary(
          "stored-reference initializer does not match the constructor "
          "borrow summary");
    }
    const auto matching = std::find_if(
        body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
          return loan.kind == MirLoanKind::Stored && loan.escapes &&
                 loan.storedField == initializer.field &&
                 loan.producedBy == initializer.arguments.front();
        });
    if (matching == body.loans.end() ||
        matching->access != initializer.borrowAccess) {
      return invalidSummary(
          "stored-reference initializer is missing its exact escaping loan");
    }
    const std::optional<MirPlaceId> canonicalSource =
        borrowSourceForPlace(body, matching->source, 0);
    const MirPlace *source =
        canonicalSource ? body.findPlace(*canonicalSource) : nullptr;
    if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
        source->binding !=
            instance.parameterBindings[instance.borrowParameter]) {
      return invalidSummary(
          "stored-reference initializer loan does not originate from the "
          "summarized formal parameter");
    }
    if (!initializerLoans.insert(matching->id).second) {
      return invalidSummary(
          "multiple stored-reference initializers reuse one escaping loan");
    }
  }
  if ((instance.borrowOrigin == BorrowOriginKind::Argument) !=
      (storedInitializers == 1)) {
    return invalidSummary(
        "constructor borrow summary does not match its stored-reference "
        "initializer");
  }
  for (const MirLoan &loan : body.loans) {
    if (loan.kind == MirLoanKind::Stored && loan.escapes &&
        !initializerLoans.contains(loan.id)) {
      return invalidSummary(
          "escaping stored-reference loan is not attached to a constructor "
          "initializer");
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirProgramBorrowContracts(const MirProgram &program) {
  MirVerificationResult result;
  append(result, verifyMirBorrowProducers(program, program.module(), 0));
  for (const MirClassInstance &instance : program.classInstances()) {
    append(result, verifyMirBorrowProducers(program, instance.fieldInitializers,
                                            instance.id));
    append(result, verifyMirBorrowProducers(
                       program, instance.staticFieldInitializers, instance.id));
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
    append(result, verifyMirFunctionBorrowSummary(program, instance));
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
    append(result, verifyMirConstructorBorrowSummary(instance));
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    append(result,
           verifyMirBorrowProducers(program, instance.body, instance.id));
  }
  return result;
}

} // namespace

MirVerificationResult verifyMirProgram(const MirProgram &program) {
  MirVerificationResult result;
  if (!program.valid()) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .message = "MIR program is marked invalid"});
  }

  append(result, verifyMirBody(program.module()));
  for (std::size_t index = 0; index < program.classInstances().size();
       ++index) {
    const MirClassInstance &instance = program.classInstances()[index];
    if (instance.id != index + 1) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class instance identity does not match stored order"});
    }
    append(result, verifyMirBody(instance.fieldInitializers, instance.id));
    append(result,
           verifyMirBody(instance.staticFieldInitializers, instance.id));
  }
  for (std::size_t index = 0; index < program.functionInstances().size();
       ++index) {
    const MirFunctionInstance &instance = program.functionInstances()[index];
    if (instance.id != index + 1) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message =
               "function instance identity does not match stored order"});
    }
    if (instance.linkage == LanguageLinkage::C &&
        instance.externalSymbol.empty()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "C-linkage function is missing its external symbol"});
    }
    if (instance.linkage == LanguageLinkage::Gti &&
        !instance.externalSymbol.empty()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "GTI-linkage function has an external C symbol"});
    }
    append(result, verifyMirBody(instance.body, instance.id));
  }
  for (std::size_t index = 0; index < program.constructorInstances().size();
       ++index) {
    const MirConstructorInstance &instance =
        program.constructorInstances()[index];
    if (instance.id != index + 1 || instance.owner == 0 ||
        instance.owner > program.classInstances().size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor instance identity or owner is invalid"});
    }
    append(result, verifyMirBody(instance.body, instance.id));
  }
  for (std::size_t index = 0; index < program.destructorInstances().size();
       ++index) {
    const MirDestructorInstance &instance =
        program.destructorInstances()[index];
    if (instance.id != index + 1 || instance.owner == 0 ||
        instance.owner > program.classInstances().size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "destructor instance identity or owner is invalid"});
    }
    append(result, verifyMirBody(instance.body, instance.id));
  }
  for (std::size_t index = 0; index < program.lambdaInstances().size();
       ++index) {
    const MirLambdaInstance &instance = program.lambdaInstances()[index];
    if (instance.id != index + 1) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Lambda,
           .owner = instance.id,
           .message = "lambda instance identity does not match stored order"});
    }
    append(result, verifyMirBody(instance.body, instance.id));
  }
  append(result, verifyMirProgramBorrowContracts(program));
  return result;
}

} // namespace lang
