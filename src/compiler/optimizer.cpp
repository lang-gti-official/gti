#include "gti/optimizer.h"

#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/coverage.h"
#include "gti/optimization/devirtualization.h"
#include "gti/optimization/pass.h"
#include "mir_verification_support.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
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
  case HirValueKind::NativeCallback:
  case HirValueKind::FunctionItem:
  case HirValueKind::DirectInitializer:
  case HirValueKind::DereferenceSet:
  case HirValueKind::PlaceSet:
  case HirValueKind::ExpectedPropagation:
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
  std::vector<MirTransformRecord> transformations;
  MirVerificationResult verification;
  bool analysesInvalidated = false;
};

[[nodiscard]] LiteralIdentityFoldResult
foldLiteralIdentities(MirProgram &program,
                      const OptimizationResult &compatibility,
                      MirBodyAnalysisManager &analyses) {
  LiteralIdentityFoldResult result{
      .report = {.name = "fold-literal-identities"}};
  // Identity, compute, and branch folds cascade: a folded
  // comparison feeds an identity that feeds a branch. The pass
  // iterates its queue-and-apply round to a bounded fixpoint so
  // one verified transform report covers the whole cascade.
  for (std::size_t round = 0; round < 8; ++round) {
    ++result.report.iterations;
    MirEditor editor(program, result.report.name);
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
                {.body = bodyAddress, .instruction = instruction.id},
                MirOperation::Identity, folded);
          } else {
            editor.queueComputeFoldReplacement(
                {.body = bodyAddress, .instruction = instruction.id},
                instruction.operation, folded);
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
    const MirEditResult edited = editor.apply(analyses);
    result.report.changed |= edited.changed;
    result.report.appliedEdits += edited.appliedPatches;
    result.report.valueUsesRebuilt |= edited.valueUsesRebuilt;
    result.report.invalidation.instructionFacts |=
        edited.invalidation.instructionFacts;
    result.report.invalidation.valueUses |= edited.invalidation.valueUses;
    result.report.invalidation.controlFlow |= edited.invalidation.controlFlow;
    result.report.invalidation.reachability |= edited.invalidation.reachability;
    result.report.invalidation.dominance |= edited.invalidation.dominance;
    result.report.invalidation.safetyChecks |= edited.invalidation.safetyChecks;
    result.report.invalidatedAnalyses.insert(
        result.report.invalidatedAnalyses.end(),
        edited.invalidatedAnalyses.begin(), edited.invalidatedAnalyses.end());
    std::sort(result.report.invalidatedAnalyses.begin(),
              result.report.invalidatedAnalyses.end());
    result.report.invalidatedAnalyses.erase(
        std::unique(result.report.invalidatedAnalyses.begin(),
                    result.report.invalidatedAnalyses.end()),
        result.report.invalidatedAnalyses.end());
    result.analysesInvalidated |= edited.analysesInvalidated;
    result.transformations.insert(result.transformations.end(),
                                  edited.transformations.begin(),
                                  edited.transformations.end());
    result.verification = edited.verification;
    if (!edited.verification.valid() || !edited.changed) {
      break;
    }
  }
  return result;
}

void mergeEditReport(OptimizationPassReport &report,
                     const MirEditResult &edited) {
  report.changed |= edited.changed;
  report.appliedEdits += edited.appliedPatches;
  report.valueUsesRebuilt |= edited.valueUsesRebuilt;
  report.invalidation.instructionFacts |= edited.invalidation.instructionFacts;
  report.invalidation.valueUses |= edited.invalidation.valueUses;
  report.invalidation.controlFlow |= edited.invalidation.controlFlow;
  report.invalidation.reachability |= edited.invalidation.reachability;
  report.invalidation.dominance |= edited.invalidation.dominance;
  report.invalidation.safetyChecks |= edited.invalidation.safetyChecks;
  report.invalidatedAnalyses.insert(report.invalidatedAnalyses.end(),
                                    edited.invalidatedAnalyses.begin(),
                                    edited.invalidatedAnalyses.end());
  std::sort(report.invalidatedAnalyses.begin(),
            report.invalidatedAnalyses.end());
  report.invalidatedAnalyses.erase(
      std::unique(report.invalidatedAnalyses.begin(),
                  report.invalidatedAnalyses.end()),
      report.invalidatedAnalyses.end());
}

[[nodiscard]] std::vector<MirBodyAddress>
changedBodies(const std::vector<MirTransformRecord> &records) {
  std::vector<MirBodyAddress> result;
  for (const MirTransformRecord &record : records) {
    if (std::find(result.begin(), result.end(), record.body) == result.end()) {
      result.push_back(record.body);
    }
  }
  std::sort(
      result.begin(), result.end(),
      [](MirBodyAddress left, MirBodyAddress right) {
        return std::pair{static_cast<std::size_t>(left.kind), left.owner} <
               std::pair{static_cast<std::size_t>(right.kind), right.owner};
      });
  return result;
}

void appendInvalidated(OptimizationPassReport &report,
                       std::vector<std::string> invalidated) {
  report.invalidatedAnalyses.insert(
      report.invalidatedAnalyses.end(),
      std::make_move_iterator(invalidated.begin()),
      std::make_move_iterator(invalidated.end()));
  std::sort(report.invalidatedAnalyses.begin(),
            report.invalidatedAnalyses.end());
  report.invalidatedAnalyses.erase(
      std::unique(report.invalidatedAnalyses.begin(),
                  report.invalidatedAnalyses.end()),
      report.invalidatedAnalyses.end());
}

class ScalarPromotionPass final : public MirPass {
public:
  [[nodiscard]] std::string_view name() const override {
    return "scalar-promotion";
  }

  [[nodiscard]] MirPassOutcome run(MirPassContext &context,
                                   MirBodyAnalysisManager &analyses) override {
    MirPassOutcome outcome;
    outcome.report.name = std::string(name());
    outcome.report.iterations = 1;
    MirEditor editor(context.program, outcome.report.name);
    for (const MirBodyAddress address :
         enumerateMirBodyAddresses(context.program)) {
      const MirCfgAnalysis *cfg = analyses.cfg(context.program, address);
      const MirDominanceInfo *dominance =
          analyses.dominance(context.program, address);
      const auto *uses = analyses.valueUses(context.program, address);
      const auto *effectFacts = analyses.effects(context.program, address);
      const MirScalarPromotionInputs *inputs =
          analyses.scalarPromotionInputs(context.program, address);
      ++outcome.report.analyzedBodies;
      if (cfg == nullptr || dominance == nullptr || uses == nullptr ||
          effectFacts == nullptr || inputs == nullptr) {
        outcome.verification.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .message = "scalar promotion could not derive its required MIR "
                        "analyses"});
        return outcome;
      }
      outcome.report.candidates += inputs->candidates.size();
      for (const MirScalarPromotionCandidate &candidate : inputs->candidates) {
        editor.queueScalarPromotion(address, candidate.place);
      }
      for (const MirScalarPromotionMiss &miss : inputs->misses) {
        // Immutable bindings, parameters, and projected storage are outside
        // this pass's admitted candidate domain. Reporting each one would make
        // ordinary prelude code drown out actionable near misses.
        if (miss.reason == MirScalarPromotionMissReason::NotMutableBinding) {
          continue;
        }
        ++outcome.report.candidates;
        ++outcome.report.missed;
        std::string explanation = std::string(lang::name(miss.reason));
        if (const auto *aliases =
                analyses.placeAliasEscape(context.program, address)) {
          if (const auto *escape = aliases->escape(miss.place);
              escape != nullptr && escape->escapesBody()) {
            explanation += "; place escapes the body";
            if (escape->has(MirPlaceEscapeReason::UnknownCall))
              explanation += " through an unknown call";
            if (escape->has(MirPlaceEscapeReason::RetainedBorrow))
              explanation += " through a retained borrow";
            if (escape->has(MirPlaceEscapeReason::RawMemory))
              explanation += " through raw memory";
          }
        }
        outcome.report.remarks.push_back(
            {.kind = OptimizationRemarkKind::Missed,
             .pass = outcome.report.name,
             .message = "not promoted: " + explanation,
             .body = address,
             .place = miss.place});
      }
    }
    if (editor.pendingScalarPromotionCount() == 0) {
      outcome.preserved = MirPreservedAnalyses::all();
      return outcome;
    }
    const MirEditResult edited = editor.apply(analyses);
    mergeEditReport(outcome.report, edited);
    outcome.transformations = edited.transformations;
    outcome.verification = edited.verification;
    outcome.analysesInvalidated = edited.analysesInvalidated;
    outcome.preserved = edited.changed ? MirPreservedAnalyses::none()
                                       : MirPreservedAnalyses::all();
    return outcome;
  }
};

class ExactReceiverPass final : public MirPass {
public:
  std::string_view name() const override { return "exact-receiver-dispatch"; }
  MirPassOutcome run(MirPassContext &context,
                     MirBodyAnalysisManager &analyses) override {
    MirPassOutcome outcome;
    outcome.report.name = std::string(name());
    MirEditor editor(context.program, std::string(name()));
    bool queued = false;
    const MirClosedWorldFacts closedWorld =
        analyzeMirClosedWorldFacts(context.program);
    for (const auto address : enumerateMirBodyAddresses(context.program)) {
      const auto *body = findMirBody(context.program, address);
      if (!body)
        continue;
      ++outcome.report.analyzedBodies;
      for (const auto &block : body->blocks)
        for (const auto &instruction : block.instructions) {
          if (instruction.tombstone ||
              instruction.dispatch != CallDispatch::Virtual)
            continue;
          ++outcome.report.candidates;
          std::string reason;
          if (proveMirExactReceiver(context.program, address, instruction.id,
                                    &reason, &closedWorld)) {
            editor.queueExactReceiver({address, instruction.id});
            queued = true;
          } else {
            ++outcome.report.missed;
            outcome.report.remarks.push_back(
                {.kind = OptimizationRemarkKind::Missed,
                 .pass = std::string(name()),
                 .message = "virtual dispatch retained: " + reason,
                 .body = address,
                 .block = block.id,
                 .instruction = instruction.id,
                 .sourceValue = instruction.hirValue,
                 .sourceStatement = instruction.hirStatement});
          }
        }
    }
    if (!queued) {
      outcome.preserved = MirPreservedAnalyses::all();
      return outcome;
    }
    const auto edited = editor.apply(analyses);
    mergeEditReport(outcome.report, edited);
    outcome.transformations = edited.transformations;
    outcome.verification = edited.verification;
    outcome.analysesInvalidated = edited.analysesInvalidated;
    outcome.preserved = edited.changed ? MirPreservedAnalyses::none()
                                       : MirPreservedAnalyses::all();
    return outcome;
  }
};

class LiteralIdentityFoldPass final : public MirPass {
public:
  explicit LiteralIdentityFoldPass(const OptimizationResult &compatibility)
      : compatibility(compatibility) {}

  [[nodiscard]] std::string_view name() const override {
    return "fold-literal-identities";
  }

  [[nodiscard]] MirPassOutcome run(MirPassContext &context,
                                   MirBodyAnalysisManager &analyses) override {
    MirPassOutcome outcome;
    outcome.report.name = std::string(name());
    for (const MirBodyAddress address :
         enumerateMirBodyAddresses(context.program)) {
      if (analyses.valueUses(context.program, address) == nullptr ||
          analyses.dominance(context.program, address) == nullptr) {
        outcome.verification.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .message = "literal folding could not derive exact uses and "
                        "dominance"});
        return outcome;
      }
      ++outcome.report.analyzedBodies;
    }
    LiteralIdentityFoldResult folded =
        foldLiteralIdentities(context.program, compatibility, analyses);
    outcome.report = std::move(folded.report);
    outcome.report.analyzedBodies =
        enumerateMirBodyAddresses(context.program).size();
    outcome.transformations = std::move(folded.transformations);
    outcome.verification = std::move(folded.verification);
    outcome.analysesInvalidated = folded.analysesInvalidated;
    outcome.preserved = outcome.report.changed ? MirPreservedAnalyses::none()
                                               : MirPreservedAnalyses::all();
    return outcome;
  }

private:
  const OptimizationResult &compatibility;
};

class SparseConditionalConstantPass final : public MirPass {
public:
  [[nodiscard]] std::string_view name() const override {
    return "sparse-conditional-constants";
  }

  [[nodiscard]] MirPassOutcome run(MirPassContext &context,
                                   MirBodyAnalysisManager &analyses) override {
    MirPassOutcome outcome;
    outcome.report.name = std::string(name());
    outcome.report.iterations = 1;
    MirEditor editor(context.program, outcome.report.name);
    for (const MirBodyAddress address :
         enumerateMirBodyAddresses(context.program)) {
      const MirBody *body = findMirBody(context.program, address);
      const MirCfgAnalysis *cfg = analyses.cfg(context.program, address);
      const MirDominanceInfo *dominance =
          analyses.dominance(context.program, address);
      const auto *uses = analyses.valueUses(context.program, address);
      const MirSccpAnalysis *sccp =
          analyses.sparseConstants(context.program, address);
      ++outcome.report.analyzedBodies;
      if (body == nullptr || cfg == nullptr || dominance == nullptr ||
          uses == nullptr || sccp == nullptr) {
        outcome.verification.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .message = "SCCP could not derive its required MIR analyses"});
        return outcome;
      }
      if (!sccp->complete) {
        ++outcome.report.missed;
        outcome.report.remarks.push_back(
            {.kind = OptimizationRemarkKind::Missed,
             .pass = outcome.report.name,
             .message = "not propagated: executable control remained "
                        "unknown within the bounded SCCP lattice",
             .body = address});
        continue;
      }

      std::vector<MirValueId> queuedValues;
      for (const MirBlock &block : body->blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          if (instruction.tombstone ||
              instruction.kind != MirInstructionKind::Compute ||
              instruction.operation == MirOperation::Literal ||
              !instruction.result) {
            continue;
          }
          const MirSccpValueFact *fact = sccp->find(*instruction.result);
          if (fact == nullptr || fact->kind != MirSccpLatticeKind::Constant ||
              !fact->literal) {
            continue;
          }
          ++outcome.report.candidates;
          editor.queueSparseConstantReplacement(
              {.body = address, .instruction = instruction.id},
              instruction.operation, *fact->literal, fact->evidence.blocks,
              fact->evidence.instructions, fact->evidence.values);
          queuedValues.push_back(*instruction.result);
        }
      }

      const bool pathSensitiveSchedules =
          !body->loans.empty() || !body->dropObligations.empty() ||
          !body->cleanupBoundaries.empty() || !body->failureRecords.empty() ||
          std::any_of(body->blocks.begin(), body->blocks.end(),
                      [](const MirBlock &block) {
                        return block.programInitializationStep != 0;
                      });
      if (pathSensitiveSchedules || address.kind != MirBodyKind::Function) {
        continue;
      }
      for (const MirSccpBranchFact &branch : sccp->branches) {
        const MirBlock *block = body->findBlock(branch.block);
        const MirInstruction *condition =
            definingInstruction(*body, branch.condition);
        if (block == nullptr || condition == nullptr ||
            block->terminator.kind != MirTerminatorKind::Branch ||
            (condition->operation != MirOperation::Literal &&
             std::find(queuedValues.begin(), queuedValues.end(),
                       branch.condition) == queuedValues.end())) {
          continue;
        }
        ++outcome.report.candidates;
        editor.queueBranchFold(address, branch.block, branch.condition,
                               branch.taken);
      }
    }
    if (editor.pendingPatchCount() == 0 &&
        editor.pendingBranchFoldCount() == 0) {
      outcome.preserved = MirPreservedAnalyses::all();
      return outcome;
    }
    const MirEditResult edited = editor.apply(analyses);
    mergeEditReport(outcome.report, edited);
    outcome.transformations = edited.transformations;
    outcome.verification = edited.verification;
    outcome.analysesInvalidated = edited.analysesInvalidated;
    outcome.preserved = edited.changed ? MirPreservedAnalyses::none()
                                       : MirPreservedAnalyses::all();
    return outcome;
  }
};

[[nodiscard]] std::optional<MirSafetyCheckFamily>
safetyFamily(DefinedFailureOutcome outcome) {
  if (outcome.code == DefinedFailureCode::IndexOutOfBounds) {
    return MirSafetyCheckFamily::Bounds;
  }
  if (outcome.code == DefinedFailureCode::IntegerOverflow) {
    return MirSafetyCheckFamily::ArithmeticOverflow;
  }
  if (outcome.code == DefinedFailureCode::ModuloByZero) {
    return MirSafetyCheckFamily::ArithmeticDomain;
  }
  return std::nullopt;
}

class SafetyCheckEliminationPass final : public MirPass {
public:
  explicit SafetyCheckEliminationPass(MirSafetyCheckFamily family)
      : family(family), passName("discharge-") {
    passName += mirSafetyCheckFamilyName(family);
    passName += "-checks";
  }

  [[nodiscard]] std::string_view name() const override { return passName; }

  [[nodiscard]] MirPassOutcome run(MirPassContext &context,
                                   MirBodyAnalysisManager &analyses) override {
    MirPassOutcome outcome;
    outcome.report.name = passName;
    outcome.report.iterations = 1;
    MirEditor editor(context.program, passName);
    for (const MirBodyAddress address :
         enumerateMirBodyAddresses(context.program)) {
      const MirBody *body = findMirBody(context.program, address);
      const MirCfgAnalysis *cfg = analyses.cfg(context.program, address);
      const MirDominanceInfo *dominance =
          analyses.dominance(context.program, address);
      const auto *uses = analyses.valueUses(context.program, address);
      const MirSafetyAnalysis *safety =
          analyses.safetyChecks(context.program, address);
      ++outcome.report.analyzedBodies;
      if (body == nullptr || cfg == nullptr || dominance == nullptr ||
          uses == nullptr || safety == nullptr) {
        outcome.verification.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .message = "safety-check elimination could not derive CFG, "
                        "dominance, uses, and canonical safety facts"});
        return outcome;
      }
      for (const MirBlock &block : body->blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          // Report conservative boundaries once, rather than repeating them
          // for every check family. These are facts about this instruction,
          // not a claim that it caused an unrelated check's missing proof.
          if (family == MirSafetyCheckFamily::Bounds &&
              !instruction.tombstone &&
              ((instruction.kind == MirInstructionKind::Call &&
                (instruction.intrinsic == IntrinsicKind::None
                     ? !exactMirInvocationSummary(context.program, *body,
                                                  instruction)
                     : optimizationCoverage(instruction.intrinsic).handling ==
                           MirOptimizationHandling::Barrier)) ||
               instruction.rawMemoryAccess ||
               instruction.unsafeOperation != UnsafeOperationKind::None ||
               instruction.kind == MirInstructionKind::CleanupBeforeAssign ||
               instruction.kind == MirInstructionKind::Drop)) {
            const std::string reason = mirOptimizationBarrierReason(
                context.program, instruction, body);
            if (!reason.empty()) {
              outcome.report.remarks.push_back(
                  {.kind = OptimizationRemarkKind::Missed,
                   .pass = passName,
                   .message = "conservative analysis boundary: " + reason,
                   .body = address,
                   .block = block.id,
                   .instruction = instruction.id,
                   .sourceValue = instruction.hirValue,
                   .sourceStatement = instruction.hirStatement});
            }
          }
          for (std::size_t origin = 0;
               origin < instruction.definedFailure.localOrigins.size();
               ++origin) {
            for (const DefinedFailureOutcome failure :
                 instruction.definedFailure.localOrigins[origin].outcomes) {
              if (safetyFamily(failure) != family) {
                if (family == MirSafetyCheckFamily::Bounds &&
                    !safetyFamily(failure)) {
                  outcome.report.remarks.push_back(
                      {.kind = OptimizationRemarkKind::Missed,
                       .pass = passName,
                       .message =
                           "check retained: unsupported failure discharge "
                           "family " +
                           std::string(definedFailureCodeName(failure.code)) +
                           "/" +
                           std::string(
                               definedFailureDetailName(failure.detail)),
                       .body = address,
                       .block = block.id,
                       .instruction = instruction.id,
                       .sourceValue = instruction.hirValue,
                       .sourceStatement = instruction.hirStatement});
                }
                continue;
              }
              const MirSafetyDischarge discharge{
                  .family = family, .origin = origin, .outcome = failure};
              if (hasMirSafetyDischarge(instruction, discharge)) {
                continue;
              }
              ++outcome.report.candidates;
              if (safety->find(instruction.id, discharge) != nullptr) {
                editor.queueSafetyDischarge(
                    {.body = address, .instruction = instruction.id},
                    discharge);
              } else {
                ++outcome.report.missed;
                outcome.report.remarks.push_back(
                    {.kind = OptimizationRemarkKind::Missed,
                     .pass = passName,
                     .message =
                         "check retained: canonical MIR facts do not prove " +
                         std::string(mirSafetyCheckFamilyName(family)) +
                         " safety",
                     .body = address,
                     .block = block.id,
                     .instruction = instruction.id,
                     .sourceValue = instruction.hirValue,
                     .sourceStatement = instruction.hirStatement});
              }
            }
          }
        }
      }
    }
    if (editor.pendingSafetyDischargeCount() == 0) {
      outcome.preserved = MirPreservedAnalyses::all();
      return outcome;
    }
    const MirEditResult edited = editor.apply(analyses);
    mergeEditReport(outcome.report, edited);
    outcome.transformations = edited.transformations;
    outcome.verification = edited.verification;
    outcome.analysesInvalidated = edited.analysesInvalidated;
    // A discharge attaches independently replayed proof metadata. It does not
    // change any CFG, value, place, effect, or abstract-state input.
    outcome.preserved = MirPreservedAnalyses::all();
    return outcome;
  }

private:
  MirSafetyCheckFamily family = MirSafetyCheckFamily::Count;
  std::string passName;
};

class UnreachableBlockCleanupPass final : public MirPass {
public:
  [[nodiscard]] std::string_view name() const override {
    return "unreachable-block-cleanup";
  }

  [[nodiscard]] MirPassOutcome run(MirPassContext &context,
                                   MirBodyAnalysisManager &analyses) override {
    MirPassOutcome outcome;
    outcome.report.name = std::string(name());
    outcome.report.iterations = 1;
    MirEditor editor(context.program, outcome.report.name);
    for (const MirBodyAddress address :
         enumerateMirBodyAddresses(context.program)) {
      const MirCfgAnalysis *cfg = analyses.cfg(context.program, address);
      const auto *uses = analyses.valueUses(context.program, address);
      const auto *effectFacts = analyses.effects(context.program, address);
      const MirDominanceInfo *dominance =
          analyses.dominance(context.program, address);
      const MirBody *body = findMirBody(context.program, address);
      ++outcome.report.analyzedBodies;
      if (cfg == nullptr || uses == nullptr || effectFacts == nullptr ||
          dominance == nullptr || body == nullptr) {
        outcome.verification.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .message = "unreachable cleanup could not derive its required "
                        "MIR analyses"});
        return outcome;
      }
      for (std::size_t index = 0; index < cfg->reachable.size(); ++index) {
        if (cfg->reachable[index]) {
          continue;
        }
        ++outcome.report.candidates;
        MirBody replay = *body;
        if (rewriteMirUnreachableBlock(replay, index + 1)) {
          editor.queueUnreachableBlockRemoval(address, index + 1);
        } else {
          ++outcome.report.missed;
          outcome.report.remarks.push_back(
              {.kind = OptimizationRemarkKind::Missed,
               .pass = outcome.report.name,
               .message = "unreachable block retained because it carries a "
                          "lifecycle, failure, loan, or boundary schedule",
               .body = address,
               .block = index + 1});
        }
      }
    }
    if (editor.pendingUnreachableBlockCount() == 0) {
      outcome.preserved = MirPreservedAnalyses::all();
      return outcome;
    }
    const MirEditResult edited = editor.apply(analyses);
    mergeEditReport(outcome.report, edited);
    outcome.transformations = edited.transformations;
    outcome.verification = edited.verification;
    outcome.analysesInvalidated = edited.analysesInvalidated;
    outcome.preserved = edited.changed ? MirPreservedAnalyses::none()
                                       : MirPreservedAnalyses::all();
    return outcome;
  }
};

class DeadUnusedComputationPass final : public MirPass {
public:
  [[nodiscard]] std::string_view name() const override {
    return "dead-unused-computations";
  }

  [[nodiscard]] MirPassOutcome run(MirPassContext &context,
                                   MirBodyAnalysisManager &analyses) override {
    MirPassOutcome outcome;
    outcome.report.name = std::string(name());
    for (std::size_t round = 0; round < 8; ++round) {
      ++outcome.report.iterations;
      MirEditor editor(context.program, outcome.report.name);
      // Every candidate below is proven against this round's unchanged
      // program, which the canonical rewrite would otherwise verify again
      // for each one.
      std::optional<bool> programVerified;
      for (const MirBodyAddress address :
           enumerateMirBodyAddresses(context.program)) {
        const MirBody *body = findMirBody(context.program, address);
        const auto *uses = analyses.valueUses(context.program, address);
        const auto *effectFacts = analyses.effects(context.program, address);
        if (body == nullptr || uses == nullptr || effectFacts == nullptr) {
          outcome.verification.errors.push_back(
              {.bodyKind = address.kind,
               .owner = address.owner,
               .message = "dead computation removal could not derive exact "
                          "uses and effects"});
          return outcome;
        }
        ++outcome.report.analyzedBodies;
        for (const MirBlock &block : body->blocks) {
          for (const MirInstruction &instruction : block.instructions) {
            if (instruction.tombstone ||
                instruction.kind != MirInstructionKind::Compute ||
                !instruction.result ||
                !body->usesOf(*instruction.result).empty()) {
              continue;
            }
            ++outcome.report.candidates;
            const bool retainedProof = std::any_of(
                body->blocks.begin(), body->blocks.end(),
                [&](const MirBlock &candidateBlock) {
                  if (candidateBlock.terminator.provenance.kind ==
                          MirTerminatorProvenanceKind::BranchFold &&
                      candidateBlock.terminator.provenance.foldSourceValue ==
                          *instruction.result) {
                    return true;
                  }
                  return std::any_of(
                      candidateBlock.instructions.begin(),
                      candidateBlock.instructions.end(),
                      [&](const MirInstruction &candidate) {
                        if (candidate.literalProvenance.kind ==
                            MirLiteralProvenanceKind::None) {
                          return false;
                        }
                        return candidate.literalProvenance.sourceValue ==
                                   *instruction.result ||
                               std::find(candidate.literalProvenance
                                             .sourceValues.begin(),
                                         candidate.literalProvenance
                                             .sourceValues.end(),
                                         *instruction.result) !=
                                   candidate.literalProvenance.sourceValues
                                       .end();
                      });
                });
            if (retainedProof) {
              ++outcome.report.missed;
              outcome.report.remarks.push_back(
                  {.kind = OptimizationRemarkKind::Missed,
                   .pass = outcome.report.name,
                   .message = "unused computation retained as verified MIR "
                              "transformation evidence",
                   .body = address,
                   .block = block.id,
                   .instruction = instruction.id,
                   .sourceValue = instruction.hirValue,
                   .sourceStatement = instruction.hirStatement});
              continue;
            }
            if (mirRecordPublishedConstructorInitializer(
                    context.program, *body, address.owner, instruction) !=
                nullptr) {
              ++outcome.report.missed;
              outcome.report.remarks.push_back(
                  {.kind = OptimizationRemarkKind::Missed,
                   .pass = outcome.report.name,
                   .message = "unused computation retained: the constructor "
                              "record publishes an unstaged field "
                              "initializer from this value",
                   .body = address,
                   .block = block.id,
                   .instruction = instruction.id,
                   .sourceValue = instruction.hirValue,
                   .sourceStatement = instruction.hirStatement});
              continue;
            }
            if (!programVerified) {
              programVerified = verifyMirProgram(context.program).valid();
            }
            MirProgram replay = context.program;
            if (*programVerified &&
                mir_detail::rewriteVerifiedMirDeadInstruction(replay, address,
                                                              instruction.id)) {
              editor.queueDeadInstructionRemoval(
                  {.body = address, .instruction = instruction.id});
            } else {
              ++outcome.report.missed;
              const std::string reason = mirOptimizationBarrierReason(
                  context.program, instruction, body);
              outcome.report.remarks.push_back(
                  {.kind = OptimizationRemarkKind::Missed,
                   .pass = outcome.report.name,
                   .message =
                       "unused computation retained: " +
                       (reason.empty()
                            ? "unsupported rewrite shape or retained metadata"
                            : reason),
                   .body = address,
                   .block = block.id,
                   .instruction = instruction.id,
                   .sourceValue = instruction.hirValue,
                   .sourceStatement = instruction.hirStatement});
            }
          }
        }
      }
      if (editor.pendingDeadInstructionCount() == 0) {
        break;
      }
      const MirEditResult edited = editor.apply(analyses);
      mergeEditReport(outcome.report, edited);
      outcome.transformations.insert(outcome.transformations.end(),
                                     edited.transformations.begin(),
                                     edited.transformations.end());
      outcome.verification = edited.verification;
      outcome.analysesInvalidated |= edited.analysesInvalidated;
      if (!edited.valid() || !edited.changed) {
        return outcome;
      }
    }
    outcome.preserved = MirPreservedAnalyses::all();
    return outcome;
  }
};

struct SafetyCensus {
  std::size_t total = 0;
  std::array<std::size_t, static_cast<std::size_t>(MirSafetyCheckFamily::Count)>
      families{};
};

[[nodiscard]] SafetyCensus countSafetyOperations(const MirProgram &program,
                                                 bool activeOnly) {
  SafetyCensus count;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.tombstone) {
          continue;
        }
        for (std::size_t origin = 0;
             origin < instruction.definedFailure.localOrigins.size();
             ++origin) {
          for (const DefinedFailureOutcome outcome :
               instruction.definedFailure.localOrigins[origin].outcomes) {
            if (activeOnly &&
                isMirFailureOutcomeDischarged(instruction, origin, outcome)) {
              continue;
            }
            ++count.total;
            if (const std::optional<MirSafetyCheckFamily> family =
                    safetyFamily(outcome)) {
              ++count.families[static_cast<std::size_t>(*family)];
            }
          }
        }
      }
    }
  }
  return count;
}

} // namespace

OptimizedProgram OptimizationPipeline::run(OptimizationRequest request) const {
  OptimizedProgram result{.sourceMir = request.mir,
                          .mir = std::move(request.mir)};
  result.report.verificationEnabled = request.options.verifyMir;

  // Vocabulary admission is unconditional. Disabling expensive verification
  // must never let an unknown terminator masquerade as a CFG exit.
  for (const MirBodyAddress address : enumerateMirBodyAddresses(result.mir)) {
    const MirBody *body = findMirBody(result.mir, address);
    if (body == nullptr) {
      result.report.inputVerification.errors.push_back(
          {.bodyKind = address.kind,
           .owner = address.owner,
           .message = "unresolved MIR optimizer body"});
      return result;
    }
    result.report.inputVerification =
        verifyMirOptimizationVocabulary(*body, address.owner);
    if (!result.report.inputVerification.valid())
      return result;
  }

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
    MirBodyAnalysisManager analyses;
    MirPassContext context{
        .hir = request.hir, .program = result.mir, .target = request.target};
    const auto execute = [&](MirPass &pass) {
      MirPassOutcome outcome = pass.run(context, analyses);
      if (!outcome.verification.valid()) {
        result.report.outputVerification = std::move(outcome.verification);
        result.report.passes.push_back(std::move(outcome.report));
        return false;
      }
      if (outcome.report.changed && !outcome.analysesInvalidated) {
        for (const MirBodyAddress address :
             changedBodies(outcome.transformations)) {
          appendInvalidated(outcome.report,
                            analyses.invalidate(address, outcome.preserved));
        }
      }
      for (const MirTransformRecord &record : outcome.transformations) {
        const std::string message =
            record.safetyDischarge ? "discharged " +
                                         std::string(mirSafetyCheckFamilyName(
                                             record.safetyDischarge->family)) +
                                         " check with replayable GTI MIR proof"
                                   : "applied verified MIR transformation";
        outcome.report.remarks.push_back(
            {.kind = OptimizationRemarkKind::Applied,
             .pass = outcome.report.name,
             .message = message,
             .body = record.body,
             .block = record.block,
             .instruction = record.instruction,
             .place = record.place,
             .sourceValue = record.sourceValue,
             .sourceStatement = record.sourceStatement,
             .proof = record.proof});
      }
      result.report.transformations.insert(result.report.transformations.end(),
                                           outcome.transformations.begin(),
                                           outcome.transformations.end());
      result.report.passes.push_back(std::move(outcome.report));
      return true;
    };

    ScalarPromotionPass scalarPromotion;
    LiteralIdentityFoldPass literalFolding(*compatibility);
    SparseConditionalConstantPass sparseConstants;
    ExactReceiverPass exactReceivers;
    SafetyCheckEliminationPass boundsChecks(MirSafetyCheckFamily::Bounds);
    SafetyCheckEliminationPass overflowChecks(
        MirSafetyCheckFamily::ArithmeticOverflow);
    SafetyCheckEliminationPass domainChecks(
        MirSafetyCheckFamily::ArithmeticDomain);
    UnreachableBlockCleanupPass unreachableCleanup;
    DeadUnusedComputationPass deadComputations;
    if (!execute(scalarPromotion) || !execute(literalFolding) ||
        ((request.level == OptimizationLevel::O2 ||
          request.level == OptimizationLevel::O3) &&
         (!execute(sparseConstants) || !execute(exactReceivers) ||
          !execute(boundsChecks) || !execute(overflowChecks) ||
          !execute(domainChecks))) ||
        !execute(unreachableCleanup) || !execute(deadComputations)) {
      return result;
    }
  }

  std::sort(result.report.transformations.begin(),
            result.report.transformations.end(), mirTransformRecordLess);

  const SafetyCensus sourceChecks =
      countSafetyOperations(result.sourceMir, false);
  const SafetyCensus emittedChecks = countSafetyOperations(result.mir, true);
  result.report.safety.sourceOperations = sourceChecks.total;
  result.report.safety.emittedOperations = emittedChecks.total;
  result.report.safety.staticallyDischarged =
      result.report.safety.sourceOperations >=
              result.report.safety.emittedOperations
          ? result.report.safety.sourceOperations -
                result.report.safety.emittedOperations
          : 0;
  result.report.safety.sourceByFamily = sourceChecks.families;
  result.report.safety.emittedByFamily = emittedChecks.families;
  for (std::size_t family = 0;
       family < result.report.safety.dischargedByFamily.size(); ++family) {
    result.report.safety.dischargedByFamily[family] =
        sourceChecks.families[family] >= emittedChecks.families[family]
            ? sourceChecks.families[family] - emittedChecks.families[family]
            : 0;
  }

  if (request.options.verifyMir) {
    // The source snapshot is the input verified above.
    result.report.outputVerification =
        mir_detail::verifyVerifiedMirOptimizationCoherence(
            result.sourceMir, result.mir, result.report.transformations, true,
            false);
  }
  return result;
}

} // namespace lang
