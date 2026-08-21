#include "gti/optimizer.h"

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_set>
#include <utility>

namespace lang {

const ConstantValue *OptimizationResult::replacement(HirValueId value) const {
  const auto found = constants.find(value);
  return found == constants.end() ? nullptr : &found->second;
}

const ConstantValue *
OptimizationResult::replacement(const HirProgram &program,
                                const Expr &expression) const {
  const ConstantValue *result = nullptr;
  for (const HirValueId id : program.valueIdsForSource(expression)) {
    const ConstantValue *candidate = replacement(id);
    if (candidate == nullptr) {
      return nullptr;
    }
    if (result != nullptr && *result != *candidate) {
      return nullptr;
    }
    result = candidate;
  }
  return result;
}

std::size_t OptimizationResult::foldedExpressionCount() const {
  return constants.size();
}

void OptimizationResult::setReplacement(HirValueId value,
                                        ConstantValue replacement) {
  if (value != 0) {
    constants.insert_or_assign(value, std::move(replacement));
  }
}

std::string_view ConstantFoldingPass::name() const {
  return "constant-folding";
}

void ConstantFoldingPass::run(const OptimizationContext &context,
                              OptimizationResult &output) {
  constants.clear();
  result = &output;
  analyze(context.program.module());
  for (const HirClassInstance &instance : context.program.classInstances()) {
    analyze(instance.fieldInitializers);
  }
  for (const HirFunctionInstance &instance :
       context.program.functionInstances()) {
    analyze(instance.body);
  }
  for (const HirConstructorInstance &instance :
       context.program.constructorInstances()) {
    analyze(instance.body);
  }
  for (const HirDestructorInstance &instance :
       context.program.destructorInstances()) {
    analyze(instance.body);
  }
  for (const HirLambda &lambda : context.program.lambdaInstances()) {
    analyze(lambda.body);
  }
}

std::optional<ConstantValue>
ConstantFoldingPass::operand(const HirValue &value, std::size_t index) const {
  if (index >= value.operands.size()) {
    return std::nullopt;
  }
  const auto found = constants.find(value.operands[index]);
  return found == constants.end() ? std::nullopt
                                  : std::optional<ConstantValue>{found->second};
}

void ConstantFoldingPass::analyze(const HirBody &body) {
  for (const HirValue &value : body.values) {
    const std::optional<ConstantValue> folded = evaluate(value);
    if (!folded) {
      continue;
    }
    constants.insert_or_assign(value.id, *folded);
    if (value.kind != HirValueKind::Literal) {
      result->setReplacement(value.id, *folded);
    }
  }
}

std::optional<ConstantValue>
ConstantFoldingPass::evaluate(const HirValue &value) const {
  if (value.constant) {
    return value.constant;
  }
  switch (value.kind) {
  case HirValueKind::Literal:
    return literal(value);
  case HirValueKind::Grouping:
    return operand(value, 0);
  case HirValueKind::Binary:
    return foldBinary(value);
  case HirValueKind::Logical:
    return logical(value);
  case HirValueKind::Unary:
    return unary(value);
  case HirValueKind::Conditional:
    return conditional(value);
  case HirValueKind::Conversion:
    return conversion(value);
  case HirValueKind::Assignment:
  case HirValueKind::ArrayInitializer:
  case HirValueKind::Call:
  case HirValueKind::Move:
  case HirValueKind::DirectInitializer:
  case HirValueKind::DereferenceSet:
  case HirValueKind::MemberAccess:
  case HirValueKind::Index:
  case HirValueKind::IndexSet:
  case HirValueKind::Lambda:
  case HirValueKind::LayoutQuery:
  case HirValueKind::PackFold:
  case HirValueKind::PackExpansion:
  case HirValueKind::PayloadConstruction:
  case HirValueKind::PayloadExtraction:
  case HirValueKind::Postfix:
  case HirValueKind::QualifiedName:
  case HirValueKind::This:
  case HirValueKind::MemberSet:
  case HirValueKind::Unexpected:
  case HirValueKind::Variable:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<ConstantValue>
ConstantFoldingPass::literal(const HirValue &value) {
  if (!value.literal) {
    return std::nullopt;
  }
  return evaluateConstantLiteral(*value.literal,
                                 constantIntegerDomain(value.info.type))
      .value;
}

std::optional<ConstantValue>
ConstantFoldingPass::logical(const HirValue &value) const {
  if (!value.operation) {
    return std::nullopt;
  }
  const std::optional<ConstantValue> left = operand(value, 0);
  const bool *leftBoolean = left ? std::get_if<bool>(&*left) : nullptr;
  if (leftBoolean != nullptr && *value.operation == TokenKind::AND &&
      !*leftBoolean) {
    return false;
  }
  if (leftBoolean != nullptr && *value.operation == TokenKind::OR &&
      *leftBoolean) {
    return true;
  }

  const std::optional<ConstantValue> right = operand(value, 1);
  const bool *rightBoolean = right ? std::get_if<bool>(&*right) : nullptr;
  if (leftBoolean == nullptr || rightBoolean == nullptr) {
    return std::nullopt;
  }
  return evaluateConstantLogical(*value.operation, *left, *right).value;
}

std::optional<ConstantValue>
ConstantFoldingPass::unary(const HirValue &value) const {
  if (!value.operation) {
    return std::nullopt;
  }
  const std::optional<ConstantValue> right = operand(value, 0);
  if (!right) {
    return std::nullopt;
  }
  return evaluateConstantUnary(*value.operation, *right,
                               constantIntegerDomain(value.info.type))
      .value;
}

std::optional<ConstantValue>
ConstantFoldingPass::foldBinary(const HirValue &value) const {
  if (!value.operation) {
    return std::nullopt;
  }
  const std::optional<ConstantValue> left = operand(value, 0);
  const std::optional<ConstantValue> right = operand(value, 1);
  if (!left || !right) {
    return std::nullopt;
  }
  return evaluateConstantBinary(*value.operation, *left, *right,
                                constantIntegerDomain(value.info.type),
                                semanticFloatFormat(value.info.type))
      .value;
}

std::optional<ConstantValue>
ConstantFoldingPass::conditional(const HirValue &value) const {
  const std::optional<ConstantValue> condition = operand(value, 0);
  const bool *selected = condition ? std::get_if<bool>(&*condition) : nullptr;
  if (selected == nullptr) {
    return std::nullopt;
  }
  return operand(value, *selected ? 1 : 2);
}

std::optional<ConstantValue>
ConstantFoldingPass::conversion(const HirValue &value) const {
  const std::optional<ConstantValue> source = operand(value, 0);
  if (!source) {
    return std::nullopt;
  }
  if (const std::optional<BinaryFloatFormat> format =
          semanticFloatFormat(value.info.type)) {
    return convertConstantFloat(*source, *format).value;
  }
  const std::optional<CheckedIntegerDomain> target =
      constantIntegerDomain(value.info.type);
  if (!target) {
    return std::nullopt;
  }
  return convertConstantInteger(*source, *target).value;
}

OptimizationResult OptimizationPipeline::run(const HirProgram &program,
                                             OptimizationLevel level,
                                             TargetInfo target) const {
  OptimizationResult result;
  if (level == OptimizationLevel::O0) {
    return result;
  }

  const OptimizationContext context{
      .program = program, .level = level, .target = std::move(target)};
  ConstantFoldingPass().run(context, result);
  return result;
}

namespace {

[[nodiscard]] const MirInstruction *definingInstruction(const MirBody &body,
                                                        MirValueId valueId) {
  const MirValue *value = body.findValue(valueId);
  if (value == nullptr) {
    return nullptr;
  }
  const MirBlock *block = body.findBlock(value->definitionBlock);
  if (block == nullptr) {
    return nullptr;
  }
  const auto found =
      std::find_if(block->instructions.begin(), block->instructions.end(),
                   [value](const MirInstruction &instruction) {
                     return instruction.id == value->definition;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

[[nodiscard]] bool foldableScalarLiteral(const Literal &literal) {
  return std::holds_alternative<std::nullptr_t>(literal) ||
         std::holds_alternative<std::uint64_t>(literal) ||
         std::holds_alternative<BinaryFloat>(literal) ||
         std::holds_alternative<CharacterLiteral>(literal) ||
         std::holds_alternative<bool>(literal);
}

[[nodiscard]] std::optional<Literal> chasedLiteral(const MirBody &body,
                                                   MirValueId value) {
  std::unordered_set<MirValueId> visited;
  while (value != 0 && visited.size() <= body.values.size() &&
         visited.insert(value).second) {
    const MirInstruction *definition = definingInstruction(body, value);
    if (definition == nullptr ||
        definition->kind != MirInstructionKind::Compute) {
      return std::nullopt;
    }
    if (definition->operation == MirOperation::Literal && definition->literal &&
        foldableScalarLiteral(*definition->literal)) {
      return definition->literal;
    }
    if (definition->operation != MirOperation::Identity ||
        definition->operands.size() != 1 ||
        definition->operands.front().kind != MirOperandKind::Value) {
      return std::nullopt;
    }
    value = definition->operands.front().value;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<Literal>
identityLiteral(const MirBody &body, const MirInstruction &identity) {
  if (identity.kind != MirInstructionKind::Compute ||
      identity.operation != MirOperation::Identity ||
      identity.operands.size() != 1 ||
      identity.operands.front().kind != MirOperandKind::Value) {
    return std::nullopt;
  }
  return chasedLiteral(body, identity.operands.front().value);
}

[[nodiscard]] bool computeFoldableOperation(MirOperation operation) {
  switch (operation) {
  case MirOperation::Convert:
  case MirOperation::Add:
  case MirOperation::Subtract:
  case MirOperation::Multiply:
  case MirOperation::Divide:
  case MirOperation::Equal:
  case MirOperation::NotEqual:
  case MirOperation::Less:
  case MirOperation::LessEqual:
  case MirOperation::Greater:
  case MirOperation::GreaterEqual:
  case MirOperation::LogicalNot:
    return true;
  default:
    return false;
  }
}

// A comparison or logical-not whose operands all chase to literals folds
// through the single MIR evaluation authority; the caller still demands
// shadow agreement with the compatibility optimizer before queueing.
[[nodiscard]] std::optional<Literal>
computeFoldLiteral(const MirBody &body, const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute ||
      !computeFoldableOperation(instruction.operation) ||
      instruction.operands.empty() || instruction.operands.size() > 2 ||
      !instruction.localFailureSites.empty() ||
      !instruction.definedFailure.empty()) {
    return std::nullopt;
  }
  std::vector<MirComputeFoldOperand> operands;
  operands.reserve(instruction.operands.size());
  for (const MirOperand &operand : instruction.operands) {
    if (operand.kind != MirOperandKind::Value || operand.value == 0) {
      return std::nullopt;
    }
    const MirInstruction *definition = definingInstruction(body, operand.value);
    const MirValue *value = body.findValue(operand.value);
    if (definition == nullptr || value == nullptr ||
        definition->kind != MirInstructionKind::Compute ||
        definition->operation != MirOperation::Literal ||
        !definition->literal || !foldableScalarLiteral(*definition->literal)) {
      return std::nullopt;
    }
    operands.push_back(
        {.literal = *definition->literal, .type = value->info.type});
  }
  return evaluateMirComputeFold(instruction.operation, operands,
                                instruction.info.type);
}

struct LiteralIdentityFoldResult {
  OptimizationPassReport report;
  MirVerificationResult verification;
};

[[nodiscard]] LiteralIdentityFoldResult
foldLiteralIdentities(MirProgram &program,
                      const OptimizationResult &compatibility) {
  LiteralIdentityFoldResult result{
      .report = {.name = "fold-literal-identities"}};
  // Identity, compute, and branch folds cascade: a folded
  // comparison feeds an identity that feeds a branch. The pass
  // iterates its queue-and-apply round to a bounded fixpoint so
  // one verified transform report covers the whole cascade.
  for (std::size_t round = 0; round < 8; ++round) {
    MirProgramEditor editor(program);
    const auto bodyKeyOf = [](MirBodyAddress address) {
      return std::pair<std::size_t, HirClassInstanceId>(
          static_cast<std::size_t>(address.kind), address.owner);
    };
    std::map<std::pair<std::size_t, HirClassInstanceId>,
             std::unordered_map<MirValueId, bool>>
        foldedBooleans;
    for (const MirBodyAddress bodyAddress :
         enumerateMirBodyAddresses(program)) {
      const MirBody *body = findMirBody(program, bodyAddress);
      if (body == nullptr) {
        continue;
      }
      for (const MirBlock &block : body->blocks) {
        for (std::size_t index = 0; index < block.instructions.size();
             ++index) {
          const MirInstruction &instruction = block.instructions[index];
          if (instruction.hirValue == 0) {
            continue;
          }
          const std::optional<Literal> literal =
              identityLiteral(*body, instruction);
          const std::optional<Literal> computeFold =
              literal ? std::nullopt : computeFoldLiteral(*body, instruction);
          if (!literal && !computeFold) {
            continue;
          }
          const Literal &folded = literal ? *literal : *computeFold;

          ++result.report.shadowComparisons;
          const ConstantEvaluation evaluated = evaluateConstantLiteral(
              folded, constantIntegerDomain(instruction.info.type));
          const ConstantValue *expected =
              compatibility.replacement(instruction.hirValue);
          if (!evaluated.value || expected == nullptr ||
              *evaluated.value != *expected) {
            ++result.report.shadowMismatches;
            continue;
          }

          if (literal) {
            editor.queueLiteralReplacement(
                {.body = bodyAddress, .block = block.id, .index = index},
                instruction.id, MirOperation::Identity, folded);
          } else {
            editor.queueComputeFoldReplacement(
                {.body = bodyAddress, .block = block.id, .index = index},
                instruction.id, instruction.operation, folded);
          }
          if (instruction.result) {
            if (const bool *boolean = std::get_if<bool>(&folded)) {
              foldedBooleans[bodyKeyOf(bodyAddress)][*instruction.result] =
                  *boolean;
            }
          }
        }
      }
    }
    // A branch whose condition is a literal bool — from lowering or from a
    // fold queued in this same batch — rewrites to a Goto to its taken
    // target. The editor re-reads the literal at application, and the
    // verifier's BranchFold replay owns the standing proof.
    for (const MirBodyAddress bodyAddress :
         enumerateMirBodyAddresses(program)) {
      const MirBody *body = findMirBody(program, bodyAddress);
      if (body == nullptr) {
        continue;
      }
      // The fold changes reachability, so this slice stays inside bodies
      // whose semantics carry no path-sensitive schedules: no loans, drop
      // obligations, cleanup boundaries, failure records, or frozen
      // program-initialization steps. Wider bodies wait for the fold to
      // learn those schedules rather than silently invalidating them.
      const bool pathSensitiveSchedules =
          !body->loans.empty() || !body->dropObligations.empty() ||
          !body->cleanupBoundaries.empty() || !body->failureRecords.empty() ||
          std::any_of(body->blocks.begin(), body->blocks.end(),
                      [](const MirBlock &block) {
                        return block.programInitializationStep != 0;
                      });
      if (pathSensitiveSchedules || bodyAddress.kind != MirBodyKind::Function) {
        continue;
      }
      const auto folded = foldedBooleans.find(bodyKeyOf(bodyAddress));
      for (const MirBlock &block : body->blocks) {
        if (block.terminator.kind != MirTerminatorKind::Branch ||
            !block.terminator.value ||
            block.terminator.value->kind != MirOperandKind::Value) {
          continue;
        }
        const MirValueId condition = block.terminator.value->value;
        std::optional<bool> taken;
        if (const MirInstruction *definition =
                definingInstruction(*body, condition);
            definition != nullptr &&
            definition->kind == MirInstructionKind::Compute &&
            definition->operation == MirOperation::Literal &&
            definition->literal) {
          if (const bool *boolean = std::get_if<bool>(&*definition->literal)) {
            taken = *boolean;
          }
        }
        if (!taken && folded != foldedBooleans.end()) {
          const auto queued = folded->second.find(condition);
          if (queued != folded->second.end()) {
            taken = queued->second;
          }
        }
        if (!taken) {
          continue;
        }
        editor.queueBranchFold(bodyAddress, block.id, condition, *taken);
      }
    }

    if (editor.pendingPatchCount() == 0 &&
        editor.pendingBranchFoldCount() == 0) {
      break;
    }
    const MirEditResult edited = editor.apply();
    result.report.changed |= edited.changed;
    result.report.appliedEdits += edited.appliedPatches;
    result.report.valueUsesRebuilt |= edited.valueUsesRebuilt;
    result.report.invalidation.instructionFacts |=
        edited.invalidation.instructionFacts;
    result.report.invalidation.valueUses |= edited.invalidation.valueUses;
    result.report.invalidation.controlFlow |= edited.invalidation.controlFlow;
    result.report.invalidation.reachability |= edited.invalidation.reachability;
    result.report.invalidation.dominance |= edited.invalidation.dominance;
    result.verification = edited.verification;
    if (!edited.verification.valid() || !edited.changed) {
      break;
    }
  }
  return result;
}

} // namespace

OptimizedProgram OptimizationPipeline::run(OptimizationRequest request) const {
  OptimizedProgram result{.sourceMir = request.mir,
                          .mir = std::move(request.mir)};
  result.report.verificationEnabled = request.options.verifyMir;

  if (request.options.verifyMir) {
    result.report.inputVerification = verifyMirProgram(result.mir);
    if (!result.report.inputVerification.valid()) {
      return result;
    }
  }

  if (request.level != OptimizationLevel::O0) {
    OptimizationResult localCompatibility;
    const OptimizationResult *compatibility = request.compatibility;
    if (compatibility == nullptr) {
      localCompatibility = run(request.hir, request.level, request.target);
      compatibility = &localCompatibility;
    }
    LiteralIdentityFoldResult pass =
        foldLiteralIdentities(result.mir, *compatibility);
    result.report.passes.push_back(std::move(pass.report));
    if (!pass.verification.valid()) {
      result.report.outputVerification = std::move(pass.verification);
      return result;
    }
  }

  if (request.options.verifyMir) {
    result.report.outputVerification =
        verifyMirOptimizationCoherence(result.sourceMir, result.mir);
  }
  return result;
}

} // namespace lang
