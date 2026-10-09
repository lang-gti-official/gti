#include "gti/optimization/sccp.h"
#include "evidence_set.h"
#include "gti/mir_editor.h"

#include "gti/mir_effects.h"
#include "gti/optimization/coverage.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace lang {
namespace {

struct State {
  std::vector<MirSccpValueFact> values;
};

template <typename T> void appendUnique(std::vector<T> &target, T value) {
  optimization_detail::insertEvidenceId(target, value);
}

[[nodiscard]] MirSccpEvidence merged(MirSccpEvidence left,
                                     const MirSccpEvidence &right) {
  optimization_detail::mergeEvidenceIds(left.blocks, right.blocks);
  optimization_detail::mergeEvidenceIds(left.instructions, right.instructions);
  optimization_detail::mergeEvidenceIds(left.values, right.values);
  return left;
}

[[nodiscard]] MirSccpValueFact unknown(const MirValue &value) {
  return {.value = value.id, .type = value.info.type};
}

[[nodiscard]] MirSccpValueFact overdefined(const MirValue &value,
                                           MirSccpEvidence evidence = {}) {
  return {.value = value.id,
          .type = value.info.type,
          .kind = MirSccpLatticeKind::Overdefined,
          .evidence = std::move(evidence)};
}

[[nodiscard]] MirSccpValueFact constant(const MirValue &value, Literal literal,
                                        MirSccpEvidence evidence = {}) {
  appendUnique(evidence.values, value.id);
  return {.value = value.id,
          .type = value.info.type,
          .kind = MirSccpLatticeKind::Constant,
          .literal = std::move(literal),
          .evidence = std::move(evidence)};
}

[[nodiscard]] MirSccpValueFact meet(const MirSccpValueFact &left,
                                    const MirSccpValueFact &right) {
  if (left.kind == MirSccpLatticeKind::Unknown) {
    return right;
  }
  if (right.kind == MirSccpLatticeKind::Unknown) {
    return left;
  }
  MirSccpEvidence evidence = merged(left.evidence, right.evidence);
  if (left.kind == MirSccpLatticeKind::Overdefined ||
      right.kind == MirSccpLatticeKind::Overdefined ||
      left.type != right.type || left.literal != right.literal) {
    MirSccpValueFact result = left;
    result.kind = MirSccpLatticeKind::Overdefined;
    result.literal.reset();
    result.evidence = std::move(evidence);
    return result;
  }
  MirSccpValueFact result = left;
  result.evidence = std::move(evidence);
  return result;
}

[[nodiscard]] const MirSccpValueFact *fact(const State &state,
                                           MirValueId value) {
  return value == 0 || value > state.values.size() ? nullptr
                                                   : &state.values[value - 1];
}

[[nodiscard]] MirSccpValueFact operandFact(const MirBody &body,
                                           const State &state,
                                           const MirOperand &operand) {
  if (operand.literal) {
    MirSccpValueFact result;
    result.type = operand.type;
    result.kind = MirSccpLatticeKind::Constant;
    result.literal = operand.literal;
    return result;
  }
  if (operand.kind == MirOperandKind::Value && operand.value != 0) {
    if (const MirSccpValueFact *value = fact(state, operand.value)) {
      return *value;
    }
  }
  MirSccpValueFact result;
  result.type = operand.type;
  result.kind = MirSccpLatticeKind::Overdefined;
  (void)body;
  return result;
}

[[nodiscard]] bool admittedCompute(const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute ||
      instruction.rawMemoryAccess ||
      instruction.unsafeOperation != UnsafeOperationKind::None ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty()) {
    return false;
  }
  const MirEffectTraits effect = effects(instruction);
  return effect.speculatable && !effect.readsPlace && !effect.writesPlace &&
         !effect.readsUnknownMemory && !effect.writesUnknownMemory &&
         !effect.invokesUserCode && !effect.maySynchronize;
}

[[nodiscard]] MirSccpValueFact
evaluateInstruction(const MirBody &body, const MirBlock &block,
                    const MirInstruction &instruction, const State &state) {
  const MirValue *result =
      instruction.result ? body.findValue(*instruction.result) : nullptr;
  if (result == nullptr) {
    return {};
  }
  MirSccpEvidence evidence;
  appendUnique(evidence.blocks, block.id);
  appendUnique(evidence.instructions, instruction.id);
  appendUnique(evidence.values, result->id);
  if (instruction.tombstone || !admittedCompute(instruction)) {
    return overdefined(*result, std::move(evidence));
  }
  if (instruction.operation == MirOperation::Literal && instruction.literal) {
    return constant(*result, *instruction.literal, std::move(evidence));
  }

  std::vector<MirSccpValueFact> operands;
  operands.reserve(instruction.operands.size());
  for (const MirOperand &operand : instruction.operands) {
    operands.push_back(operandFact(body, state, operand));
    evidence = merged(std::move(evidence), operands.back().evidence);
    if (operand.value != 0) {
      appendUnique(evidence.values, operand.value);
    }
  }
  if (std::any_of(operands.begin(), operands.end(), [](const auto &operand) {
        return operand.kind == MirSccpLatticeKind::Overdefined;
      })) {
    return overdefined(*result, std::move(evidence));
  }
  if (std::any_of(operands.begin(), operands.end(), [](const auto &operand) {
        return operand.kind == MirSccpLatticeKind::Unknown;
      })) {
    MirSccpValueFact pending = unknown(*result);
    pending.evidence = std::move(evidence);
    return pending;
  }
  if (instruction.operation == MirOperation::Identity && operands.size() == 1 &&
      operands.front().literal) {
    return constant(*result, *operands.front().literal, std::move(evidence));
  }
  std::vector<MirComputeFoldOperand> foldOperands;
  for (const MirSccpValueFact &operand : operands) {
    if (!operand.literal) {
      return overdefined(*result, std::move(evidence));
    }
    foldOperands.push_back({.literal = *operand.literal, .type = operand.type});
  }
  const std::optional<Literal> folded = evaluateMirComputeFold(
      instruction.operation, foldOperands, instruction.info.type);
  return folded ? constant(*result, *folded, std::move(evidence))
                : overdefined(*result, std::move(evidence));
}

void transferInstructions(const MirBody &body, const MirBlock &block,
                          State &state) {
  for (const MirInstruction &instruction : block.instructions) {
    if (!instruction.result || *instruction.result == 0 ||
        *instruction.result > state.values.size()) {
      continue;
    }
    state.values[*instruction.result - 1] =
        evaluateInstruction(body, block, instruction, state);
  }
}

void bindArguments(const MirBody &body, MirBlockId target,
                   const std::vector<MirOperand> &arguments, State &state) {
  const MirBlock *block = body.findBlock(target);
  if (block == nullptr || block->parameters.size() != arguments.size()) {
    return;
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const MirValueId parameter = block->parameters[index].value;
    if (parameter == 0 || parameter > state.values.size()) {
      continue;
    }
    MirSccpValueFact bound = operandFact(body, state, arguments[index]);
    bound.value = parameter;
    bound.type = block->parameters[index].type;
    appendUnique(bound.evidence.blocks, target);
    appendUnique(bound.evidence.values, parameter);
    state.values[parameter - 1] = std::move(bound);
  }
}

[[nodiscard]] std::vector<MirForwardDataflowEdge<State>>
outgoing(const MirBody &body, const MirBlock &block, State state) {
  transferInstructions(body, block, state);
  std::vector<MirForwardDataflowEdge<State>> result;
  const auto append = [&](MirBlockId target,
                          const std::vector<MirOperand> &arguments,
                          std::size_t ordinal) {
    if (target == 0) {
      return;
    }
    State edge = state;
    bindArguments(body, target, arguments, edge);
    result.push_back(
        {.target = target, .ordinal = ordinal, .state = std::move(edge)});
  };
  const MirTerminator &terminator = block.terminator;
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    append(terminator.target, terminator.targetArguments, 0);
    break;
  case MirTerminatorKind::Branch: {
    const MirSccpValueFact condition =
        terminator.value ? operandFact(body, state, *terminator.value)
                         : MirSccpValueFact{};
    const bool *taken =
        condition.kind == MirSccpLatticeKind::Constant && condition.literal
            ? std::get_if<bool>(&*condition.literal)
            : nullptr;
    if (taken == nullptr) {
      if (condition.kind == MirSccpLatticeKind::Overdefined) {
        append(terminator.target, terminator.targetArguments, 0);
        append(terminator.elseTarget, terminator.elseArguments, 1);
      }
    } else if (*taken) {
      append(terminator.target, terminator.targetArguments, 0);
    } else {
      append(terminator.elseTarget, terminator.elseArguments, 1);
    }
    break;
  }
  case MirTerminatorKind::Switch:
    append(terminator.target, terminator.targetArguments, 0);
    for (std::size_t index = 0; index < terminator.switchTargets.size();
         ++index) {
      append(terminator.switchTargets[index].target,
             terminator.switchTargets[index].arguments, index + 1);
    }
    break;
  case MirTerminatorKind::Invoke:
    append(terminator.target, terminator.targetArguments, 0);
    append(terminator.elseTarget, terminator.elseArguments, 1);
    break;
  default:
    break;
  }
  return result;
}

[[nodiscard]] bool mergeState(State &target, const State &incoming) {
  if (target.values.size() != incoming.values.size()) {
    return false;
  }
  bool changed = false;
  for (std::size_t index = 0; index < target.values.size(); ++index) {
    auto &current = target.values[index];
    const auto &next = incoming.values[index];
    // The meet is idempotent, including its canonical evidence. Avoid copying
    // every type and evidence vector just to rediscover an unchanged fact.
    if (current == next) {
      continue;
    }
    MirSccpValueFact joined = meet(current, next);
    if (current != joined) {
      current = std::move(joined);
      changed = true;
    }
  }
  return changed;
}

} // namespace

const MirSccpValueFact *MirSccpAnalysis::find(MirValueId value) const {
  return value == 0 || value > values.size() ? nullptr : &values[value - 1];
}

const MirSccpBranchFact *MirSccpAnalysis::branch(MirBlockId block) const {
  const auto found = std::find_if(
      branches.begin(), branches.end(),
      [&](const MirSccpBranchFact &fact) { return fact.block == block; });
  return found == branches.end() ? nullptr : &*found;
}

MirSccpAnalysis analyzeMirSccp(const MirProgram &program,
                               MirBodyAddress address) {
  MirSccpAnalysis result;
  const MirBody *body = findMirBody(program, address);
  if (body == nullptr || !verifyMirOptimizationVocabulary(*body).valid()) {
    return result;
  }
  State entry;
  entry.values.reserve(body->values.size());
  for (const MirValue &value : body->values) {
    entry.values.push_back(unknown(value));
  }
  const MirBlock *entryBlock = body->findBlock(body->entry);
  if (entryBlock == nullptr) {
    return result;
  }
  for (const MirBlockParameter &parameter : entryBlock->parameters) {
    if (parameter.value != 0 && parameter.value <= entry.values.size()) {
      entry.values[parameter.value - 1] =
          overdefined(body->values[parameter.value - 1],
                      {.blocks = {body->entry}, .values = {parameter.value}});
    }
  }
  const MirForwardDataflowResult<State> solved = runMirForwardDataflow(
      *body, std::move(entry),
      [&](const MirBlock &block, const State &incoming) {
        return outgoing(*body, block, incoming);
      },
      [](State &target, const State &incoming, MirDataflowMergeContext) {
        return mergeState(target, incoming);
      },
      {.widenAfterUpdates = 0});
  if (!solved.converged()) {
    return result;
  }
  result.values.reserve(body->values.size());
  for (const MirValue &value : body->values) {
    result.values.push_back(unknown(value));
  }
  bool unresolvedControl = false;
  for (std::size_t index = 0; index < body->blocks.size(); ++index) {
    if (!solved.incoming[index]) {
      continue;
    }
    const MirBlock &block = body->blocks[index];
    result.executableBlocks.push_back(block.id);
    State state = *solved.incoming[index];
    for (const MirBlockParameter &parameter : block.parameters) {
      if (parameter.value != 0 && parameter.value <= state.values.size()) {
        result.values[parameter.value - 1] =
            meet(result.values[parameter.value - 1],
                 state.values[parameter.value - 1]);
      }
    }
    transferInstructions(*body, block, state);
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.result && *instruction.result <= state.values.size()) {
        result.values[*instruction.result - 1] =
            meet(result.values[*instruction.result - 1],
                 state.values[*instruction.result - 1]);
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Branch &&
        block.terminator.value) {
      const MirSccpValueFact condition =
          operandFact(*body, state, *block.terminator.value);
      const bool *taken =
          condition.kind == MirSccpLatticeKind::Constant && condition.literal
              ? std::get_if<bool>(&*condition.literal)
              : nullptr;
      if (taken != nullptr) {
        result.branches.push_back({.block = block.id,
                                   .condition = block.terminator.value->value,
                                   .taken = *taken,
                                   .evidence = condition.evidence});
      } else if (condition.kind == MirSccpLatticeKind::Unknown) {
        unresolvedControl = true;
      }
    }
  }
  result.executableEdges = solved.executableEdges;
  result.complete = !unresolvedControl;
  return result;
}

} // namespace lang
