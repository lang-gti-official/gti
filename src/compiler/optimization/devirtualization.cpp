#include "gti/optimization/devirtualization.h"

#include "gti/constant_evaluator.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/optimization/coverage.h"

#include <algorithm>

namespace lang {
namespace {
struct Location {
  const MirBlock *block = nullptr;
  const MirInstruction *instruction = nullptr;
  std::size_t order = 0;
};
Location locate(const MirBody &body, MirInstructionId id) {
  for (const auto &block : body.blocks)
    for (std::size_t order = 0; order < block.instructions.size(); ++order) {
      const auto &instruction = block.instructions[order];
      if (instruction.id == id)
        return {&block, &instruction, order};
    }
  return {};
}
const MirInstruction *definition(const MirBody &body, MirValueId id) {
  const auto *value = body.findValue(id);
  return value ? locate(body, value->definition).instruction : nullptr;
}
bool scalar(const SemanticType &type) {
  return type.kind == SemanticType::Bool ||
         constantIntegerDomain(type).has_value() ||
         semanticFloatFormat(type).has_value();
}
} // namespace

std::optional<MirExactReceiverProof>
proveMirExactReceiver(const MirProgram &program, MirBodyAddress address,
                      MirInstructionId id, std::string *reason) {
  const auto reject =
      [&](const char *why) -> std::optional<MirExactReceiverProof> {
    if (reason)
      *reason = why;
    return std::nullopt;
  };
  const auto *body = findMirBody(program, address);
  if (!body || address.kind != MirBodyKind::Function ||
      !verifyMirOptimizationVocabulary(*body).valid())
    return reject("unsupported body or MIR vocabulary");
  const auto location = locate(*body, id);
  const auto *call = location.instruction;
  if (!call || call->tombstone || call->kind != MirInstructionKind::Call ||
      call->dispatch != CallDispatch::Virtual || !call->functionTarget ||
      !call->receiver || call->receiver->kind != MirOperandKind::Value ||
      call->intrinsic != IntrinsicKind::None || call->callableInvocation ||
      call->callableBoundary || !call->callableArguments.empty())
    return reject("not an ordinary virtual receiver call");
  const auto *root = program.findFunctionInstance(*call->functionTarget);
  const auto *input = definition(*body, call->receiver->value);
  if (!root || !root->owner || !input ||
      input->kind != MirInstructionKind::CallInput ||
      input->callInputRole != MirCallInputRole::Receiver ||
      input->callInputKind != HirCallInputKind::ReadBorrow ||
      input->operands.size() != 1 ||
      input->operands[0].kind != MirOperandKind::BorrowRead ||
      root->receiverMutability != ReceiverMutability::ReadOnly ||
      !scalar(root->returnType) ||
      root->returnBorrowOrigin != BorrowOriginKind::None ||
      !std::all_of(root->parameterTypes.begin(), root->parameterTypes.end(),
                   scalar))
    return reject("unsupported receiver, result, or argument lifetime");
  const auto *view = body->findPlace(input->operands[0].place);
  if (!view || view->root != MirPlaceRootKind::Binding)
    return reject("receiver has no exact local object origin");
  const MirPlace *object = view;
  HirBindingId carrier = 0;
  MirLoanId receiverLoan = 0;
  if (!view->projections.empty()) {
    if (view->projections.size() != 1 ||
        view->projections[0].kind != MirProjectionKind::Dereference)
      return reject("unsupported receiver projection");
    const MirLoan *origin = nullptr;
    for (const auto &loan : body->loans) {
      if (std::find(loan.carriers.begin(), loan.carriers.end(),
                    view->binding) == loan.carriers.end())
        continue;
      if (origin || loan.carriers.size() != 1 ||
          loan.kind != MirLoanKind::Local || loan.parent || loan.entry ||
          loan.escapes || loan.access != AccessMode::ReadOnly ||
          !loan.additionalSources.empty() || !loan.designatedFields.empty() ||
          loan.lifecycleBound || loan.twoPhase)
        return reject("receiver loan is not one confined local origin");
      origin = &loan;
    }
    if (!origin)
      return reject("receiver reference has no exact local loan");
    object = body->findPlace(origin->source);
    carrier = view->binding;
    receiverLoan = origin->id;
  }
  if (!object || object->root != MirPlaceRootKind::Binding ||
      !object->projections.empty() || object->initiallyAvailable ||
      object->type.kind != SemanticType::Class ||
      object->access != AccessMode::ReadOnly)
    return reject("receiver is not an immutable complete local object");
  const MirClassInstance *concrete = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == object->type)
      concrete = &candidate;
  const auto *interface = program.findClassInstance(*root->owner);
  if (!concrete || !interface || concrete->abstract ||
      concrete->requiresActiveCleanup ||
      concrete->destructorStatus != SpecialMemberStatus::Generated ||
      concrete->bases.size() != 1 ||
      concrete->bases[0].type != interface->type ||
      interface->kind != ClassKind::Interface || !interface->bases.empty())
    return reject("unsupported inheritance or cleanup shape");
  const MirFunctionInstance *target = nullptr;
  for (const auto &candidate : program.functionInstances()) {
    if (candidate.owner != concrete->id ||
        std::find(candidate.virtualRoots.begin(), candidate.virtualRoots.end(),
                  root->declaration) == candidate.virtualRoots.end())
      continue;
    if (target)
      return reject("ambiguous exact override");
    target = &candidate;
  }
  if (!target || target->pureVirtual || target->staticMember ||
      target->requiresUnsafe || target->linkage != LanguageLinkage::Gti ||
      target->definitionKind != MirDefinitionKind::Source ||
      target->parameterTypes != root->parameterTypes ||
      target->returnType != root->returnType ||
      target->receiverMutability != ReceiverMutability::ReadOnly ||
      !target->mayRaiseDefinedFailure ||
      target->returnBorrowOrigin != BorrowOriginKind::None ||
      !target->callableParameters.empty())
    return reject("unsupported exact target or failure-channel shape");
  const auto &summary = target->effectSummary;
  if (summary.receiverEscapes ||
      std::any_of(summary.argumentsEscape.begin(),
                  summary.argumentsEscape.end(), [](bool v) { return v; }) ||
      summary.traits.writesPlace || summary.traits.readsUnknownMemory ||
      summary.traits.writesUnknownMemory || summary.traits.allocates ||
      summary.traits.maySynchronize || summary.traits.targetDependent)
    return reject(
        "target escapes, synchronizes, or has unsupported memory effects");
  const auto dominance = computeMirDominance(*body);
  if (!dominance || !dominance->isReachable(location.block->id))
    return reject("unreachable receiver or unavailable dominance");
  const MirInstruction *initialization = nullptr;
  const MirBlock *initializationBlock = nullptr;
  std::size_t initializationOrder = 0;
  const auto touches = [&](MirPlaceId placeId) {
    const auto *place = body->findPlace(placeId);
    return place && place->root == MirPlaceRootKind::Binding &&
           (place->binding == object->binding ||
            (carrier && place->binding == carrier));
  };
  for (const auto &loan : body->loans) {
    if (loan.id != receiverLoan &&
        (touches(loan.source) ||
         std::any_of(loan.additionalSources.begin(),
                     loan.additionalSources.end(), touches)))
      return reject("receiver has another loan or escape path");
  }
  for (const auto &block : body->blocks) {
    for (std::size_t order = 0; order < block.instructions.size(); ++order) {
      const auto &instruction = block.instructions[order];
      if (instruction.tombstone)
        continue;
      if (instruction.rawMemoryAccess ||
          instruction.unsafeOperation != UnsafeOperationKind::None ||
          instruction.synchronization.kind !=
              SynchronizationOperationKind::None)
        return reject(
            "raw-memory or synchronization boundary in receiver body");
      if (instruction.destination && touches(*instruction.destination) &&
          instruction.kind != MirInstructionKind::Drop) {
        if (instruction.kind != MirInstructionKind::Initialize)
          return reject("receiver or reference may be replaced");
        if (*instruction.destination == object->id) {
          if (initialization)
            return reject("multiple receiver initializations");
          initialization = &instruction;
          initializationBlock = &block;
          initializationOrder = order;
        }
      }
      for (const auto &operand : instruction.operands) {
        if (!operand.place || !touches(operand.place))
          continue;
        if (instruction.id == input->id ||
            instruction.kind == MirInstructionKind::Borrow ||
            instruction.kind == MirInstructionKind::EndBorrow ||
            instruction.kind == MirInstructionKind::Drop ||
            instruction.kind == MirInstructionKind::Lifecycle)
          continue;
        if (carrier && instruction.kind == MirInstructionKind::Initialize &&
            instruction.destination &&
            body->findPlace(*instruction.destination)->binding == carrier &&
            operand.kind == MirOperandKind::Loan &&
            operand.loan == receiverLoan)
          continue;
        return reject("receiver has another escaping or unsupported use");
      }
    }
  }
  if (!initialization || !initializationBlock ||
      !dominance->dominates(initializationBlock->id, location.block->id) ||
      (initializationBlock == location.block &&
       initializationOrder >= location.order))
    return reject(
        "complete receiver initialization does not dominate the call");
  if (body->usesOf(call->receiver->value).size() != 1)
    return reject("receiver value has multiple consumers");
  return MirExactReceiverProof{location.block->id,    id,         input->id,
                               call->receiver->value, object->id, target->id};
}

std::optional<MirTransformRecord>
rewriteMirExactReceiver(MirProgram &program, MirBodyAddress address,
                        MirInstructionId id, std::string pass) {
  const auto proof = proveMirExactReceiver(program, address, id);
  if (!proof)
    return std::nullopt;
  auto *body = findMirBody(program, address);
  const auto *object = body->findPlace(proof->concretePlace);
  for (auto &block : body->blocks)
    for (auto &instruction : block.instructions) {
      if (instruction.id == proof->receiverInput) {
        instruction.operands[0].place = object->id;
        instruction.operands[0].type = object->type;
        instruction.info.type = object->type;
        instruction.info.traits = object->traits;
        body->values[proof->receiverValue - 1].info = instruction.info;
      }
      if (instruction.id == id) {
        instruction.receiver->type = object->type;
        instruction.functionTarget = proof->target;
        instruction.dispatch = CallDispatch::Static;
        instruction.dispatchOwner = SemanticType::Unknown;
        instruction.definedFailure.propagation =
            FailurePropagationKind::DirectCall;
      }
    }
  const auto *call = locate(*body, id).instruction;
  return MirTransformRecord{.pass = std::move(pass),
                            .kind = MirTransformKind::DevirtualizeCall,
                            .proof = MirTransformProofKind::ExactReceiver,
                            .body = address,
                            .block = proof->block,
                            .instruction = id,
                            .sourceValue = call->hirValue,
                            .sourceStatement = call->hirStatement,
                            .relatedInstructions = {proof->receiverInput},
                            .relatedValues = {proof->receiverValue},
                            .relatedPlaces = {proof->concretePlace},
                            .requiredAnalyses = {.valueUses = true,
                                                 .effects = true,
                                                 .reachability = true,
                                                 .dominance = true}};
}
} // namespace lang
