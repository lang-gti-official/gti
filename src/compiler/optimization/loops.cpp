#include "gti/optimization/loops.h"

#include "gti/optimization/effects.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] std::vector<MirBlockId>
successors(const MirTerminator &terminator) {
  std::vector<MirBlockId> result;
  const auto append = [&](MirBlockId block) {
    if (block != 0) {
      result.push_back(block);
    }
  };
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    append(terminator.target);
    break;
  case MirTerminatorKind::Branch:
  case MirTerminatorKind::Invoke:
    append(terminator.target);
    append(terminator.elseTarget);
    break;
  case MirTerminatorKind::Switch:
    append(terminator.target);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      append(target.target);
    }
    break;
  default:
    break;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

[[nodiscard]] std::vector<std::vector<MirBlockId>>
predecessors(const MirBody &body) {
  std::vector<std::vector<MirBlockId>> result(body.blocks.size());
  for (const MirBlock &block : body.blocks) {
    for (const MirBlockId target : successors(block.terminator)) {
      if (target != 0 && target <= result.size()) {
        result[target - 1].push_back(block.id);
      }
    }
  }
  for (auto &blocks : result) {
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
  }
  return result;
}

[[nodiscard]] const std::vector<MirOperand> *
edgeArguments(const MirBlock &source, MirBlockId target) {
  const MirTerminator &terminator = source.terminator;
  if ((terminator.kind == MirTerminatorKind::Goto ||
       terminator.kind == MirTerminatorKind::Branch ||
       terminator.kind == MirTerminatorKind::Invoke) &&
      terminator.target == target) {
    return &terminator.targetArguments;
  }
  if ((terminator.kind == MirTerminatorKind::Branch ||
       terminator.kind == MirTerminatorKind::Invoke) &&
      terminator.elseTarget == target) {
    return &terminator.elseArguments;
  }
  if (terminator.kind == MirTerminatorKind::Switch) {
    if (terminator.target == target) {
      return &terminator.targetArguments;
    }
    for (const MirSwitchTarget &candidate : terminator.switchTargets) {
      if (candidate.target == target) {
        return &candidate.arguments;
      }
    }
  }
  return nullptr;
}

[[nodiscard]] const MirInstruction *definition(const MirBody &body,
                                               MirValueId value) {
  const MirValue *mirValue = body.findValue(value);
  const MirBlock *block =
      mirValue == nullptr ? nullptr : body.findBlock(mirValue->definitionBlock);
  if (block == nullptr || mirValue->definition == 0) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [&](const MirInstruction &instruction) {
                     return instruction.id == mirValue->definition &&
                            instruction.result == value;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

[[nodiscard]] std::optional<MirOperand>
literalOperand(const MirBody &body, const MirOperand &operand) {
  if (operand.literal) {
    return operand;
  }
  if (operand.kind != MirOperandKind::Value || operand.value == 0) {
    return std::nullopt;
  }
  const MirInstruction *instruction = definition(body, operand.value);
  if (instruction == nullptr ||
      instruction->kind != MirInstructionKind::Compute ||
      instruction->operation != MirOperation::Literal ||
      !instruction->literal) {
    return std::nullopt;
  }
  return MirOperand{.kind = MirOperandKind::Constant,
                    .literal = instruction->literal,
                    .type = instruction->info.type};
}

[[nodiscard]] std::optional<std::uint64_t>
positiveIntegerLiteral(const MirBody &body, const MirOperand &operand) {
  const std::optional<MirOperand> literal = literalOperand(body, operand);
  if (!literal || !literal->literal) {
    return std::nullopt;
  }
  return std::get_if<std::uint64_t>(&*literal->literal) == nullptr
             ? std::nullopt
             : std::optional<std::uint64_t>{
                   *std::get_if<std::uint64_t>(&*literal->literal)};
}

[[nodiscard]] bool comparison(MirOperation operation) {
  switch (operation) {
  case MirOperation::Equal:
  case MirOperation::NotEqual:
  case MirOperation::Less:
  case MirOperation::LessEqual:
  case MirOperation::Greater:
  case MirOperation::GreaterEqual:
    return true;
  default:
    return false;
  }
}

[[nodiscard]] MirOperation reversedComparison(MirOperation operation) {
  switch (operation) {
  case MirOperation::Less:
    return MirOperation::Greater;
  case MirOperation::LessEqual:
    return MirOperation::GreaterEqual;
  case MirOperation::Greater:
    return MirOperation::Less;
  case MirOperation::GreaterEqual:
    return MirOperation::LessEqual;
  default:
    return operation;
  }
}

[[nodiscard]] std::optional<MirRelationalRangeFact>
rangeFor(const MirBody &body, const MirNaturalLoop &loop, MirValueId value) {
  for (const MirBlockId blockId : loop.blocks) {
    const MirBlock *block = body.findBlock(blockId);
    if (block == nullptr ||
        block->terminator.kind != MirTerminatorKind::Branch ||
        !block->terminator.value ||
        block->terminator.value->kind != MirOperandKind::Value) {
      continue;
    }
    const MirInstruction *condition =
        definition(body, block->terminator.value->value);
    if (condition == nullptr ||
        condition->kind != MirInstructionKind::Compute ||
        !comparison(condition->operation) || condition->operands.size() != 2) {
      continue;
    }
    MirOperation relation = condition->operation;
    MirOperand bound;
    if (condition->operands[0].kind == MirOperandKind::Value &&
        condition->operands[0].value == value) {
      bound = condition->operands[1];
    } else if (condition->operands[1].kind == MirOperandKind::Value &&
               condition->operands[1].value == value) {
      relation = reversedComparison(relation);
      bound = condition->operands[0];
    } else {
      continue;
    }
    if (bound.kind == MirOperandKind::Value && bound.value != 0) {
      const MirValue *boundValue = body.findValue(bound.value);
      if (boundValue == nullptr ||
          (loop.contains(boundValue->definitionBlock) &&
           !std::binary_search(loop.invariantValues.begin(),
                               loop.invariantValues.end(), bound.value))) {
        continue;
      }
    } else if (bound.kind != MirOperandKind::Constant && !bound.literal) {
      continue;
    }
    const bool trueInside = loop.contains(block->terminator.target);
    const bool falseInside = loop.contains(block->terminator.elseTarget);
    if (trueInside == falseInside) {
      continue;
    }
    if (!trueInside) {
      switch (relation) {
      case MirOperation::Equal:
        relation = MirOperation::NotEqual;
        break;
      case MirOperation::NotEqual:
        relation = MirOperation::Equal;
        break;
      case MirOperation::Less:
        relation = MirOperation::GreaterEqual;
        break;
      case MirOperation::LessEqual:
        relation = MirOperation::Greater;
        break;
      case MirOperation::Greater:
        relation = MirOperation::LessEqual;
        break;
      case MirOperation::GreaterEqual:
        relation = MirOperation::Less;
        break;
      default:
        break;
      }
    }
    return MirRelationalRangeFact{
        .value = value,
        .relation = relation,
        .bound = std::move(bound),
        .conditionBlock = blockId,
        .constrainedSuccessor = trueInside ? block->terminator.target
                                           : block->terminator.elseTarget};
  }
  return std::nullopt;
}

[[nodiscard]] bool operandInvariant(const MirBody &body,
                                    const MirNaturalLoop &loop,
                                    const std::set<MirValueId> &invariants,
                                    const MirOperand &operand) {
  if (operand.kind == MirOperandKind::Constant || operand.literal) {
    return true;
  }
  if (operand.kind != MirOperandKind::Value || operand.value == 0) {
    return false;
  }
  if (invariants.contains(operand.value)) {
    return true;
  }
  const MirValue *value = body.findValue(operand.value);
  return value != nullptr && !loop.contains(value->definitionBlock);
}

void findInvariants(const MirBody &body, MirNaturalLoop &loop) {
  std::set<MirValueId> invariants;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const MirBlockId blockId : loop.blocks) {
      const MirBlock *block = body.findBlock(blockId);
      if (block == nullptr) {
        continue;
      }
      for (const MirInstruction &instruction : block->instructions) {
        if (instruction.tombstone || !instruction.result ||
            invariants.contains(*instruction.result) ||
            instruction.kind != MirInstructionKind::Compute ||
            instruction.rawMemoryAccess ||
            instruction.unsafeOperation != UnsafeOperationKind::None ||
            !instruction.definedFailure.empty() ||
            !instruction.localFailureSites.empty()) {
          continue;
        }
        const MirEffectTraits effect = effects(instruction);
        if (!effect.speculatable || effect.readsPlace || effect.writesPlace ||
            effect.readsUnknownMemory || effect.writesUnknownMemory ||
            effect.invokesUserCode || effect.maySynchronize ||
            std::any_of(
                instruction.operands.begin(), instruction.operands.end(),
                [&](const MirOperand &operand) {
                  return !operandInvariant(body, loop, invariants, operand);
                })) {
          continue;
        }
        invariants.insert(*instruction.result);
        changed = true;
      }
    }
  }
  loop.invariantValues.assign(invariants.begin(), invariants.end());
}

void findInductions(const MirBody &body,
                    const std::vector<std::vector<MirBlockId>> &preds,
                    MirNaturalLoop &loop) {
  const MirBlock *header = body.findBlock(loop.header);
  if (header == nullptr || loop.latches.size() != 1) {
    return;
  }
  const MirBlockId latch = loop.latches.front();
  const MirBlock *latchBlock = body.findBlock(latch);
  if (latchBlock == nullptr) {
    return;
  }
  for (std::size_t parameterIndex = 0;
       parameterIndex < header->parameters.size(); ++parameterIndex) {
    const MirValueId parameter = header->parameters[parameterIndex].value;
    std::optional<MirOperand> initial;
    std::optional<MirOperand> recurrence;
    bool valid = true;
    for (const MirBlockId predecessor : preds[loop.header - 1]) {
      const MirBlock *source = body.findBlock(predecessor);
      const std::vector<MirOperand> *arguments =
          source == nullptr ? nullptr : edgeArguments(*source, loop.header);
      if (arguments == nullptr || parameterIndex >= arguments->size()) {
        valid = false;
        break;
      }
      if (loop.contains(predecessor)) {
        if (predecessor != latch || recurrence) {
          valid = false;
          break;
        }
        recurrence = (*arguments)[parameterIndex];
      } else {
        const std::optional<MirOperand> candidate =
            literalOperand(body, (*arguments)[parameterIndex]);
        if (!candidate || (initial && *initial != *candidate)) {
          valid = false;
          break;
        }
        initial = candidate;
      }
    }
    if (!valid || !initial || !recurrence ||
        recurrence->kind != MirOperandKind::Value) {
      continue;
    }
    const MirInstruction *update = definition(body, recurrence->value);
    const MirValue *recurrenceValue = body.findValue(recurrence->value);
    if (update == nullptr || update->kind != MirInstructionKind::Compute ||
        recurrenceValue == nullptr ||
        (update->operation != MirOperation::Add &&
         update->operation != MirOperation::Subtract) ||
        update->operands.size() != 2 ||
        !loop.contains(recurrenceValue->definitionBlock)) {
      continue;
    }
    std::optional<std::uint64_t> step;
    bool decreasing = update->operation == MirOperation::Subtract;
    if (update->operands[0].kind == MirOperandKind::Value &&
        update->operands[0].value == parameter) {
      step = positiveIntegerLiteral(body, update->operands[1]);
    } else if (update->operation == MirOperation::Add &&
               update->operands[1].kind == MirOperandKind::Value &&
               update->operands[1].value == parameter) {
      step = positiveIntegerLiteral(body, update->operands[0]);
    }
    if (!step || *step == 0) {
      continue;
    }
    MirAffineInductionFact fact{
        .parameter = parameter,
        .header = loop.header,
        .latch = latch,
        .initial = *initial,
        .stepMagnitude = *step,
        .decreasing = decreasing,
        .range = rangeFor(body, loop, parameter),
        .relatedInstructions = {update->id},
        .relatedValues = {parameter, recurrence->value}};
    std::sort(fact.relatedValues.begin(), fact.relatedValues.end());
    loop.inductions.push_back(std::move(fact));
  }
}

[[nodiscard]] bool
irreducible(const MirBody &body,
            const std::vector<std::vector<MirBlockId>> &preds) {
  const std::size_t count = body.blocks.size();
  std::vector<int> index(count, -1);
  std::vector<int> low(count, -1);
  std::vector<bool> onStack(count, false);
  std::vector<MirBlockId> stack;
  int next = 0;
  bool found = false;
  std::function<void(MirBlockId)> visit = [&](MirBlockId blockId) {
    const std::size_t blockIndex = blockId - 1;
    index[blockIndex] = low[blockIndex] = next++;
    stack.push_back(blockId);
    onStack[blockIndex] = true;
    for (const MirBlockId target :
         successors(body.blocks[blockIndex].terminator)) {
      if (target == 0 || target > count) {
        continue;
      }
      if (index[target - 1] == -1) {
        visit(target);
        low[blockIndex] = std::min(low[blockIndex], low[target - 1]);
      } else if (onStack[target - 1]) {
        low[blockIndex] = std::min(low[blockIndex], index[target - 1]);
      }
    }
    if (low[blockIndex] != index[blockIndex]) {
      return;
    }
    std::vector<MirBlockId> component;
    while (!stack.empty()) {
      const MirBlockId member = stack.back();
      stack.pop_back();
      onStack[member - 1] = false;
      component.push_back(member);
      if (member == blockId) {
        break;
      }
    }
    const std::vector<MirBlockId> componentSuccessors =
        component.size() == 1
            ? successors(body.blocks[component.front() - 1].terminator)
            : std::vector<MirBlockId>{};
    const bool selfLoop =
        component.size() == 1 &&
        std::binary_search(componentSuccessors.begin(),
                           componentSuccessors.end(), component.front());
    if (component.size() == 1 && !selfLoop) {
      return;
    }
    std::sort(component.begin(), component.end());
    std::set<MirBlockId> entries;
    for (const MirBlockId member : component) {
      for (const MirBlockId predecessor : preds[member - 1]) {
        if (!std::binary_search(component.begin(), component.end(),
                                predecessor)) {
          entries.insert(member);
        }
      }
    }
    found |= entries.size() > 1;
  };
  if (body.entry != 0 && body.entry <= count) {
    visit(body.entry);
  }
  return found;
}

} // namespace

bool MirNaturalLoop::contains(MirBlockId block) const {
  return std::binary_search(blocks.begin(), blocks.end(), block);
}

const MirNaturalLoop *MirLoopAnalysis::loopForHeader(MirBlockId header) const {
  const auto found =
      std::find_if(loops.begin(), loops.end(), [&](const MirNaturalLoop &loop) {
        return loop.header == header;
      });
  return found == loops.end() ? nullptr : &*found;
}

MirLoopAnalysis analyzeMirLoops(const MirBody &body,
                                const MirDominanceInfo &dominance) {
  MirLoopAnalysis result;
  if (body.blocks.size() != dominance.blockCount()) {
    return result;
  }
  const std::vector<std::vector<MirBlockId>> preds = predecessors(body);
  result.hasIrreducibleControlFlow = irreducible(body, preds);
  for (const MirBlock &source : body.blocks) {
    if (!dominance.isReachable(source.id)) {
      continue;
    }
    for (const MirBlockId target : successors(source.terminator)) {
      if (!dominance.dominates(target, source.id)) {
        continue;
      }
      std::set<MirBlockId> members{target, source.id};
      std::vector<MirBlockId> pending{source.id};
      while (!pending.empty()) {
        const MirBlockId current = pending.back();
        pending.pop_back();
        if (current == target) {
          continue;
        }
        for (const MirBlockId predecessor : preds[current - 1]) {
          if (dominance.dominates(target, predecessor) &&
              members.insert(predecessor).second) {
            pending.push_back(predecessor);
          }
        }
      }
      auto found = std::find_if(
          result.loops.begin(), result.loops.end(),
          [&](const MirNaturalLoop &loop) { return loop.header == target; });
      if (found == result.loops.end()) {
        result.loops.push_back({.header = target,
                                .latches = {source.id},
                                .blocks = {members.begin(), members.end()}});
      } else {
        found->latches.push_back(source.id);
        found->blocks.insert(found->blocks.end(), members.begin(),
                             members.end());
        std::sort(found->blocks.begin(), found->blocks.end());
        found->blocks.erase(
            std::unique(found->blocks.begin(), found->blocks.end()),
            found->blocks.end());
      }
    }
  }
  std::sort(result.loops.begin(), result.loops.end(),
            [](const MirNaturalLoop &left, const MirNaturalLoop &right) {
              return left.header < right.header;
            });
  for (MirNaturalLoop &loop : result.loops) {
    std::sort(loop.latches.begin(), loop.latches.end());
    loop.latches.erase(std::unique(loop.latches.begin(), loop.latches.end()),
                       loop.latches.end());
  }
  for (std::size_t child = 0; child < result.loops.size(); ++child) {
    std::optional<std::size_t> parent;
    for (std::size_t candidate = 0; candidate < result.loops.size();
         ++candidate) {
      if (candidate == child ||
          result.loops[candidate].blocks.size() <=
              result.loops[child].blocks.size() ||
          !std::includes(result.loops[candidate].blocks.begin(),
                         result.loops[candidate].blocks.end(),
                         result.loops[child].blocks.begin(),
                         result.loops[child].blocks.end())) {
        continue;
      }
      if (!parent || result.loops[candidate].blocks.size() <
                         result.loops[*parent].blocks.size()) {
        parent = candidate;
      }
    }
    result.loops[child].parent = parent;
    if (parent) {
      result.loops[*parent].children.push_back(child);
    }
  }
  for (std::size_t index = 0; index < result.loops.size(); ++index) {
    std::size_t depth = 0;
    for (std::optional<std::size_t> parent = result.loops[index].parent; parent;
         parent = result.loops[*parent].parent) {
      ++depth;
    }
    result.loops[index].depth = depth;
    findInvariants(body, result.loops[index]);
    findInductions(body, preds, result.loops[index]);
  }
  return result;
}

} // namespace lang
