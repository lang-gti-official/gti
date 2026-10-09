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

// The dereferenced reference parameter of the enclosing function, if any.
std::optional<std::size_t> receiverParameter(const MirProgram &program,
                                             MirBodyAddress address,
                                             const MirPlace &view) {
  const auto *function = address.kind == MirBodyKind::Function
                             ? program.findFunctionInstance(address.owner)
                             : nullptr;
  if (!function || view.root != MirPlaceRootKind::Binding || view.capture ||
      view.projections.size() != 1 ||
      view.projections[0].kind != MirProjectionKind::Dereference)
    return std::nullopt;
  const auto found = std::find(function->parameterBindings.begin(),
                               function->parameterBindings.end(), view.binding);
  if (found == function->parameterBindings.end())
    return std::nullopt;
  return static_cast<std::size_t>(
      std::distance(function->parameterBindings.begin(), found));
}

// Selects the single source override of `root` in the exact class `type`.
// Both receiver forms share these inheritance, target and effect limits.
const MirFunctionInstance *exactOverride(const MirProgram &program,
                                         const MirFunctionInstance &root,
                                         const SemanticType &type,
                                         bool requireTrivialCleanup,
                                         const char **reason) {
  const MirClassInstance *concrete = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == type)
      concrete = &candidate;
  const auto *interface = program.findClassInstance(*root.owner);
  if (!concrete || !interface || concrete->abstract ||
      (requireTrivialCleanup &&
       (concrete->requiresActiveCleanup ||
        concrete->destructorStatus != SpecialMemberStatus::Generated)) ||
      concrete->bases.size() != 1 ||
      concrete->bases[0].type != interface->type ||
      interface->kind != ClassKind::Interface || !interface->bases.empty()) {
    *reason = "unsupported inheritance or cleanup shape";
    return nullptr;
  }
  const MirFunctionInstance *target = nullptr;
  for (const auto &candidate : program.functionInstances()) {
    if (candidate.owner != concrete->id ||
        std::find(candidate.virtualRoots.begin(), candidate.virtualRoots.end(),
                  root.declaration) == candidate.virtualRoots.end())
      continue;
    if (target) {
      *reason = "ambiguous exact override";
      return nullptr;
    }
    target = &candidate;
  }
  if (!target || target->pureVirtual || target->staticMember ||
      target->requiresUnsafe || target->linkage != LanguageLinkage::Gti ||
      target->definitionKind != MirDefinitionKind::Source ||
      target->parameterTypes != root.parameterTypes ||
      target->returnType != root.returnType ||
      target->receiverMutability != ReceiverMutability::ReadOnly ||
      !target->mayRaiseDefinedFailure ||
      target->returnBorrowOrigin != BorrowOriginKind::None ||
      !target->callableParameters.empty()) {
    *reason = "unsupported exact target or failure-channel shape";
    return nullptr;
  }
  const auto &summary = target->effectSummary;
  if (summary.receiverEscapes ||
      std::any_of(summary.argumentsEscape.begin(),
                  summary.argumentsEscape.end(), [](bool v) { return v; }) ||
      summary.traits.writesPlace || summary.traits.readsUnknownMemory ||
      summary.traits.writesUnknownMemory || summary.traits.allocates ||
      summary.traits.maySynchronize || summary.traits.targetDependent) {
    *reason = "target escapes, synchronizes, or has unsupported memory effects";
    return nullptr;
  }
  return target;
}
} // namespace

std::optional<MirExactReceiverProof>
proveMirExactReceiver(const MirProgram &program, MirBodyAddress address,
                      MirInstructionId id, std::string *reason,
                      const MirClosedWorldFacts *closedWorld) {
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
  if (const auto parameter = receiverParameter(program, address, *view)) {
    // The parameter form keeps the receiver borrow, its place and loans
    // unchanged, so the referent's own cleanup is not part of this proof.
    const SemanticType *exact =
        closedWorld
            ? closedWorld->exactParameterClass(address.owner, *parameter)
            : nullptr;
    if (!exact)
      return reject("receiver parameter has no closed-world exact class");
    const auto *interface = program.findClassInstance(*root->owner);
    if (!interface || view->type != interface->type)
      return reject("receiver parameter is not viewed as the root interface");
    const char *why = nullptr;
    const auto *target = exactOverride(program, *root, *exact, false, &why);
    if (!target)
      return reject(why);
    const auto dominance = computeMirDominance(*body);
    if (!dominance || !dominance->isReachable(location.block->id))
      return reject("unreachable receiver or unavailable dominance");
    if (body->usesOf(call->receiver->value).size() != 1)
      return reject("receiver value has multiple consumers");
    return MirExactReceiverProof{.block = location.block->id,
                                 .instruction = id,
                                 .receiverInput = input->id,
                                 .receiverValue = call->receiver->value,
                                 .concretePlace = view->id,
                                 .target = target->id,
                                 .parameter = parameter,
                                 .exactClass = *exact};
  }
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
          loan.storedDependency || loan.escapes ||
          loan.access != AccessMode::ReadOnly ||
          !loan.additionalSources().empty() || !loan.designatedFields.empty() ||
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
  const char *why = nullptr;
  const auto *target = exactOverride(program, *root, object->type, true, &why);
  if (!target)
    return reject(why);
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
         std::any_of(
             loan.additionalDependencies.begin(),
             loan.additionalDependencies.end(), [&](const auto &dependency) {
               return dependency.source != 0 && touches(dependency.source);
             })))
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
  return MirExactReceiverProof{.block = location.block->id,
                               .instruction = id,
                               .receiverInput = input->id,
                               .receiverValue = call->receiver->value,
                               .concretePlace = object->id,
                               .target = target->id};
}

std::optional<MirTransformRecord>
rewriteMirExactReceiver(MirProgram &program, MirBodyAddress address,
                        MirInstructionId id, std::string pass,
                        const MirClosedWorldFacts *closedWorld) {
  const auto proof =
      proveMirExactReceiver(program, address, id, nullptr, closedWorld);
  if (!proof)
    return std::nullopt;
  auto *body = findMirBody(program, address);
  if (proof->parameter) {
    for (auto &block : body->blocks)
      for (auto &instruction : block.instructions)
        if (instruction.id == id) {
          // A static call whose receiver keeps its interface place and is
          // presented at the proven exact class.
          instruction.functionTarget = proof->target;
          instruction.dispatch = CallDispatch::Static;
          instruction.dispatchOwner = proof->exactClass;
          instruction.definedFailure.propagation =
              FailurePropagationKind::DirectCall;
        }
    const auto *call = locate(*body, id).instruction;
    return MirTransformRecord{.pass = std::move(pass),
                              .kind = MirTransformKind::DevirtualizeCall,
                              .proof =
                                  MirTransformProofKind::ExactParameterReceiver,
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
                                                   .dominance = true,
                                                   .closedWorld = true}};
  }
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
