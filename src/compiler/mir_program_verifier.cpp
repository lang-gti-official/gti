#include "gti/mir.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/safety.h"
#include "gti/optimization/sccp.h"
#include "mir_verification_queries.h"
#include "mir_verification_support.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] const MirClassInstance *
exactMirCapabilityClass(const MirProgram &program, const SemanticType &type,
                        CompilerCapabilityTypeKind capability);
[[nodiscard]] bool mirPublicBaseConversion(const MirProgram &program,
                                           const SemanticType &derived,
                                           const SemanticType &base);

struct MirStoredCarrierReference {
  std::vector<SymbolId> fields;
  SemanticType reference = SemanticType::Unknown;
};

[[nodiscard]] std::vector<MirStoredCarrierReference>
findStoredCarrierReferences(const MirProgram &program,
                            const SemanticType &type);

[[nodiscard]] bool
exactRestoredBorrowFields(const MirProgram &program, const SemanticType &type,
                          const std::vector<RestoredBorrowField> &fields) {
  const auto paths = findStoredCarrierReferences(program, type);
  if (paths.size() != fields.size())
    return false;
  for (std::size_t index = 0; index < paths.size(); ++index)
    if (paths[index].fields != fields[index].fields ||
        paths[index].reference != fields[index].referenceType)
      return false;
  return true;
}

// Follow only executable reference publications. A summary flag or matching
// signature alone never establishes restoration authority, including through
// recursive helpers: every finite proof must reach the checked intrinsic.
[[nodiscard]] const MirInstruction *
restorationProducer(const MirBody &body, MirLoanId loan,
                    std::unordered_set<MirLoanId> &seen) {
  if (!seen.insert(loan).second)
    return nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions) {
      if (instruction.loan != loan)
        continue;
      if (instruction.kind == MirInstructionKind::Call)
        return instruction.restoresReferenceResult ? &instruction : nullptr;
      if (instruction.kind == MirInstructionKind::Borrow &&
          instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Loan)
        return restorationProducer(body, instruction.operands.front().loan,
                                   seen);
    }
  const auto *record = body.findLoan(loan);
  return record && record->parent
             ? restorationProducer(body, record->parent, seen)
             : nullptr;
}

[[nodiscard]] bool
verifiedRestoreSummary(const MirProgram &program,
                       const MirFunctionInstance &function,
                       std::unordered_set<HirFunctionInstanceId> &visiting) {
  if (!function.restoresReferenceResult ||
      function.definitionKind != MirDefinitionKind::Source ||
      function.returnType.kind != SemanticType::Reference ||
      function.returnType.arguments.size() != 1 ||
      function.returnBorrowOrigin != BorrowOriginKind::Argument ||
      function.returnBorrowParameter >= function.parameterBindings.size() ||
      !function.returnBorrowFields.empty() ||
      !function.returnBorrowParameters.empty() || function.returnBorrowPlace ||
      !exactRestoredBorrowFields(program, function.returnType.arguments.front(),
                                 function.restoredBorrowFields) ||
      !visiting.insert(function.id).second)
    return false;
  const auto &body = function.body;
  bool returned = false;
  for (const auto &block : body.blocks) {
    if (block.terminator.kind != MirTerminatorKind::Return)
      continue;
    returned = true;
    if (!block.terminator.returnLoan)
      return false;
    std::unordered_set<MirLoanId> seen;
    const auto *producer =
        restorationProducer(body, *block.terminator.returnLoan, seen);
    const auto *loan =
        producer && producer->loan ? body.findLoan(*producer->loan) : nullptr;
    const auto *source = loan ? body.findPlace(loan->source) : nullptr;
    if (!producer || !source || source->root != MirPlaceRootKind::Binding ||
        source->binding !=
            function.parameterBindings[function.returnBorrowParameter] ||
        !source->projections.empty() ||
        producer->info.type != function.returnType.arguments.front() ||
        producer->borrowAccess != function.returnType.referenceAccess ||
        producer->restoredBorrowFields != function.restoredBorrowFields)
      return false;
    if (producer->functionTarget) {
      const auto *target =
          program.findFunctionInstance(*producer->functionTarget);
      if (!target || !verifiedRestoreSummary(program, *target, visiting))
        return false;
    } else if ((producer->intrinsic != IntrinsicKind::RestoreReference &&
                producer->intrinsic != IntrinsicKind::RestoreReferenceMut) ||
               !mir_detail::erasedReferenceResultLoan(program, body,
                                                      *producer)) {
      return false;
    }
  }
  visiting.erase(function.id);
  return returned;
}

[[nodiscard]] bool verifiedRestoreCall(const MirProgram &program,
                                       const MirBody &body,
                                       const MirInstruction &instruction) {
  if (!instruction.restoresReferenceResult ||
      instruction.kind != MirInstructionKind::Call || !instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument >= instruction.parameterTypes.size() ||
      instruction.borrowArgument >= instruction.operands.size() ||
      !instruction.borrowFields.empty() ||
      !instruction.borrowArguments.empty() || instruction.borrowPlace ||
      !exactRestoredBorrowFields(program, instruction.info.type,
                                 instruction.restoredBorrowFields))
    return false;
  const auto &witness = instruction.parameterTypes[instruction.borrowArgument];
  if (witness.kind != SemanticType::Reference ||
      witness.arguments.size() != 1 ||
      !exactMirCapabilityClass(program, witness.arguments.front(),
                               CompilerCapabilityTypeKind::ErasedObject) ||
      (instruction.borrowAccess == AccessMode::Mutable &&
       witness.referenceAccess != AccessMode::Mutable))
    return false;
  if (!instruction.functionTarget)
    return (instruction.intrinsic == IntrinsicKind::RestoreReference ||
            instruction.intrinsic == IntrinsicKind::RestoreReferenceMut) &&
           mir_detail::erasedReferenceResultLoan(program, body, instruction);
  const auto *target =
      program.findFunctionInstance(*instruction.functionTarget);
  std::unordered_set<HirFunctionInstanceId> visiting;
  return target &&
         instruction.restoredBorrowFields == target->restoredBorrowFields &&
         instruction.borrowArgument == target->returnBorrowParameter &&
         verifiedRestoreSummary(program, *target, visiting);
}

[[nodiscard]] std::optional<SemanticType>
mirFieldProjectionType(const MirProgram &program, const SemanticType &receiver,
                       SymbolId symbol);

[[nodiscard]] std::optional<BorrowedFieldOrigin> constructorFieldOrigin(
    const MirProgram &program, const MirConstructorInstance &instance,
    const MirStoredCarrierReference &path, std::size_t depth = 0);

bool exactCThreadRuntimeFunction(const MirFunctionInstance &function) {
  const auto operation = function.cThreadOperation;
  if (operation == CThreadRuntimeOperation::None) {
    return true;
  }
  if (operation >= CThreadRuntimeOperation::Count || function.owner ||
      function.linkage != LanguageLinkage::C ||
      function.definitionKind != MirDefinitionKind::Declaration ||
      !function.typeArguments.empty() || !function.valueArguments.empty() ||
      function.cArrayCountParameter || function.requiresUnsafe) {
    return false;
  }
  const SemanticType context = SemanticType::rawPointerTo(SemanticType::Void);
  switch (operation) {
  case CThreadRuntimeOperation::Create:
    return function.externalSymbol == "gti_rt_cthread_create_v1" &&
           function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{
                   SemanticType::rawPointerTo(SemanticType::UInt64),
                   SemanticType::nativeFunctionType(SemanticType::Int32,
                                                    std::array{context}),
                   context};
  case CThreadRuntimeOperation::Join:
    return function.externalSymbol == "gti_rt_cthread_join_v1" &&
           function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{
                   SemanticType::UInt64,
                   SemanticType::rawPointerTo(SemanticType::Int32)};
  case CThreadRuntimeOperation::MutexInit:
    return function.externalSymbol == "gti_rt_mtx_init_v1" &&
           function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{
                   SemanticType::rawPointerTo(SemanticType::UInt64),
                   SemanticType::Int32};
  case CThreadRuntimeOperation::InvokeOnceInitializer:
    return function.externalSymbol == "gti_rt_once_invoke_v1" &&
           function.returnType == SemanticType::Void &&
           function.parameterTypes ==
               std::vector<SemanticType>{
                   SemanticType::nativeFunctionType(SemanticType::Void, {})};
  case CThreadRuntimeOperation::ConditionInit:
    return function.externalSymbol == "gti_rt_cnd_init_v1" &&
           function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{
                   SemanticType::rawPointerTo(SemanticType::UInt64)};
  case CThreadRuntimeOperation::ConditionDestroy:
    return function.externalSymbol == "gti_rt_cnd_destroy_v1" &&
           function.returnType == SemanticType::Void &&
           function.parameterTypes ==
               std::vector<SemanticType>{SemanticType::UInt64};
  case CThreadRuntimeOperation::ConditionSignal:
  case CThreadRuntimeOperation::ConditionBroadcast:
    return function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{SemanticType::UInt64} &&
           function.externalSymbol ==
               (operation == CThreadRuntimeOperation::ConditionSignal
                    ? "gti_rt_cnd_signal_v1"
                    : "gti_rt_cnd_broadcast_v1");
  case CThreadRuntimeOperation::ConditionWait:
    return function.externalSymbol == "gti_rt_cnd_wait_v1" &&
           function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{SemanticType::UInt64,
                                         SemanticType::UInt64};
  case CThreadRuntimeOperation::ConditionTimedWait:
    return function.externalSymbol == "gti_rt_cnd_timedwait_v1" &&
           function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{
                   SemanticType::UInt64, SemanticType::UInt64,
                   SemanticType::Int64, SemanticType::Int64};
  case CThreadRuntimeOperation::MutexDestroy:
    return function.externalSymbol == "gti_rt_mtx_destroy_v1" &&
           function.returnType == SemanticType::Void &&
           function.parameterTypes ==
               std::vector<SemanticType>{SemanticType::UInt64};
  case CThreadRuntimeOperation::MutexTimedLock:
    return function.externalSymbol == "gti_rt_mtx_timedlock_v1" &&
           function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{SemanticType::UInt64,
                                         SemanticType::Int64,
                                         SemanticType::Int64};
  case CThreadRuntimeOperation::MutexLock:
  case CThreadRuntimeOperation::MutexTryLock:
  case CThreadRuntimeOperation::MutexUnlock:
    return function.returnType == SemanticType::Int32 &&
           function.parameterTypes ==
               std::vector<SemanticType>{SemanticType::UInt64} &&
           function.externalSymbol ==
               (operation == CThreadRuntimeOperation::MutexLock
                    ? "gti_rt_mtx_lock_v1"
                : operation == CThreadRuntimeOperation::MutexTryLock
                    ? "gti_rt_mtx_trylock_v1"
                    : "gti_rt_mtx_unlock_v1");
  case CThreadRuntimeOperation::Begin:
  case CThreadRuntimeOperation::End:
    return function.returnType == SemanticType::Void &&
           function.parameterTypes.empty() &&
           function.externalSymbol ==
               (operation == CThreadRuntimeOperation::Begin
                    ? "gti_rt_cthread_begin_v1"
                    : "gti_rt_cthread_end_v1");
  case CThreadRuntimeOperation::None:
  case CThreadRuntimeOperation::Count:
    return false;
  }
  return false;
}

// Re-establishes the declared process-allocator runtime contract from the
// declaration itself, so a forged classification cannot lend its precise
// effect summary to an unrelated native declaration.
bool exactProcessMemoryRuntimeFunction(const MirProgram &program,
                                       const MirFunctionInstance &function) {
  const auto operation = function.processMemoryOperation;
  if (operation == ProcessMemoryRuntimeOperation::None) {
    return true;
  }
  return operation < ProcessMemoryRuntimeOperation::Count && !function.owner &&
         !function.staticMember &&
         function.cThreadOperation == CThreadRuntimeOperation::None &&
         function.linkage == LanguageLinkage::C &&
         function.definitionKind == MirDefinitionKind::Declaration &&
         function.typeArguments.empty() && function.valueArguments.empty() &&
         !function.cArrayCountParameter && !function.requiresUnsafe &&
         !function.neverReturns && !function.tracksCaller &&
         !function.virtualMethod && !function.pureVirtual &&
         exactProcessMemoryRuntimeOperation(
             function.externalSymbol, function.returnType,
             function.parameterTypes) == operation &&
         processMemoryRuntimeSelectedForTarget(operation, program.target().os);
}

SynchronizationOperationKind
cThreadSynchronization(const MirProgram &program,
                       const MirInstruction &instruction) {
  const MirFunctionInstance *target =
      instruction.functionTarget
          ? program.findFunctionInstance(*instruction.functionTarget)
          : nullptr;
  if (target == nullptr || instruction.kind != MirInstructionKind::Call ||
      instruction.intrinsic != IntrinsicKind::None) {
    return SynchronizationOperationKind::None;
  }
  return target->cThreadOperation == CThreadRuntimeOperation::Create
             ? SynchronizationOperationKind::ThreadSpawn
         : target->cThreadOperation == CThreadRuntimeOperation::Join
             ? SynchronizationOperationKind::ThreadJoin
         : (target->cThreadOperation == CThreadRuntimeOperation::MutexLock ||
            target->cThreadOperation == CThreadRuntimeOperation::MutexTryLock ||
            target->cThreadOperation == CThreadRuntimeOperation::MutexTimedLock)
             ? SynchronizationOperationKind::MutexLock
         : target->cThreadOperation == CThreadRuntimeOperation::MutexUnlock
             ? SynchronizationOperationKind::MutexUnlock
         : (target->cThreadOperation ==
                CThreadRuntimeOperation::ConditionWait ||
            target->cThreadOperation ==
                CThreadRuntimeOperation::ConditionTimedWait)
             ? SynchronizationOperationKind::ConditionWait
         : (target->cThreadOperation ==
                CThreadRuntimeOperation::ConditionSignal ||
            target->cThreadOperation ==
                CThreadRuntimeOperation::ConditionBroadcast)
             ? SynchronizationOperationKind::ConditionNotify
             : SynchronizationOperationKind::None;
}

using mir_detail::appendVerification;
using mir_detail::canonicalBorrowOrigin;
using mir_detail::CanonicalPlace;
using mir_detail::constantMatchesType;
using mir_detail::freshOwnedValueInto;
using mir_detail::linearDefinition;
using mir_detail::movedValueInto;
using mir_detail::receiverIsConsumed;
using mir_detail::referenceInitializerCall;
using mir_detail::sameCanonicalPlace;
using mir_detail::sameGlobalBorrowPlace;
using mir_detail::terminatorSuccessors;
using mir_detail::verificationFailure;

[[nodiscard]] bool
exactAtomicSynchronization(IntrinsicKind intrinsic,
                           const SynchronizationOperation &operation) {
  SynchronizationOperationKind expected = SynchronizationOperationKind::None;
  switch (intrinsic) {
  case IntrinsicKind::AtomicLoad:
    expected = SynchronizationOperationKind::AtomicLoad;
    break;
  case IntrinsicKind::AtomicWait:
    expected = SynchronizationOperationKind::AtomicWait;
    break;
  case IntrinsicKind::AtomicNotifyOne:
  case IntrinsicKind::AtomicNotifyAll:
    expected = SynchronizationOperationKind::AtomicNotify;
    break;
  case IntrinsicKind::AtomicStore:
    expected = SynchronizationOperationKind::AtomicStore;
    break;
  case IntrinsicKind::AtomicExchange:
    expected = SynchronizationOperationKind::AtomicReadModifyWrite;
    break;
  case IntrinsicKind::AtomicCompareExchange:
    expected = SynchronizationOperationKind::AtomicCompareExchange;
    break;
  default:
    return false;
  }
  return operation.kind == expected && validSynchronizationOperation(operation);
}

[[nodiscard]] bool
exactMutexSynchronization(IntrinsicKind intrinsic,
                          const SynchronizationOperation &operation) {
  const SynchronizationOperationKind expected =
      intrinsic == IntrinsicKind::MutexLock
          ? SynchronizationOperationKind::MutexLock
      : intrinsic == IntrinsicKind::MutexUnlock
          ? SynchronizationOperationKind::MutexUnlock
          : SynchronizationOperationKind::None;
  return expected != SynchronizationOperationKind::None &&
         operation.kind == expected && validSynchronizationOperation(operation);
}

[[nodiscard]] const MirInstruction *definitionFor(const MirBody &body,
                                                  MirValueId valueId) {
  return mir_detail::definitionFor(body, valueId);
}

[[nodiscard]] bool callableContractRequiresOnce(
    const MirProgram &program, HirFunctionInstanceId function,
    std::size_t parameterIndex,
    std::vector<std::pair<HirFunctionInstanceId, std::size_t>> &visiting) {
  const MirFunctionInstance *target = program.findFunctionInstance(function);
  if (target == nullptr) {
    return false;
  }
  const auto contract = std::find_if(
      target->callableParameters.begin(), target->callableParameters.end(),
      [&](const MirCallableParameter &candidate) {
        return candidate.parameterIndex == parameterIndex;
      });
  if (contract == target->callableParameters.end() ||
      contract->boundary != CallableBoundary::Confined) {
    return false;
  }
  if (std::any_of(contract->signatures.begin(), contract->signatures.end(),
                  [](const MirCallableSignature &signature) {
                    return signature.requiredCapability ==
                           CallableInvocationCapability::Once;
                  })) {
    return true;
  }

  const std::pair key{function, parameterIndex};
  if (std::find(visiting.begin(), visiting.end(), key) != visiting.end()) {
    return false;
  }
  visiting.push_back(key);
  const bool requiresOnce =
      std::any_of(contract->forwardings.begin(), contract->forwardings.end(),
                  [&](const MirCallableForwarding &forwarding) {
                    return forwarding.functionTarget &&
                           callableContractRequiresOnce(
                               program, *forwarding.functionTarget,
                               forwarding.parameterIndex, visiting);
                  });
  visiting.pop_back();
  return requiresOnce;
}

[[nodiscard]] bool callableContractRequiresOnce(const MirProgram &program,
                                                HirFunctionInstanceId function,
                                                std::size_t parameterIndex) {
  std::vector<std::pair<HirFunctionInstanceId, std::size_t>> visiting;
  return callableContractRequiresOnce(program, function, parameterIndex,
                                      visiting);
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForOperand(const MirBody &body, const MirOperand &operand,
                                std::size_t depth = 0);

[[nodiscard]] bool exactMoveFromBinding(const MirBody &body,
                                        const MirOperand &operand,
                                        HirBindingId binding,
                                        std::size_t depth = 0) {
  if ((depth > body.values.size() &&
       depth > body.values.size() + body.instructionCount()) ||
      operand.kind != MirOperandKind::Value || binding == 0) {
    return false;
  }
  const MirInstruction *definition = definitionFor(body, operand.value);
  if (definition == nullptr || definition->operands.size() != 1) {
    return false;
  }
  if (definition->kind == MirInstructionKind::Move) {
    return definition->operands.front().kind == MirOperandKind::Move &&
           definition->ownership &&
           definition->ownership->kind == OwnershipEventKind::Move &&
           definition->ownership->before == OwnershipStateSet::Available &&
           definition->ownership->after == OwnershipStateSet::Moved &&
           callableSourceBindingForOperand(body,
                                           definition->operands.front()) ==
               std::optional<HirBindingId>{binding};
  }
  const bool transparent =
      (definition->kind == MirInstructionKind::Compute &&
       definition->operation == MirOperation::Identity) ||
      (definition->kind == MirInstructionKind::CallInput &&
       definition->callInputKind == HirCallInputKind::MoveValue);
  return transparent && exactMoveFromBinding(body, definition->operands.front(),
                                             binding, depth + 1);
}

[[nodiscard]] bool
hasOneExactExecutableValueUse(const MirBody &body, MirValueId valueId,
                              MirValueUseKind expectedKind,
                              MirInstructionId expectedInstruction = 0,
                              std::size_t expectedOperandIndex = 0) {
  const MirInstruction *definition = linearDefinition(body, valueId);
  if (definition == nullptr) {
    return false;
  }

  std::optional<MirPlaceId> bookkeepingPlace;
  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value && place.value == valueId) {
      const bool exactObligation = std::any_of(
          body.dropObligations.begin(), body.dropObligations.end(),
          [&](const MirDropObligation &obligation) {
            return obligation.kind == MirDropObligationKind::Value &&
                   obligation.place == place.id &&
                   obligation.value == definition->hirValue;
          });
      if (bookkeepingPlace || !place.projections.empty() ||
          place.sourceValue != definition->hirValue || !exactObligation) {
        return false;
      }
      bookkeepingPlace = place.id;
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Index ||
           projection.kind == MirProjectionKind::RawIndex) &&
          projection.index == valueId) {
        return false;
      }
    }
  }

  std::size_t executableUses = 0;
  bool matched = false;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      // An unpublished return still belongs to the callee while its remaining
      // cleanup runs. Its failure-only drop is not another successful use of
      // the returned value. Body lifecycle verification owns reachability and
      // activation; retain exact value/place/obligation identity here.
      const bool pendingReturnCleanup =
          expectedKind == MirValueUseKind::Terminator && bookkeepingPlace &&
          instruction.kind == MirInstructionKind::Drop &&
          instruction.destination == bookkeepingPlace &&
          block.activeFailure != 0 && instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
          instruction.lifecycle.front().failureCleanup && [&] {
            const MirDropObligation *drop =
                body.findDropObligation(instruction.lifecycle.front().source);
            return drop != nullptr &&
                   drop->kind == MirDropObligationKind::ReturnResult &&
                   drop->place == *bookkeepingPlace &&
                   drop->value == definition->hirValue &&
                   drop->dropType.type == definition->info.type;
          }();
      if (bookkeepingPlace &&
          ((instruction.destination &&
            *instruction.destination == *bookkeepingPlace &&
            !pendingReturnCleanup) ||
           (instruction.receiver &&
            instruction.receiver->place == *bookkeepingPlace) ||
           std::any_of(instruction.operands.begin(), instruction.operands.end(),
                       [&](const MirOperand &candidate) {
                         return candidate.place == *bookkeepingPlace;
                       }))) {
        return false;
      }
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value &&
          instruction.receiver->value == valueId) {
        ++executableUses;
        matched =
            matched || (expectedKind == MirValueUseKind::InstructionReceiver &&
                        instruction.id == expectedInstruction);
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind != MirOperandKind::Value ||
            instruction.operands[index].value != valueId) {
          continue;
        }
        ++executableUses;
        matched =
            matched || (expectedKind == MirValueUseKind::InstructionOperand &&
                        instruction.id == expectedInstruction &&
                        index == expectedOperandIndex);
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value &&
        block.terminator.value->value == valueId) {
      ++executableUses;
      matched = matched || expectedKind == MirValueUseKind::Terminator;
    }
    if (bookkeepingPlace && block.terminator.value &&
        block.terminator.value->place == *bookkeepingPlace) {
      return false;
    }
  }
  return executableUses == 1 && matched;
}

[[nodiscard]] bool exactReturnedMoveFromBinding(
    const MirBody &body, const MirOperand &operand, HirBindingId binding,
    MirValueUseKind expectedKind = MirValueUseKind::Terminator,
    MirInstructionId expectedInstruction = 0,
    std::size_t expectedOperandIndex = 0, std::size_t depth = 0) {
  if ((depth > body.values.size() &&
       depth > body.values.size() + body.instructionCount()) ||
      operand.kind != MirOperandKind::Value || binding == 0 ||
      !hasOneExactExecutableValueUse(body, operand.value, expectedKind,
                                     expectedInstruction,
                                     expectedOperandIndex)) {
    return false;
  }
  const MirInstruction *definition = linearDefinition(body, operand.value);
  if (definition == nullptr || definition->operands.size() != 1) {
    return false;
  }
  if (definition->kind == MirInstructionKind::Move) {
    return definition->operands.front().kind == MirOperandKind::Move &&
           definition->ownership &&
           definition->ownership->kind == OwnershipEventKind::Move &&
           definition->ownership->before == OwnershipStateSet::Available &&
           definition->ownership->after == OwnershipStateSet::Moved &&
           callableSourceBindingForOperand(body,
                                           definition->operands.front()) ==
               std::optional<HirBindingId>{binding};
  }
  return definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Identity &&
         exactReturnedMoveFromBinding(
             body, definition->operands.front(), binding,
             MirValueUseKind::InstructionOperand, definition->id, 0, depth + 1);
}

[[nodiscard]] bool valueTransferredOut(const MirBody &body,
                                       const MirOperand &operand,
                                       bool directDestination = false) {
  if (operand.kind != MirOperandKind::Value) {
    return false;
  }
  const MirInstruction *definition = linearDefinition(body, operand.value);
  if (definition == nullptr) {
    return false;
  }
  if (definition->info.traits.drop != DropKind::Lexical) {
    return true;
  }
  std::vector<MirDropObligationId> obligations;
  for (const MirPlace &place : body.places) {
    if (place.root != MirPlaceRootKind::Value || place.value != operand.value) {
      continue;
    }
    for (const MirDropObligation &obligation : body.dropObligations) {
      if (obligation.kind == MirDropObligationKind::Value &&
          obligation.place == place.id &&
          obligation.value == definition->hirValue) {
        obligations.push_back(obligation.id);
      }
    }
  }
  if (obligations.empty()) {
    return directDestination;
  }
  if (obligations.size() != 1) {
    return false;
  }
  const MirDropObligationId obligation = obligations.front();
  std::size_t consumed = 0;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      consumed += static_cast<std::size_t>(std::count_if(
          instruction.lifecycle.begin(), instruction.lifecycle.end(),
          [&](const MirLifecycleEvent &event) {
            if (event.source != obligation) {
              return false;
            }
            if (event.kind == MirLifecycleEventKind::TransferOut) {
              return true;
            }
            // A staged owned-parameter field consumes the moved value by
            // reparenting it into the armed ConstructionRollback obligation
            // instead of transferring it out silently.
            if (event.kind != MirLifecycleEventKind::Reparent) {
              return false;
            }
            const MirDropObligation *target =
                body.findDropObligation(event.target);
            if (target != nullptr &&
                target->kind == MirDropObligationKind::ReturnResult &&
                target->place == body.findDropObligation(obligation)->place &&
                target->value == definition->hirValue &&
                target->dropType.type == definition->info.type) {
              // Reparenting is not publication. Require the exact pending
              // lifetime to be retired at its matching final Return as well.
              std::size_t publications = 0;
              for (const MirBlock &returnBlock : body.blocks) {
                if (!returnBlock.reachable ||
                    returnBlock.terminator.kind != MirTerminatorKind::Return ||
                    !returnBlock.terminator.value ||
                    returnBlock.terminator.value->kind !=
                        MirOperandKind::Value ||
                    returnBlock.terminator.value->value != operand.value ||
                    returnBlock.instructions.empty())
                  continue;
                const MirInstruction &publication =
                    returnBlock.instructions.back();
                if (publication.kind != MirInstructionKind::Lifecycle ||
                    publication.hirValue != target->value)
                  continue;
                publications += std::count_if(
                    publication.lifecycle.begin(), publication.lifecycle.end(),
                    [&](const MirLifecycleEvent &retirement) {
                      return retirement.kind ==
                                 MirLifecycleEventKind::TransferOut &&
                             retirement.source == target->id &&
                             retirement.target == 0 &&
                             !retirement.conditional &&
                             !retirement.failureCleanup;
                    });
              }
              return publications == 1;
            }
            return target != nullptr &&
                   target->kind == MirDropObligationKind::ConstructionRollback;
          }));
    }
  }
  return consumed == 1;
}

[[nodiscard]] const MirInstruction *
exactReturnedDefinition(const MirFunctionInstance &instance,
                        const MirBlock &block) {
  if (!block.reachable || block.terminator.kind != MirTerminatorKind::Return ||
      !block.terminator.value ||
      block.terminator.value->kind != MirOperandKind::Value) {
    return nullptr;
  }
  return definitionFor(instance.body, block.terminator.value->value);
}

[[nodiscard]] bool constructorOwnsExactParameter(
    const MirProgram &program, HirConstructorInstanceId target, SymbolId field,
    const SemanticType &callableType, const SemanticType &ownerType) {
  const MirConstructorInstance *constructor =
      program.findConstructorInstance(target);
  const MirClassInstance *owner =
      constructor == nullptr ? nullptr
                             : program.findClassInstance(constructor->owner);
  const auto ownedField =
      owner == nullptr ? std::vector<MirClassFieldInfo>::const_iterator{}
                       : std::find_if(owner->declaredFields.begin(),
                                      owner->declaredFields.end(),
                                      [&](const MirClassFieldInfo &candidate) {
                                        return candidate.symbol == field &&
                                               candidate.type == callableType;
                                      });
  if (constructor == nullptr || constructor->parameterTypes.size() != 1 ||
      constructor->parameterBindings.size() != 1 ||
      constructor->parameterTypes.front() != callableType || owner == nullptr ||
      owner->type != ownerType || ownedField == owner->declaredFields.end() ||
      (ownedField->dropKind == DropKind::Lexical &&
       std::none_of(owner->fieldDropOrder.begin(), owner->fieldDropOrder.end(),
                    [&](const MirFieldDrop &candidate) {
                      return candidate.symbol == field &&
                             candidate.type == callableType;
                    }))) {
    return false;
  }
  const auto initializer = std::find_if(
      constructor->initializers.begin(), constructor->initializers.end(),
      [&](const MirConstructorInitializer &candidate) {
        return candidate.kind == ConstructorInitializerTargetKind::Field &&
               candidate.field == field &&
               candidate.targetType == callableType &&
               candidate.ownedParameter == std::optional<std::size_t>{0} &&
               candidate.arguments.size() == 1;
      });
  if (initializer == constructor->initializers.end()) {
    return false;
  }
  const auto value = std::find_if(
      constructor->body.values.begin(), constructor->body.values.end(),
      [&](const MirValue &candidate) {
        return candidate.sourceValue == initializer->arguments.front();
      });
  return value != constructor->body.values.end() &&
         std::count_if(
             constructor->body.values.begin(), constructor->body.values.end(),
             [&](const MirValue &candidate) {
               return candidate.sourceValue == initializer->arguments.front();
             }) == 1 &&
         exactMoveFromBinding(constructor->body,
                              MirOperand{.kind = MirOperandKind::Value,
                                         .value = value->id,
                                         .type = value->info.type},
                              constructor->parameterBindings.front()) &&
         valueTransferredOut(constructor->body,
                             MirOperand{.kind = MirOperandKind::Value,
                                        .value = value->id,
                                        .type = value->info.type},
                             true);
}

[[nodiscard]] std::optional<SemanticTypeTraits>
concreteMirTypeTraits(const MirProgram &program, const SemanticType &type,
                      std::size_t depth = 0) {
  // Nominal class/enum traits are published MIR facts, not a fresh field/type
  // analysis. Only explicit aggregate components and capture identities need
  // recursion. Keep malformed type/capture graphs bounded independently of
  // how many lambdas happen to be instantiated in a valid program.
  if (depth >
      256 + program.lambdaInstances().size() + program.classInstances().size())
    return std::nullopt;
  if (type.kind == SemanticType::Class) {
    const auto owner = std::find_if(program.classInstances().begin(),
                                    program.classInstances().end(),
                                    [&](const MirClassInstance &candidate) {
                                      return candidate.type == type;
                                    });
    return owner == program.classInstances().end()
               ? std::nullopt
               : std::optional{owner->traits};
  }
  if (type.kind == SemanticType::Enum) {
    if (const auto *enumeration = program.findPayloadEnum(type))
      return enumeration->traits;
    return program.findIntegralEnum(type) != nullptr
               ? std::optional{semanticTraits(type)}
               : std::nullopt;
  }
  if (type.kind == SemanticType::Array) {
    return type.arguments.size() == 1
               ? concreteMirTypeTraits(program, type.arguments.front(),
                                       depth + 1)
               : std::nullopt;
  }
  if (type.kind == SemanticType::UniqueOwner) {
    if (type.arguments.size() != 1)
      return std::nullopt;
    const auto pointee =
        concreteMirTypeTraits(program, type.arguments.front(), depth + 1);
    if (!pointee)
      return std::nullopt;
    // The owner keeps its own transport/lifetime traits. Only concurrency
    // capabilities follow the exact pointee; its copy/drop/borrow facts do
    // not describe the owning handle.
    auto traits = semanticTraits(type);
    traits.transferCapable = pointee->transferCapable;
    traits.shareCapable = pointee->shareCapable;
    return traits;
  }
  if (type.kind == SemanticType::Expected ||
      type.kind == SemanticType::Unexpected) {
    const std::size_t alternatives =
        type.kind == SemanticType::Expected ? 2 : 1;
    if (type.arguments.size() != alternatives)
      return std::nullopt;
    SemanticTypeTraits traits = semanticTraits(type);
    traits.relocation = RelocationKind::Structural;
    // Unresolved nominal children make semanticTraits conservative. Resolved
    // semantics instead folds capabilities from true over actual components.
    traits.transferCapable = true;
    traits.shareCapable = true;
    for (std::size_t index = 0; index < alternatives; ++index) {
      const auto &alternative = type.arguments[index];
      if (type.kind == SemanticType::Expected && index == 0 &&
          alternative == SemanticType::Void)
        continue;
      const auto child = concreteMirTypeTraits(program, alternative, depth + 1);
      if (!child)
        return std::nullopt;
      if (child->ownership == OwnershipKind::Unique ||
          (traits.ownership == OwnershipKind::Value &&
           child->ownership == OwnershipKind::Shared))
        traits.ownership = child->ownership;
      traits.copyable = traits.copyable && child->copyable;
      traits.movable = traits.movable && child->movable;
      traits.copyAssignable = traits.copyAssignable && child->copyAssignable;
      traits.moveAssignable = traits.moveAssignable && child->moveAssignable;
      traits.containsBorrowedState =
          traits.containsBorrowedState || child->containsBorrowedState;
      traits.transferCapable = traits.transferCapable && child->transferCapable;
      traits.shareCapable = traits.shareCapable && child->shareCapable;
      if (child->relocation == RelocationKind::Unavailable)
        traits.relocation = RelocationKind::Unavailable;
    }
    return traits;
  }
  if (type.kind != SemanticType::Lambda)
    return semanticTraits(type);
  const auto lambda = std::find_if(program.lambdaInstances().begin(),
                                   program.lambdaInstances().end(),
                                   [&](const MirLambdaInstance &candidate) {
                                     return candidate.type == type;
                                   });
  if (lambda == program.lambdaInstances().end() || !type.hasLambdaShape() ||
      !std::equal(lambda->captureTypes.begin(), lambda->captureTypes.end(),
                  type.lambdaCaptureTypes().begin(),
                  type.lambdaCaptureTypes().end()))
    return std::nullopt;
  SemanticTypeTraits traits;
  traits.relocation = mirTypeRelocationKind(program, type);
  for (const SemanticType &captureType : lambda->captureTypes) {
    const auto capture = concreteMirTypeTraits(program, captureType, depth + 1);
    if (!capture)
      return std::nullopt;
    if (capture->ownership == OwnershipKind::Shared)
      traits.ownership = OwnershipKind::Shared;
    if (capture->drop == DropKind::Lexical)
      traits.drop = DropKind::Lexical;
    traits.copyable = traits.copyable && capture->copyable;
    if (captureType.kind == SemanticType::Reference &&
        captureType.referenceAccess == AccessMode::Mutable)
      traits.copyable = false;
    traits.movable = traits.movable && capture->movable;
    traits.containsBorrowedState =
        traits.containsBorrowedState || capture->containsBorrowedState;
    traits.transferCapable = traits.transferCapable && capture->transferCapable;
    traits.shareCapable = traits.shareCapable && capture->shareCapable;
  }
  if (traits.containsBorrowedState) {
    traits.copyAssignable = false;
    traits.moveAssignable = false;
  }
  return traits;
}

// Storage owns its element lifetime but carries no external-owner loan. Read
// concrete class/enum facts from MIR, combining only real aggregate components
// and captures; nominal generic arguments are not stored fields.
[[nodiscard]] std::optional<SemanticTypeTraits>
storagePayloadTraits(const MirProgram &program, const SemanticType &type,
                     std::size_t depth = 0) {
  // Aggregate type syntax is parser-bounded; named class nesting additionally
  // permits one step per concrete class. Stop malformed recursive MIR rather
  // than trusting an erased borrowed-state flag to terminate the walk.
  if (depth > 256 + program.classInstances().size())
    return std::nullopt;
  auto traits = concreteMirTypeTraits(program, type);
  if (!traits)
    return std::nullopt;
  if (type.kind == SemanticType::Enum) {
    if (const auto *enumeration = program.findPayloadEnum(type))
      return enumeration->traits;
  }
  const auto include = [&](const SemanticType &component,
                           bool mergeMovability = true) {
    const auto child = storagePayloadTraits(program, component, depth + 1);
    if (!child)
      return false;
    if (mergeMovability)
      traits->movable = traits->movable && child->movable;
    traits->containsBorrowedState =
        traits->containsBorrowedState || child->containsBorrowedState;
    return true;
  };
  if (type.kind == SemanticType::Class) {
    const auto owner = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &candidate) { return candidate.type == type; });
    if (owner == program.classInstances().end())
      return std::nullopt;
    for (const auto &base : owner->structuralBases)
      if (!base.interface && !include(base.type, false))
        return std::nullopt;
    for (const auto &field : owner->declaredFields)
      if (!include(field.type, false))
        return std::nullopt;
  } else if (type.kind == SemanticType::Array ||
             type.kind == SemanticType::Expected ||
             type.kind == SemanticType::Unexpected) {
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      const auto &component = type.arguments[index];
      if (type.kind == SemanticType::Expected && index == 0 &&
          component == SemanticType::Void)
        continue;
      if (!include(component))
        return std::nullopt;
    }
  } else if (type.kind == SemanticType::Lambda) {
    for (const auto &capture : type.lambdaCaptureTypes())
      if (!include(capture))
        return std::nullopt;
  }
  return traits;
}

// Nominal capability facts come from concrete MIR classes/enums. Only genuine
// builtin aggregate components are combined here; raw pointees are not owned
// components, and generic arguments alone do not describe class storage.
[[nodiscard]] std::optional<ConcurrencyCapabilities>
conditionalCapabilities(const MirProgram &program, const SemanticType &type,
                        std::size_t depth = 0) {
  if (depth > 256 || type.kind == SemanticType::Unknown ||
      type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack)
    return std::nullopt;
  if (type.kind == SemanticType::Array ||
      type.kind == SemanticType::UniqueOwner ||
      type.kind == SemanticType::Expected ||
      type.kind == SemanticType::Unexpected) {
    if (type.arguments.empty())
      return std::nullopt;
    ConcurrencyCapabilities result{.transferCapable = true,
                                   .shareCapable = true};
    for (std::size_t index = 0; index < type.arguments.size(); ++index) {
      if (type.kind == SemanticType::Expected && index == 0 &&
          type.arguments[index] == SemanticType::Void)
        continue;
      const auto child =
          conditionalCapabilities(program, type.arguments[index], depth + 1);
      if (!child)
        return std::nullopt;
      result.transferCapable &= child->transferCapable;
      result.shareCapable &= child->shareCapable;
    }
    return result;
  }
  if (type.kind == SemanticType::Enum) {
    if (const auto *enumeration = program.findPayloadEnum(type))
      return ConcurrencyCapabilities{
          .transferCapable = enumeration->traits.transferCapable,
          .shareCapable = enumeration->traits.shareCapable};
  }
  const auto traits = concreteMirTypeTraits(program, type);
  if (!traits)
    return std::nullopt;
  return ConcurrencyCapabilities{.transferCapable = traits->transferCapable,
                                 .shareCapable = traits->shareCapable};
}

// Verify scoped transport structurally, without changing ordinary ownership
// transfer. Borrowed fields get permission only for the joined invocation;
// their referents retain the ordinary transfer/share requirements.
[[nodiscard]] bool scopedTransferAllowed(const MirProgram &program,
                                         const SemanticType &type,
                                         std::vector<SemanticType> &visiting) {
  if (type.kind == SemanticType::Reference) {
    if (type.arguments.size() != 1)
      return false;
    const auto referent = conditionalCapabilities(program, type.arguments[0]);
    return referent && (type.referenceAccess == AccessMode::Mutable
                            ? referent->transferCapable
                            : referent->shareCapable);
  }
  const auto traits = concreteMirTypeTraits(program, type);
  if (!traits)
    return false;
  if (!traits->containsBorrowedState)
    return traits->transferCapable;
  if (type.kind != SemanticType::Class ||
      std::find(visiting.begin(), visiting.end(), type) != visiting.end())
    return false;
  const auto found = std::find_if(
      program.classInstances().begin(), program.classInstances().end(),
      [&](const auto &candidate) { return candidate.type == type; });
  if (found == program.classInstances().end() ||
      found->transferPolicy == ConcurrencyCapabilityPolicy::Denied)
    return false;
  visiting.push_back(type);
  bool permitted = found->destructorStatus != SpecialMemberStatus::Declared;
  for (const auto &base : found->structuralBases)
    if (!base.interface)
      permitted &= scopedTransferAllowed(program, base.type, visiting);
  for (const auto &field : found->declaredFields)
    permitted &= scopedTransferAllowed(program, field.type, visiting);
  visiting.pop_back();
  if (found->transferPolicy == ConcurrencyCapabilityPolicy::UnsafeAsserted)
    permitted = true;
  if (found->transferPolicy == ConcurrencyCapabilityPolicy::UnsafeConditional) {
    const auto condition =
        found->transferCondition
            ? conditionalCapabilities(program, *found->transferCondition)
            : std::nullopt;
    permitted = condition && condition->transferCapable;
  }
  // Unsafe ownership assertions never erase the checked referent contract.
  const auto paths = findStoredCarrierReferences(program, type);
  if (paths.empty())
    return false;
  for (const auto &path : paths)
    permitted &= scopedTransferAllowed(program, path.reference, visiting);
  return permitted;
}

[[nodiscard]] bool storageOwnershipTypeValid(const MirProgram &program,
                                             const SemanticType &type) {
  if (type.kind == SemanticType::Class)
    return true; // The exact class's declared fields are checked separately.
  return std::all_of(type.arguments.begin(), type.arguments.end(),
                     [&](const auto &component) {
                       return storageOwnershipTypeValid(program, component);
                     });
}

[[nodiscard]] std::optional<std::string>
ownedTransportFailure(const MirProgram &program,
                      const MirFunctionInstance &instance,
                      const MirCallableParameter &parameter) {
  if (!parameter.ownedTransport ||
      parameter.parameterIndex >= instance.parameterTypes.size()) {
    return "owned transport does not name an exact formal parameter";
  }
  const CallableOwnedTransport &transport = *parameter.ownedTransport;
  const SemanticType &callableType =
      instance.parameterTypes[parameter.parameterIndex];
  if (transport.kind == CallableOwnedTransportKind::Value) {
    if (transport.destinationType != callableType || transport.field != 0 ||
        callableType.kind == SemanticType::Reference)
      return "owned value transport does not match its exact entry type";
    // Bindings preserve source pack grouping; callable contracts and the
    // function signature use physical element indices. Derive the entry place
    // from that grouping instead of indexing the binding array by argument.
    const MirPlace *place = nullptr;
    std::optional<std::size_t> packElement;
    std::size_t physicalIndex = 0;
    for (const HirBindingId binding : instance.parameterBindings) {
      const auto root =
          std::find_if(instance.body.places.begin(), instance.body.places.end(),
                       [&](const MirPlace &candidate) {
                         return candidate.root == MirPlaceRootKind::Binding &&
                                candidate.binding == binding &&
                                candidate.projections.empty();
                       });
      const bool pack = root != instance.body.places.end() &&
                        root->type.kind == SemanticType::TypePack &&
                        root->type.concretePack;
      const std::size_t count = pack ? root->type.arguments.size() : 1;
      if (parameter.parameterIndex >= physicalIndex &&
          parameter.parameterIndex - physicalIndex < count) {
        if (root != instance.body.places.end()) {
          place = &*root;
          if (pack)
            packElement = parameter.parameterIndex - physicalIndex;
        }
        break;
      }
      physicalIndex += count;
    }
    SemanticTypeTraits traits =
        place == nullptr ? SemanticTypeTraits{} : place->traits;
    if (packElement && place != nullptr) {
      if (place->type.arguments[*packElement] != callableType)
        return "owned pack transport does not match its exact element type";
      const auto elementTraits = concreteMirTypeTraits(program, callableType);
      if (!elementTraits)
        return "owned pack transport lacks its concrete element traits";
      traits = *elementTraits;
    }
    if (place == nullptr || (!packElement && place->type != callableType) ||
        !place->initiallyAvailable || !traits.movable ||
        traits.containsBorrowedState)
      return "owned value transport lacks an available owning parameter place";
    if (traits.drop == DropKind::Lexical &&
        std::count_if(
            instance.body.dropObligations.begin(),
            instance.body.dropObligations.end(),
            [&](const MirDropObligation &drop) {
              const MirPlace *dropPlace = instance.body.findPlace(drop.place);
              const bool exactPlace =
                  !packElement
                      ? drop.place == place->id
                      : dropPlace != nullptr &&
                            dropPlace->root == MirPlaceRootKind::Binding &&
                            dropPlace->binding == place->binding &&
                            dropPlace->type == callableType &&
                            dropPlace->initiallyAvailable &&
                            dropPlace->projections.size() == 1 &&
                            dropPlace->projections.front().kind ==
                                MirProjectionKind::PackElement &&
                            dropPlace->projections.front().constantIndex ==
                                *packElement;
              return drop.kind == MirDropObligationKind::Binding &&
                     drop.binding == place->binding && exactPlace &&
                     drop.initiallyActive && drop.dropType.type == callableType;
            }) != 1)
      return "owned value transport lacks its exact entry cleanup obligation";
    // Body lifecycle and loan verification govern every subsequent transfer,
    // return, and cleanup path. No prescribed AST/result shape is required.
    return std::nullopt;
  }
  if (parameter.parameterIndex >= instance.parameterBindings.size())
    return "owned transport does not name an exact formal parameter";
  if (transport.kind == CallableOwnedTransportKind::ThreadTask) {
    if (transport.destinationType == SemanticType::Unknown ||
        transport.field != 0) {
      return "thread-task transport has an invalid destination contract";
    }
    std::size_t exactSpawns = 0;
    for (const MirBlock &block : instance.body.blocks) {
      if (!block.reachable) {
        continue;
      }
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.intrinsic != IntrinsicKind::ThreadSpawn) {
          continue;
        }
        const MirThreadTaskAdapter *adapter =
            instruction.threadTaskAdapter
                ? program.findThreadTaskAdapter(*instruction.threadTaskAdapter)
                : nullptr;
        if (instruction.operands.size() != 2 ||
            instruction.operands[1].type != callableType ||
            adapter == nullptr ||
            adapter->resultType != transport.destinationType ||
            !exactMoveFromBinding(
                instance.body, instruction.operands[1],
                instance.parameterBindings[parameter.parameterIndex]) ||
            (!movedValueInto(instance.body, instruction.operands[1],
                             instruction.id, 1) &&
             !receiverIsConsumed(instance.body, instruction.operands[1],
                                 MirValueUseKind::InstructionOperand,
                                 instruction.id, 1)) ||
            !valueTransferredOut(instance.body, instruction.operands[1],
                                 true)) {
          return "thread-task transport is not rooted in the parameter move";
        }
        ++exactSpawns;
      }
    }
    return exactSpawns == 1
               ? std::nullopt
               : std::optional<std::string>{
                     "thread-task transport must feed exactly one spawn"};
  }
  if (transport.destinationType != instance.returnType) {
    return "owned transport destination does not match the function return";
  }
  bool foundReturn = false;
  for (const MirBlock &block : instance.body.blocks) {
    if (!block.reachable ||
        block.terminator.kind != MirTerminatorKind::Return) {
      continue;
    }
    foundReturn = true;
    if (!block.terminator.value) {
      return "owned transport has a reachable valueless return";
    }
    if (transport.kind == CallableOwnedTransportKind::ExactReturn) {
      if (transport.field != 0 || instance.returnType != callableType ||
          block.terminator.value->type != callableType ||
          !exactReturnedMoveFromBinding(
              instance.body, *block.terminator.value,
              instance.parameterBindings[parameter.parameterIndex]) ||
          !valueTransferredOut(instance.body, *block.terminator.value, true)) {
        return "exact-return transport is not rooted in the parameter move";
      }
      continue;
    }
    const MirInstruction *construction =
        exactReturnedDefinition(instance, block);
    if (transport.field == 0 ||
        instance.returnType.kind != SemanticType::Class ||
        construction == nullptr ||
        construction->kind != MirInstructionKind::Construct ||
        construction->info.type != instance.returnType ||
        !construction->result || !construction->constructorTarget ||
        construction->operands.size() != 1 ||
        !hasOneExactExecutableValueUse(instance.body, *construction->result,
                                       MirValueUseKind::Terminator) ||
        !valueTransferredOut(instance.body,
                             MirOperand{.kind = MirOperandKind::Value,
                                        .value = *construction->result,
                                        .type = construction->info.type},
                             true) ||
        !exactMoveFromBinding(
            instance.body, construction->operands.front(),
            instance.parameterBindings[parameter.parameterIndex]) ||
        !movedValueInto(instance.body, construction->operands.front(),
                        construction->id, 0) ||
        !constructorOwnsExactParameter(
            program, *construction->constructorTarget, transport.field,
            callableType, instance.returnType)) {
      return "exact-field transport does not match its constructor and field";
    }
  }
  return foundReturn ? std::nullopt
                     : std::optional<std::string>{
                           "owned transport has no reachable return"};
}

void appendCanonicalParameterRoles(const SemanticType &type,
                                   std::vector<SemanticType> &roles) {
  if (type.kind != SemanticType::TypePack) {
    roles.push_back(type);
    return;
  }
  if (!type.concretePack) {
    // An empty pack instance currently retains its declaration-level symbolic
    // pack identity. Preserve that single role so equal empty/symbolic pack
    // groupings remain exact while unrelated pack identities still differ.
    roles.push_back(type);
    return;
  }
  for (const SemanticType &element : type.arguments) {
    appendCanonicalParameterRoles(element, roles);
  }
}

[[nodiscard]] bool
exactParameterRoles(const std::vector<SemanticType> &call,
                    const std::vector<SemanticType> &target) {
  std::vector<SemanticType> callRoles;
  std::vector<SemanticType> targetRoles;
  for (const SemanticType &type : call) {
    appendCanonicalParameterRoles(type, callRoles);
  }
  for (const SemanticType &type : target) {
    appendCanonicalParameterRoles(type, targetRoles);
  }
  return callRoles == targetRoles;
}

[[nodiscard]] bool
exactInPlaceConstructionRoles(const std::vector<SemanticType> &arguments,
                              const std::vector<SemanticType> &parameters) {
  std::vector<SemanticType> argumentRoles;
  std::vector<SemanticType> parameterRoles;
  for (const SemanticType &type : arguments) {
    appendCanonicalParameterRoles(type, argumentRoles);
  }
  for (const SemanticType &type : parameters) {
    appendCanonicalParameterRoles(type, parameterRoles);
  }
  if (argumentRoles.size() != parameterRoles.size()) {
    return false;
  }
  for (std::size_t index = 0; index < argumentRoles.size(); ++index) {
    const SemanticType &argument = argumentRoles[index];
    const SemanticType &parameter = parameterRoles[index];
    if (argument == parameter) {
      continue;
    }
    if (parameter.kind != SemanticType::RawPointer) {
      return false;
    }
    if (argument == SemanticType::NullPtr) {
      continue;
    }
    if (argument.kind != SemanticType::RawPointer ||
        parameter.arguments.size() != 1 || argument.arguments.size() != 1 ||
        parameter.arguments.front() != argument.arguments.front() ||
        (parameter.pointerAccess == AccessMode::Mutable &&
         argument.pointerAccess != AccessMode::Mutable)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool callResultMatches(const ExpressionInfo &result,
                                     const SemanticType &returnType) {
  if (returnType.kind != SemanticType::Reference) {
    return result.type == returnType;
  }
  return returnType.arguments.size() == 1 &&
         result.type == returnType.arguments.front() &&
         result.category == ValueCategory::Place &&
         result.access == returnType.referenceAccess;
}

// Temporary argument borrows and borrowed-carrier receivers cannot export
// reference-like results, including untracked raw/native address values.
[[nodiscard]] bool
supportsEphemeralBorrowResult(const MirFunctionInstance *target,
                              const ExpressionInfo &result) {
  return target != nullptr &&
         target->returnType.kind != SemanticType::Reference &&
         target->returnType.kind != SemanticType::StringView &&
         target->returnType.kind != SemanticType::CString &&
         target->returnType.kind != SemanticType::RawPointer &&
         target->returnType.kind != SemanticType::NativeFunction &&
         target->returnBorrowOrigin == BorrowOriginKind::None &&
         !result.traits.containsBorrowedState;
}

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForPlace(const MirBody &body, MirPlaceId placeId,
                     std::size_t depth) {
  return mir_detail::borrowSourceForPlace(body, placeId, depth);
}

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForOperand(const MirBody &body, const MirOperand &operand,
                       std::size_t depth = 0) {
  return mir_detail::borrowSourceForOperand(body, operand, depth);
}

// A Loan operand used as a reference argument may carry a concrete runtime
// place in addition to its provenance identity. The place must resolve through
// its binding carrier or projection chain to the same borrow source. This
// keeps a forged same-typed place from borrowing one object while passing
// another, while admitting owner-tied class values whose binding has no
// sourceValue of its own.
[[nodiscard]] bool
exactLoanArgumentRuntimePlace(const MirBody &body, const MirOperand &operand,
                              const PlaceKey *expectedPlace = nullptr) {
  if (operand.kind != MirOperandKind::Loan || operand.loan == 0) {
    return false;
  }
  const MirLoan *loan = body.findLoan(operand.loan);
  if (loan == nullptr) {
    return false;
  }
  if (operand.place == 0) {
    return true;
  }
  const MirPlace *place = body.findPlace(operand.place);
  if (place == nullptr || operand.type.kind != SemanticType::Reference ||
      operand.type.arguments.size() != 1 ||
      place->type != operand.type.arguments.front()) {
    return false;
  }
  if (expectedPlace != nullptr &&
      (!place->key || *place->key != *expectedPlace)) {
    return false;
  }
  const std::optional<MirPlaceId> source =
      borrowSourceForPlace(body, place->id, 0);
  return source && *source == loan->source;
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForValue(const MirBody &body, MirValueId valueId,
                              std::size_t depth);

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForPlace(const MirBody &body, MirPlaceId placeId,
                              std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size() &&
      depth > body.places.size() + body.values.size() + body.loans.size() +
                  body.instructionCount()) {
    return std::nullopt;
  }
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  // A checked-reference formal denotes the callable through one dereference.
  // Keep that binding identity, but never attribute a field/index/raw-pointer
  // projection to the whole callable parameter. Ordinary place verification
  // proves the reference type and the associated entry/reborrow loan.
  if (place->root == MirPlaceRootKind::Binding &&
      (place->projections.empty() ||
       (place->projections.size() == 1 &&
        place->projections.front().kind == MirProjectionKind::Dereference))) {
    return place->binding == 0 ? std::nullopt
                               : std::optional<HirBindingId>{place->binding};
  }
  if (place->root == MirPlaceRootKind::Value) {
    return callableSourceBindingForValue(body, place->value, depth + 1);
  }
  if (place->root == MirPlaceRootKind::Loan) {
    const MirLoan *loan = body.findLoan(place->loan);
    return loan == nullptr
               ? std::nullopt
               : callableSourceBindingForPlace(body, loan->source, depth + 1);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForOperand(const MirBody &body, const MirOperand &operand,
                                std::size_t depth) {
  switch (operand.kind) {
  case MirOperandKind::Loan: {
    const MirLoan *loan = body.findLoan(operand.loan);
    return loan == nullptr
               ? std::nullopt
               : callableSourceBindingForPlace(body, loan->source, depth + 1);
  }
  case MirOperandKind::Address:
  case MirOperandKind::Copy:
  case MirOperandKind::Move:
  case MirOperandKind::BorrowRead:
  case MirOperandKind::BorrowWrite:
    return callableSourceBindingForPlace(body, operand.place, depth + 1);
  case MirOperandKind::Value:
    return callableSourceBindingForValue(body, operand.value, depth + 1);
  case MirOperandKind::Constant:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<HirBindingId>
callableSourceBindingForValue(const MirBody &body, MirValueId valueId,
                              std::size_t depth) {
  if (depth > body.places.size() + body.values.size() + body.loans.size() &&
      depth > body.places.size() + body.values.size() + body.loans.size() +
                  body.instructionCount()) {
    return std::nullopt;
  }
  const MirInstruction *definition = definitionFor(body, valueId);
  if (definition == nullptr || definition->operands.size() != 1) {
    return std::nullopt;
  }
  return callableSourceBindingForOperand(body, definition->operands.front(),
                                         depth + 1);
}

[[nodiscard]] bool isCallOperatorTarget(const MirFunctionInstance *target) {
  return target != nullptr &&
         target->overloadedOperator == OverloadedOperator::Call;
}

[[nodiscard]] bool
callableSignatureMatches(const MirCallableSignature &signature,
                         const MirInstruction &instruction) {
  const bool sameTarget =
      (signature.functionTarget &&
       signature.functionTarget == instruction.functionTarget &&
       !instruction.lambdaTarget) ||
      (signature.lambdaTarget &&
       signature.lambdaTarget == instruction.lambdaTarget &&
       !instruction.functionTarget) ||
      (signature.nativeCalleeType &&
       signature.nativeCalleeType == instruction.nativeCalleeType &&
       !instruction.functionTarget && !instruction.lambdaTarget);
  return sameTarget && signature.selectedCapability &&
         instruction.callableInvocation == signature.selectedCapability &&
         callResultMatches(instruction.info, signature.returnType) &&
         signature.parameterTypes == instruction.parameterTypes;
}

[[nodiscard]] const MirClassInstance *
uniqueMirClassInstanceForType(const MirProgram &program,
                              const SemanticType &type);

[[nodiscard]] bool exactRetainedOwnedPointee(const MirProgram &program,
                                             const MirBody &body,
                                             const MirFunctionInstance *caller,
                                             const MirInstruction &input) {
  if (!input.retainedOwnerPlace)
    return true;
  const MirPlace *anchor = body.findPlace(*input.retainedOwnerPlace);
  const MirPlace *referent = input.operands.size() == 1
                                 ? body.findPlace(input.operands.front().place)
                                 : nullptr;
  const MirLoan *loan =
      referent != nullptr && referent->root == MirPlaceRootKind::Loan
          ? body.findLoan(referent->loan)
          : nullptr;
  if (caller == nullptr || !caller->owner || anchor == nullptr ||
      referent == nullptr || loan == nullptr ||
      anchor->root != MirPlaceRootKind::This || !anchor->key ||
      anchor->projections.empty() ||
      anchor->key->projections.size() != anchor->projections.size())
    return false;
  // The executable loan must be backed by an actually declared, mutable
  // owned path in this concrete caller. A matching-looking place key alone
  // must not manufacture ownership of another object or a reference field.
  const MirClassInstance *enclosing = program.findClassInstance(*caller->owner);
  if (enclosing == nullptr)
    return false;
  SemanticType projected = enclosing->type;
  for (std::size_t index = 0; index < anchor->projections.size(); ++index) {
    const auto &projection = anchor->projections[index];
    const auto &key = anchor->key->projections[index];
    const auto *record = uniqueMirClassInstanceForType(program, projected);
    if (record == nullptr || projection.kind != MirProjectionKind::Field ||
        key.kind != PlaceProjectionKind::Field || key.field != projection.field)
      return false;
    const auto field =
        std::find_if(record->declaredFields.begin(),
                     record->declaredFields.end(), [&](const auto &candidate) {
                       return candidate.symbol == projection.field;
                     });
    if (field == record->declaredFields.end() ||
        field->access != AccessMode::Mutable ||
        field->type.kind != SemanticType::Class)
      return false;
    projected = field->type;
  }
  if (projected != anchor->type)
    return false;

  const MirInstruction *accessorCall = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == MirInstructionKind::Call &&
          instruction.loan == loan->id &&
          instruction.hirValue == loan->producedBy) {
        if (accessorCall != nullptr)
          return false;
        accessorCall = &instruction;
      }
  const MirFunctionInstance *accessor =
      accessorCall != nullptr && accessorCall->functionTarget
          ? program.findFunctionInstance(*accessorCall->functionTarget)
          : nullptr;
  const MirInstruction *receiver =
      accessorCall != nullptr && accessorCall->receiver &&
              accessorCall->receiver->kind == MirOperandKind::Value
          ? definitionFor(body, accessorCall->receiver->value)
          : nullptr;
  const MirOperand *receiverOperand =
      receiver != nullptr && receiver->operands.size() == 1
          ? &receiver->operands.front()
      : accessorCall != nullptr && accessorCall->receiver
          ? &*accessorCall->receiver
          : nullptr;
  const MirPlace *receiverPlace = receiverOperand != nullptr
                                      ? body.findPlace(receiverOperand->place)
                                      : nullptr;
  const MirClassInstance *accessorOwner =
      accessor != nullptr && accessor->owner
          ? program.findClassInstance(*accessor->owner)
          : nullptr;
  if (accessor == nullptr || !accessor->ownedPointeeProjection ||
      accessor->ownedPointeeProjection->borrowIntrinsic !=
          IntrinsicKind::UniqueOwnerBorrowMut ||
      accessorOwner == nullptr || accessorOwner->type != anchor->type ||
      accessor->returnType !=
          SemanticType::referenceTo(input.info.type, AccessMode::Mutable) ||
      accessor->receiverMutability != ReceiverMutability::Mutable ||
      !accessor->parameterTypes.empty() || accessor->virtualMethod ||
      accessorCall->dispatch != CallDispatch::Static ||
      accessorCall->borrowOrigin != BorrowOriginKind::Receiver ||
      accessorCall->borrowAccess != AccessMode::Mutable ||
      !accessorCall->operands.empty() || !accessorCall->result ||
      (receiver != nullptr &&
       (receiver->kind != MirInstructionKind::CallInput ||
        receiver->callInputRole != MirCallInputRole::Receiver ||
        receiver->callInputKind != HirCallInputKind::MutableBorrow)) ||
      receiverOperand == nullptr ||
      receiverOperand->kind != MirOperandKind::BorrowWrite ||
      receiverPlace == nullptr || receiverPlace->root != anchor->root ||
      receiverPlace->key != anchor->key ||
      receiverPlace->projections != anchor->projections ||
      receiverPlace->type != anchor->type)
    return false;

  // The accessor capability cannot also feed a reference local, reborrow,
  // unrelated call, return, or second invocation. EndBorrow on normal/failure
  // exits is cleanup, not another executable use.
  if (!body.usesOf(*accessorCall->result).empty())
    return false;
  const auto usesLoan = [&](const MirOperand &operand) {
    const MirPlace *place = body.findPlace(operand.place);
    return (operand.kind == MirOperandKind::Loan && operand.loan == loan->id) ||
           (place != nullptr && place->root == MirPlaceRootKind::Loan &&
            place->loan == loan->id);
  };
  std::size_t stageUses = 0;
  for (const auto &other : body.loans)
    if (other.parent == loan->id)
      return false;
  for (const auto &block : body.blocks) {
    for (const auto &instruction : block.instructions) {
      if (instruction.loan == loan->id && instruction.id != accessorCall->id &&
          instruction.kind != MirInstructionKind::EndBorrow)
        return false;
      if (instruction.receiver && usesLoan(*instruction.receiver))
        return false;
      for (const auto &operand : instruction.operands) {
        if (!usesLoan(operand))
          continue;
        if (instruction.id != input.id)
          return false;
        ++stageUses;
      }
      if (instruction.destination) {
        const auto *place = body.findPlace(*instruction.destination);
        if (place != nullptr && place->root == MirPlaceRootKind::Loan &&
            place->loan == loan->id)
          return false;
      }
    }
    if (block.terminator.returnLoan == loan->id ||
        (block.terminator.value && usesLoan(*block.terminator.value)))
      return false;
  }
  return stageUses == 1;
}

[[nodiscard]] MirVerificationResult
verifyMirCallableMetadata(const MirProgram &program, const MirBody &body,
                          std::size_t owner,
                          const MirFunctionInstance *caller = nullptr) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.nativeCalleeType &&
          !isMirNativeIndirectCall(program, body, instruction)) {
        return verificationFailure(body, owner,
                                   "invalid native indirect call metadata",
                                   block.id, instruction.id);
      }
      if (instruction.kind == MirInstructionKind::Move &&
          instruction.operands.size() == 1) {
        const MirPlace *source =
            body.findPlace(instruction.operands.front().place);
        if (source != nullptr && source->root == MirPlaceRootKind::This &&
            source->projections.empty() &&
            (caller == nullptr || !caller->owner || caller->staticMember ||
             caller->receiverMutability != ReceiverMutability::Consuming ||
             !source->traits.movable || source->traits.containsBorrowedState)) {
          return verificationFailure(body, owner,
                                     "whole receiver move requires a consuming "
                                     "method with movable owned storage",
                                     block.id, instruction.id);
        }
      }
      if (instruction.kind == MirInstructionKind::CallInput &&
          instruction.callInputKind == HirCallInputKind::RetainedReceiver) {
        const auto &uses = body.usesOf(instruction.result.value_or(0));
        if (uses.size() != 1 ||
            uses.front().kind != MirValueUseKind::InstructionReceiver ||
            std::none_of(body.blocks.begin(), body.blocks.end(),
                         [&](const auto &consumerBlock) {
                           return std::any_of(
                               consumerBlock.instructions.begin(),
                               consumerBlock.instructions.end(),
                               [&](const auto &consumer) {
                                 return consumer.id ==
                                            uses.front().instruction &&
                                        consumer.kind ==
                                            MirInstructionKind::Call;
                               });
                         })) {
          return verificationFailure(
              body, owner,
              "retained receiver capability requires one exact invocation use",
              block.id, instruction.id);
        }
      }
      if (instruction.kind != MirInstructionKind::Call) {
        continue;
      }

      const MirFunctionInstance *functionTarget =
          instruction.functionTarget
              ? program.findFunctionInstance(*instruction.functionTarget)
              : nullptr;
      const MirLambdaInstance *lambdaTarget =
          instruction.lambdaTarget
              ? program.findLambda(*instruction.lambdaTarget)
              : nullptr;
      if (functionTarget != nullptr &&
          functionTarget->receiverMutability == ReceiverMutability::Consuming &&
          (functionTarget->returnType.kind == SemanticType::Reference ||
           functionTarget->returnBorrowOrigin != BorrowOriginKind::None ||
           instruction.info.traits.containsBorrowedState)) {
        return verificationFailure(body, owner,
                                   "consuming receiver invocation cannot "
                                   "publish tracked borrowed state",
                                   block.id, instruction.id);
      }
      const MirInstruction *receiverInput =
          instruction.receiver &&
                  instruction.receiver->kind == MirOperandKind::Value
              ? definitionFor(body, instruction.receiver->value)
              : nullptr;
      if (instruction.receiver &&
          isMirExactReceiverPresentation(program, instruction,
                                         instruction.receiver->type) &&
          (functionTarget->receiverMutability != ReceiverMutability::ReadOnly ||
           receiverInput == nullptr ||
           receiverInput->kind != MirInstructionKind::CallInput ||
           receiverInput->callInputRole != MirCallInputRole::Receiver ||
           receiverInput->callInputKind != HirCallInputKind::ReadBorrow ||
           receiverInput->operands.size() != 1 ||
           receiverInput->operands.front().kind != MirOperandKind::BorrowRead ||
           receiverInput->info.type != instruction.receiver->type)) {
        return verificationFailure(body, owner,
                                   "exact receiver presentation requires one "
                                   "read-only interface borrow",
                                   block.id, instruction.id);
      }
      const bool temporaryReceiver =
          receiverInput != nullptr &&
          (receiverInput->callInputKind ==
               HirCallInputKind::ReadTemporaryBorrow ||
           receiverInput->callInputKind ==
               HirCallInputKind::MutableTemporaryBorrow);
      if (receiverInput != nullptr &&
          receiverInput->callInputKind == HirCallInputKind::RetainedReceiver &&
          (caller == nullptr || !caller->owner || caller->staticMember ||
           !exactRetainedOwnedPointee(program, body, caller, *receiverInput) ||
           caller->receiverMutability != ReceiverMutability::Consuming ||
           (!isCallOperatorTarget(functionTarget) && lambdaTarget == nullptr) ||
           (functionTarget != nullptr &&
            (!functionTarget->owner || functionTarget->staticMember ||
             functionTarget->returnType.kind == SemanticType::Reference)) ||
           (lambdaTarget != nullptr &&
            (lambdaTarget->returnType.kind == SemanticType::Reference ||
             lambdaTarget->type != receiverInput->info.type)) ||
           !instruction.callableInvocation ||
           instruction.info.traits.containsBorrowedState)) {
        return verificationFailure(
            body, owner,
            "retained receiver requires an owned field of a consuming caller "
            "and a non-borrowing callable target",
            block.id, instruction.id);
      }
      if (temporaryReceiver) {
        const MirPlace *receiverPlace =
            receiverInput->operands.empty()
                ? nullptr
                : body.findPlace(receiverInput->operands.front().place);
        const MirTemporaryMaterialization materialization =
            receiverPlace == nullptr
                ? MirTemporaryMaterialization{}
                : findMirTemporaryMaterialization(body, *receiverPlace,
                                                  receiverInput->hirValue);
        const MirInstruction *producer = materialization.producer;
        const MirClassInstance *ownerClass =
            functionTarget != nullptr && functionTarget->owner
                ? program.findClassInstance(*functionTarget->owner)
                : nullptr;
        // The materialized temporary is exactly its producer's class. The
        // selected member may be declared by that class or inherited from one
        // of its recorded base instances; the borrow is of the whole object
        // either way, and dispatch below remains the member's own.
        const auto receiverInstance =
            receiverPlace == nullptr
                ? program.classInstances().end()
                : std::find_if(program.classInstances().begin(),
                               program.classInstances().end(),
                               [&](const MirClassInstance &candidate) {
                                 return candidate.type == receiverPlace->type;
                               });
        const MirClassInstance *receiverClass =
            receiverInstance == program.classInstances().end()
                ? nullptr
                : &*receiverInstance;
        const auto inheritsOwner = [&](const auto &self,
                                       const MirClassInstance &candidate,
                                       std::size_t depth) -> bool {
          if (ownerClass == nullptr || depth > program.classInstances().size())
            return false;
          if (candidate.id == ownerClass->id)
            return true;
          return std::any_of(candidate.bases.begin(), candidate.bases.end(),
                             [&](const HirBaseInstance &base) {
                               const MirClassInstance *instance =
                                   program.findClassInstance(base.instance);
                               return instance != nullptr &&
                                      instance->type == base.type &&
                                      self(self, *instance, depth + 1);
                             });
        };
        const bool mutableInput = receiverInput->callInputKind ==
                                  HirCallInputKind::MutableTemporaryBorrow;
        const bool borrowedCarrier =
            receiverInput->info.traits.containsBorrowedState;
        const bool exactReceiverQualifier =
            functionTarget != nullptr &&
            ((mutableInput &&
              receiverAllowsMutation(functionTarget->receiverMutability)) ||
             (!mutableInput && functionTarget->receiverMutability ==
                                   ReceiverMutability::ReadOnly));
        const std::size_t receiverObligations =
            receiverPlace == nullptr
                ? 0
                : static_cast<std::size_t>(std::count_if(
                      body.dropObligations.begin(), body.dropObligations.end(),
                      [&](const MirDropObligation &obligation) {
                        return obligation.kind ==
                                   MirDropObligationKind::Value &&
                               obligation.value == receiverInput->hirValue &&
                               obligation.place == receiverPlace->id;
                      }));
        bool exactProducer = false;
        if (producer != nullptr && receiverPlace != nullptr &&
            receiverPlace->root == MirPlaceRootKind::Temporary &&
            (producer->kind == MirInstructionKind::Load ||
             producer->kind == MirInstructionKind::Move)) {
          exactProducer = true; // Exact join identity was checked by
                                // materialization lookup.
        } else if (producer != nullptr &&
                   producer->kind == MirInstructionKind::Construct) {
          const MirConstructorInstance *target =
              producer->constructorTarget ? program.findConstructorInstance(
                                                *producer->constructorTarget)
                                          : nullptr;
          const MirClassInstance *producerOwner =
              target == nullptr ? nullptr
                                : program.findClassInstance(target->owner);
          exactProducer =
              target != nullptr && producerOwner != nullptr &&
              producerOwner->type == receiverPlace->type &&
              producer->constructorKind == ConstructorKind::Ordinary;
          if (!producer->constructorTarget && receiverClass != nullptr) {
            exactProducer =
                (producer->constructorKind == ConstructorKind::Copy &&
                 receiverClass->copyConstructor ==
                     SpecialMemberStatus::Generated) ||
                (producer->constructorKind == ConstructorKind::Move &&
                 receiverClass->moveConstructor ==
                     SpecialMemberStatus::Generated);
          }
        } else if (producer != nullptr &&
                   producer->kind == MirInstructionKind::Call) {
          const MirFunctionInstance *target =
              producer->functionTarget
                  ? program.findFunctionInstance(*producer->functionTarget)
                  : nullptr;
          exactProducer =
              target != nullptr && target->linkage == LanguageLinkage::Gti &&
              target->returnType == receiverPlace->type &&
              (borrowedCarrier
                   ? target->returnBorrowOrigin != BorrowOriginKind::None &&
                         !target->virtualMethod &&
                         producer->dispatch == CallDispatch::Static
                   : target->returnBorrowOrigin == BorrowOriginKind::None);
          if (producer->lambdaTarget && !producer->functionTarget) {
            const MirLambdaInstance *lambda =
                program.findLambda(*producer->lambdaTarget);
            exactProducer = lambda != nullptr &&
                            lambda->returnType == receiverPlace->type &&
                            !producer->info.traits.containsBorrowedState;
          }
          if (!producer->functionTarget && receiverClass != nullptr &&
              (producer->intrinsic == IntrinsicKind::ExpectedValue ||
               producer->intrinsic == IntrinsicKind::ExpectedError) &&
              producer->receiver &&
              producer->receiver->kind == MirOperandKind::Value &&
              producer->receiver->type.kind == SemanticType::Expected &&
              producer->receiver->type.arguments.size() == 2) {
            const std::size_t alternative =
                producer->intrinsic == IntrinsicKind::ExpectedValue ? 0 : 1;
            exactProducer = producer->receiver->type.arguments[alternative] ==
                            receiverPlace->type;
          }
          if (!producer->functionTarget && receiverClass != nullptr &&
              receiverClass->defaultConstructor ==
                  SpecialMemberStatus::Generated &&
              !producer->constructorTarget && !producer->lambdaTarget &&
              !producer->receiver && !producer->loan && !producer->ownership &&
              producer->operands.empty() && producer->parameterTypes.empty() &&
              producer->constructorKind == ConstructorKind::Ordinary &&
              producer->intrinsic == IntrinsicKind::None &&
              producer->info.type == receiverClass->type) {
            exactProducer = true;
          }
        }
        const bool readObservation =
            functionTarget != nullptr && !mutableInput &&
            functionTarget->returnBorrowOrigin == BorrowOriginKind::Receiver &&
            functionTarget->returnBorrowAccess == AccessMode::ReadOnly &&
            (functionTarget->returnType.kind == SemanticType::StringView ||
             (functionTarget->returnType.kind == SemanticType::Reference &&
              functionTarget->returnType.referenceAccess ==
                  AccessMode::ReadOnly));
        const bool ownedResult =
            functionTarget != nullptr &&
            functionTarget->returnType.kind != SemanticType::Reference &&
            functionTarget->returnBorrowOrigin == BorrowOriginKind::None &&
            !instruction.info.traits.containsBorrowedState;
        // Dispatch is exactly the target's own: a virtual member keeps its
        // dynamic dispatch and failure contract over the borrowed temporary,
        // and a non-virtual member is never dispatched through the table.
        const CallDispatch exactDispatch =
            functionTarget != nullptr && functionTarget->virtualMethod
                ? CallDispatch::Virtual
                : CallDispatch::Static;
        if (functionTarget == nullptr || receiverPlace == nullptr ||
            ownerClass == nullptr || functionTarget->staticMember ||
            functionTarget->linkage != LanguageLinkage::Gti ||
            instruction.dispatch != exactDispatch || receiverClass == nullptr ||
            !inheritsOwner(inheritsOwner, *receiverClass, 0) ||
            !exactReceiverQualifier || !exactProducer ||
            receiverObligations != 1 || (!readObservation && !ownedResult) ||
            (borrowedCarrier && !supportsEphemeralBorrowResult(
                                    functionTarget, instruction.info))) {
          return verificationFailure(
              body, owner,
              "temporary receiver input does not match its exact "
              "ordinary target and producer contract",
              block.id, instruction.id);
        }
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        const MirInstruction *argumentInput =
            instruction.operands[index].kind == MirOperandKind::Value
                ? definitionFor(body, instruction.operands[index].value)
                : nullptr;
        if (argumentInput == nullptr ||
            argumentInput->kind != MirInstructionKind::CallInput ||
            argumentInput->callInputKind !=
                HirCallInputKind::ReadTemporaryBorrow ||
            argumentInput->callInputRole != MirCallInputRole::Argument) {
          continue;
        }

        const MirPlace *argumentPlace =
            argumentInput->operands.empty()
                ? nullptr
                : body.findPlace(argumentInput->operands.front().place);
        const MirTemporaryMaterialization materialization =
            argumentPlace == nullptr
                ? MirTemporaryMaterialization{}
                : findMirTemporaryMaterialization(body, *argumentPlace,
                                                  argumentInput->hirValue);
        const MirInstruction *producer = materialization.producer;
        const MirLoan *loan =
            argumentInput->loan ? body.findLoan(*argumentInput->loan) : nullptr;
        bool exactProducer = false;
        if (producer != nullptr &&
            producer->kind == MirInstructionKind::Construct) {
          const MirConstructorInstance *target =
              producer->constructorTarget ? program.findConstructorInstance(
                                                *producer->constructorTarget)
                                          : nullptr;
          const MirClassInstance *producerOwner =
              target == nullptr ? nullptr
                                : program.findClassInstance(target->owner);
          exactProducer =
              target != nullptr && producerOwner != nullptr &&
              argumentPlace != nullptr &&
              producerOwner->type == argumentPlace->type &&
              producer->constructorKind == ConstructorKind::Ordinary;
        } else if (producer != nullptr &&
                   producer->kind == MirInstructionKind::Call) {
          const MirFunctionInstance *target =
              producer->functionTarget
                  ? program.findFunctionInstance(*producer->functionTarget)
                  : nullptr;
          exactProducer = target != nullptr && argumentPlace != nullptr &&
                          target->definitionKind == MirDefinitionKind::Source &&
                          target->linkage == LanguageLinkage::Gti &&
                          target->returnType == argumentPlace->type &&
                          target->returnBorrowOrigin == BorrowOriginKind::None;
        }
        const bool supportedResult =
            supportsEphemeralBorrowResult(functionTarget, instruction.info);
        const std::size_t argumentObligations =
            argumentPlace == nullptr
                ? 0
                : static_cast<std::size_t>(std::count_if(
                      body.dropObligations.begin(), body.dropObligations.end(),
                      [&](const MirDropObligation &obligation) {
                        return obligation.kind ==
                                   MirDropObligationKind::Value &&
                               obligation.value == argumentInput->hirValue &&
                               obligation.place == argumentPlace->id &&
                               obligation.hirFullExpression != 0 &&
                               obligation.fullExpression != 0;
                      }));
        if (functionTarget == nullptr ||
            functionTarget->definitionKind != MirDefinitionKind::Source ||
            functionTarget->linkage != LanguageLinkage::Gti ||
            functionTarget->overloadedOperator ||
            argumentInput->callInputIndex != index ||
            index >= functionTarget->parameterTypes.size() ||
            functionTarget->parameterTypes[index] != argumentInput->info.type ||
            argumentInput->info.type.kind != SemanticType::Reference ||
            argumentInput->info.type.referenceAccess != AccessMode::ReadOnly ||
            argumentInput->info.type.arguments.size() != 1 ||
            argumentPlace == nullptr ||
            argumentInput->info.type.arguments.front() != argumentPlace->type ||
            loan == nullptr || loan->kind != MirLoanKind::CallArgument ||
            loan->source != argumentPlace->id ||
            loan->producedBy != instruction.hirValue || !exactProducer ||
            argumentObligations != 1 || !supportedResult) {
          return verificationFailure(
              body, owner,
              "temporary argument input does not match its exact "
              "ordinary target, producer, loan, and result contract",
              block.id, instruction.id);
        }
      }
      const bool exactFunctionCallable =
          functionTarget != nullptr && !instruction.lambdaTarget &&
          (isCallOperatorTarget(functionTarget) ||
           (functionTarget->owner && !functionTarget->staticMember &&
            (functionTarget->receiverMutability ==
                 ReceiverMutability::Consuming ||
             (receiverInput != nullptr &&
              receiverInput->kind == MirInstructionKind::CallInput &&
              receiverInput->callInputRole == MirCallInputRole::Receiver &&
              receiverInput->callInputKind == HirCallInputKind::MoveValue))));
      const bool exactLambdaCallable =
          lambdaTarget != nullptr && !instruction.functionTarget;
      const bool exactNativeCallable =
          isMirNativeIndirectCall(program, body, instruction);
      const bool exactCallableTarget = int(exactFunctionCallable) +
                                           int(exactLambdaCallable) +
                                           int(exactNativeCallable) ==
                                       1;

      if (instruction.callableInvocation.has_value() != exactCallableTarget) {
        return verificationFailure(
            body, owner,
            "callable invocation capability does not match an "
            "exact lambda, native signature, call operator, or consuming "
            "receiver target",
            block.id, instruction.id);
      }
      if (instruction.callableInvocation) {
        const CallableInvocationCapability expected =
            exactFunctionCallable ? callableInvocationCapability(
                                        functionTarget->receiverMutability)
            : exactLambdaCallable ? lambdaTarget->capability
                                  : CallableInvocationCapability::Read;
        if (*instruction.callableInvocation != expected) {
          return verificationFailure(
              body, owner,
              "callable invocation capability does not match its "
              "exact target",
              block.id, instruction.id);
        }
      }
      if (exactFunctionCallable &&
          (!callResultMatches(instruction.info, functionTarget->returnType) ||
           !exactParameterRoles(instruction.parameterTypes,
                                functionTarget->parameterTypes))) {
        return verificationFailure(
            body, owner,
            "operator() invocation does not match its exact target "
            "signature",
            block.id, instruction.id);
      }
      if (exactLambdaCallable &&
          (!callResultMatches(instruction.info, lambdaTarget->returnType) ||
           !exactParameterRoles(instruction.parameterTypes,
                                lambdaTarget->parameterTypes))) {
        return verificationFailure(
            body, owner,
            "lambda invocation does not match its exact target "
            "signature",
            block.id, instruction.id);
      }

      const std::optional<HirBindingId> receiverBinding =
          instruction.receiver
              ? callableSourceBindingForOperand(body, *instruction.receiver)
              : std::nullopt;
      const MirCallableParameter *matchedContract = nullptr;
      const MirCallableSignature *matchedSignature = nullptr;
      if (caller != nullptr && receiverBinding) {
        for (const MirCallableParameter &parameter :
             caller->callableParameters) {
          if (parameter.parameterIndex >= caller->parameterBindings.size() ||
              caller->parameterBindings[parameter.parameterIndex] !=
                  *receiverBinding) {
            continue;
          }
          const auto signature = std::find_if(
              parameter.signatures.begin(), parameter.signatures.end(),
              [&](const MirCallableSignature &candidate) {
                return callableSignatureMatches(candidate, instruction);
              });
          if (signature != parameter.signatures.end()) {
            matchedContract = &parameter;
            matchedSignature = &*signature;
            break;
          }
        }
      }
      if (instruction.callableBoundary.has_value() !=
          (matchedContract != nullptr)) {
        return verificationFailure(
            body, owner,
            "callable invocation boundary does not match its "
            "enclosing parameter binding",
            block.id, instruction.id);
      }
      if (instruction.callableBoundary &&
          *instruction.callableBoundary != matchedContract->boundary) {
        return verificationFailure(
            body, owner,
            "confined callable invocation does not match an "
            "enclosing parameter signature",
            block.id, instruction.id);
      }
      if (matchedSignature != nullptr &&
          matchedSignature->requiredCapability ==
              CallableInvocationCapability::Once &&
          (!instruction.receiver ||
           !receiverIsConsumed(body, *instruction.receiver,
                               MirValueUseKind::InstructionReceiver,
                               instruction.id))) {
        return verificationFailure(
            body, owner,
            "once-callable invocation is not rooted in an exact "
            "ownership move",
            block.id, instruction.id);
      }

      if (!instruction.callableArguments.empty() && functionTarget == nullptr) {
        return verificationFailure(
            body, owner,
            "callable argument descriptors require an exact "
            "function target",
            block.id, instruction.id);
      }
      if (functionTarget != nullptr) {
        const auto &expected = functionTarget->callableParameters;
        if (instruction.callableArguments.size() != expected.size()) {
          return verificationFailure(
              body, owner,
              "callable argument descriptors do not match the "
              "target function contract",
              block.id, instruction.id);
        }
        for (std::size_t index = 0; index < expected.size(); ++index) {
          if (instruction.callableArguments[index].parameterIndex !=
                  expected[index].parameterIndex ||
              instruction.callableArguments[index].boundary !=
                  expected[index].boundary) {
            return verificationFailure(
                body, owner,
                "callable argument descriptor does not match the "
                "target parameter contract",
                block.id, instruction.id);
          }
          if (expected[index].ownedTransport) {
            const std::size_t operandIndex = expected[index].parameterIndex;
            const MirInstruction *input =
                operandIndex < instruction.operands.size() &&
                        instruction.operands[operandIndex].kind ==
                            MirOperandKind::Value
                    ? definitionFor(body,
                                    instruction.operands[operandIndex].value)
                    : nullptr;
            const MirPlace *copiedSource =
                input != nullptr &&
                        input->kind == MirInstructionKind::CallInput &&
                        input->callInputKind == HirCallInputKind::CopyValue &&
                        input->operands.size() == 1 &&
                        input->operands.front().kind == MirOperandKind::Copy
                    ? body.findPlace(input->operands.front().place)
                    : nullptr;
            // A copyable local, field, or pack element can initialize an
            // independent owned argument. The ordinary CallInput verifier
            // proves source traits, the fresh prepared lifetime, and its
            // exactly-once transfer; ownership does not require moving the
            // original value when semantics selected a copy.
            const bool ownedPreparedCopy =
                expected[index].ownedTransport->kind ==
                    CallableOwnedTransportKind::Value &&
                copiedSource != nullptr && copiedSource->traits.copyable &&
                !copiedSource->traits.containsBorrowedState &&
                input->preparedParameterDrop &&
                input->callSite == instruction.callSite &&
                input->callInputIndex == operandIndex;
            if (operandIndex >= instruction.operands.size() ||
                operandIndex >= functionTarget->parameterTypes.size() ||
                instruction.operands[operandIndex].type !=
                    functionTarget->parameterTypes[operandIndex] ||
                (!movedValueInto(body, instruction.operands[operandIndex],
                                 instruction.id, operandIndex) &&
                 !receiverIsConsumed(body, instruction.operands[operandIndex],
                                     MirValueUseKind::InstructionOperand,
                                     instruction.id, operandIndex) &&
                 !freshOwnedValueInto(program, body,
                                      instruction.operands[operandIndex],
                                      instruction.id, operandIndex) &&
                 !ownedPreparedCopy)) {
              return verificationFailure(
                  body, owner,
                  "owned callable argument is not rooted in its "
                  "exact ownership move",
                  block.id, instruction.id);
            }
          }
        }
      }
    }
  }
  return {};
}

[[nodiscard]] std::optional<MirPlaceId>
nestedCarrierRoot(const MirProgram &program, const MirBody &body,
                  MirPlaceId placeId);

[[nodiscard]] MirVerificationResult
verifyStoredEntryPaths(const MirProgram &program, const MirBody &body,
                       std::size_t owner);

[[nodiscard]] MirVerificationResult
verifyCarrierShellDependencies(const MirProgram &program, const MirBody &body,
                               std::size_t owner);

[[nodiscard]] bool
independentlyStaticTextBinding(const MirBody &body, const MirPlace &source,
                               std::unordered_set<HirBindingId> &visited) {
  if (source.root != MirPlaceRootKind::Binding || !source.projections.empty() ||
      source.initiallyAvailable || source.traits.containsBorrowedState ||
      (source.type.kind != SemanticType::StringView &&
       source.type.kind != SemanticType::CString) ||
      !visited.insert(source.binding).second)
    return false;
  // Prove the actual initializer and absence of writes independently: erased
  // borrowed-state metadata alone is never evidence of static lifetime.
  const MirInstruction *initialization = nullptr;
  for (const auto &block : body.blocks) {
    for (const auto &instruction : block.instructions) {
      const auto *destination = instruction.destination
                                    ? body.findPlace(*instruction.destination)
                                    : nullptr;
      if (destination && destination->root == MirPlaceRootKind::Binding &&
          destination->binding == source.binding) {
        if (instruction.kind != MirInstructionKind::Initialize ||
            initialization != nullptr || !destination->projections.empty())
          return false;
        initialization = &instruction;
      }
      for (const auto &operand : instruction.operands) {
        const auto *place = body.findPlace(operand.place);
        if (operand.kind == MirOperandKind::BorrowWrite && place &&
            place->root == MirPlaceRootKind::Binding &&
            place->binding == source.binding)
          return false;
      }
    }
  }
  const auto *literal =
      initialization && initialization->operands.size() == 1 &&
              initialization->operands.front().kind == MirOperandKind::Value
          ? definitionFor(body, initialization->operands.front().value)
          : nullptr;
  if (literal && literal->kind == MirInstructionKind::Load &&
      literal->operands.size() == 1 &&
      literal->operands.front().kind == MirOperandKind::Copy &&
      literal->info.type == source.type &&
      !literal->info.traits.containsBorrowedState && !literal->loan &&
      !literal->destination && !literal->receiver &&
      literal->lifecycle.empty() && literal->definedFailure.empty() &&
      literal->localFailureSites.empty()) {
    const auto *copied = body.findPlace(literal->operands.front().place);
    return copied && copied->type == source.type &&
           independentlyStaticTextBinding(body, *copied, visited);
  }
  return literal && literal->kind == MirInstructionKind::Compute &&
         literal->operation == MirOperation::Literal && literal->literal &&
         !literal->loan && literal->operands.empty() && !literal->destination &&
         !literal->receiver && literal->lifecycle.empty() &&
         literal->definedFailure.empty() &&
         literal->localFailureSites.empty() &&
         !literal->info.traits.containsBorrowedState &&
         ((source.type.kind == SemanticType::StringView &&
           literal->info.type == source.type &&
           std::holds_alternative<std::string>(*literal->literal)) ||
          (source.type.kind == SemanticType::CString &&
           literal->info.type.kind == SemanticType::NullPtr &&
           std::holds_alternative<std::nullptr_t>(*literal->literal)));
}

[[nodiscard]] bool independentlyStaticTextBinding(const MirBody &body,
                                                  const MirPlace &source) {
  std::unordered_set<HirBindingId> visited;
  return independentlyStaticTextBinding(body, source, visited);
}

[[nodiscard]] bool isPotentiallyBorrowedCarrier(const MirProgram &program,
                                                const SemanticType &type) {
  // Built-in text views can carry an external owner without nominal reference
  // fields. A reference to the view still has a separate physical shell; do
  // not manufacture a nominal field path for that opaque dependency.
  return type.kind == SemanticType::StringView ||
         type.kind == SemanticType::CString ||
         !findStoredCarrierReferences(program, type).empty();
}

[[nodiscard]] bool isCarrierReferenceResult(const MirProgram &program,
                                            const MirInstruction &instruction);

[[nodiscard]] MirVerificationResult
verifyMirBorrowProducers(const MirProgram &program, const MirBody &body,
                         std::size_t owner) {
  if (auto entries = verifyStoredEntryPaths(program, body, owner);
      !entries.valid()) {
    return entries;
  }
  if (auto shells = verifyCarrierShellDependencies(program, body, owner);
      !shells.valid()) {
    return shells;
  }
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  for (const MirLoan &loan : body.loans) {
    for (const HirBindingId carrier : loan.carriers) {
      bindingLoans[carrier].push_back(loan.id);
    }
  }
  const auto selectedSource = [&](const MirOperand &operand) {
    if (operand.kind == MirOperandKind::Loan) {
      const MirLoan *loan = body.findLoan(operand.loan);
      return loan == nullptr ? std::optional<MirPlaceId>{}
                             : std::optional<MirPlaceId>{loan->source};
    }
    if (operand.place != 0) {
      return std::optional<MirPlaceId>{operand.place};
    }
    const std::optional<MirPlaceId> source =
        borrowSourceForOperand(body, operand);
    if (source && operand.kind == MirOperandKind::Value) {
      const MirInstruction *definition = definitionFor(body, operand.value);
      if (definition != nullptr &&
          definition->kind == MirInstructionKind::CallInput &&
          definition->operands.size() == 1) {
        const MirOperand &prepared = definition->operands.front();
        if (prepared.kind == MirOperandKind::Loan) {
          const MirLoan *loan = body.findLoan(prepared.loan);
          if (loan != nullptr) {
            return std::optional<MirPlaceId>{loan->source};
          }
        }
        if (prepared.place != 0) {
          return std::optional<MirPlaceId>{prepared.place};
        }
      }
    }
    return source;
  };
  const auto projectedFieldSource =
      [&](MirPlaceId source,
          const std::vector<SymbolId> &fields) -> MirPlaceId {
    const auto *base = body.findPlace(source);
    if (base == nullptr)
      return 0;
    MirPlace expected = *base;
    const auto dereference = [&] {
      if (expected.type.kind == SemanticType::Reference &&
          expected.type.arguments.size() == 1) {
        SemanticType referent = expected.type.arguments.front();
        expected.type = std::move(referent);
        expected.projections.push_back(
            {.kind = MirProjectionKind::Dereference});
      }
    };
    dereference();
    for (const auto field : fields) {
      const auto member = mirFieldProjectionType(program, expected.type, field);
      if (!member)
        return 0;
      expected.type = *member;
      expected.projections.push_back(
          {.kind = MirProjectionKind::Field, .field = field});
      dereference();
    }
    for (const auto &place : body.places)
      if (place.root == expected.root && place.binding == expected.binding &&
          place.symbol == expected.symbol && place.loan == expected.loan &&
          place.value == expected.value &&
          place.temporary == expected.temporary &&
          place.projections == expected.projections &&
          place.type == expected.type)
        return place.id;
    return 0;
  };
  const auto concreteDependencies = [&](const MirLoan &loan,
                                        bool fields = false) {
    std::vector<MirLoanDependency> result{
        {.source = loan.source,
         .designatedFields = loan.designatedFields,
         .access = loan.access}};
    for (const auto &dependency : loan.additionalDependencies)
      if (dependency.source != 0 || (fields && loan.entry))
        result.push_back(dependency);
    if (fields && loan.entry && loan.kind == MirLoanKind::Stored)
      for (auto &dependency : result)
        dependency.source =
            projectedFieldSource(loan.source, dependency.designatedFields);
    return result;
  };
  const auto dependencySources =
      [&](const auto &self, const MirOperand &operand, std::size_t depth = 0,
          bool fields = false,
          bool projectEntryFields = true) -> std::vector<MirLoanDependency> {
    if (depth > body.values.size() + 1) {
      return {};
    }
    if (operand.kind == MirOperandKind::Value) {
      const auto *definition = definitionFor(body, operand.value);
      if (definition != nullptr &&
          (definition->kind == MirInstructionKind::CallInput ||
           definition->kind == MirInstructionKind::Load ||
           definition->kind == MirInstructionKind::Move) &&
          definition->operands.size() == 1) {
        return self(self, definition->operands.front(), depth + 1, fields,
                    projectEntryFields);
      }
      if (definition != nullptr && definition->loan) {
        const auto *loan = body.findLoan(*definition->loan);
        if (loan != nullptr) {
          return concreteDependencies(*loan, fields && projectEntryFields);
        }
      }
    }
    const MirLoan *loan = operand.kind == MirOperandKind::Loan
                              ? body.findLoan(operand.loan)
                              : nullptr;
    const auto source = selectedSource(operand);
    if (loan == nullptr && source) {
      const auto *place = body.findPlace(*source);
      if (place != nullptr && place->root == MirPlaceRootKind::Loan) {
        loan = body.findLoan(place->loan);
      } else if (place != nullptr && place->root == MirPlaceRootKind::Binding) {
        const auto found = bindingLoans.find(place->binding);
        if (found != bindingLoans.end() && !found->second.empty()) {
          if (!place->projections.empty() &&
              place->projections.front().kind ==
                  MirProjectionKind::PackElement) {
            for (const auto id : found->second) {
              const auto *candidate = body.findLoan(id);
              const auto *origin = candidate == nullptr
                                       ? nullptr
                                       : body.findPlace(candidate->source);
              // One binding can contain several borrowed pack elements. Select
              // the exact element, never the first loan of the entire pack.
              if (origin == nullptr || !candidate->entry ||
                  origin->projections != place->projections)
                continue;
              if (loan != nullptr)
                return {};
              loan = candidate;
            }
          } else {
            loan = body.findLoan(found->second.front());
          }
        }
        if (loan == nullptr && place->projections.empty()) {
          for (const auto &candidate : body.loans) {
            if (!mir_detail::bindingReceivedStoredLoan(body, place->binding,
                                                       candidate.id))
              continue;
            if (loan != nullptr)
              return {};
            loan = &candidate;
          }
        }
      }
    }
    if (loan != nullptr) {
      const auto *selected = source ? body.findPlace(*source) : nullptr;
      const bool rangeStorage =
          selected != nullptr && selected->root == MirPlaceRootKind::Binding &&
          selected->projections.empty() &&
          (loan->kind == MirLoanKind::Local ||
           loan->kind == MirLoanKind::CallResult) &&
          loan->designatedFields.empty() &&
          loan->additionalDependencies.empty() &&
          std::any_of(body.rangeIterations.begin(), body.rangeIterations.end(),
                      [&](const MirRangeIteration &range) {
                        const auto *iterator = body.findPlace(range.iterator);
                        return iterator != nullptr &&
                               iterator->root == MirPlaceRootKind::Binding &&
                               iterator->binding == selected->binding;
                      });
      // Synthesized iterators keep the range's physical storage loan. The
      // separately retained resource is not the owner of the element address.
      if (fields && loan->kind != MirLoanKind::Stored &&
          loan->storedDependency != 0 && !rangeStorage) {
        loan = body.findLoan(loan->storedDependency);
        if (loan == nullptr)
          return {};
      }
      auto result = concreteDependencies(*loan, fields && projectEntryFields);
      if (selected != nullptr && selected->type == operand.type &&
          !selected->projections.empty() &&
          !(result.size() == 1 && result.front().designatedFields.empty()) &&
          std::all_of(selected->projections.begin(),
                      selected->projections.end(), [](const auto &projection) {
                        return projection.kind == MirProjectionKind::Field;
                      })) {
        std::erase_if(result, [&](auto &dependency) {
          if (dependency.designatedFields.size() <
                  selected->projections.size() ||
              !std::equal(selected->projections.begin(),
                          selected->projections.end(),
                          dependency.designatedFields.begin(),
                          [](const auto &projection, SymbolId field) {
                            return projection.field == field;
                          }))
            return true;
          dependency.designatedFields.erase(
              dependency.designatedFields.begin(),
              dependency.designatedFields.begin() +
                  selected->projections.size());
          return false;
        });
      }
      return result;
    }
    if (fields && source) {
      // A direct receiver/field place need not have a local carrier loan.
      // Project its concrete stored-reference fields rather than treating
      // the enclosing shell as the referenced object (and losing permission).
      std::vector<MirLoanDependency> result;
      auto carrierType = operand.type;
      if (carrierType.kind == SemanticType::Reference &&
          carrierType.arguments.size() == 1)
        carrierType = SemanticType(carrierType.arguments.front());
      for (const auto &path : findStoredCarrierReferences(program, carrierType))
        result.push_back({.source = projectedFieldSource(*source, path.fields),
                          .designatedFields = path.fields,
                          .access = path.reference.referenceAccess});
      if (!result.empty())
        return result;
    }
    return source ? std::vector<MirLoanDependency>{{.source = *source}}
                  : std::vector<MirLoanDependency>{};
  };
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.intrinsic == IntrinsicKind::ThreadScope) {
        if (instruction.scopedThreadTasks.size() != instruction.operands.size())
          return verificationFailure(body, owner,
                                     "parallel scope lost a task operand",
                                     block.id, instruction.id);
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          const auto &operand = instruction.operands[index];
          const auto &task = instruction.scopedThreadTasks[index];
          const auto traits = concreteMirTypeTraits(program, operand.type);
          std::vector<SemanticType> visiting;
          if (!traits || !traits->movable ||
              !scopedTransferAllowed(program, operand.type, visiting) ||
              (!movedValueInto(body, operand, instruction.id, index) &&
               !receiverIsConsumed(body, operand,
                                   MirValueUseKind::InstructionOperand,
                                   instruction.id, index) &&
               !freshOwnedValueInto(program, body, operand, instruction.id,
                                    index)))
            return verificationFailure(
                body, owner,
                "parallel task lost its consumed transport contract", block.id,
                instruction.id);
          const auto paths = findStoredCarrierReferences(program, operand.type);
          const auto *loan = body.findLoan(task.dependency);
          if (!traits->containsBorrowedState) {
            if (task.dependency != 0 || !paths.empty())
              return verificationFailure(
                  body, owner, "owned parallel task has a spurious loan",
                  block.id, instruction.id);
            continue;
          }
          if (loan == nullptr || paths.empty())
            return verificationFailure(body, owner,
                                       "parallel task lost its stored origins",
                                       block.id, instruction.id);
          auto actual = concreteDependencies(*loan, true);
          const auto expected =
              dependencySources(dependencySources, operand, 0, true);
          if (actual != expected || actual.size() != paths.size())
            return verificationFailure(
                body, owner,
                "parallel task loan does not match its operand origins",
                block.id, instruction.id);
          for (std::size_t field = 0; field < paths.size(); ++field) {
            const bool compact =
                actual.size() == 1 && actual[field].designatedFields.empty();
            if (actual[field].source == 0 ||
                (!compact &&
                 actual[field].designatedFields != paths[field].fields) ||
                actual[field].access != paths[field].reference.referenceAccess)
              return verificationFailure(
                  body, owner,
                  "parallel task lost a field origin or access mode", block.id,
                  instruction.id);
          }
        }
      }
      if (instruction.kind == MirInstructionKind::CallInput &&
          instruction.callInputRole == MirCallInputRole::Argument &&
          instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Loan &&
          !exactLoanArgumentRuntimePlace(body, instruction.operands.front(),
                                         instruction.ownership
                                             ? &instruction.ownership->place
                                             : nullptr)) {
        return verificationFailure(
            body, owner,
            "reference call-input place is not the exact runtime "
            "carrier for its loan",
            block.id, instruction.id);
      }
      if (instruction.kind == MirInstructionKind::CallInput &&
          instruction.callInputRole == MirCallInputRole::Argument &&
          instruction.operands.size() == 1) {
        const auto &operand = instruction.operands.front();
        const auto *place = body.findPlace(operand.place);
        if ((operand.kind == MirOperandKind::BorrowRead ||
             operand.kind == MirOperandKind::BorrowWrite) &&
            place != nullptr && place->type.kind == SemanticType::Class &&
            place->traits.containsBorrowedState &&
            (!instruction.ownership || !place->key ||
             *place->key != instruction.ownership->place)) {
          return verificationFailure(body, owner,
                                     "reference call-input place is not the "
                                     "exact runtime carrier for its loan",
                                     block.id, instruction.id);
        }
      }
      const bool preparation =
          instruction.kind == MirInstructionKind::CleanupBeforeAssign;
      const bool replacement = instruction.kind == MirInstructionKind::Assign &&
                               (instruction.operation == MirOperation::Assign ||
                                instruction.operation == MirOperation::None);
      const MirPlace *replacementPlace =
          instruction.destination ? body.findPlace(*instruction.destination)
                                  : nullptr;
      const auto replacementCleanup =
          (preparation || replacement) && replacementPlace
              ? mirUniqueCleanupType(program, replacementPlace->type)
              : std::nullopt;
      if ((preparation &&
           instruction.replacementCleanup != replacementCleanup) ||
          ((preparation || (replacement && replacementCleanup)) !=
           (instruction.replacementPair != 0))) {
        return verificationFailure(
            body, owner, "replacement lacks its exact paired cleanup recipe",
            block.id, instruction.id);
      }
      if (preparation) {
        const bool indexed = replacementPlace != nullptr &&
                             !replacementPlace->projections.empty() &&
                             replacementPlace->projections.back().kind ==
                                 MirProjectionKind::Index;
        const auto &origins = instruction.definedFailure.localOrigins;
        if ((indexed &&
             (origins.size() != 1 ||
              origins.front().outcomes !=
                  std::vector<DefinedFailureOutcome>{
                      {.code = DefinedFailureCode::IndexOutOfBounds,
                       .detail = DefinedFailureDetail::FixedArray}})) ||
            (!indexed && !origins.empty())) {
          return verificationFailure(
              body, owner,
              "replacement preparation lacks its exact bounds detector",
              block.id, instruction.id);
        }
        // Cleanup has already emptied every active subobject. The remaining
        // commit must be generated, non-failing field/handle transfer, not an
        // unchecked call to a native assignment policy.
        const auto generatedAssignment = [&](const auto &self,
                                             const SemanticType &type) -> bool {
          if (type.kind == SemanticType::Array)
            return type.arguments.size() == 1 &&
                   self(self, type.arguments.front());
          if (type.kind != SemanticType::Class)
            return mirTypeRelocationIsDefinedFailureFree(program, type);
          for (const auto &instance : program.classInstances()) {
            if (instance.type != type)
              continue;
            return instance.moveAssignment == SpecialMemberStatus::Generated &&
                   mirTypeRelocationIsDefinedFailureFree(program, type) &&
                   std::all_of(instance.declaredFields.begin(),
                               instance.declaredFields.end(),
                               [&](const auto &field) {
                                 return self(self, field.type);
                               }) &&
                   std::all_of(instance.bases.begin(), instance.bases.end(),
                               [&](const auto &base) {
                                 const auto *selected =
                                     program.findClassInstance(base.instance);
                                 return selected != nullptr &&
                                        self(self, selected->type);
                               });
          }
          return false;
        };
        if (!replacementPlace ||
            !generatedAssignment(generatedAssignment, replacementPlace->type))
          return verificationFailure(
              body, owner,
              "replacement lacks failure-free generated assignment", block.id,
              instruction.id);
        const MirInstruction *commit = nullptr;
        const MirBlock *commitBlock = nullptr;
        for (const auto &candidateBlock : body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.id == instruction.replacementPair) {
              commit = &candidate;
              commitBlock = &candidateBlock;
            }
        const auto propagation =
            replacementCleanup &&
                    mirDropMayRaiseDefinedFailure(program, *replacementCleanup)
                ? FailurePropagationKind::Destructor
                : FailurePropagationKind::None;
        bool adjacent = false;
        if (commitBlock == &block) {
          const auto position =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const auto &candidate) {
                             return candidate.id == instruction.id;
                           });
          adjacent = position != block.instructions.end() &&
                     std::next(position) != block.instructions.end() &&
                     &*std::next(position) == commit &&
                     propagation == FailurePropagationKind::None &&
                     origins.empty();
        } else if (commitBlock != nullptr &&
                   !commitBlock->instructions.empty()) {
          adjacent = &block.instructions.back() == &instruction &&
                     block.terminator.kind == MirTerminatorKind::Invoke &&
                     block.terminator.invokeInstruction == instruction.id &&
                     block.terminator.target == commitBlock->id &&
                     &commitBlock->instructions.front() == commit;
          std::size_t incoming = 0;
          for (const auto &candidateBlock : body.blocks)
            for (const auto successor :
                 terminatorSuccessors(candidateBlock.terminator))
              if (successor == commitBlock->id)
                ++incoming;
          adjacent = adjacent && incoming == 1 && commitBlock->id != body.entry;
        }
        if (!replacementCleanup || commit == nullptr ||
            commit->kind != MirInstructionKind::Assign ||
            commit->replacementPair != instruction.id ||
            commit->destination != instruction.destination ||
            commit->hirValue != instruction.hirValue || !adjacent ||
            !commit->definedFailure.empty() ||
            instruction.definedFailure.propagation != propagation) {
          return verificationFailure(
              body, owner,
              "replacement cleanup is not followed by its success-only commit",
              block.id, instruction.id);
        }
      } else if (instruction.replacementPair != 0) {
        bool paired = false;
        for (const auto &candidateBlock : body.blocks)
          for (const auto &candidate : candidateBlock.instructions)
            if (candidate.id == instruction.replacementPair)
              paired =
                  candidate.kind == MirInstructionKind::CleanupBeforeAssign &&
                  candidate.replacementPair == instruction.id;
        if (!paired)
          return verificationFailure(
              body, owner, "replacement commit has no matching preparation",
              block.id, instruction.id);
      }
      std::optional<MirDropType> elementCleanup;
      const bool removal =
          instruction.kind == MirInstructionKind::Call &&
          (instruction.intrinsic == IntrinsicKind::ObjectDestroy ||
           instruction.intrinsic == IntrinsicKind::OptionalStorageReset);
      if (removal && !instruction.operands.empty()) {
        const SemanticType &parameter = instruction.operands.front().type;
        const SemanticType &storage =
            parameter.kind == SemanticType::Reference &&
                    parameter.arguments.size() == 1
                ? parameter.arguments.front()
                : parameter;
        const bool optionalStorage =
            instruction.intrinsic == IntrinsicKind::OptionalStorageReset &&
            exactMirCapabilityClass(
                program, storage, CompilerCapabilityTypeKind::OptionalStorage);
        if (((instruction.intrinsic == IntrinsicKind::ObjectDestroy &&
              storage.kind == SemanticType::RawPointer) ||
             optionalStorage) &&
            storage.arguments.size() == 1) {
          elementCleanup =
              mirUniqueCleanupType(program, storage.arguments.front());
        }
      }
      if (instruction.elementCleanup != elementCleanup) {
        return verificationFailure(
            body, owner,
            "element removal does not name its exact cleanup recipe", block.id,
            instruction.id);
      }
      if (removal) {
        const auto propagation =
            elementCleanup &&
                    mirDropMayRaiseDefinedFailure(program, *elementCleanup)
                ? FailurePropagationKind::Destructor
                : FailurePropagationKind::None;
        if (instruction.definedFailure.propagation != propagation) {
          return verificationFailure(
              body, owner,
              "element removal does not retain its destructor failure effect",
              block.id, instruction.id);
        }
      }
      if (instruction.kind == MirInstructionKind::Call ||
          instruction.kind == MirInstructionKind::Construct) {
        for (const MirOperand &operand : instruction.operands) {
          if (operand.kind == MirOperandKind::Loan &&
              !exactLoanArgumentRuntimePlace(body, operand)) {
            return verificationFailure(
                body, owner,
                "reference call argument place is not the exact "
                "runtime carrier for its loan",
                block.id, instruction.id);
          }
        }
      }
      if (instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct) {
        if (instruction.kind == MirInstructionKind::Compute &&
            instruction.operation == MirOperation::Closure) {
          const MirLambdaInstance *lambda =
              instruction.lambdaTarget
                  ? program.findLambda(*instruction.lambdaTarget)
                  : nullptr;
          const bool exactCaptures =
              lambda != nullptr &&
              instruction.info.type.kind == SemanticType::Lambda &&
              lambda->type == instruction.info.type &&
              instruction.closureCaptureTypes == lambda->captureTypes &&
              instruction.closureCaptureModes == lambda->captureModes &&
              instruction.operands.size() == lambda->captureTypes.size();
          if (!exactCaptures) {
            return verificationFailure(
                body, owner,
                "closure construction does not match its exact "
                "concrete lambda instance",
                block.id, instruction.id);
          }
          for (std::size_t index = 0;
               lambda != nullptr && index < instruction.operands.size();
               ++index) {
            const MirOperand &capture = instruction.operands[index];
            const bool exactType = capture.type == lambda->captureTypes[index];
            const auto mode = lambda->captureModes[index];
            const auto *place = body.findPlace(capture.place);
            const auto *loan = body.findLoan(capture.loan);
            const bool borrowed = isBorrowingCapture(mode);
            const AccessMode access = mode == LambdaCaptureMode::BorrowMut
                                          ? AccessMode::Mutable
                                          : AccessMode::ReadOnly;
            const bool exactBorrow =
                borrowed && capture.type.kind == SemanticType::Reference &&
                capture.type.arguments.size() == 1 &&
                capture.type.referenceAccess == access && place &&
                place->type == capture.type.arguments.front() &&
                (access == AccessMode::ReadOnly ||
                 place->access == AccessMode::Mutable) &&
                ((capture.kind == MirOperandKind::BorrowRead &&
                  access == AccessMode::ReadOnly) ||
                 (capture.kind == MirOperandKind::BorrowWrite &&
                  access == AccessMode::Mutable) ||
                 (capture.kind == MirOperandKind::Loan && loan &&
                  (access == AccessMode::ReadOnly ||
                   loan->access == AccessMode::Mutable)));
            const bool exactMode =
                borrowed ? exactBorrow
                : mode == LambdaCaptureMode::Copy
                    ? capture.kind == MirOperandKind::Copy && capture.place != 0
                    : movedValueInto(body, capture, instruction.id, index);
            if (!exactType || !exactMode) {
              return verificationFailure(
                  body, owner,
                  "closure capture initialization does not match "
                  "its exact ownership mode",
                  block.id, instruction.id);
            }
          }
          std::vector<std::pair<std::size_t, MirLoanDependency>> borrows;
          for (std::size_t index = 0; index < lambda->captureModes.size();
               ++index) {
            if (isBorrowingCapture(lambda->captureModes[index])) {
              borrows.emplace_back(
                  index,
                  MirLoanDependency{
                      .source = instruction.operands[index].place,
                      .designatedFields = {lambda->captureSymbols[index]},
                      .access = lambda->captureTypes[index].referenceAccess});
              continue;
            }
            const auto paths = findStoredCarrierReferences(
                program, lambda->captureTypes[index]);
            if (paths.empty())
              continue;
            const auto inputs = dependencySources(
                dependencySources, instruction.operands[index], 0, true);
            for (const auto &path : paths) {
              const auto input = std::find_if(
                  inputs.begin(), inputs.end(), [&](const auto &candidate) {
                    return candidate.designatedFields == path.fields &&
                           candidate.access == path.reference.referenceAccess;
                  });
              if (input == inputs.end() || input->source == 0)
                return verificationFailure(
                    body, owner,
                    "captured carrier does not relay its exact stored origins",
                    block.id, instruction.id);
              auto dependency = *input;
              dependency.designatedFields.insert(
                  dependency.designatedFields.begin(),
                  lambda->captureSymbols[index]);
              borrows.emplace_back(index, std::move(dependency));
            }
          }
          const auto *retained =
              instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
          if (borrows.empty() != (retained == nullptr) ||
              (retained &&
               (retained->kind != MirLoanKind::Stored || retained->entry ||
                retained->escapes ||
                retained->producedBy != instruction.hirValue ||
                retained->additionalDependencies.size() + 1 != borrows.size() ||
                instruction.borrowOrigin != BorrowOriginKind::Argument ||
                instruction.borrowArgument != borrows.front().first)))
            return verificationFailure(
                body, owner,
                "closure does not retain its exact capture dependencies",
                block.id, instruction.id);
          for (std::size_t ordinal = 0; ordinal < borrows.size(); ++ordinal) {
            const auto &[index, expected] = borrows[ordinal];
            const auto *input = body.findPlace(expected.source);
            const auto source =
                ordinal == 0
                    ? retained->source
                    : retained->additionalDependencies[ordinal - 1].source;
            const auto access =
                ordinal == 0
                    ? retained->access
                    : retained->additionalDependencies[ordinal - 1].access;
            const auto &fields =
                ordinal == 0 ? retained->designatedFields
                             : retained->additionalDependencies[ordinal - 1]
                                   .designatedFields;
            const auto *origin = body.findPlace(source);
            if (!input || !origin || origin->root != input->root ||
                origin->binding != input->binding ||
                origin->symbol != input->symbol ||
                origin->capture != input->capture ||
                origin->value != input->value || origin->loan != input->loan ||
                origin->projections != input->projections ||
                origin->type != input->type || access != expected.access ||
                fields != expected.designatedFields)
              return verificationFailure(body, owner,
                                         "closure capture dependency differs "
                                         "from its exact input place",
                                         block.id, instruction.id);
          }
        }
        if (instruction.borrowOrigin != BorrowOriginKind::None &&
            instruction.operation != MirOperation::Closure) {
          return verificationFailure(
              body, owner,
              "only call and construct instructions may carry a "
              "borrow-result origin",
              block.id, instruction.id);
        }
        continue;
      }

      const auto transfersOut = [](const MirInstruction &candidate) {
        return std::any_of(
            candidate.lifecycle.begin(), candidate.lifecycle.end(),
            [](const MirLifecycleEvent &event) {
              return event.kind == MirLifecycleEventKind::TransferOut;
            });
      };
      const bool preparedOwnershipTransfer =
          (instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Construct) &&
          ((instruction.receiver &&
            [&] {
              const MirInstruction *prepared =
                  instruction.receiver->kind == MirOperandKind::Value
                      ? definitionFor(body, instruction.receiver->value)
                      : nullptr;
              return prepared != nullptr &&
                     prepared->kind == MirInstructionKind::CallInput &&
                     prepared->callInputKind == HirCallInputKind::MoveValue &&
                     transfersOut(*prepared);
            }()) ||
           std::any_of(instruction.operands.begin(), instruction.operands.end(),
                       [&](const MirOperand &operand) {
                         const MirInstruction *prepared =
                             operand.kind == MirOperandKind::Value
                                 ? definitionFor(body, operand.value)
                                 : nullptr;
                         return prepared != nullptr &&
                                prepared->kind ==
                                    MirInstructionKind::CallInput &&
                                (prepared->callInputKind ==
                                     HirCallInputKind::MoveValue ||
                                 prepared->callInputKind ==
                                     HirCallInputKind::MoveBorrowedTemporary) &&
                                transfersOut(*prepared);
                       }));
      const bool transfersOwnership =
          transfersOut(instruction) || preparedOwnershipTransfer;
      if (instruction.kind == MirInstructionKind::Call && transfersOwnership &&
          instruction.intrinsic == IntrinsicKind::None &&
          !instruction.functionTarget && !instruction.lambdaTarget &&
          !instruction.nativeCalleeType) {
        return verificationFailure(
            body, owner, "ordinary call is missing its exact target identity",
            block.id, instruction.id);
      }
      if (instruction.kind == MirInstructionKind::Construct &&
          instruction.constructorKind == ConstructorKind::Ordinary &&
          transfersOwnership && !instruction.constructorTarget) {
        return verificationFailure(
            body, owner,
            "ordinary construction is missing its exact constructor target",
            block.id, instruction.id);
      }

      for (std::size_t index = 0; index < instruction.parameterTypes.size() &&
                                  index < instruction.operands.size();
           ++index) {
        const SemanticType &parameter = instruction.parameterTypes[index];
        const MirOperand &operand = instruction.operands[index];
        if (parameter.kind != SemanticType::Reference || operand.place == 0) {
          continue;
        }
        const MirPlace *source = body.findPlace(operand.place);
        const MirOperandKind expectedKind =
            parameter.referenceAccess == AccessMode::Mutable
                ? MirOperandKind::BorrowWrite
                : MirOperandKind::BorrowRead;
        const MirLoan *provenance = operand.kind == MirOperandKind::Loan
                                        ? body.findLoan(operand.loan)
                                        : nullptr;
        const bool exactBorrowKind = operand.kind == expectedKind;
        const bool exactLoanKind = provenance != nullptr;
        if (source == nullptr || parameter.arguments.size() != 1 ||
            operand.type != parameter || (!exactBorrowKind && !exactLoanKind) ||
            source->type != parameter.arguments.front() ||
            (parameter.referenceAccess == AccessMode::Mutable &&
             (source->access != AccessMode::Mutable ||
              (exactLoanKind && provenance->access != AccessMode::Mutable)))) {
          return verificationFailure(
              body, owner,
              "direct reference argument does not match its exact "
              "parameter pointee and access",
              block.id, instruction.id);
        }
      }

      const MirFunctionInstance *target = nullptr;
      if (instruction.nativeCalleeType &&
          !isMirNativeIndirectCall(program, body, instruction)) {
        return verificationFailure(
            body, owner,
            "native indirect call lost its exact signature, ordered callee "
            "inputs or unsafe contract",
            block.id, instruction.id);
      }
      if (instruction.callerLocation &&
          program.failureMetadata().callerSiteFor(
              *instruction.callerLocation) != instruction.callerLocationSite) {
        return verificationFailure(
            body, owner,
            "call's caller location differs from canonical source metadata",
            block.id, instruction.id);
      }
      if (instruction.unsafeOperation ==
              UnsafeOperationKind::DeclaredUnsafeCall &&
          (instruction.kind != MirInstructionKind::Call ||
           !instruction.functionTarget)) {
        return verificationFailure(body, owner,
                                   "unsafe call lacks an exact function target",
                                   block.id, instruction.id);
      }
      if (instruction.functionTarget) {
        target = program.findFunctionInstance(*instruction.functionTarget);
        if (target == nullptr) {
          return verificationFailure(
              body, owner,
              "call borrow contract references an invalid function "
              "target",
              block.id, instruction.id);
        }
        if (target->requiresUnsafe !=
            (instruction.unsafeOperation ==
             UnsafeOperationKind::DeclaredUnsafeCall)) {
          return verificationFailure(
              body, owner, "call does not retain its target's unsafe contract",
              block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Call &&
            (target->neverReturns != instruction.neverReturns ||
             target->tracksCaller != instruction.tracksCaller)) {
          return verificationFailure(
              body, owner,
              "call does not retain its target's terminal/caller contract",
              block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::None &&
            bool(instruction.receiver) !=
                bool(target->owner && !target->staticMember)) {
          return verificationFailure(
              body, owner,
              "ordinary call receiver does not match its exact member target",
              block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::None &&
            instruction.receiver && target->owner && !target->staticMember) {
          const MirClassInstance *targetOwner =
              program.findClassInstance(*target->owner);
          SemanticType receiverType = instruction.receiver->type;
          if (instruction.unsafeOperation == UnsafeOperationKind::RawMember &&
              receiverType.kind == SemanticType::RawPointer &&
              receiverType.arguments.size() == 1)
            receiverType = receiverType.arguments.front();
          if (targetOwner == nullptr ||
              (receiverType != targetOwner->type &&
               !mirPublicBaseConversion(program, receiverType,
                                        targetOwner->type) &&
               !isMirExactReceiverPresentation(program, instruction,
                                               receiverType))) {
            return verificationFailure(body, owner,
                                       "ordinary member receiver is unrelated "
                                       "to its exact target owner",
                                       block.id, instruction.id);
          }
        }
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::None &&
            instruction.receiver &&
            instruction.receiver->kind == MirOperandKind::Value) {
          const MirValue *receiver =
              body.findValue(instruction.receiver->value);
          if (receiver == nullptr ||
              receiver->info.type != instruction.receiver->type) {
            return verificationFailure(
                body, owner,
                "ordinary member receiver differs from its typed input value",
                block.id, instruction.id);
          }
          const MirInstruction *input =
              receiver ? definitionFor(body, instruction.receiver->value)
                       : nullptr;
          if (input != nullptr &&
              input->kind == MirInstructionKind::CallInput &&
              (input->callInputKind == HirCallInputKind::ReadBorrow ||
               input->callInputKind == HirCallInputKind::MutableBorrow) &&
              target->receiverMutability !=
                  (input->callInputKind == HirCallInputKind::MutableBorrow
                       ? ReceiverMutability::Mutable
                       : ReceiverMutability::ReadOnly)) {
            return verificationFailure(
                body, owner,
                "ordinary member target access differs from its staged borrow",
                block.id, instruction.id);
          }
        }
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::None &&
            instruction.receiver &&
            (instruction.receiver->kind == MirOperandKind::BorrowRead ||
             instruction.receiver->kind == MirOperandKind::BorrowWrite)) {
          const MirPlace *receiver =
              body.findPlace(instruction.receiver->place);
          const bool mutableReceiver =
              instruction.receiver->kind == MirOperandKind::BorrowWrite;
          if (receiver == nullptr ||
              receiver->type != instruction.receiver->type ||
              (mutableReceiver && receiver->access != AccessMode::Mutable) ||
              (target->receiverMutability != ReceiverMutability::Consuming &&
               (target->receiverMutability == ReceiverMutability::Mutable) !=
                   mutableReceiver)) {
            return verificationFailure(
                body, owner,
                "ordinary member receiver differs from its typed borrow "
                "and target access",
                block.id, instruction.id);
          }
        }
        if (instruction.borrowOrigin != target->returnBorrowOrigin ||
            instruction.borrowArgument != target->returnBorrowParameter ||
            instruction.borrowArguments != target->returnBorrowParameters ||
            instruction.borrowFields != target->returnBorrowFields ||
            instruction.borrowAccess != target->returnBorrowAccess ||
            instruction.borrowPlace != target->returnBorrowPlace ||
            instruction.restoresReferenceResult !=
                target->restoresReferenceResult ||
            instruction.restoredBorrowFields != target->restoredBorrowFields) {
          return verificationFailure(
              body, owner,
              "call-result borrow origin does not match the target "
              "function summary (call origin " +
                  std::to_string(static_cast<int>(instruction.borrowOrigin)) +
                  ", parameter " + std::to_string(instruction.borrowArgument) +
                  "; target origin " +
                  std::to_string(static_cast<int>(target->returnBorrowOrigin)) +
                  ", parameter " +
                  std::to_string(target->returnBorrowParameter) + ")",
              block.id, instruction.id);
        }
        if (!exactParameterRoles(instruction.parameterTypes,
                                 target->parameterTypes)) {
          return verificationFailure(
              body, owner,
              "call parameter roles do not match the exact target "
              "signature",
              block.id, instruction.id);
        }
        const FailurePropagationKind expectedPropagation =
            target->linkage == LanguageLinkage::C ||
                    instruction.intrinsic != IntrinsicKind::None
                ? FailurePropagationKind::None
            : instruction.dispatch == CallDispatch::Virtual
                ? FailurePropagationKind::VirtualCall
            : !target->mayRaiseDefinedFailure
                ? FailurePropagationKind::None
                : FailurePropagationKind::DirectCall;
        if (instruction.definedFailure.propagation != expectedPropagation) {
          return verificationFailure(
              body, owner,
              "call failure propagation does not match its exact "
              "target and dispatch",
              block.id, instruction.id);
        }
      }
      const MirConstructorInstance *constructorTarget = nullptr;
      if (instruction.unsafeOperation ==
              UnsafeOperationKind::DeclaredUnsafeConstruction &&
          !instruction.constructorTarget) {
        return verificationFailure(
            body, owner,
            "unsafe construction lacks an exact constructor target", block.id,
            instruction.id);
      }
      if (!instruction.constructorTarget && !instruction.functionTarget &&
          (instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Construct) &&
          instruction.intrinsic == IntrinsicKind::None &&
          instruction.constructorKind == ConstructorKind::Ordinary &&
          instruction.info.type.kind == SemanticType::Class &&
          instruction.parameterTypes.empty() && instruction.operands.empty()) {
        const auto constructed = std::find_if(
            program.classInstances().begin(), program.classInstances().end(),
            [&](const MirClassInstance &candidate) {
              return candidate.type == instruction.info.type;
            });
        if (constructed != program.classInstances().end() &&
            constructed->generatedDefaultRequiresSchedule)
          return verificationFailure(
              body, owner,
              "generated construction is missing its selected schedule",
              block.id, instruction.id);
      }
      std::optional<std::size_t> inPlaceArgumentOffset;
      SemanticType::Kind inPlaceStorageKind = SemanticType::Unknown;
      if (instruction.kind == MirInstructionKind::Call) {
        switch (instruction.intrinsic) {
        case IntrinsicKind::ObjectConstruct:
          inPlaceArgumentOffset = 1;
          inPlaceStorageKind = SemanticType::RawPointer;
          break;
        case IntrinsicKind::OptionalStorageConstruct:
          inPlaceArgumentOffset = 1;
          inPlaceStorageKind = SemanticType::Class;
          break;
        default:
          break;
        }
      }
      const bool rawConstruction =
          instruction.intrinsic == IntrinsicKind::ObjectConstruct;
      const std::size_t storageArgumentCount =
          inPlaceArgumentOffset.value_or(0);
      if (inPlaceArgumentOffset &&
          instruction.parameterTypes.size() >= storageArgumentCount &&
          instruction.operands.size() == instruction.parameterTypes.size() &&
          instruction.parameterTypes.front().kind == inPlaceStorageKind &&
          instruction.parameterTypes.front().arguments.size() == 1 &&
          !instruction.constructorTarget) {
        const auto &element =
            instruction.parameterTypes.front().arguments.front();
        const auto elementOwner = std::find_if(
            program.classInstances().begin(), program.classInstances().end(),
            [&](const MirClassInstance &candidate) {
              return candidate.type == element;
            });
        const bool generatedRawTransfer =
            rawConstruction && elementOwner != program.classInstances().end() &&
            instruction.parameterTypes.size() == 2 &&
            instruction.parameterTypes[1] == element &&
            ((instruction.constructorKind == ConstructorKind::Move &&
              elementOwner->traits.movable) ||
             (instruction.constructorKind == ConstructorKind::Copy &&
              elementOwner->traits.copyable));
        const bool exactDefault =
            instruction.parameterTypes.size() == storageArgumentCount &&
            instruction.constructorKind == ConstructorKind::Ordinary;
        const bool requiredOrdinaryTarget =
            rawConstruction &&
            (instruction.parameterTypes.size() != storageArgumentCount ||
             (elementOwner != program.classInstances().end() &&
              elementOwner->defaultConstructor ==
                  SpecialMemberStatus::Declared));
        if (elementOwner != program.classInstances().end() &&
            !generatedRawTransfer &&
            (requiredOrdinaryTarget ||
             (exactDefault &&
              elementOwner->generatedDefaultRequiresSchedule))) {
          return verificationFailure(body, owner,
                                     "in-place construction is missing "
                                     "its exact constructor target",
                                     block.id, instruction.id);
        }
      }
      if (instruction.constructorTarget) {
        constructorTarget =
            program.findConstructorInstance(*instruction.constructorTarget);
        if (constructorTarget == nullptr) {
          return verificationFailure(
              body, owner,
              "construct borrow contract references an invalid "
              "constructor target",
              block.id, instruction.id);
        }
        if (constructorTarget->requiresUnsafe &&
            instruction.unsafeOperation !=
                UnsafeOperationKind::DeclaredUnsafeConstruction) {
          return verificationFailure(
              body, owner,
              "construction does not retain its target's unsafe contract",
              block.id, instruction.id);
        }
        if (!constructorTarget->requiresUnsafe &&
            instruction.unsafeOperation ==
                UnsafeOperationKind::DeclaredUnsafeConstruction) {
          return verificationFailure(
              body, owner, "construction has a forged unsafe contract",
              block.id, instruction.id);
        }
        if (inPlaceArgumentOffset) {
          const MirClassInstance *constructorOwner =
              program.findClassInstance(constructorTarget->owner);
          const bool exactStorageSurface =
              instruction.parameterTypes.size() >= *inPlaceArgumentOffset &&
              !instruction.parameterTypes.empty() &&
              instruction.parameterTypes.front().kind == inPlaceStorageKind &&
              instruction.parameterTypes.front().arguments.size() == 1;
          std::vector<SemanticType> constructorArguments;
          if (exactStorageSurface) {
            constructorArguments.assign(
                instruction.parameterTypes.begin() +
                    static_cast<std::ptrdiff_t>(*inPlaceArgumentOffset),
                instruction.parameterTypes.end());
          }
          if (!exactStorageSurface || constructorOwner == nullptr ||
              constructorOwner->type !=
                  instruction.parameterTypes.front().arguments.front() ||
              !exactInPlaceConstructionRoles(
                  constructorArguments, constructorTarget->parameterTypes)) {
            return verificationFailure(
                body, owner,
                "in-place storage construction does not match its "
                "exact element constructor target",
                block.id, instruction.id);
          }
        }
        const auto *constructorOwner =
            program.findClassInstance(constructorTarget->owner);
        if (instruction.kind == MirInstructionKind::Construct &&
            (constructorOwner == nullptr ||
             constructorOwner->type != instruction.info.type)) {
          return verificationFailure(
              body, owner,
              "construction result type differs from its exact constructor "
              "owner",
              block.id, instruction.id);
        }
        const bool ordinaryDefault =
            instruction.intrinsic == IntrinsicKind::None &&
            !instruction.functionTarget && instruction.parameterTypes.empty() &&
            instruction.operands.empty();
        const bool inPlaceDefault =
            inPlaceArgumentOffset &&
            instruction.parameterTypes.size() == *inPlaceArgumentOffset &&
            instruction.operands.size() == instruction.parameterTypes.size();
        if (constructorOwner != nullptr &&
            constructorOwner->generatedDefaultRequiresSchedule &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            (ordinaryDefault || inPlaceDefault) &&
            !constructorTarget->generatedDefault) {
          return verificationFailure(
              body, owner,
              "default construction does not select its required generated "
              "schedule",
              block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Construct &&
            (instruction.borrowOrigin != constructorTarget->borrowOrigin ||
             instruction.borrowArgument != constructorTarget->borrowParameter ||
             instruction.borrowArguments !=
                 constructorTarget->borrowParameters ||
             instruction.borrowAccess != constructorTarget->borrowAccess)) {
          return verificationFailure(
              body, owner,
              "construct-result borrow origin does not match the "
              "target constructor summary",
              block.id, instruction.id);
        }
        if (instruction.kind == MirInstructionKind::Construct &&
            !exactParameterRoles(instruction.parameterTypes,
                                 constructorTarget->parameterTypes)) {
          return verificationFailure(
              body, owner,
              "construct parameter roles do not match the exact "
              "target signature",
              block.id, instruction.id);
        }
        const FailurePropagationKind expectedPropagation =
            constructorTarget->mayRaiseDefinedFailure
                ? FailurePropagationKind::Constructor
                : FailurePropagationKind::None;
        if (instruction.kind == MirInstructionKind::Call && rawConstruction &&
            !instruction.parameterTypes.empty() &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            (instruction.operands.size() != instruction.parameterTypes.size() ||
             constructorTarget->parameterTypes.size() + storageArgumentCount !=
                 instruction.parameterTypes.size() ||
             !std::equal(constructorTarget->parameterTypes.begin(),
                         constructorTarget->parameterTypes.end(),
                         instruction.parameterTypes.begin() +
                             storageArgumentCount) ||
             instruction.definedFailure.propagation != expectedPropagation)) {
          return verificationFailure(
              body, owner,
              "in-place construction does not match its exact "
              "constructor failure and ownership contract",
              block.id, instruction.id);
        }
        if (instruction.objectAllocation != 0) {
          const MirClassInstance *constructorOwner =
              program.findClassInstance(constructorTarget->owner);
          if (instruction.kind != MirInstructionKind::Call ||
              instruction.intrinsic != IntrinsicKind::AllocateUniqueOwner ||
              constructorOwner == nullptr ||
              instruction.info.type.kind != SemanticType::UniqueOwner ||
              instruction.info.type.arguments !=
                  std::vector<SemanticType>{constructorOwner->type} ||
              !exactParameterRoles(instruction.parameterTypes,
                                   constructorTarget->parameterTypes) ||
              instruction.definedFailure.propagation != expectedPropagation) {
            return verificationFailure(
                body, owner,
                "object allocation construction does not match its exact "
                "constructor target, parameters, or failure effect",
                block.id, instruction.id);
          }
        }
        if (instruction.kind == MirInstructionKind::Construct &&
            instruction.definedFailure.propagation != expectedPropagation) {
          return verificationFailure(
              body, owner,
              "construction failure propagation does not match "
              "its exact constructor target",
              block.id, instruction.id);
        }
      }
      if (instruction.lambdaTarget && !instruction.functionTarget) {
        const MirLambdaInstance *lambda =
            program.findLambda(*instruction.lambdaTarget);
        if (lambda == nullptr ||
            !exactParameterRoles(instruction.parameterTypes,
                                 lambda->parameterTypes)) {
          return verificationFailure(
              body, owner,
              "lambda-call parameter roles do not match the exact "
              "target signature",
              block.id, instruction.id);
        }
        if (instruction.definedFailure.propagation !=
            FailurePropagationKind::Callable) {
          return verificationFailure(
              body, owner,
              "lambda-call failure propagation does not preserve "
              "the callable channel",
              block.id, instruction.id);
        }
      }
      if (instruction.kind == MirInstructionKind::Call &&
          !instruction.functionTarget && !instruction.lambdaTarget &&
          !instruction.nativeCalleeType &&
          instruction.intrinsic == IntrinsicKind::None &&
          instruction.callableInvocation &&
          instruction.definedFailure.propagation !=
              FailurePropagationKind::Callable) {
        return verificationFailure(
            body, owner,
            "deferred callable invocation is missing its failure "
            "propagation channel",
            block.id, instruction.id);
      }

      if (instruction.borrowOrigin == BorrowOriginKind::None) {
        if (instruction.loan || instruction.borrowPlace) {
          return verificationFailure(
              body, owner,
              "call or construct produces a loan without a borrow "
              "origin",
              block.id, instruction.id);
        }
        continue;
      }
      if (!instruction.loan) {
        const MirInstruction *argumentInput =
            instruction.kind == MirInstructionKind::Call &&
                    instruction.borrowOrigin == BorrowOriginKind::Argument &&
                    instruction.borrowArgument < instruction.operands.size() &&
                    instruction.operands[instruction.borrowArgument].kind ==
                        MirOperandKind::Value
                ? definitionFor(
                      body,
                      instruction.operands[instruction.borrowArgument].value)
                : nullptr;
        const bool ownerlessTextResult =
            instruction.kind == MirInstructionKind::Call &&
            instruction.borrowOrigin == BorrowOriginKind::Argument &&
            instruction.borrowAccess == AccessMode::ReadOnly &&
            !instruction.borrowPlace &&
            (instruction.info.type.kind == SemanticType::StringView ||
             instruction.info.type.kind == SemanticType::CString) &&
            !instruction.info.traits.containsBorrowedState &&
            instruction.borrowArgument < instruction.parameterTypes.size() &&
            instruction.parameterTypes[instruction.borrowArgument] ==
                instruction.info.type &&
            argumentInput != nullptr &&
            argumentInput->kind == MirInstructionKind::CallInput &&
            argumentInput->callInputRole == MirCallInputRole::Argument &&
            argumentInput->callInputIndex == instruction.borrowArgument &&
            argumentInput->callInputKind == HirCallInputKind::Value &&
            argumentInput->info.type == instruction.info.type &&
            !argumentInput->info.traits.containsBorrowedState;
        if (ownerlessTextResult) {
          continue;
        }
        return verificationFailure(
            body, owner,
            "owner-dependent call or construct is missing its "
            "result loan",
            block.id, instruction.id);
      }
      const MirLoan *loan = body.findLoan(*instruction.loan);
      if (loan == nullptr || loan->access != instruction.borrowAccess) {
        return verificationFailure(
            body, owner,
            "call-result loan access does not match its borrow "
            "origin",
            block.id, instruction.id);
      }
      if (loan->kind != MirLoanKind::CallResult &&
          loan->kind != MirLoanKind::Stored) {
        return verificationFailure(
            body, owner,
            "call or construct result must retain a CallResult or Stored loan",
            block.id, instruction.id);
      }

      std::vector<MirLoanDependency> mappedCallSources;
      for (const auto &mapping : instruction.borrowFields) {
        const MirOperand *input =
            mapping.origin == BorrowOriginKind::Receiver && instruction.receiver
                ? &*instruction.receiver
            : mapping.origin == BorrowOriginKind::Argument &&
                    mapping.parameter < instruction.operands.size()
                ? &instruction.operands[mapping.parameter]
                : nullptr;
        if (input == nullptr)
          return verificationFailure(body, owner,
                                     "returned field has no selected input",
                                     block.id, instruction.id);
        auto carrierType = input->type;
        if (carrierType.kind == SemanticType::Reference &&
            carrierType.arguments.size() == 1)
          carrierType = SemanticType(carrierType.arguments.front());
        const auto inputPaths =
            findStoredCarrierReferences(program, carrierType);
        const auto abstractSources =
            dependencySources(dependencySources, *input, 0, true, false);
        const bool compactCarrier =
            instruction.info.category == ValueCategory::Value &&
            instruction.info.type.kind == SemanticType::Class &&
            instruction.info.traits.containsBorrowedState &&
            instruction.info.traits.copyable && inputPaths.size() == 1 &&
            inputPaths.front().fields == mapping.sourceFields &&
            abstractSources.size() == 1 &&
            abstractSources.front().access == AccessMode::ReadOnly &&
            abstractSources.front().designatedFields == mapping.sourceFields &&
            std::any_of(
                body.loans.begin(), body.loans.end(), [&](const auto &entry) {
                  return entry.entry && entry.kind == MirLoanKind::Stored &&
                         entry.source == abstractSources.front().source &&
                         entry.additionalDependencies.empty() &&
                         entry.designatedFields == mapping.sourceFields;
                });
        // A reference alias of an entry carrier still denotes the same
        // abstract external input. Follow that alias's checked dependency,
        // without requiring a runtime field projection of the entry shell.
        auto sources = compactCarrier ? abstractSources
                                      : dependencySources(dependencySources,
                                                          *input, 0, true);
        const auto prefixOf = [](const auto &prefix, const auto &fields) {
          return prefix.size() <= fields.size() &&
                 std::equal(prefix.begin(), prefix.end(), fields.begin());
        };
        auto selected = std::find_if(
            sources.begin(), sources.end(), [&](const auto &source) {
              return !source.designatedFields.empty() &&
                     prefixOf(source.designatedFields, mapping.sourceFields);
            });
        std::size_t prefixLength =
            selected == sources.end() ? 0 : selected->designatedFields.size();
        if (selected == sources.end() && sources.size() == 1 &&
            sources.front().designatedFields.empty()) {
          const auto &paths = inputPaths;
          if (mapping.sourceFields.empty() ||
              (paths.size() == 1 &&
               prefixOf(paths.front().fields, mapping.sourceFields))) {
            selected = sources.begin();
            prefixLength =
                mapping.sourceFields.empty() ? 0 : paths.front().fields.size();
          }
        }
        if (selected == sources.end() || selected->source == 0)
          return verificationFailure(
              body, owner, "returned field loses its selected input field",
              block.id, instruction.id);
        auto access = selected->access;
        if (mapping.origin == BorrowOriginKind::Argument &&
            mapping.parameter < instruction.parameterTypes.size() &&
            instruction.parameterTypes[mapping.parameter].kind ==
                SemanticType::Reference &&
            mapping.sourceFields.empty())
          access =
              instruction.parameterTypes[mapping.parameter].referenceAccess;
        if (mapping.access == AccessMode::Mutable &&
            access != AccessMode::Mutable)
          return verificationFailure(
              body, owner,
              "returned field upgrades its selected input permission", block.id,
              instruction.id);
        const auto projected =
            prefixLength == mapping.sourceFields.size()
                ? selected->source
                : projectedFieldSource(
                      selected->source,
                      {mapping.sourceFields.begin() + prefixLength,
                       mapping.sourceFields.end()});
        if (projected == 0)
          return verificationFailure(
              body, owner, "returned field has no exact referent projection",
              block.id, instruction.id);
        mappedCallSources.push_back({.source = projected,
                                     .designatedFields = mapping.resultFields,
                                     .access = mapping.access});
      }
      std::optional<MirPlaceId> expectedSource;
      if (instruction.borrowOrigin == BorrowOriginKind::Receiver) {
        if (instruction.kind != MirInstructionKind::Call ||
            !instruction.receiver) {
          return verificationFailure(
              body, owner, "receiver-dependent result has no call receiver",
              block.id, instruction.id);
        }
        expectedSource = selectedSource(*instruction.receiver);
      } else if (instruction.borrowOrigin == BorrowOriginKind::Argument) {
        if (instruction.borrowArgument >= instruction.operands.size()) {
          return verificationFailure(
              body, owner,
              "owner-dependent argument index is outside the call "
              "operands",
              block.id, instruction.id);
        }
        expectedSource =
            selectedSource(instruction.operands[instruction.borrowArgument]);
      }
      if (!mappedCallSources.empty())
        expectedSource = mappedCallSources.front().source;
      const auto receiverSource = instruction.receiver
                                      ? selectedSource(*instruction.receiver)
                                      : std::nullopt;
      const auto *receiverPlace =
          receiverSource ? body.findPlace(*receiverSource) : nullptr;
      const auto receiverLoans =
          receiverPlace && receiverPlace->root == MirPlaceRootKind::Binding
              ? bindingLoans.find(receiverPlace->binding)
              : bindingLoans.end();
      const auto *receiverLoan =
          receiverLoans != bindingLoans.end() &&
                  receiverLoans->second.size() == 1 && receiverPlace != nullptr
              ? body.findLoan(receiverLoans->second.front())
              : nullptr;
      const bool opaqueRangeCarrier =
          receiverLoan != nullptr &&
          ((receiverLoan->kind == MirLoanKind::Local &&
            receiverLoan->storedDependency == 0) ||
           ((receiverLoan->kind == MirLoanKind::Local ||
             receiverLoan->kind == MirLoanKind::CallResult) &&
            std::any_of(body.rangeIterations.begin(),
                        body.rangeIterations.end(),
                        [&](const MirRangeIteration &range) {
                          const auto *iterator = body.findPlace(range.iterator);
                          return iterator != nullptr &&
                                 iterator->root == MirPlaceRootKind::Binding &&
                                 iterator->binding == receiverPlace->binding;
                        })));
      const bool receiverStorageResult =
          !opaqueRangeCarrier && instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic == IntrinsicKind::None && target != nullptr &&
          target->returnType.kind == SemanticType::Reference &&
          target->returnBorrowOrigin == BorrowOriginKind::Receiver &&
          target->returnBorrowFields.empty() && instruction.receiver &&
          !findStoredCarrierReferences(program, instruction.receiver->type)
               .empty();
      const bool shellResult =
          isCarrierReferenceResult(program, instruction) ||
          (instruction.info.category == ValueCategory::Place &&
           ((instruction.info.type.kind == SemanticType::Class &&
             instruction.info.traits.containsBorrowedState) ||
            (exactMirCapabilityClass(
                 program, instruction.info.type,
                 CompilerCapabilityTypeKind::ErasedObject) &&
             instruction.borrowOrigin == BorrowOriginKind::Argument &&
             instruction.borrowArgument < instruction.parameterTypes.size() &&
             instruction.parameterTypes[instruction.borrowArgument].kind ==
                 SemanticType::Reference &&
             !findStoredCarrierReferences(
                  program,
                  instruction.parameterTypes[instruction.borrowArgument])
                  .empty()) ||
            receiverStorageResult));
      if (shellResult &&
          instruction.borrowOrigin == BorrowOriginKind::Receiver &&
          instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value) {
        const MirOperand *receiver = &*instruction.receiver;
        const auto *stage = definitionFor(body, receiver->value);
        if (stage != nullptr && stage->kind == MirInstructionKind::CallInput &&
            stage->callInputRole == MirCallInputRole::Receiver &&
            stage->callInputKind == HirCallInputKind::Value &&
            stage->operands.size() == 1)
          receiver = &stage->operands.front();
        if (receiver->kind == MirOperandKind::Value) {
          // A Value receiver can still be an addressable, full-expression
          // temporary. Recover only its exact producer-owned materialization,
          // never a same-typed shell or the producer's external borrow source.
          std::optional<MirPlaceId> temporarySource;
          const auto *receiverValue = body.findValue(receiver->value);
          for (const auto &place : body.places) {
            if (receiverValue == nullptr ||
                place.sourceValue != receiverValue->sourceValue ||
                place.type != receiver->type)
              continue;
            const auto temporary =
                findMirTemporaryCarrierDependency(body, place);
            if (!temporary ||
                temporary.materialization.value->id != receiver->value)
              continue;
            if (temporarySource && *temporarySource != place.id)
              return verificationFailure(
                  body, owner,
                  "temporary receiver has ambiguous materialization", block.id,
                  instruction.id);
            temporarySource = place.id;
          }
          if (temporarySource)
            expectedSource = temporarySource;
          else if (const auto *producer = definitionFor(body, receiver->value);
                   producer != nullptr &&
                   (producer->kind == MirInstructionKind::Call ||
                    producer->kind == MirInstructionKind::Construct) &&
                   producer->info.category == ValueCategory::Value &&
                   producer->info.type.kind == SemanticType::Class &&
                   producer->info.traits.containsBorrowedState)
            return verificationFailure(
                body, owner,
                "temporary receiver is missing its exact materialization and "
                "drop authority",
                block.id, instruction.id);
        }
      }
      if (instruction.kind == MirInstructionKind::Construct &&
          instruction.borrowOrigin == BorrowOriginKind::Argument &&
          instruction.borrowArgument < instruction.operands.size()) {
        const MirOperand *input =
            &instruction.operands[instruction.borrowArgument];
        if (input->kind == MirOperandKind::Value) {
          const auto *prepared = definitionFor(body, input->value);
          if (prepared != nullptr &&
              prepared->kind == MirInstructionKind::CallInput &&
              prepared->operands.size() == 1)
            input = &prepared->operands.front();
        }
        const auto *place = body.findPlace(input->place);
        const auto *inputLoan =
            input->kind == MirOperandKind::Loan ? body.findLoan(input->loan)
            : place != nullptr && place->root == MirPlaceRootKind::Loan
                ? body.findLoan(place->loan)
                : nullptr;
        if (inputLoan != nullptr &&
            inputLoan->kind == MirLoanKind::CallResult &&
            loan->parent != inputLoan->id)
          return verificationFailure(
              body, owner,
              "stored construction must preserve its accessor parent", block.id,
              instruction.id);
      }
      // A selected stored-reference field already has an independently
      // verified projected source. Flattening that projection to its enclosing
      // carrier would lose which external reference the target returned.
      const bool exactFieldSource =
          !mappedCallSources.empty() &&
          !instruction.borrowFields.front().sourceFields.empty();
      const CanonicalPlace expectedPlace =
          expectedSource
              ? canonicalBorrowOrigin(
                    body,
                    shellResult || exactFieldSource
                        ? *expectedSource
                        : nestedCarrierRoot(program, body, *expectedSource)
                              .value_or(*expectedSource),
                    bindingLoans, shellResult)
              : CanonicalPlace{.ambiguous = true};
      const CanonicalPlace actualPlace =
          canonicalBorrowOrigin(body, loan->source, bindingLoans, shellResult);
      if (instruction.borrowOrigin == BorrowOriginKind::Global) {
        if (!instruction.borrowPlace ||
            !sameGlobalBorrowPlace(*instruction.borrowPlace, actualPlace)) {
          return verificationFailure(
              body, owner,
              "global call-result loan does not preserve its "
              "summarized static storage place",
              block.id, instruction.id);
        }
        continue;
      }
      if (instruction.borrowPlace) {
        return verificationFailure(
            body, owner, "non-global call-result carries a global origin place",
            block.id, instruction.id);
      }
      const bool exactSource =
          expectedSource && *expectedSource == loan->source;
      const MirPlace *selectedPlace =
          expectedSource ? body.findPlace(*expectedSource) : nullptr;
      const MirLoan *selectedLoan =
          selectedPlace != nullptr &&
                  selectedPlace->root == MirPlaceRootKind::Loan
              ? body.findLoan(selectedPlace->loan)
              : nullptr;
      if (selectedLoan != nullptr &&
          selectedLoan->kind == MirLoanKind::CallResult &&
          !selectedPlace->projections.empty() && !exactSource) {
        return verificationFailure(
            body, owner,
            "projected call-result borrow must preserve the selected referent "
            "place rather than flattening it into its dependency origin",
            block.id, instruction.id);
      }
      if (!exactSource && !sameCanonicalPlace(expectedPlace, actualPlace)) {
        const std::string expected =
            expectedSource ? std::to_string(*expectedSource) : "unresolved";
        return verificationFailure(
            body, owner,
            "call-result loan does not preserve the selected "
            "receiver or argument source identity (expected " +
                expected + ", found " + std::to_string(loan->source) + ")",
            block.id, instruction.id);
      }
      // Physical source and inherited stored authority were verified
      // independently. Do not flatten the latter into a homogeneous source
      // list and thereby erase shell identity or mix permissions.
      if (shellResult) {
        const auto *source = body.findPlace(loan->source);
        const bool independentText =
            source && independentlyStaticTextBinding(body, *source);
        if ((loan->storedDependency == 0 && !independentText) ||
            !loan->additionalSources().empty() ||
            !instruction.borrowArguments.empty())
          return verificationFailure(
              body, owner,
              "carrier reference result requires one exact stored dependency",
              block.id, instruction.id);
        continue;
      }
      {
        std::vector<MirLoanDependency> expectedSources;
        const auto appendSources = [&](const MirOperand &operand,
                                       const SemanticType *parameter =
                                           nullptr) {
          auto sources = dependencySources(dependencySources, operand);
          // A direct reference input lends exactly its declared permission,
          // irrespective of the mutability of the caller's owning binding.
          // Borrowed-carrier inputs retain their independently checked fields.
          if (parameter != nullptr &&
              parameter->kind == SemanticType::Reference &&
              parameter->arguments.size() == 1 &&
              findStoredCarrierReferences(program, parameter->arguments.front())
                  .empty())
            for (auto &source : sources)
              source.access = parameter->referenceAccess;
          expectedSources.insert(expectedSources.end(), sources.begin(),
                                 sources.end());
        };
        if (instruction.borrowOrigin == BorrowOriginKind::Receiver &&
            instruction.receiver) {
          appendSources(*instruction.receiver);
        } else {
          const auto parameters =
              instruction.borrowArguments.empty()
                  ? std::vector<std::size_t>{instruction.borrowArgument}
                  : instruction.borrowArguments;
          for (const auto parameter : parameters) {
            if (parameter >= instruction.operands.size()) {
              return verificationFailure(
                  body, owner,
                  "composed borrow origin exceeds the call arguments", block.id,
                  instruction.id);
            }
            appendSources(instruction.operands[parameter],
                          parameter < instruction.parameterTypes.size()
                              ? &instruction.parameterTypes[parameter]
                              : nullptr);
          }
        }
        const auto actualSources = concreteDependencies(*loan);
        if (!mappedCallSources.empty())
          expectedSources = mappedCallSources;
        if (expectedSources.size() <= 1 && actualSources.size() <= 1 &&
            instruction.borrowArguments.empty()) {
          // A compact sole-origin loan may omit its destination path, but an
          // explicitly published path must still describe the result, never
          // a field from the selected input's different carrier layout.
          if (mappedCallSources.size() == 1 && actualSources.size() == 1 &&
              !actualSources.front().designatedFields.empty() &&
              actualSources.front().designatedFields !=
                  mappedCallSources.front().designatedFields)
            return verificationFailure(
                body, owner,
                "call-result loan loses its mapped destination field", block.id,
                instruction.id);
          continue;
        }
        const auto canonical = [&](MirPlaceId source) {
          return canonicalBorrowOrigin(
              body, nestedCarrierRoot(program, body, source).value_or(source),
              bindingLoans);
        };
        const auto resultPaths =
            findStoredCarrierReferences(program, instruction.info.type);
        const bool mappedConstruction =
            instruction.kind == MirInstructionKind::Construct &&
            instruction.constructorTarget && resultPaths.size() > 1;
        if (mappedConstruction) {
          if (instruction.borrowFields.size() != resultPaths.size())
            return verificationFailure(
                body, owner,
                "constructor field contract does not cover its result layout",
                block.id, instruction.id);
          const auto *callee =
              program.findConstructorInstance(*instruction.constructorTarget);
          if (callee == nullptr)
            return verificationFailure(
                body, owner, "stored field mapping has no constructor",
                block.id, instruction.id);
          std::vector<MirLoanDependency> mapped;
          for (const auto &path : resultPaths) {
            const auto origin = constructorFieldOrigin(program, *callee, path);
            if (!origin || origin->parameter >= instruction.operands.size())
              return verificationFailure(
                  body, owner,
                  "stored field mapping has no exact formal origin", block.id,
                  instruction.id);
            const auto &mapping = instruction.borrowFields[mapped.size()];
            if (mapping != *origin)
              return verificationFailure(body, owner,
                                         "constructor field contract disagrees "
                                         "with its initializer publication",
                                         block.id, instruction.id);
            // The field resolver above already proved this input's selected
            // external source, including compact single-path entry carriers.
            // Re-projecting the formal's shell here would fabricate a runtime
            // field place instead of preserving that opaque entry identity.
            // The independent constructor proof still binds every mapping to
            // its exact initializer and result layout.
            if (mapped.size() >= mappedCallSources.size() ||
                mappedCallSources[mapped.size()].source == 0 ||
                mappedCallSources[mapped.size()].designatedFields !=
                    path.fields ||
                mappedCallSources[mapped.size()].access !=
                    path.reference.referenceAccess)
              return verificationFailure(
                  body, owner,
                  "stored field mapping loses a nested formal path", block.id,
                  instruction.id);
            mapped.push_back({.source = mappedCallSources[mapped.size()].source,
                              .designatedFields = path.fields,
                              .access = path.reference.referenceAccess});
          }
          if (mapped.size() != actualSources.size())
            return verificationFailure(
                body, owner,
                "stored field mapping must preserve every field identity",
                block.id, instruction.id);
          expectedSources = std::move(mapped);
        }
        const auto includes = [&](const std::vector<MirLoanDependency> &sources,
                                  const MirLoanDependency &source) {
          const auto expected = canonical(source.source);
          return std::any_of(
              sources.begin(), sources.end(), [&](const auto &candidate) {
                return candidate.access == source.access &&
                       ((!mappedConstruction && mappedCallSources.empty()) ||
                        candidate.designatedFields ==
                            source.designatedFields) &&
                       sameCanonicalPlace(expected,
                                          canonical(candidate.source));
              });
        };
        if (std::any_of(expectedSources.begin(), expectedSources.end(),
                        [&](const auto &source) {
                          return !includes(actualSources, source);
                        }) ||
            std::any_of(actualSources.begin(), actualSources.end(),
                        [&](const auto &source) {
                          return !includes(expectedSources, source);
                        })) {
          const auto describe =
              [](const std::vector<MirLoanDependency> &sources) {
                std::string text;
                for (const auto &source : sources) {
                  if (!text.empty())
                    text += ',';
                  text += std::to_string(source.source);
                  text +=
                      source.access == AccessMode::Mutable ? ":mut" : ":read";
                  text += ":fields=";
                  for (const auto field : source.designatedFields)
                    text += std::to_string(field) + '.';
                }
                return text;
              };
          return verificationFailure(body, owner,
                                     "composed borrow result does not preserve "
                                     "every independent owner "
                                     "(expected places " +
                                         describe(expectedSources) +
                                         "; actual places " +
                                         describe(actualSources) + ")",
                                     block.id, instruction.id);
        }
      }
    }
  }
  return {};
}

[[nodiscard]] std::vector<MirStoredCarrierReference>
findStoredCarrierReferences(const MirProgram &program,
                            const SemanticType &type) {
  std::vector<SemanticType> visiting;
  const auto visit = [&](const auto &self, const SemanticType &current)
      -> std::vector<MirStoredCarrierReference> {
    if (std::find(visiting.begin(), visiting.end(), current) !=
        visiting.end()) {
      return {};
    }
    visiting.push_back(current);
    if (current.kind == SemanticType::Lambda) {
      const auto instance = std::find_if(
          program.lambdaInstances().begin(), program.lambdaInstances().end(),
          [&](const auto &row) { return row.type == current; });
      std::vector<MirStoredCarrierReference> paths;
      if (instance != program.lambdaInstances().end() &&
          instance->captureSymbols.size() == instance->captureTypes.size()) {
        for (std::size_t index = 0; index < instance->captureTypes.size();
             ++index) {
          const auto &field = instance->captureTypes[index];
          auto children =
              field.kind == SemanticType::Reference
                  ? std::vector<MirStoredCarrierReference>{{.reference = field}}
                  : self(self, field);
          for (auto &child : children) {
            child.fields.insert(child.fields.begin(),
                                instance->captureSymbols[index]);
            paths.push_back(std::move(child));
          }
        }
      }
      visiting.pop_back();
      return paths;
    }
    if (current.kind != SemanticType::Class) {
      visiting.pop_back();
      return {};
    }
    const auto instance = std::find_if(program.classInstances().begin(),
                                       program.classInstances().end(),
                                       [&](const MirClassInstance &candidate) {
                                         return candidate.type == current;
                                       });
    if (instance == program.classInstances().end() ||
        !instance->traits.containsBorrowedState) {
      visiting.pop_back();
      return {};
    }
    std::vector<MirStoredCarrierReference> references;
    bool invalid = false;
    const auto containsBorrow = [&](const SemanticType &candidateType) {
      if (semanticTraits(candidateType).containsBorrowedState) {
        return true;
      }
      const auto candidate = std::find_if(program.classInstances().begin(),
                                          program.classInstances().end(),
                                          [&](const MirClassInstance &row) {
                                            return row.type == candidateType;
                                          });
      return candidate != program.classInstances().end() &&
             candidate->traits.containsBorrowedState;
    };
    if (instance->kind == ClassKind::Union || instance->cAbiRecord ||
        (instance->destructor && !instance->lifecycleBoundBorrowCarrier)) {
      invalid = true;
    }
    for (const HirBaseInstance &base : instance->structuralBases) {
      invalid = invalid || (!base.interface && containsBorrow(base.type));
    }
    for (const MirClassFieldInfo &field : instance->declaredFields) {
      std::vector<MirStoredCarrierReference> children;
      if (field.type.kind == SemanticType::Reference) {
        children.push_back(MirStoredCarrierReference{.reference = field.type});
      } else {
        children = self(self, field.type);
        invalid = invalid || (children.empty() && containsBorrow(field.type));
      }
      for (auto &child : children) {
        if (field.symbol == 0 || child.reference.arguments.size() != 1) {
          invalid = true;
          continue;
        }
        child.fields.insert(child.fields.begin(), field.symbol);
        references.push_back(std::move(child));
      }
    }
    visiting.pop_back();
    if (invalid) {
      return {};
    }
    return references;
  };
  return visit(visit, type);
}

[[nodiscard]] std::optional<MirStoredCarrierReference>
findStoredCarrierReference(const MirProgram &program,
                           const SemanticType &type) {
  auto references = findStoredCarrierReferences(program, type);
  return references.empty()
             ? std::nullopt
             : std::optional<MirStoredCarrierReference>(references.front());
}

[[nodiscard]] bool isCarrierReferenceResult(const MirProgram &program,
                                            const MirInstruction &instruction) {
  if ((instruction.kind != MirInstructionKind::Construct &&
       (instruction.kind != MirInstructionKind::Call ||
        instruction.intrinsic != IntrinsicKind::None ||
        !instruction.functionTarget)) ||
      !instruction.borrowArguments.empty() ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.type.kind != SemanticType::Class)
    return false;
  const auto paths =
      findStoredCarrierReferences(program, instruction.info.type);
  if (paths.size() != 1)
    return false;
  if (instruction.borrowOrigin == BorrowOriginKind::Receiver) {
    const auto *target =
        instruction.functionTarget
            ? program.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    // An ordinary compact receiver result can reference an inline owned
    // field rather than the entire receiver type. Its verified callee return
    // contract establishes that path; keeping the whole physical receiver
    // alive at the call site is the conservative public contract.
    return instruction.kind == MirInstructionKind::Call &&
           instruction.receiver && instruction.borrowFields.empty() &&
           target != nullptr && target->returnType == instruction.info.type &&
           target->returnBorrowOrigin == BorrowOriginKind::Receiver &&
           target->returnBorrowFields.empty() &&
           target->returnBorrowParameters.empty() &&
           findStoredCarrierReference(program, instruction.receiver->type)
               .has_value();
  }
  if (instruction.borrowOrigin != BorrowOriginKind::Argument ||
      instruction.borrowArgument >= instruction.parameterTypes.size() ||
      instruction.borrowFields.size() > 1 ||
      (!instruction.borrowFields.empty() &&
       (!instruction.borrowFields.front().sourceFields.empty() ||
        instruction.borrowFields.front().origin != BorrowOriginKind::Argument ||
        instruction.borrowFields.front().parameter !=
            instruction.borrowArgument)))
    return false;
  const auto &parameter =
      instruction.parameterTypes[instruction.borrowArgument];
  if (parameter.kind != SemanticType::Reference ||
      parameter.arguments.size() != 1 ||
      !isPotentiallyBorrowedCarrier(program, parameter.arguments.front()))
    return false;
  return paths.front().reference.arguments == parameter.arguments;
}

// Zero denotes an exact caller-owned receiver/reference source. A nonzero
// binding requires a separately proved local carrier publication. Field
// projections are admitted only when their concrete type and every returned
// reference path are a structural suffix of the entry's declared layout.
[[nodiscard]] std::optional<HirBindingId>
storedCarrierValueOrigin(const MirProgram &program, const MirBody &body,
                         const MirLoan &entry, const MirPlace &source,
                         const SemanticType &carrierType) {
  if (source.type != carrierType)
    return std::nullopt;
  if (source.root == MirPlaceRootKind::Binding && source.projections.empty() &&
      std::find(entry.carriers.begin(), entry.carriers.end(), source.binding) !=
          entry.carriers.end())
    return source.binding;
  const MirPlace *origin = body.findPlace(entry.source);
  if (origin == nullptr || !entry.entry || !origin->projections.empty())
    return std::nullopt;
  const bool reference = origin->root == MirPlaceRootKind::Binding &&
                         origin->type.kind == SemanticType::Reference &&
                         origin->type.arguments.size() == 1;
  if (!reference && origin->root != MirPlaceRootKind::This)
    return std::nullopt;
  SemanticType projected =
      reference ? origin->type.arguments.front() : origin->type;
  std::size_t first = 0;
  if (source.root == MirPlaceRootKind::Loan) {
    const MirLoan *physical = body.findLoan(source.loan);
    if (physical == nullptr || physical->storedDependency != entry.id ||
        !source.projections.empty() || projected != source.type)
      return std::nullopt;
  } else if (reference) {
    if (source.root != MirPlaceRootKind::Binding ||
        source.binding != origin->binding || source.projections.empty() ||
        source.projections.front().kind != MirProjectionKind::Dereference)
      return std::nullopt;
    first = 1;
  } else if (source.root != MirPlaceRootKind::This) {
    return std::nullopt;
  }
  const auto rootPaths = findStoredCarrierReferences(program, projected);
  std::vector<SymbolId> prefix;
  for (std::size_t index = first; index < source.projections.size(); ++index) {
    const auto &projection = source.projections[index];
    if (projection.kind != MirProjectionKind::Field)
      return std::nullopt;
    const auto owner = std::find_if(
        program.classInstances().begin(), program.classInstances().end(),
        [&](const auto &instance) { return instance.type == projected; });
    if (owner == program.classInstances().end())
      return std::nullopt;
    const auto field =
        std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                     [&](const auto &candidate) {
                       return candidate.symbol == projection.field;
                     });
    if (field == owner->declaredFields.end())
      return std::nullopt;
    prefix.push_back(field->symbol);
    projected = field->type;
  }
  if (projected != carrierType)
    return std::nullopt;
  const auto paths = findStoredCarrierReferences(program, carrierType);
  if (paths.empty())
    return std::nullopt;
  for (const auto &path : paths) {
    auto fields = prefix;
    fields.insert(fields.end(), path.fields.begin(), path.fields.end());
    if (std::none_of(rootPaths.begin(), rootPaths.end(),
                     [&](const auto &rootPath) {
                       return rootPath.fields == fields &&
                              rootPath.reference == path.reference;
                     }))
      return std::nullopt;
  }
  return HirBindingId{0};
}

// An entry's carrier list records lifetime users, not evidence that those
// users received its references. Prove each local publication independently
// before accepting value copies of a receiver/reference-formal carrier.
[[nodiscard]] bool exactAbstractStoredCarrierPublications(
    const MirProgram &program, const MirBody &body,
    const std::vector<HirBindingId> &parameterBindings, const MirLoan &entry,
    const SemanticType &carrierType) {
  if (entry.carriers.empty())
    return true;
  struct Publication {
    const MirBlock *block = nullptr;
    const MirInstruction *initialize = nullptr;
    std::size_t position = 0;
  };
  std::unordered_map<HirBindingId, Publication> publications;
  for (const HirBindingId carrier : entry.carriers) {
    if (std::find(parameterBindings.begin(), parameterBindings.end(),
                  carrier) != parameterBindings.end())
      return false;
    bool hasStorage = false;
    for (const MirPlace &place : body.places) {
      if (place.root != MirPlaceRootKind::Binding || place.binding != carrier ||
          !place.projections.empty())
        continue;
      if (place.type != carrierType || place.initiallyAvailable)
        return false;
      hasStorage = true;
    }
    if (!hasStorage)
      return false;
    Publication publication;
    for (const MirBlock &block : body.blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const MirInstruction &instruction = block.instructions[index];
        const MirPlace *destination =
            instruction.destination ? body.findPlace(*instruction.destination)
                                    : nullptr;
        if (destination == nullptr ||
            destination->root != MirPlaceRootKind::Binding ||
            destination->binding != carrier ||
            !destination->projections.empty())
          continue;
        if (instruction.kind == MirInstructionKind::Assign)
          return false; // Replacement needs its own dependency publication.
        if (instruction.kind != MirInstructionKind::Initialize)
          continue;
        if (publication.initialize != nullptr ||
            destination->type != carrierType ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value ||
            instruction.operands.front().type != carrierType)
          return false;
        publication = {&block, &instruction, index};
      }
    }
    if (publication.initialize == nullptr ||
        !publications.emplace(carrier, publication).second)
      return false;
  }
  const auto dominance = computeMirDominance(body);
  if (!dominance)
    return false;
  const MirPlace *entrySource = body.findPlace(entry.source);
  if (entrySource == nullptr)
    return false;
  std::unordered_set<HirBindingId> proven;
  std::unordered_set<HirBindingId> visiting;
  const auto prove = [&](const auto &self, HirBindingId carrier) -> bool {
    if (proven.contains(carrier))
      return true;
    const auto found = publications.find(carrier);
    if (found == publications.end() || !visiting.insert(carrier).second)
      return false;
    const Publication &publication = found->second;
    const MirValue *value =
        body.findValue(publication.initialize->operands.front().value);
    if (value == nullptr || value->info.type != carrierType ||
        value->definitionBlock != publication.block->id)
      return false;
    const auto producer =
        std::find_if(publication.block->instructions.begin(),
                     publication.block->instructions.end(),
                     [&](const MirInstruction &instruction) {
                       return instruction.id == value->definition;
                     });
    if (producer == publication.block->instructions.end() ||
        static_cast<std::size_t>(producer -
                                 publication.block->instructions.begin()) >=
            publication.position ||
        producer->result != value->id || producer->info.type != carrierType ||
        producer->operands.size() != 1 ||
        (producer->kind != MirInstructionKind::Load &&
         producer->kind != MirInstructionKind::Move))
      return false;
    const MirOperand &operand = producer->operands.front();
    const MirPlace *source = body.findPlace(operand.place);
    const bool moved = producer->kind == MirInstructionKind::Move;
    if (operand.type != carrierType || source == nullptr ||
        source->type != carrierType ||
        operand.kind != (moved ? MirOperandKind::Move : MirOperandKind::Copy) ||
        !(moved ? source->traits.movable : source->traits.copyable))
      return false;
    const auto origin =
        storedCarrierValueOrigin(program, body, entry, *source, carrierType);
    bool received = origin && *origin == 0;
    if (origin && *origin != 0 && self(self, *origin)) {
      const Publication &prior = publications.at(*origin);
      received =
          prior.block == publication.block
              ? prior.position <
                    static_cast<std::size_t>(
                        producer - publication.block->instructions.begin())
              : dominance->dominates(prior.block->id, publication.block->id);
    }
    if (!received)
      return false;
    visiting.erase(carrier);
    proven.insert(carrier);
    return true;
  };
  return std::all_of(
      entry.carriers.begin(), entry.carriers.end(),
      [&](HirBindingId carrier) { return prove(prove, carrier); });
}

[[nodiscard]] MirVerificationResult
verifyCarrierShellDependencies(const MirProgram &program, const MirBody &body,
                               std::size_t owner) {
  for (const auto &loan : body.loans) {
    const auto *source = body.findPlace(loan.source);
    if (source == nullptr)
      continue; // The body verifier owns malformed place identities.
    const SemanticType &sourceType =
        source->type.kind == SemanticType::Reference &&
                source->type.arguments.size() == 1
            ? source->type.arguments.front()
            : source->type;
    const bool returnShell = loan.kind == MirLoanKind::Return &&
                             body.returnType.kind == SemanticType::Reference &&
                             body.returnType.arguments.size() == 1 &&
                             isPotentiallyBorrowedCarrier(
                                 program, body.returnType.arguments.front());
    const auto *constructor = body.kind == MirBodyKind::Constructor
                                  ? program.findConstructorInstance(owner)
                                  : nullptr;
    const auto *constructedType =
        constructor != nullptr ? program.findClassInstance(constructor->owner)
                               : nullptr;
    // An owned carrier field transfers its stored referent; only a field
    // whose type is itself a reference to a carrier publishes a shell borrow.
    // Private storage capabilities and references to ordinary owners are not
    // carrier shells merely because the constructor transports a view.
    const bool carrierReferenceField =
        constructedType != nullptr &&
        std::any_of(constructedType->declaredFields.begin(),
                    constructedType->declaredFields.end(),
                    [&](const auto &field) {
                      return field.symbol == loan.storedField &&
                             field.type.kind == SemanticType::Reference &&
                             field.type.arguments.size() == 1 &&
                             isPotentiallyBorrowedCarrier(
                                 program, field.type.arguments.front());
                    });
    const auto *originalDependency = body.findLoan(loan.storedDependency);
    const bool transformedReferenceField =
        constructor && originalDependency &&
        originalDependency->kind == MirLoanKind::Stored &&
        originalDependency->entry &&
        std::any_of(
            constructor->initializers.begin(), constructor->initializers.end(),
            [&](const auto &initializer) {
              if (initializer.field != loan.storedField)
                return false;
              const auto *call =
                  referenceInitializerCall(program, *constructor, initializer);
              const auto *result =
                  call && call->loan ? body.findLoan(*call->loan) : nullptr;
              return result && loan.producedBy == call->hirValue &&
                     loan.source == result->source &&
                     loan.access == result->access &&
                     loan.storedDependency == result->storedDependency &&
                     loan.additionalDependencies ==
                         result->additionalDependencies;
            });
    const bool publishedShell =
        body.kind == MirBodyKind::Constructor &&
        loan.kind == MirLoanKind::Stored && loan.storedField != 0 &&
        loan.escapes && (carrierReferenceField || transformedReferenceField) &&
        isPotentiallyBorrowedCarrier(program, sourceType);
    bool constructedShell = false;
    if (loan.kind == MirLoanKind::Stored && !loan.entry && !loan.escapes &&
        loan.storedField == 0 &&
        isPotentiallyBorrowedCarrier(program, sourceType)) {
      for (const auto &block : body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.loan != loan.id ||
              !isCarrierReferenceResult(program, instruction))
            continue;
          const auto &input =
              instruction.borrowOrigin == BorrowOriginKind::Receiver
                  ? instruction.receiver->type
                  : instruction.parameterTypes[instruction.borrowArgument];
          const auto &referent = input.kind == SemanticType::Reference &&
                                         input.arguments.size() == 1
                                     ? input.arguments.front()
                                     : input;
          constructedShell = referent == sourceType;
        }
      }
    }
    const bool localShell = publishedShell || constructedShell || returnShell ||
                            ((loan.kind == MirLoanKind::Local ||
                              loan.kind == MirLoanKind::Parameter) &&
                             isPotentiallyBorrowedCarrier(program, sourceType));
    if (loan.kind == MirLoanKind::Stored && loan.parent == 0 &&
        loan.storedDependency != 0 && !publishedShell && !constructedShell)
      return verificationFailure(
          body, owner,
          "stored shell dependency requires an exact "
          "carrier-reference construction or publication");
    if (!localShell && loan.storedDependency == 0)
      continue;
    MirLoanId expected = 0;
    const auto *restoration = mir_detail::restoredBorrowAuthority(body, loan);
    if (restoration != nullptr &&
        verifiedRestoreCall(program, body, *restoration)) {
      const auto *result = body.findLoan(*restoration->loan);
      const auto *witness = result ? body.findLoan(result->parent) : nullptr;
      if (witness && result->storedDependency == witness->id)
        expected = witness->id;
    } else if (source->root == MirPlaceRootKind::Loan) {
      const auto *parent = body.findLoan(source->loan);
      expected = parent == nullptr ? 0 : parent->storedDependency;
    } else if (source->root == MirPlaceRootKind::Value ||
               source->root == MirPlaceRootKind::Temporary) {
      const auto temporary = findMirTemporaryCarrierDependency(body, *source);
      expected = temporary ? temporary.loan->id : 0;
    } else if (source->root == MirPlaceRootKind::Binding ||
               source->root == MirPlaceRootKind::This) {
      for (const auto &candidate : body.loans) {
        const auto *entrySource = body.findPlace(candidate.source);
        // verifyStoredEntryPaths has already independently checked every
        // attached local value publication; the source identity, not an empty
        // carrier list, distinguishes this caller-owned entry.
        const bool abstractEntry =
            candidate.entry && candidate.kind == MirLoanKind::Stored &&
            entrySource != nullptr && entrySource->root == source->root &&
            entrySource->binding == source->binding &&
            entrySource->projections.empty() &&
            (entrySource->root == MirPlaceRootKind::This ||
             entrySource->type.kind == SemanticType::Reference);
        const bool carried =
            source->root == MirPlaceRootKind::Binding &&
            (std::find(candidate.carriers.begin(), candidate.carriers.end(),
                       source->binding) != candidate.carriers.end() ||
             (candidate.kind == MirLoanKind::Stored &&
              candidate.lifecycleBound &&
              mir_detail::bindingReceivedStoredLoan(body, source->binding,
                                                    candidate.id)));
        if (candidate.id == loan.id || (!abstractEntry && !carried))
          continue;
        const MirLoanId dependency = candidate.kind == MirLoanKind::Stored
                                         ? candidate.id
                                         : candidate.storedDependency;
        if (dependency == 0)
          continue;
        if (expected != 0 && expected != dependency)
          return verificationFailure(
              body, owner,
              "carrier shell has ambiguous stored-owner authority");
        expected = dependency;
      }
    }
    if (expected == 0 && loan.storedDependency == 0 &&
        independentlyStaticTextBinding(body, *source))
      continue;
    if (expected == 0 || loan.storedDependency != expected)
      return verificationFailure(
          body, owner,
          "carrier shell loan does not preserve its exact stored dependency");
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyStoredEntryPaths(const MirProgram &program, const MirBody &body,
                       std::size_t owner) {
  for (const MirLoan &loan : body.loans) {
    if (loan.kind != MirLoanKind::Stored || !loan.entry || loan.escapes) {
      continue;
    }
    const MirPlace *source = body.findPlace(loan.source);
    if (source == nullptr) {
      continue;
    }
    const auto &carrierType = source->type.kind == SemanticType::Reference &&
                                      source->type.arguments.size() == 1
                                  ? source->type.arguments.front()
                                  : source->type;
    const bool abstractEntry =
        source->root == MirPlaceRootKind::This ||
        (body.kind == MirBodyKind::Lambda &&
         source->root == MirPlaceRootKind::Symbol && source->capture != 0) ||
        (source->root == MirPlaceRootKind::Binding &&
         source->type.kind == SemanticType::Reference);
    if (abstractEntry && loan.lifecycleBound)
      return verificationFailure(body, owner,
                                 "abstract stored dependency cannot own a "
                                 "lifecycle release obligation");
    const auto *function = body.kind == MirBodyKind::Function
                               ? program.findFunctionInstance(owner)
                               : nullptr;
    const auto *lambda =
        body.kind == MirBodyKind::Lambda ? program.findLambda(owner) : nullptr;
    const auto *constructor = body.kind == MirBodyKind::Constructor
                                  ? program.findConstructorInstance(owner)
                                  : nullptr;
    const auto *destructor = body.kind == MirBodyKind::Destructor
                                 ? program.findDestructorInstance(owner)
                                 : nullptr;
    const std::vector<HirBindingId> noParameters;
    const auto *parameterBindings =
        function != nullptr      ? &function->parameterBindings
        : lambda != nullptr      ? &lambda->parameterBindings
        : constructor != nullptr ? &constructor->parameterBindings
        : destructor != nullptr  ? &noParameters
                                 : nullptr;
    const auto *parameterTypes = function != nullptr ? &function->parameterTypes
                                 : lambda != nullptr ? &lambda->parameterTypes
                                 : constructor != nullptr
                                     ? &constructor->parameterTypes
                                     : nullptr;
    if (abstractEntry || loan.carriers.empty()) {
      const auto *receiver = function != nullptr && function->owner
                                 ? program.findClassInstance(*function->owner)
                             : destructor != nullptr
                                 ? program.findClassInstance(destructor->owner)
                                 : nullptr;
      const bool receiverEntry =
          source->root == MirPlaceRootKind::This && receiver != nullptr &&
          (destructor != nullptr || !function->staticMember) &&
          carrierType == receiver->type &&
          source->access ==
              (destructor != nullptr ||
                       receiverAllowsMutation(function->receiverMutability)
                   ? AccessMode::Mutable
                   : AccessMode::ReadOnly);
      bool parameterEntry = false;
      if (parameterBindings != nullptr && parameterTypes != nullptr &&
          source->root == MirPlaceRootKind::Binding &&
          source->type.kind == SemanticType::Reference) {
        for (std::size_t index = 0; index < parameterBindings->size() &&
                                    index < parameterTypes->size();
             ++index)
          parameterEntry = parameterEntry ||
                           ((*parameterBindings)[index] == source->binding &&
                            (*parameterTypes)[index] == source->type);
      }
      const bool captureEntry =
          lambda && source->root == MirPlaceRootKind::Symbol &&
          source->capture != 0 &&
          source->capture <= lambda->captureTypes.size() &&
          source->capture <= lambda->captureSymbols.size() &&
          source->symbol == lambda->captureSymbols[source->capture - 1] &&
          source->type == lambda->captureTypes[source->capture - 1];
      if ((!receiverEntry && !parameterEntry && !captureEntry) ||
          !source->projections.empty())
        return verificationFailure(body, owner,
                                   "abstract stored dependency must name the "
                                   "exact receiver or reference formal");
    }
    const auto paths = findStoredCarrierReferences(program, carrierType);
    if (paths.empty()) {
      if (abstractEntry && (carrierType.kind == SemanticType::StringView ||
                            carrierType.kind == SemanticType::CString)) {
        // The exact reference-formal identity was checked above. Unlike a
        // nominal carrier, text's external owner has no source-visible field
        // path. Retain a separate read-only entry witness, never an invented
        // field, a mutable authority, or an unchecked carrier publication.
        if (loan.access != AccessMode::ReadOnly ||
            !loan.designatedFields.empty() ||
            !loan.additionalDependencies.empty() ||
            parameterBindings == nullptr ||
            !exactAbstractStoredCarrierPublications(
                program, body, *parameterBindings, loan, carrierType))
          return verificationFailure(
              body, owner,
              "text reference entry has an invalid owner dependency");
        continue;
      }
      if (abstractEntry || loan.carriers.empty())
        return verificationFailure(
            body, owner,
            "abstract stored dependency has no exact reference fields");
      continue;
    }
    if (loan.access != paths.front().reference.referenceAccess)
      return verificationFailure(body, owner,
                                 "stored entry dependency access does not "
                                 "match its exact reference field");
    std::vector<MirLoanDependency> additional;
    for (std::size_t index = 1; index < paths.size(); ++index) {
      additional.push_back({.designatedFields = paths[index].fields,
                            .access = paths[index].reference.referenceAccess});
    }
    if (loan.designatedFields != paths.front().fields ||
        loan.additionalDependencies != additional ||
        !loan.additionalSources().empty()) {
      return verificationFailure(body, owner,
                                 "stored entry loan does not preserve every "
                                 "exact reference-field path");
    }
    if (abstractEntry &&
        (parameterBindings == nullptr ||
         !exactAbstractStoredCarrierPublications(
             program, body, *parameterBindings, loan, carrierType)))
      return verificationFailure(body, owner,
                                 "abstract stored dependency carrier lacks an "
                                 "exact local value publication");
  }
  return {};
}

[[nodiscard]] std::optional<MirPlaceId>
nestedCarrierRoot(const MirProgram &program, const MirBody &body,
                  MirPlaceId placeId) {
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr || place->root != MirPlaceRootKind::Binding ||
      place->projections.empty() ||
      !findStoredCarrierReference(program, place->type)) {
    return std::nullopt;
  }
  const auto root = std::find_if(
      body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
        return candidate.root == MirPlaceRootKind::Binding &&
               candidate.binding == place->binding &&
               candidate.projections.empty();
      });
  if (root == body.places.end()) {
    return std::nullopt;
  }
  const auto paths = findStoredCarrierReferences(program, root->type);
  const auto carrier = std::find_if(
      paths.begin(), paths.end(), [&](const MirStoredCarrierReference &path) {
        return place->projections.size() < path.fields.size() &&
               std::equal(
                   place->projections.begin(), place->projections.end(),
                   path.fields.begin(),
                   [](const MirPlaceProjection &projection, SymbolId field) {
                     return projection.kind == MirProjectionKind::Field &&
                            projection.field == field;
                   });
      });
  if (carrier == paths.end()) {
    return std::nullopt;
  }
  SemanticType projected = root->type;
  for (std::size_t index = 0; index < place->projections.size(); ++index) {
    const MirPlaceProjection &projection = place->projections[index];
    if (projection.kind != MirProjectionKind::Field ||
        projection.field != carrier->fields[index]) {
      return std::nullopt;
    }
    const auto instance = std::find_if(program.classInstances().begin(),
                                       program.classInstances().end(),
                                       [&](const MirClassInstance &candidate) {
                                         return candidate.type == projected;
                                       });
    if (instance == program.classInstances().end()) {
      return std::nullopt;
    }
    const auto field = std::find_if(
        instance->declaredFields.begin(), instance->declaredFields.end(),
        [&](const MirClassFieldInfo &candidate) {
          return candidate.symbol == projection.field;
        });
    if (field == instance->declaredFields.end()) {
      return std::nullopt;
    }
    projected = field->type;
  }
  return projected == place->type ? std::optional<MirPlaceId>{root->id}
                                  : std::nullopt;
}

[[nodiscard]] bool
exactStoredCarrierValueTransfer(const MirProgram &program, const MirBody &body,
                                MirValueId valueId, const MirLoan &entry,
                                const SemanticType &carrierType,
                                const MirInstruction &transfer) {
  const MirValue *value = body.findValue(valueId);
  const MirBlock *definition =
      value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
  if (value == nullptr || value->info.type != carrierType ||
      definition == nullptr) {
    return false;
  }
  const auto producer = std::find_if(
      definition->instructions.begin(), definition->instructions.end(),
      [&](const MirInstruction &instruction) {
        return instruction.id == value->definition;
      });
  if (producer == definition->instructions.end() || !producer->result ||
      *producer->result != valueId ||
      (producer->kind != MirInstructionKind::Load &&
       producer->kind != MirInstructionKind::Move) ||
      producer->operands.size() != 1) {
    return false;
  }
  const auto transferPosition = std::find_if(
      definition->instructions.begin(), definition->instructions.end(),
      [&](const MirInstruction &instruction) {
        return instruction.id == transfer.id;
      });
  if (transferPosition == definition->instructions.end() ||
      producer >= transferPosition) {
    return false;
  }
  const MirOperand &source = producer->operands.front();
  const MirPlace *place = body.findPlace(source.place);
  return source.type == carrierType && place != nullptr &&
         storedCarrierValueOrigin(program, body, entry, *place, carrierType)
             .has_value();
}

// Replay constructor dependency composition from executable child producers.
// Caller/callee borrowFields alone are not proof: each recursive edge selects
// an actual Construct SSA result and exact enclosing formal operand.
[[nodiscard]] std::optional<BorrowedFieldOrigin> constructorFieldOrigin(
    const MirProgram &program, const MirConstructorInstance &instance,
    const MirStoredCarrierReference &path, std::size_t depth) {
  if (path.fields.empty() || depth > program.constructorInstances().size())
    return std::nullopt;
  const auto &body = instance.body;
  const auto initializer =
      std::find_if(instance.initializers.begin(), instance.initializers.end(),
                   [&](const auto &candidate) {
                     return candidate.field == path.fields.front();
                   });
  if (initializer == instance.initializers.end())
    return std::nullopt;
  const auto published =
      std::find_if(body.loans.begin(), body.loans.end(), [&](const auto &loan) {
        return loan.kind == MirLoanKind::Stored && loan.escapes &&
               loan.storedField == initializer->field;
      });
  if (published == body.loans.end())
    return std::nullopt;
  const auto formal = [&](MirPlaceId source) -> std::optional<std::size_t> {
    const auto canonical = borrowSourceForPlace(body, source, 0);
    const auto *place = canonical ? body.findPlace(*canonical) : nullptr;
    if (!place || place->root != MirPlaceRootKind::Binding ||
        std::any_of(place->projections.begin(), place->projections.end(),
                    [](const auto &projection) {
                      return projection.kind != MirProjectionKind::Dereference;
                    }))
      return std::nullopt;
    const auto found =
        std::find(instance.parameterBindings.begin(),
                  instance.parameterBindings.end(), place->binding);
    if (found == instance.parameterBindings.end())
      return std::nullopt;
    return std::distance(instance.parameterBindings.begin(), found);
  };
  BorrowedFieldOrigin result{.resultFields = path.fields,
                             .origin = BorrowOriginKind::Argument,
                             .access = path.reference.referenceAccess};
  if (initializer->storesReference) {
    const auto parameter = formal(published->source);
    if (!parameter || path.fields.size() != 1 ||
        *parameter >= instance.parameterTypes.size() ||
        instance.parameterTypes[*parameter] != initializer->targetType)
      return std::nullopt;
    result.parameter = *parameter;
    return result;
  }
  const MirInstruction *transfer = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.kind == MirInstructionKind::Borrow &&
          instruction.loan == published->id) {
        if (transfer)
          return std::nullopt;
        transfer = &instruction;
      }
  if (!transfer || transfer->operands.size() != 1 ||
      transfer->operands.front().kind != MirOperandKind::Loan)
    return std::nullopt;
  const auto &operand = transfer->operands.front();
  const auto *source = body.findLoan(operand.loan);
  if (!source || source->source != published->source ||
      source->access != published->access ||
      source->additionalDependencies != published->additionalDependencies ||
      operand.type != initializer->targetType || operand.place != 0)
    return std::nullopt;
  const std::vector<SymbolId> suffix(path.fields.begin() + 1,
                                     path.fields.end());
  if (source->entry) {
    const auto parameter = formal(source->source);
    if (!parameter || *parameter >= instance.parameterTypes.size() ||
        instance.parameterTypes[*parameter] != initializer->targetType ||
        !exactStoredCarrierValueTransfer(program, body, operand.value, *source,
                                         initializer->targetType, *transfer))
      return std::nullopt;
    result.parameter = *parameter;
    result.sourceFields = suffix;
    return result;
  }
  const auto *producer = definitionFor(body, operand.value);
  if (!producer || producer->kind != MirInstructionKind::Construct ||
      !producer->constructorTarget || producer->loan != source->id ||
      producer->result != operand.value ||
      producer->info.type != initializer->targetType ||
      source->producedBy != producer->hirValue)
    return std::nullopt;
  const auto *callee =
      program.findConstructorInstance(*producer->constructorTarget);
  if (!callee)
    return std::nullopt;
  const auto child = constructorFieldOrigin(
      program, *callee, {.fields = suffix, .reference = path.reference},
      depth + 1);
  if (!child || child->parameter >= producer->operands.size() ||
      child->parameter >= producer->parameterTypes.size())
    return std::nullopt;

  // A child formal can receive another constructed carrier, not just one
  // enclosing parameter. Replay that argument's selected field through its
  // executable producer before testing the final enclosing formal. Looking at
  // a carrier loan's primary source here would lose reordered sibling fields
  // and compare the referent's type with the intermediate carrier's type.
  const auto argumentOrigin =
      [&](const auto &self, const MirOperand &argument,
          const SemanticType &parameterType,
          const std::vector<SymbolId> &fields,
          std::size_t argumentDepth) -> std::optional<BorrowedFieldOrigin> {
    if (argumentDepth > body.values.size())
      return std::nullopt;
    const auto paths = findStoredCarrierReferences(program, parameterType);
    const bool exactPath =
        parameterType.kind == SemanticType::Reference
            ? fields.empty() && parameterType == path.reference
            : std::any_of(paths.begin(), paths.end(),
                          [&](const auto &candidate) {
                            return candidate.fields == fields &&
                                   candidate.reference == path.reference;
                          });
    if (!exactPath)
      return std::nullopt;
    const auto *definition = definitionFor(body, argument.value);
    if (definition && definition->kind == MirInstructionKind::CallInput) {
      if (definition->operands.size() != 1 ||
          definition->result != argument.value)
        return std::nullopt;
      return self(self, definition->operands.front(), parameterType, fields,
                  argumentDepth + 1);
    }
    const auto origin = borrowSourceForOperand(body, argument);
    const auto parameter = origin ? formal(*origin) : std::nullopt;
    if ((!definition || definition->kind != MirInstructionKind::Construct) &&
        parameter && *parameter < instance.parameterTypes.size() &&
        instance.parameterTypes[*parameter] == parameterType)
      return BorrowedFieldOrigin{.resultFields = fields,
                                 .origin = BorrowOriginKind::Argument,
                                 .parameter = *parameter,
                                 .sourceFields = fields,
                                 .access = path.reference.referenceAccess};

    if (!definition || definition->kind != MirInstructionKind::Construct ||
        !definition->constructorTarget || !definition->loan ||
        definition->result != argument.value ||
        definition->info.type != parameterType)
      return std::nullopt;
    const auto *retained = body.findLoan(*definition->loan);
    const auto *target =
        program.findConstructorInstance(*definition->constructorTarget);
    const auto *targetClass =
        target ? program.findClassInstance(target->owner) : nullptr;
    if (!retained || retained->kind != MirLoanKind::Stored || retained->entry ||
        retained->escapes || retained->producedBy != definition->hirValue ||
        !targetClass || targetClass->type != parameterType)
      return std::nullopt;
    const auto mapping = constructorFieldOrigin(
        program, *target, {.fields = fields, .reference = path.reference},
        depth + 1);
    if (!mapping || mapping->parameter >= definition->operands.size() ||
        mapping->parameter >= definition->parameterTypes.size())
      return std::nullopt;
    return self(self, definition->operands[mapping->parameter],
                definition->parameterTypes[mapping->parameter],
                mapping->sourceFields, argumentDepth + 1);
  };
  const auto origin = argumentOrigin(
      argumentOrigin, producer->operands[child->parameter],
      producer->parameterTypes[child->parameter], child->sourceFields, 0);
  if (!origin)
    return std::nullopt;
  result.parameter = origin->parameter;
  result.sourceFields = origin->sourceFields;
  return result;
}

[[nodiscard]] MirVerificationResult
verifyMirFunctionBorrowSummary(const MirProgram &program,
                               const MirFunctionInstance &instance) {
  const MirBody &body = instance.body;
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  for (const MirLoan &loan : body.loans) {
    for (const HirBindingId carrier : loan.carriers) {
      bindingLoans[carrier].push_back(loan.id);
    }
  }
  const auto invalidSummary = [&](std::string message) {
    return verificationFailure(body, instance.id, std::move(message));
  };
  if (instance.restoresReferenceResult) {
    std::unordered_set<HirFunctionInstanceId> visiting;
    if (!verifiedRestoreSummary(program, instance, visiting))
      return invalidSummary("restored reference summary lacks a complete typed "
                            "intrinsic/witness proof");
  } else if (!instance.restoredBorrowFields.empty()) {
    return invalidSummary(
        "ordinary reference summary carries unproved restored fields");
  }
  if (!instance.returnBorrowFields.empty()) {
    const auto paths =
        findStoredCarrierReferences(program, instance.returnType);
    const bool reference = instance.returnType.kind == SemanticType::Reference;
    if (instance.returnBorrowFields.size() != (reference ? 1 : paths.size()))
      return invalidSummary(
          "returned field summary does not cover the complete result layout");
    for (std::size_t index = 0; index < instance.returnBorrowFields.size();
         ++index) {
      const auto &mapping = instance.returnBorrowFields[index];
      if (mapping.resultFields !=
              (reference ? std::vector<SymbolId>{} : paths[index].fields) ||
          mapping.access != (reference
                                 ? instance.returnType.referenceAccess
                                 : paths[index].reference.referenceAccess) ||
          mapping.origin != instance.returnBorrowOrigin ||
          (mapping.origin != BorrowOriginKind::Receiver &&
           mapping.origin != BorrowOriginKind::Argument) ||
          (mapping.origin == BorrowOriginKind::Receiver &&
           mapping.parameter != 0) ||
          (mapping.origin == BorrowOriginKind::Argument &&
           mapping.parameter >= instance.parameterBindings.size()))
        return invalidSummary("returned field summary has an invalid result "
                              "path, permission, or input");
    }
  }
  std::optional<MirStoredCarrierReference> storedCarrierReference;
  const MirLoan *storedCarrierEntryLoan = nullptr;
  bool dynamicTextCarrier = false;
  bool abstractCarrierValue = false;
  switch (instance.returnBorrowOrigin) {
  case BorrowOriginKind::None:
    if (instance.returnBorrowParameter != 0 || instance.returnBorrowPlace) {
      return invalidSummary(
          "function without a return dependency has a nonzero origin index");
    }
    break;
  case BorrowOriginKind::Receiver:
    if (!instance.owner || instance.staticMember ||
        *instance.owner > program.classInstances().size() ||
        instance.returnBorrowParameter != 0 || instance.returnBorrowPlace) {
      return invalidSummary(
          "receiver return dependency requires a non-static class method");
    }
    if (instance.returnType.kind == SemanticType::Class) {
      const auto *receiver = program.findClassInstance(*instance.owner);
      storedCarrierReference =
          receiver == nullptr
              ? std::nullopt
              : findStoredCarrierReference(program, receiver->type);
      if (storedCarrierReference) {
        abstractCarrierValue = true;
        for (const auto &candidate : body.loans) {
          const auto *origin = body.findPlace(candidate.source);
          if (candidate.kind != MirLoanKind::Stored || !candidate.entry ||
              candidate.escapes || origin == nullptr ||
              origin->root != MirPlaceRootKind::This ||
              !origin->projections.empty())
            continue;
          if (storedCarrierEntryLoan != nullptr)
            return invalidSummary(
                "receiver carrier return has no unique stored entry");
          storedCarrierEntryLoan = &candidate;
        }
        if (storedCarrierEntryLoan == nullptr)
          return invalidSummary(
              "receiver carrier return has no exact stored entry");
      }
    }
    break;
  case BorrowOriginKind::Argument: {
    const std::optional<mir_detail::MirReturnOriginFormal> originFormal =
        mir_detail::returnOriginFormal(instance.parameterTypes,
                                       instance.parameterBindings,
                                       instance.returnBorrowParameter);
    if (!originFormal || originFormal->binding == 0) {
      return invalidSummary(
          "return dependency argument index is outside the formal parameters "
          "or their exact bindings");
    }
    if (instance.returnBorrowPlace) {
      return invalidSummary(
          "argument return dependency carries a global origin place");
    }
    {
      const SemanticType &parameter =
          instance.parameterTypes[instance.returnBorrowParameter];
      const bool abstractValueCarrier =
          parameter.kind == SemanticType::Reference &&
          parameter.arguments.size() == 1 &&
          instance.returnType.kind == SemanticType::Class &&
          parameter.arguments.front() == instance.returnType;
      abstractCarrierValue = abstractValueCarrier;
      storedCarrierReference = findStoredCarrierReference(
          program,
          abstractValueCarrier ? parameter.arguments.front() : parameter);
      const bool exactReference =
          parameter.kind == SemanticType::Reference &&
          parameter.arguments.size() == 1 &&
          parameter.referenceAccess == instance.returnBorrowAccess;
      const bool exactCarrier =
          storedCarrierReference &&
          storedCarrierReference->reference.referenceAccess ==
              instance.returnBorrowAccess;
      dynamicTextCarrier = (parameter.kind == SemanticType::StringView ||
                            parameter.kind == SemanticType::CString) &&
                           instance.returnType.kind == parameter.kind &&
                           instance.returnBorrowAccess == AccessMode::ReadOnly;
      if (!exactReference && !exactCarrier && !dynamicTextCarrier) {
        return invalidSummary(
            "argument return dependency does not identify one exact "
            "access-compatible reference, stored-reference carrier, or dynamic "
            "text-borrow parameter");
      }
      if (exactCarrier || dynamicTextCarrier) {
        std::optional<SemanticType> resultReferent;
        if (dynamicTextCarrier) {
          resultReferent = instance.returnType;
        } else if (instance.returnType.kind == SemanticType::Reference &&
                   instance.returnType.arguments.size() == 1) {
          resultReferent = instance.returnType.arguments.front();
        } else if (instance.returnType.kind == SemanticType::Expected &&
                   instance.returnType.arguments.size() == 2 &&
                   instance.returnType.arguments.front().kind ==
                       SemanticType::Reference &&
                   instance.returnType.arguments.front().referenceAccess ==
                       AccessMode::ReadOnly &&
                   instance.returnType.arguments.front().arguments.size() ==
                       1) {
          resultReferent =
              instance.returnType.arguments.front().arguments.front();
        } else if (const std::optional<SemanticType> returnedCarrier =
                       [&]() -> std::optional<SemanticType> {
                     const auto carrier = findStoredCarrierReference(
                         program, instance.returnType);
                     return carrier
                                ? std::optional<SemanticType>{carrier
                                                                  ->reference}
                                : std::nullopt;
                   }();
                   returnedCarrier && returnedCarrier->arguments.size() == 1) {
          resultReferent = returnedCarrier->arguments.front();
        }
        if (!resultReferent ||
            (!dynamicTextCarrier &&
             *resultReferent !=
                 storedCarrierReference->reference.arguments.front())) {
          return invalidSummary(
              "stored-carrier return dependency type does not match its "
              "exact external referent type");
        }

        const HirBindingId parameterBinding =
            mir_detail::returnOriginFormal(instance.parameterTypes,
                                           instance.parameterBindings,
                                           instance.returnBorrowParameter)
                ->binding;
        for (const MirLoan &loan : body.loans) {
          const MirPlace *source = body.findPlace(loan.source);
          const bool exactEntry =
              loan.kind == MirLoanKind::Stored && loan.entry && !loan.escapes &&
              loan.access == instance.returnBorrowAccess && source != nullptr &&
              source->root == MirPlaceRootKind::Binding &&
              source->projections.empty() &&
              source->binding == parameterBinding &&
              loan.designatedFields == (dynamicTextCarrier
                                            ? std::vector<SymbolId>{}
                                            : storedCarrierReference->fields) &&
              (abstractValueCarrier
                   ? std::find(loan.carriers.begin(), loan.carriers.end(),
                               parameterBinding) == loan.carriers.end()
                   : std::find(loan.carriers.begin(), loan.carriers.end(),
                               parameterBinding) != loan.carriers.end());
          if (!exactEntry) {
            continue;
          }
          if (storedCarrierEntryLoan != nullptr) {
            return invalidSummary(
                "stored-carrier return dependency has multiple entry loans");
          }
          storedCarrierEntryLoan = &loan;
        }
        if (storedCarrierEntryLoan == nullptr) {
          return invalidSummary(
              "stored-carrier return dependency has no exact entry loan");
        }
      }
    }
    break;
  }
  case BorrowOriginKind::Global:
    if ((instance.owner && !instance.staticMember) ||
        instance.returnBorrowParameter != 0 || !instance.returnBorrowPlace ||
        !instance.returnBorrowPlace->valid()) {
      return invalidSummary(
          "global return dependency requires a free or static function and "
          "one exact static storage place");
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
        return verificationFailure(
            body, instance.id,
            "return escapes a loan but the function has no return "
            "dependency summary",
            block.id);
      }
      continue;
    }
    if (!block.terminator.returnLoan) {
      // A dependency summary bounds the lifetime of dependent alternatives;
      // it does not give a static text literal a fictitious owner. Require
      // its exact literal producer rather than trusting an erased loan or a
      // cleared value-level borrowed-state flag.
      const auto *returned =
          block.terminator.value &&
                  block.terminator.value->kind == MirOperandKind::Value
              ? body.findValue(block.terminator.value->value)
              : nullptr;
      const auto *definition =
          returned ? linearDefinition(body, returned->id) : nullptr;
      const bool independentTextLiteral =
          instance.returnBorrowAccess == AccessMode::ReadOnly && returned &&
          !returned->info.traits.containsBorrowedState && definition &&
          definition->kind == MirInstructionKind::Compute &&
          definition->operation == MirOperation::Literal &&
          definition->literal &&
          ((instance.returnType.kind == SemanticType::StringView &&
            returned->info.type == instance.returnType &&
            std::holds_alternative<std::string>(*definition->literal)) ||
           (instance.returnType.kind == SemanticType::CString &&
            returned->info.type.kind == SemanticType::NullPtr &&
            std::holds_alternative<std::nullptr_t>(*definition->literal))) &&
          definition->result == returned->id &&
          definition->info.type == returned->info.type &&
          !definition->info.traits.containsBorrowedState && !definition->loan &&
          definition->operands.empty() && !definition->destination &&
          !definition->receiver && definition->lifecycle.empty() &&
          definition->definedFailure.empty() &&
          definition->localFailureSites.empty();
      // Semantics rejects storing a dynamic view in a field or fixed-array
      // element, so storage declared as the view itself holds an ownerless
      // view. An exact copy of it is lifetime independent like a literal. A
      // reference field ends in a dereference and stays owner dependent.
      const MirPlace *stored =
          definition && definition->kind == MirInstructionKind::Load &&
                  definition->operands.size() == 1 &&
                  definition->operands.front().kind == MirOperandKind::Copy
              ? body.findPlace(definition->operands.front().place)
              : nullptr;
      const bool independentStoredText =
          instance.returnBorrowAccess == AccessMode::ReadOnly && stored &&
          (instance.returnType.kind == SemanticType::StringView ||
           instance.returnType.kind == SemanticType::CString) &&
          !stored->projections.empty() &&
          (stored->projections.back().kind == MirProjectionKind::Field ||
           stored->projections.back().kind == MirProjectionKind::Index) &&
          stored->type == instance.returnType &&
          !stored->traits.containsBorrowedState &&
          returned->info.type == instance.returnType &&
          !returned->info.traits.containsBorrowedState &&
          definition->result == returned->id &&
          definition->info.type == returned->info.type &&
          definition->operands.front().type == instance.returnType &&
          !definition->info.traits.containsBorrowedState && !definition->loan &&
          !definition->destination && !definition->receiver &&
          definition->lifecycle.empty();
      const bool fallibleBorrowErrorReturn =
          instance.returnType.kind == SemanticType::Expected &&
          instance.returnType.arguments.size() == 2 &&
          instance.returnType.arguments.front().kind ==
              SemanticType::Reference &&
          instance.returnType.arguments.front().referenceAccess ==
              AccessMode::ReadOnly &&
          instance.returnType.arguments.front().arguments.size() == 1 &&
          block.terminator.value &&
          block.terminator.value->type.kind == SemanticType::Unexpected &&
          block.terminator.value->type.arguments.size() == 1 &&
          block.terminator.value->type.arguments.front() ==
              instance.returnType.arguments.back();
      if (fallibleBorrowErrorReturn || independentTextLiteral ||
          independentStoredText) {
        continue;
      }
      return verificationFailure(
          body, instance.id,
          "owner-dependent return is missing its escaping loan", block.id);
    }
    const MirLoan *loan = body.findLoan(*block.terminator.returnLoan);
    if (loan == nullptr || loan->kind != MirLoanKind::Return ||
        !loan->escapes || loan->access != instance.returnBorrowAccess) {
      return verificationFailure(
          body, instance.id,
          "owner-dependent return loan does not match the function "
          "summary",
          block.id);
    }
    const MirLoan *verifiedValueEntry = nullptr;
    const MirPlace *verifiedValueSource = nullptr;
    const std::optional<MirPlaceId> canonicalSource =
        borrowSourceForPlace(body, loan->source, 0);
    const MirPlace *source =
        canonicalSource ? body.findPlace(*canonicalSource) : nullptr;
    if (source == nullptr) {
      return verificationFailure(
          body, instance.id,
          "owner-dependent return loan has no source identity", block.id);
    }
    if (instance.returnType.kind == SemanticType::Reference &&
        instance.returnType.arguments.size() == 1 &&
        findStoredCarrierReference(program,
                                   instance.returnType.arguments.front())) {
      const auto physical =
          canonicalBorrowOrigin(body, loan->source, bindingLoans, true);
      const auto *dependency = body.findLoan(loan->storedDependency);
      const auto *dependencySource =
          dependency == nullptr ? nullptr : body.findPlace(dependency->source);
      const bool exactReceiver =
          instance.returnBorrowOrigin == BorrowOriginKind::Receiver &&
          physical.root == MirPlaceRootKind::This &&
          dependencySource != nullptr &&
          dependencySource->root == MirPlaceRootKind::This;
      const bool exactParameter =
          instance.returnBorrowOrigin == BorrowOriginKind::Argument &&
          physical.root == MirPlaceRootKind::Binding &&
          dependencySource != nullptr &&
          dependencySource->root == MirPlaceRootKind::Binding &&
          [&] {
            const auto formal = mir_detail::returnOriginFormal(
                instance.parameterTypes, instance.parameterBindings,
                instance.returnBorrowParameter);
            return formal && physical.binding == formal->binding &&
                   dependencySource->type.kind == SemanticType::Reference;
          }() &&
          dependencySource->binding == physical.binding;
      const auto *restoredAuthority =
          mir_detail::restoredBorrowAuthority(body, *loan);
      const bool exactRestoredParameter =
          instance.restoresReferenceResult && exactParameter && dependency &&
          dependency->kind == MirLoanKind::Parameter &&
          dependency->access == instance.returnBorrowAccess &&
          restoredAuthority &&
          verifiedRestoreCall(program, body, *restoredAuthority);
      if (physical.ambiguous || dependency == nullptr || !dependency->entry ||
          (dependency->kind != MirLoanKind::Stored &&
           !exactRestoredParameter) ||
          dependency->parent != 0 || dependency->storedDependency != 0 ||
          (!exactReceiver && !exactParameter))
        return verificationFailure(
            body, instance.id,
            "returned carrier reference does not export its exact caller-owned "
            "shell and stored authority",
            block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Receiver &&
        source->root != MirPlaceRootKind::This) {
      return verificationFailure(
          body, instance.id,
          "receiver-dependent return does not originate from this", block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Argument &&
        (source->root != MirPlaceRootKind::Binding || [&] {
          const auto formal = mir_detail::returnOriginFormal(
              instance.parameterTypes, instance.parameterBindings,
              instance.returnBorrowParameter);
          return !formal || (instance.returnBorrowParameters.empty() &&
                             source->binding != formal->binding);
        }())) {
      return verificationFailure(
          body, instance.id,
          "argument-dependent return does not originate from the "
          "summarized formal parameter",
          block.id);
    }
    if (!instance.returnBorrowParameters.empty()) {
      std::unordered_set<HirBindingId> expected;
      for (const auto parameter : instance.returnBorrowParameters) {
        if (parameter >= instance.parameterBindings.size()) {
          return invalidSummary(
              "composed return origin exceeds the formal parameters");
        }
        expected.insert(instance.parameterBindings[parameter]);
      }
      std::unordered_set<HirBindingId> actual;
      auto sources = loan->additionalSources();
      sources.insert(sources.begin(), loan->source);
      for (const auto id : sources) {
        const auto canonical = borrowSourceForPlace(body, id, 0);
        const auto *place = canonical ? body.findPlace(*canonical) : nullptr;
        if (place == nullptr || place->root != MirPlaceRootKind::Binding) {
          return invalidSummary(
              "composed return contains an unproved owner origin");
        }
        actual.insert(place->binding);
      }
      if (actual != expected) {
        return invalidSummary(
            "composed return does not preserve its full formal dependency set");
      }
    }
    // Constructor/call-produced carriers already have an independently
    // checked producer contract. Merely seeding a receiver's abstract entry
    // does not replace those loans with a copy-publication protocol. Apply the
    // latter only when the Return publication actually consumes that entry.
    const bool consumesAbstractEntry =
        ((!storedCarrierReference ||
          (instance.returnType.kind == SemanticType::Reference ||
           instance.returnType.kind == SemanticType::Expected)) &&
         !abstractCarrierValue) ||
        (storedCarrierEntryLoan != nullptr &&
         std::any_of(body.blocks.begin(), body.blocks.end(),
                     [&](const MirBlock &candidate) {
                       return std::any_of(
                           candidate.instructions.begin(),
                           candidate.instructions.end(),
                           [&](const MirInstruction &instruction) {
                             return instruction.kind ==
                                        MirInstructionKind::Borrow &&
                                    instruction.loan == loan->id &&
                                    instruction.operands.size() == 1 &&
                                    instruction.operands.front().kind ==
                                        MirOperandKind::Loan &&
                                    instruction.operands.front().loan ==
                                        storedCarrierEntryLoan->id;
                           });
                     }));
    if ((storedCarrierReference || dynamicTextCarrier) &&
        consumesAbstractEntry && instance.returnBorrowParameters.empty()) {
      const bool fallibleReference =
          instance.returnType.kind == SemanticType::Expected &&
          instance.returnType.arguments.size() == 2 &&
          instance.returnType.arguments.front().kind ==
              SemanticType::Reference &&
          instance.returnType.arguments.front().referenceAccess ==
              AccessMode::ReadOnly &&
          instance.returnType.arguments.front().arguments.size() == 1;
      const SemanticType expectedPayload =
          instance.returnType.kind == SemanticType::Reference
              ? instance.returnType.arguments.front()
          : fallibleReference
              ? instance.returnType.arguments.front().arguments.front()
              : instance.returnType;
      const MirInstruction *transfer = nullptr;
      for (const MirBlock &candidateBlock : body.blocks) {
        for (const MirInstruction &instruction : candidateBlock.instructions) {
          if (instruction.loan != loan->id ||
              instruction.kind == MirInstructionKind::EndBorrow ||
              instruction.kind == MirInstructionKind::ActivateBorrow) {
            continue;
          }
          if (transfer != nullptr) {
            return verificationFailure(
                body, instance.id,
                "stored-carrier return loan has multiple transfer "
                "producers",
                block.id);
          }
          transfer = &instruction;
        }
      }
      const bool exactTransfer =
          storedCarrierEntryLoan != nullptr && !loan->entry &&
          storedCarrierEntryLoan->access == instance.returnBorrowAccess &&
          loan->source == storedCarrierEntryLoan->source && loan->parent == 0 &&
          loan->additionalDependencies ==
              storedCarrierEntryLoan->additionalDependencies &&
          transfer != nullptr && transfer->kind == MirInstructionKind::Borrow &&
          transfer->operands.size() == 1 &&
          transfer->operands.front().kind == MirOperandKind::Loan &&
          transfer->operands.front().loan == storedCarrierEntryLoan->id &&
          transfer->operands.front().type == expectedPayload &&
          transfer->info.type == expectedPayload;
      if (!exactTransfer) {
        return verificationFailure(
            body, instance.id,
            "stored-carrier return loan does not transfer its "
            "exact entry dependency and payload type",
            block.id);
      }
      if (!block.terminator.value ||
          block.terminator.value->type != expectedPayload) {
        return verificationFailure(
            body, instance.id,
            "stored-carrier return value does not match its exact "
            "dependency payload type",
            block.id);
      }
      if (instance.returnType.kind == SemanticType::Reference ||
          fallibleReference) {
        const MirOperand &published = transfer->operands.front();
        const MirPlace *runtime = body.findPlace(published.place);
        const bool exactDesignatedReferent =
            published.value == 0 && runtime != nullptr &&
            runtime->root == MirPlaceRootKind::Binding &&
            std::find(storedCarrierEntryLoan->carriers.begin(),
                      storedCarrierEntryLoan->carriers.end(),
                      runtime->binding) !=
                storedCarrierEntryLoan->carriers.end() &&
            runtime->projections.size() ==
                storedCarrierEntryLoan->designatedFields.size() + 1 &&
            std::equal(
                storedCarrierEntryLoan->designatedFields.begin(),
                storedCarrierEntryLoan->designatedFields.end(),
                runtime->projections.begin(),
                [](SymbolId field, const MirPlaceProjection &projection) {
                  return projection.kind == MirProjectionKind::Field &&
                         projection.field == field;
                }) &&
            runtime->projections.back().kind ==
                MirProjectionKind::Dereference &&
            runtime->type == expectedPayload;
        if (!exactDesignatedReferent ||
            block.terminator.value->kind != MirOperandKind::Loan ||
            block.terminator.value->loan != loan->id ||
            block.terminator.value->place != published.place) {
          return verificationFailure(
              body, instance.id,
              "stored-carrier reference return does not preserve "
              "its exact designated stored referent place",
              block.id);
        }
        verifiedValueSource = runtime;
      } else {
        const MirOperand &published = transfer->operands.front();
        const bool exactCarrier =
            published.place == 0 && published.value != 0 &&
            block.terminator.value->kind == MirOperandKind::Value &&
            block.terminator.value->value == published.value &&
            exactStoredCarrierValueTransfer(program, body, published.value,
                                            *storedCarrierEntryLoan,
                                            instance.returnType, *transfer);
        if (!exactCarrier) {
          return verificationFailure(
              body, instance.id,
              "stored-carrier value return does not transfer one "
              "exact runtime carrier of its entry dependency",
              block.id);
        }
        verifiedValueEntry = storedCarrierEntryLoan;
        const auto *producer = definitionFor(body, published.value);
        verifiedValueSource = body.findPlace(producer->operands.front().place);
      }
    }
    if (!instance.returnBorrowFields.empty()) {
      std::vector<MirLoanDependency> actual{
          {.source = loan->source,
           .designatedFields = loan->designatedFields,
           .access = loan->access}};
      actual.insert(actual.end(), loan->additionalDependencies.begin(),
                    loan->additionalDependencies.end());
      if (actual.size() != instance.returnBorrowFields.size())
        return invalidSummary(
            "return publication loses a reference-bearing result field");
      for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto &mapping = instance.returnBorrowFields[index];
        const auto &dependency = actual[index];
        // An abstract entry names fields, not caller-local owner places.
        // Use those paths only after independently proving that the returned
        // SSA value transfers this exact entry's complete carrier shell.
        const auto origin = canonicalBorrowOrigin(
            body,
            verifiedValueSource ? verifiedValueSource->id : dependency.source,
            bindingLoans);
        const bool root =
            mapping.origin == BorrowOriginKind::Receiver
                ? origin.root == MirPlaceRootKind::This
                : origin.root == MirPlaceRootKind::Binding &&
                      origin.binding ==
                          instance.parameterBindings[mapping.parameter];
        std::vector<SymbolId> fields;
        bool validPath = true;
        for (const auto &projection : origin.projections) {
          if (projection.kind == MirProjectionKind::Field)
            fields.push_back(projection.field);
          else if (projection.kind != MirProjectionKind::Dereference)
            validPath = false;
        }
        if (verifiedValueEntry)
          fields.insert(fields.end(), dependency.designatedFields.begin(),
                        dependency.designatedFields.end());
        if (!verifiedValueEntry && fields.empty() &&
            instance.returnType.kind == SemanticType::Class &&
            mapping.origin == BorrowOriginKind::Argument && root) {
          const auto paths = findStoredCarrierReferences(
              program, instance.parameterTypes[mapping.parameter]);
          if (paths.size() == 1 && paths.front().fields == mapping.sourceFields)
            fields = paths.front().fields;
        }
        if (origin.ambiguous || !root || !validPath ||
            fields != mapping.sourceFields ||
            dependency.designatedFields != mapping.resultFields ||
            dependency.access != mapping.access)
          return invalidSummary("return publication disagrees with its exact "
                                "field-to-source contract");
      }
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Global) {
      const CanonicalPlace actual =
          canonicalBorrowOrigin(body, loan->source, bindingLoans);
      if (!instance.returnBorrowPlace ||
          !sameGlobalBorrowPlace(*instance.returnBorrowPlace, actual)) {
        return verificationFailure(
            body, instance.id,
            "global-dependent return does not originate from the "
            "summarized static storage place",
            block.id);
      }
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
verifyMirConstructorBorrowSummary(const MirProgram &program,
                                  const MirConstructorInstance &instance) {
  const MirBody &body = instance.body;
  const auto invalidSummary = [&](std::string message) {
    return verificationFailure(body, instance.id, std::move(message));
  };
  std::vector<SemanticType> bindingTypes;
  std::unordered_set<HirBindingId> parameterBindings;
  for (const HirBindingId binding : instance.parameterBindings) {
    const auto place = std::find_if(
        body.places.begin(), body.places.end(), [&](const MirPlace &candidate) {
          return candidate.root == MirPlaceRootKind::Binding &&
                 candidate.binding == binding && candidate.projections.empty();
        });
    if (binding == 0 || !parameterBindings.insert(binding).second ||
        place == body.places.end()) {
      return invalidSummary("constructor parameter has no exact binding place");
    }
    bindingTypes.push_back(place->type);
  }
  const auto exactPackShape = [](const std::vector<SemanticType> &types) {
    for (std::size_t index = 0; index < types.size(); ++index) {
      const SemanticType &type = types[index];
      if (type.kind == SemanticType::TypePack &&
          (index + 1 != types.size() || !type.concretePack ||
           std::any_of(type.arguments.begin(), type.arguments.end(),
                       [](const SemanticType &element) {
                         return element.kind == SemanticType::TypePack;
                       }))) {
        return false;
      }
    }
    return true;
  };
  if (!exactPackShape(bindingTypes) ||
      !exactPackShape(instance.parameterTypes) ||
      !exactParameterRoles(bindingTypes, instance.parameterTypes)) {
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
  case BorrowOriginKind::Argument: {
    if (instance.borrowParameter >= instance.parameterTypes.size() ||
        instance.borrowParameter >= instance.parameterBindings.size()) {
      return invalidSummary(
          "constructor borrow dependency is outside the formal parameters");
    }
    const SemanticType &borrowParameter =
        instance.parameterTypes[instance.borrowParameter];
    const MirClassInstance *constructedOwner =
        program.findClassInstance(instance.owner);
    const bool mutexGuardInteriorBorrow = [&] {
      if (constructedOwner == nullptr ||
          !constructedOwner->mutexGuardBorrowCarrier ||
          instance.borrowAccess != AccessMode::Mutable ||
          borrowParameter.kind != SemanticType::Reference ||
          borrowParameter.referenceAccess != AccessMode::ReadOnly ||
          borrowParameter.arguments.size() != 1) {
        return false;
      }
      const auto storage = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const MirClassInstance &candidate) {
            return candidate.type == borrowParameter.arguments.front();
          });
      return storage != program.classInstances().end() &&
             storage->compilerCapability ==
                 CompilerCapabilityTypeKind::MutexStorage;
    }();
    const auto carrierParameter =
        findStoredCarrierReference(program, borrowParameter);
    if (instance.parameterBindings[instance.borrowParameter] == 0 ||
        !((borrowParameter.kind == SemanticType::Reference &&
           (borrowParameter.referenceAccess == instance.borrowAccess ||
            mutexGuardInteriorBorrow)) ||
          (carrierParameter && carrierParameter->reference.referenceAccess ==
                                   instance.borrowAccess))) {
      return invalidSummary(
          "constructor borrow dependency does not identify an exact "
          "reference or single-owner carrier formal parameter");
    }
    break;
  }
  case BorrowOriginKind::Global:
    return invalidSummary("constructor result cannot depend on global storage");
  }

  std::unordered_set<MirLoanId> initializerLoans;
  std::unordered_set<std::size_t> dependencyParameters;
  std::size_t storedInitializers = 0;
  for (const MirConstructorInitializer &initializer : instance.initializers) {
    const auto nestedCarrier =
        findStoredCarrierReference(program, initializer.targetType);
    if (!initializer.storesReference && !nestedCarrier) {
      continue;
    }
    ++storedInitializers;
    // A lifecycle-bound synchronization guard retains exclusive authority
    // granted by its trusted storage capability, not by the shared reference
    // used to address that storage. Ordinary stored fields use their declared
    // reference permission. This is the same capability boundary checked for
    // the constructor's summarized formal above.
    const auto *constructedOwner = program.findClassInstance(instance.owner);
    const bool synchronizedInterior =
        constructedOwner != nullptr &&
        constructedOwner->mutexGuardBorrowCarrier &&
        initializer.storesReference &&
        initializer.targetType.kind == SemanticType::Reference &&
        initializer.targetType.referenceAccess == AccessMode::ReadOnly &&
        initializer.targetType.arguments.size() == 1 &&
        std::any_of(program.classInstances().begin(),
                    program.classInstances().end(), [&](const auto &type) {
                      return type.type ==
                                 initializer.targetType.arguments.front() &&
                             type.compilerCapability ==
                                 CompilerCapabilityTypeKind::MutexStorage;
                    });
    if (instance.borrowOrigin != BorrowOriginKind::Argument ||
        initializer.field == 0 || initializer.arguments.size() != 1 ||
        initializer.borrowAccess !=
            (synchronizedInterior ? AccessMode::Mutable
             : initializer.storesReference
                 ? initializer.targetType.referenceAccess
                 : nestedCarrier->reference.referenceAccess)) {
      return invalidSummary(
          "stored-reference initializer does not match the constructor "
          "borrow summary");
    }
    const auto matching = std::find_if(
        body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
          return loan.kind == MirLoanKind::Stored && loan.escapes &&
                 loan.storedField == initializer.field &&
                 (loan.producedBy == initializer.arguments.front() ||
                  (loan.entry && loan.producedBy == 0));
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
    const auto parameter =
        source == nullptr
            ? instance.parameterBindings.end()
            : std::find(instance.parameterBindings.begin(),
                        instance.parameterBindings.end(), source->binding);
    const std::size_t parameterIndex = static_cast<std::size_t>(
        std::distance(instance.parameterBindings.begin(), parameter));
    const bool summarized =
        instance.borrowParameters.empty()
            ? parameterIndex == instance.borrowParameter
            : std::find(instance.borrowParameters.begin(),
                        instance.borrowParameters.end(),
                        parameterIndex) != instance.borrowParameters.end();
    if (source == nullptr || source->root != MirPlaceRootKind::Binding ||
        !summarized) {
      return invalidSummary(
          "stored-reference initializer loan does not originate from the "
          "summarized formal parameter");
    }
    dependencyParameters.insert(parameterIndex);
    if (initializer.storesReference &&
        instance.parameterTypes[parameterIndex] != initializer.targetType &&
        !referenceInitializerCall(program, instance, initializer))
      return invalidSummary(
          "stored-reference initializer does not bind its exact reference "
          "parameter type and permission");
    if (!initializerLoans.insert(matching->id).second) {
      return invalidSummary(
          "multiple stored-reference initializers reuse one escaping loan");
    }
    if (nestedCarrier) {
      const MirClassInstance *owner = program.findClassInstance(instance.owner);
      const auto ownerPaths =
          owner == nullptr ? std::vector<MirStoredCarrierReference>{}
                           : findStoredCarrierReferences(program, owner->type);
      std::vector<SymbolId> expectedPath = nestedCarrier->fields;
      expectedPath.insert(expectedPath.begin(), initializer.field);
      if (std::none_of(ownerPaths.begin(), ownerPaths.end(),
                       [&](const MirStoredCarrierReference &path) {
                         return path.fields == expectedPath;
                       })) {
        return invalidSummary(
            "nested carrier initializer does not preserve its exact nominal "
            "field path and carrier parameter type");
      }
      const MirInstruction *transfer = nullptr;
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          if (instruction.loan == matching->id &&
              instruction.kind != MirInstructionKind::EndBorrow &&
              instruction.kind != MirInstructionKind::ActivateBorrow) {
            if (transfer != nullptr) {
              return invalidSummary(
                  "nested carrier publication has multiple producers");
            }
            transfer = &instruction;
          }
        }
      }
      if (transfer == nullptr || transfer->kind != MirInstructionKind::Borrow ||
          transfer->operands.size() != 1 ||
          transfer->operands.front().kind != MirOperandKind::Loan) {
        return invalidSummary(
            "nested carrier publication lacks an exact entry-loan transfer");
      }
      const MirOperand &value = transfer->operands.front();
      const MirLoan *entry = body.findLoan(value.loan);
      const auto *construction = definitionFor(body, value.value);
      const bool constructed =
          entry && !entry->entry && construction &&
          construction->kind == MirInstructionKind::Construct &&
          construction->constructorTarget && construction->loan == entry->id &&
          construction->result == value.value &&
          construction->info.type == initializer.targetType &&
          entry->producedBy == construction->hirValue;
      if (entry == nullptr || entry->kind != MirLoanKind::Stored ||
          (!entry->entry && !constructed) || entry->escapes ||
          entry->source != matching->source ||
          entry->access != initializer.borrowAccess ||
          (!constructed && entry->designatedFields != nestedCarrier->fields) ||
          matching->additionalDependencies != entry->additionalDependencies ||
          value.place != 0 || value.type != initializer.targetType ||
          (!constructed && !exactStoredCarrierValueTransfer(
                               program, body, value.value, *entry,
                               initializer.targetType, *transfer))) {
        return invalidSummary(
            "nested carrier publication does not transfer its exact parameter "
            "value and owner dependency");
      }
      for (const auto &path : ownerPaths) {
        if (path.fields.empty() || path.fields.front() != initializer.field)
          continue;
        const auto origin = constructorFieldOrigin(program, instance, path);
        if (!origin || origin->parameter >= instance.parameterTypes.size())
          return invalidSummary("nested constructor publication has no exact "
                                "executable field-to-formal origin");
        dependencyParameters.insert(origin->parameter);
      }
    }
  }
  if ((instance.borrowOrigin == BorrowOriginKind::Argument) !=
          (storedInitializers != 0) ||
      (!instance.borrowParameters.empty() &&
       (dependencyParameters.size() != instance.borrowParameters.size() ||
        std::any_of(instance.borrowParameters.begin(),
                    instance.borrowParameters.end(),
                    [&](std::size_t parameter) {
                      return !dependencyParameters.contains(parameter);
                    })))) {
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
  appendVerification(
      result, verifyMirCallableMetadata(program, program.module(), 0, nullptr));
  appendVerification(result,
                     verifyMirBorrowProducers(program, program.module(), 0));
  for (const MirClassInstance &instance : program.classInstances()) {
    appendVerification(
        result, verifyMirCallableMetadata(program, instance.fieldInitializers,
                                          instance.id));
    appendVerification(
        result, verifyMirBorrowProducers(program, instance.fieldInitializers,
                                         instance.id));
    appendVerification(
        result, verifyMirCallableMetadata(
                    program, instance.staticFieldInitializers, instance.id));
    appendVerification(
        result, verifyMirBorrowProducers(
                    program, instance.staticFieldInitializers, instance.id));
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    appendVerification(result,
                       verifyMirCallableMetadata(program, instance.body,
                                                 instance.id, &instance));
    appendVerification(
        result, verifyMirBorrowProducers(program, instance.body, instance.id));
    appendVerification(result,
                       verifyMirFunctionBorrowSummary(program, instance));
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    appendVerification(
        result, verifyMirCallableMetadata(program, instance.body, instance.id));
    appendVerification(
        result, verifyMirBorrowProducers(program, instance.body, instance.id));
    appendVerification(result,
                       verifyMirConstructorBorrowSummary(program, instance));
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    appendVerification(
        result, verifyMirCallableMetadata(program, instance.body, instance.id));
    appendVerification(
        result, verifyMirBorrowProducers(program, instance.body, instance.id));
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    appendVerification(
        result, verifyMirCallableMetadata(program, instance.body, instance.id));
    appendVerification(
        result, verifyMirBorrowProducers(program, instance.body, instance.id));
  }
  return result;
}

[[nodiscard]] MirVerificationResult verifyMirDropTargets(
    const MirProgram &program, const MirBody &body, std::size_t owner,
    const std::function<std::optional<bool>(const SemanticType &)>
        &typeRequiresActiveCleanup) {
  for (const MirDropObligation &obligation : body.dropObligations) {
    std::optional<bool> exactActiveCleanup;
    if (obligation.dropType.type.kind == SemanticType::Lambda &&
        obligation.dropType.lambdaInstance) {
      const MirLambdaInstance *lambda =
          program.findLambda(*obligation.dropType.lambdaInstance);
      if (lambda != nullptr &&
          lambda->captureTypes.size() ==
              lambda->captureRequiresActiveCleanup.size()) {
        exactActiveCleanup =
            std::any_of(lambda->captureRequiresActiveCleanup.begin(),
                        lambda->captureRequiresActiveCleanup.end(),
                        [](bool active) { return active; });
      }
    } else {
      exactActiveCleanup = typeRequiresActiveCleanup(obligation.dropType.type);
    }
    if (!exactActiveCleanup ||
        *exactActiveCleanup != obligation.dropType.requiresActiveCleanup) {
      return verificationFailure(
          body, owner,
          "drop obligation " + std::to_string(obligation.id) +
              " does not name its exact active-cleanup descriptor");
    }
    const auto uniqueCleanup =
        obligation.kind == MirDropObligationKind::ObjectAllocation
            ? std::nullopt
            : mirUniqueCleanupType(program, obligation.dropType.type);
    if (uniqueCleanup) {
      if (obligation.dropType != *uniqueCleanup) {
        return verificationFailure(
            body, owner,
            "unique-owner drop does not name its exact pointee cleanup");
      }
      continue;
    }
    if (obligation.dropType.uniquePointeeClass ||
        obligation.dropType.polymorphicCleanup ||
        obligation.dropType.recursiveCleanup ||
        !obligation.dropType.aggregate.empty() ||
        obligation.dropType.uniqueOwnerField != 0 ||
        obligation.dropType.uniqueOwnerBase ||
        obligation.dropType.captureIndex ||
        obligation.dropType.payloadVariant ||
        obligation.dropType.payloadField) {
      return verificationFailure(
          body, owner, "drop carries an unsupported unique cleanup path");
    }
    if (obligation.dropType.type.kind == SemanticType::Array) {
      const MirFixedArrayDropSchedule *schedule =
          obligation.dropType.fixedArray ? &*obligation.dropType.fixedArray
                                         : nullptr;
      SemanticType leaf = obligation.dropType.type;
      std::vector<std::size_t> extents;
      while (leaf.kind == SemanticType::Array && leaf.arguments.size() == 1 &&
             leaf.arrayLengthParameterId == 0 && leaf.arrayLength != 0) {
        extents.push_back(leaf.arrayLength);
        SemanticType element = leaf.arguments.front();
        leaf = std::move(element);
      }
      const MirClassInstance *leafClass =
          schedule != nullptr && schedule->leafClassInstance
              ? program.findClassInstance(*schedule->leafClassInstance)
              : nullptr;
      const bool exactSpecializationLeaf =
          leaf.kind == SemanticType::Class && leafClass != nullptr &&
          leafClass->type.kind == SemanticType::Class &&
          leafClass->type.classId == leaf.classId &&
          leafClass->type.arguments.empty() &&
          leafClass->type.valueArguments.empty();
      const bool exactLeafClass =
          leaf.kind != SemanticType::Class
              ? leafClass == nullptr && !obligation.dropType.destructor
              : leafClass != nullptr &&
                    (leafClass->type == leaf || exactSpecializationLeaf) &&
                    leafClass->destructor == obligation.dropType.destructor;
      if (obligation.dropType.classInstance ||
          obligation.dropType.lambdaInstance) {
        return verificationFailure(
            body, owner,
            "fixed-array drop obligation names direct class or "
            "lambda cleanup metadata");
      }
      if (schedule == nullptr || schedule->extents != extents ||
          schedule->leafType != leaf) {
        return verificationFailure(
            body, owner,
            "fixed-array drop obligation does not retain its exact "
            "concrete shape and leaf type");
      }
      if (leaf.kind == SemanticType::Class && leafClass == nullptr) {
        return verificationFailure(
            body, owner,
            "fixed-array drop obligation does not retain its exact "
            "leaf class instance");
      }
      if (leaf.kind == SemanticType::Class && leafClass->type != leaf &&
          !exactSpecializationLeaf) {
        return verificationFailure(
            body, owner,
            "fixed-array drop obligation leaf class instance has "
            "a different concrete type");
      }
      if (!exactLeafClass) {
        return verificationFailure(
            body, owner,
            "fixed-array drop obligation does not retain its exact "
            "leaf destructor identity");
      }
      continue;
    }
    if (obligation.dropType.type.kind == SemanticType::Lambda) {
      const MirLambdaInstance *lambda =
          obligation.dropType.lambdaInstance
              ? program.findLambda(*obligation.dropType.lambdaInstance)
              : nullptr;
      if (lambda == nullptr || lambda->type != obligation.dropType.type ||
          obligation.dropType.classInstance || obligation.dropType.destructor ||
          obligation.dropType.fixedArray) {
        return verificationFailure(
            body, owner,
            "lambda drop obligation does not name its exact "
            "concrete cleanup descriptor");
      }
      continue;
    }
    if (obligation.dropType.type.kind != SemanticType::Class) {
      if (obligation.dropType.classInstance ||
          obligation.dropType.lambdaInstance ||
          obligation.dropType.destructor || obligation.dropType.fixedArray) {
        return verificationFailure(
            body, owner,
            "non-class drop obligation names class cleanup metadata");
      }
      continue;
    }
    const MirClassInstance *classInstance =
        obligation.dropType.classInstance
            ? program.findClassInstance(*obligation.dropType.classInstance)
            : nullptr;
    if (classInstance == nullptr || obligation.dropType.lambdaInstance ||
        obligation.dropType.fixedArray ||
        classInstance->type != obligation.dropType.type ||
        classInstance->destructor != obligation.dropType.destructor) {
      return verificationFailure(
          body, owner,
          "drop obligation " + std::to_string(obligation.id) +
              " does not name the exact class cleanup descriptor");
    }
    if (obligation.dropType.destructor) {
      const MirDestructorInstance *destructor =
          program.findDestructorInstance(*obligation.dropType.destructor);
      if (destructor == nullptr || destructor->owner != classInstance->id) {
        return verificationFailure(
            body, owner,
            "drop obligation names a destructor for another class");
      }
    }
  }
  std::optional<MirInvokeContinuations> continuations;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop) {
        continue;
      }
      const MirDropObligation *obligation =
          body.findDropObligation(instruction.lifecycle.front().source);
      const bool mayRaise =
          obligation != nullptr &&
          mirDropMayRaiseDefinedFailure(program, obligation->dropType);
      if (mayRaise && !continuations)
        continuations = mirInvokeContinuations(body);
      const FailurePropagationKind expected =
          mayRaise &&
                  !mirDropFollowsDefiniteMove(body, *continuations, block,
                                              obligation->id, instruction.id)
              ? FailurePropagationKind::Destructor
              : FailurePropagationKind::None;
      if (obligation == nullptr ||
          instruction.definedFailure.propagation != expected ||
          !instruction.definedFailure.localOrigins.empty()) {
        return verificationFailure(
            body, owner,
            "drop propagation does not match its exact destructor "
            "failure summary",
            block.id, instruction.id);
      }
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult
verifyMirPayloadEnums(const MirProgram &program) {
  MirVerificationResult result;
  std::vector<SemanticType> declarations;
  const auto typeRequiresActiveCleanup =
      [&](const SemanticType &type, const auto &query) -> std::optional<bool> {
    switch (type.kind) {
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
      return true;
    case SemanticType::Array:
    case SemanticType::Unexpected:
      return type.arguments.size() == 1 ? query(type.arguments.front(), query)
                                        : std::nullopt;
    case SemanticType::Expected:
      for (const SemanticType &argument : type.arguments) {
        if (argument == SemanticType::Void) {
          continue;
        }
        const std::optional<bool> active = query(argument, query);
        if (!active) {
          return std::nullopt;
        }
        if (*active) {
          return true;
        }
      }
      return false;
    case SemanticType::Class: {
      const auto found = std::find_if(program.classInstances().begin(),
                                      program.classInstances().end(),
                                      [&](const MirClassInstance &candidate) {
                                        return candidate.type == type;
                                      });
      return found == program.classInstances().end()
                 ? std::nullopt
                 : std::optional<bool>{found->requiresActiveCleanup};
    }
    case SemanticType::Enum: {
      const MirPayloadEnum *enumeration = program.findPayloadEnum(type);
      return enumeration == nullptr
                 ? std::optional<bool>{false}
                 : std::optional<bool>{enumeration->requiresActiveCleanup};
    }
    case SemanticType::Lambda: {
      const MirLambdaInstance *lambda = program.findLambdaForType(type);
      if (lambda == nullptr ||
          lambda->captureTypes.size() !=
              lambda->captureRequiresActiveCleanup.size()) {
        return std::nullopt;
      }
      return std::any_of(lambda->captureRequiresActiveCleanup.begin(),
                         lambda->captureRequiresActiveCleanup.end(),
                         [](bool active) { return active; });
    }
    default:
      return false;
    }
  };

  for (const MirPayloadEnum &enumeration : program.payloadEnums()) {
    bool exact = enumeration.declaration != 0 && enumeration.sourceUnit != 0 &&
                 std::find(declarations.begin(), declarations.end(),
                           enumeration.type) == declarations.end() &&
                 enumeration.type.kind == SemanticType::Enum &&
                 enumeration.type.enumId == enumeration.declaration &&
                 !enumeration.variants.empty();
    declarations.push_back(enumeration.type);
    bool active = false;
    for (std::size_t variantIndex = 0;
         variantIndex < enumeration.variants.size(); ++variantIndex) {
      const MirPayloadEnumVariant &variant = enumeration.variants[variantIndex];
      exact = exact && variant.variantIndex == variantIndex &&
              variant.payloadTypes.size() ==
                  variant.payloadRequiresActiveCleanup.size();
      for (std::size_t fieldIndex = 0; fieldIndex < variant.payloadTypes.size();
           ++fieldIndex) {
        if (fieldIndex >= variant.payloadRequiresActiveCleanup.size())
          break;
        const SemanticType &field = variant.payloadTypes[fieldIndex];
        const std::optional<bool> fieldActive =
            typeRequiresActiveCleanup(field, typeRequiresActiveCleanup);
        exact =
            exact && field != SemanticType::Unknown &&
            field != SemanticType::Void && fieldActive &&
            *fieldActive == variant.payloadRequiresActiveCleanup[fieldIndex];
        // Active alternatives enter through concrete class instances, which
        // carry their own partial construction and recursive cleanup recipe.
        // Direct non-class owning carriers remain outside this contract.
        exact = exact && (!variant.payloadRequiresActiveCleanup[fieldIndex] ||
                          field.kind == SemanticType::Class);
        active = active || variant.payloadRequiresActiveCleanup[fieldIndex];
      }
    }
    exact = exact && active == enumeration.requiresActiveCleanup &&
            (!active || enumeration.traits.drop == DropKind::Lexical) &&
            enumeration.traits.relocation ==
                mirTypeRelocationKind(program, enumeration.type);
    if (!exact) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Module,
           .message = "payload-enum lifecycle table is unresolved or "
                      "incoherent"});
    }
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.operation != MirOperation::PayloadConstruct &&
            instruction.operation != MirOperation::PayloadExtract) {
          continue;
        }
        const MirPayloadEnum *enumeration =
            instruction.enumOwner
                ? program.findPayloadEnum(mirInstructionEnumType(instruction))
                : nullptr;
        const MirPayloadEnumVariant *variant =
            enumeration != nullptr && instruction.enumVariant &&
                    *instruction.enumVariant < enumeration->variants.size()
                ? &enumeration->variants[*instruction.enumVariant]
                : nullptr;
        bool exact = enumeration != nullptr && variant != nullptr &&
                     enumeration->declaration == *instruction.enumOwner &&
                     variant->variantIndex == *instruction.enumVariant;
        if (!exact) {
          result.errors.push_back(
              {.bodyKind = address.kind,
               .owner = address.owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "payload-enum instruction differs from its "
                          "verified variant table"});
          continue;
        }
        if (instruction.operation == MirOperation::PayloadConstruct) {
          const MirValue *value = instruction.result
                                      ? body->findValue(*instruction.result)
                                      : nullptr;
          exact = exact && value != nullptr &&
                  value->info.type == enumeration->type &&
                  value->info.traits == enumeration->traits &&
                  instruction.operands.size() == variant->payloadTypes.size();
          for (std::size_t index = 0;
               exact && index < instruction.operands.size(); ++index) {
            exact = instruction.operands[index].type ==
                    variant->payloadTypes[index];
          }
          std::vector<bool> transferred(variant->payloadTypes.size(), false);
          std::size_t resultInitializations = 0;
          for (const MirLifecycleEvent &event : instruction.lifecycle) {
            if (event.kind == MirLifecycleEventKind::TransferOut) {
              const MirDropObligation *source =
                  body->findDropObligation(event.source);
              bool matched = false;
              for (std::size_t fieldIndex = 0;
                   source != nullptr && fieldIndex < transferred.size();
                   ++fieldIndex) {
                if (!transferred[fieldIndex] &&
                    variant->payloadRequiresActiveCleanup[fieldIndex] &&
                    source->dropType.type ==
                        variant->payloadTypes[fieldIndex] &&
                    source->dropType.requiresActiveCleanup) {
                  transferred[fieldIndex] = true;
                  matched = true;
                  break;
                }
              }
              exact = exact && matched && event.target == 0 &&
                      !event.conditional && !event.failureCleanup;
            } else if (event.kind == MirLifecycleEventKind::Initialize) {
              const MirDropObligation *target =
                  body->findDropObligation(event.target);
              ++resultInitializations;
              exact = exact && enumeration->requiresActiveCleanup &&
                      event.source == 0 && target != nullptr &&
                      target->dropType.type == enumeration->type &&
                      target->dropType.requiresActiveCleanup &&
                      !event.conditional && !event.failureCleanup;
            } else {
              exact = false;
            }
          }
          for (std::size_t fieldIndex = 0; fieldIndex < transferred.size();
               ++fieldIndex) {
            exact =
                exact && transferred[fieldIndex] ==
                             variant->payloadRequiresActiveCleanup[fieldIndex];
          }
          exact = exact && resultInitializations ==
                               static_cast<std::size_t>(
                                   enumeration->requiresActiveCleanup);
        } else {
          const MirValue *value = instruction.result
                                      ? body->findValue(*instruction.result)
                                      : nullptr;
          exact = exact && instruction.payloadIndex && value != nullptr &&
                  *instruction.payloadIndex < variant->payloadTypes.size() &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().type == enumeration->type &&
                  value->info.type ==
                      variant->payloadTypes[*instruction.payloadIndex];
          const bool consuming = exact && instruction.operands.front().kind ==
                                              MirOperandKind::Move;
          exact = exact && (consuming || instruction.lifecycle.empty());
          if (consuming) {
            // A consuming extraction moves one field out of a switch-owned
            // subject binding at the selected arm's entry. The arm is entered
            // only through the matching variant edge of a switch over that
            // exact binding, and only the arm's other extractions and their
            // binding initializations precede it. The arm's extractions take
            // every field of the active alternative exactly once; when the
            // subject owns cleanup, the last of them alone transfers the
            // enum's obligation out, so the drained shell is never dropped.
            const MirOperand &operand = instruction.operands.front();
            const MirPlace *subject = body->findPlace(operand.place);
            exact = value->info.category == ValueCategory::Value &&
                    subject != nullptr &&
                    subject->root == MirPlaceRootKind::Binding &&
                    subject->projections.empty() &&
                    subject->type == enumeration->type;
            const auto consumingExtract = [&](const MirInstruction &candidate) {
              return !candidate.tombstone &&
                     candidate.kind == MirInstructionKind::Compute &&
                     candidate.operation == MirOperation::PayloadExtract &&
                     candidate.operands.size() == 1 &&
                     candidate.operands.front().kind == MirOperandKind::Move &&
                     candidate.operands.front().place == operand.place;
            };
            std::size_t incoming = 0;
            bool guarded = block.id != body->entry;
            for (const MirBlock &predecessor : body->blocks) {
              const auto &terminator = predecessor.terminator;
              const auto successors = terminatorSuccessors(terminator);
              if (std::find(successors.begin(), successors.end(), block.id) ==
                  successors.end())
                continue;
              ++incoming;
              if (terminator.kind != MirTerminatorKind::Switch ||
                  !terminator.value ||
                  terminator.value->kind != MirOperandKind::BorrowRead ||
                  terminator.value->place != operand.place ||
                  terminator.target == block.id) {
                guarded = false;
                continue;
              }
              std::size_t matchingEdges = 0;
              bool rightVariant = false;
              for (const auto &target : terminator.switchTargets) {
                if (target.target != block.id)
                  continue;
                ++matchingEdges;
                rightVariant =
                    target.value &&
                    target.value->enumOwner == instruction.enumOwner &&
                    target.value->value.magnitude == *instruction.enumVariant &&
                    !target.value->value.negative;
              }
              guarded = guarded && matchingEdges == 1 && rightVariant;
            }
            exact = exact && guarded && incoming == 1;
            const MirDropObligation *owner = nullptr;
            for (const MirDropObligation &obligation : body->dropObligations) {
              if (obligation.kind == MirDropObligationKind::Binding &&
                  obligation.place == operand.place)
                owner = &obligation;
            }
            exact = exact &&
                    (owner != nullptr || !enumeration->requiresActiveCleanup);
            std::vector<bool> taken(variant->payloadTypes.size(), false);
            std::size_t transfers = 0;
            bool retired = false;
            bool prefix = true;
            std::unordered_set<MirValueId> extracted;
            for (const MirInstruction &candidate : block.instructions) {
              if (candidate.id == instruction.id)
                prefix = false;
              if (consumingExtract(candidate)) {
                const bool field =
                    candidate.enumVariant == instruction.enumVariant &&
                    candidate.payloadIndex &&
                    *candidate.payloadIndex < taken.size() &&
                    !taken[*candidate.payloadIndex];
                exact = exact && field && !retired && candidate.result;
                if (field)
                  taken[*candidate.payloadIndex] = true;
                if (candidate.result)
                  extracted.insert(*candidate.result);
                for (const MirLifecycleEvent &event : candidate.lifecycle) {
                  exact = exact && owner != nullptr &&
                          event.kind == MirLifecycleEventKind::TransferOut &&
                          event.source == owner->id && event.target == 0 &&
                          !event.conditional && !event.failureCleanup;
                  ++transfers;
                  retired = true;
                }
                continue;
              }
              if (!prefix)
                continue;
              const MirPlace *destination =
                  candidate.destination
                      ? body->findPlace(*candidate.destination)
                      : nullptr;
              const bool bindingInitialization =
                  candidate.kind == MirInstructionKind::Initialize &&
                  destination != nullptr &&
                  destination->root == MirPlaceRootKind::Binding &&
                  destination->projections.empty() &&
                  destination->id != operand.place &&
                  candidate.operands.size() == 1 &&
                  candidate.operands.front().kind == MirOperandKind::Value &&
                  extracted.contains(candidate.operands.front().value);
              exact =
                  exact && (candidate.tombstone ||
                            candidate.kind == MirInstructionKind::EndBorrow ||
                            bindingInitialization);
            }
            exact = exact &&
                    std::all_of(taken.begin(), taken.end(),
                                [](bool field) { return field; }) &&
                    transfers == (owner != nullptr ? 1u : 0u);
          }
          if (exact && value->info.category == ValueCategory::Place) {
            const MirOperand &operand = instruction.operands.front();
            const MirPlace *subject = body->findPlace(operand.place);
            bool guarded = block.id != body->entry;
            std::size_t incoming = 0;
            // A borrowed extraction is introduced at the selected arm entry.
            // Prove the exact subject/variant edge, not just the enum type.
            for (const MirBlock &predecessor : body->blocks) {
              const auto &terminator = predecessor.terminator;
              const auto successors = terminatorSuccessors(terminator);
              if (std::find(successors.begin(), successors.end(), block.id) ==
                  successors.end())
                continue;
              ++incoming;
              if (terminator.kind != MirTerminatorKind::Switch ||
                  !terminator.value || *terminator.value != operand ||
                  terminator.target == block.id) {
                guarded = false;
                continue;
              }
              std::size_t matchingEdges = 0;
              bool rightVariant = false;
              for (const auto &target : terminator.switchTargets) {
                if (target.target != block.id)
                  continue;
                ++matchingEdges;
                rightVariant =
                    target.value &&
                    target.value->enumOwner == instruction.enumOwner &&
                    target.value->value.magnitude == *instruction.enumVariant &&
                    !target.value->value.negative;
              }
              guarded = guarded && matchingEdges == 1 && rightVariant;
            }
            // Arm bindings precede user code. No operation before this
            // extraction may invalidate the selected alternative. Other
            // extractions and binding/loan setup do not mutate its owner.
            for (const auto &prefix : block.instructions) {
              if (prefix.id == instruction.id)
                break;
              const auto *destination =
                  prefix.destination ? body->findPlace(*prefix.destination)
                                     : nullptr;
              const bool bindingInitialization =
                  prefix.kind == MirInstructionKind::Initialize &&
                  destination != nullptr &&
                  destination->root == MirPlaceRootKind::Binding &&
                  destination->projections.empty() && destination->key &&
                  subject != nullptr && subject->key &&
                  placeRelation(*destination->key, *subject->key).relation ==
                      PlaceRelation::Disjoint;
              guarded = guarded &&
                        (prefix.operation == MirOperation::PayloadExtract ||
                         prefix.kind == MirInstructionKind::Borrow ||
                         bindingInitialization);
            }
            exact = guarded && incoming == 1 && subject != nullptr &&
                    subject->key &&
                    (operand.kind == MirOperandKind::BorrowRead ||
                     operand.kind == MirOperandKind::BorrowWrite) &&
                    (value->info.access != AccessMode::Mutable ||
                     subject->access == AccessMode::Mutable) &&
                    instruction.lifecycle.empty();
            bool borrowed = false;
            for (const MirLoan &loan : body->loans) {
              const MirPlace *field = body->findPlace(loan.source);
              if (field == nullptr || field->root != MirPlaceRootKind::Value ||
                  field->value != value->id ||
                  loan.producedBy != instruction.hirValue)
                continue;
              borrowed = true;
              const auto relation =
                  field->key && subject != nullptr && subject->key
                      ? placeRelation(*field->key, *subject->key).relation
                      : PlaceRelation::MayAlias;
              exact = exact && field->projections.empty() &&
                      field->type == value->info.type &&
                      (relation == PlaceRelation::Equal ||
                       relation == PlaceRelation::LeftStrictPrefix ||
                       (subject != nullptr &&
                        mirCanonicalPlaceContains(*body, subject->id,
                                                  field->id))) &&
                      field->access == value->info.access &&
                      loan.access == value->info.access &&
                      loan.producedBy == instruction.hirValue &&
                      (loan.kind == MirLoanKind::Local ||
                       (loan.kind == MirLoanKind::Return && loan.escapes));
            }
            exact = exact && borrowed;
          }
        }
        if (!exact) {
          result.errors.push_back(
              {.bodyKind = address.kind,
               .owner = address.owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "payload-enum instruction differs from its "
                          "verified variant table"});
        }
      }
    }
  }
  return result;
}

struct MirCheckedIntegerFailureContract {
  bool applicable = false;
  bool validShape = true;
  std::vector<DefinedFailureOutcome> outcomes;
};

[[nodiscard]] bool integerConversionMayFail(CheckedIntegerDomain source,
                                            CheckedIntegerDomain target) {
  if (source.signedValue == target.signedValue) {
    return source.width > target.width;
  }
  if (!source.signedValue && target.signedValue) {
    return source.width >= target.width;
  }
  return true;
}

[[nodiscard]] MirCheckedIntegerFailureContract
checkedIntegerFailureContract(const MirBody &body,
                              const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Compute) {
    return {};
  }

  const std::optional<CheckedIntegerDomain> resultDomain =
      constantIntegerDomain(instruction.info.type);
  const auto operandDomain = [&](std::size_t index) {
    return index < instruction.operands.size()
               ? constantIntegerDomain(instruction.operands[index].type)
               : std::nullopt;
  };
  const auto sourceIntegerLiteralMagnitude =
      [&](std::size_t index) -> std::optional<std::uint64_t> {
    if (index >= instruction.operands.size() ||
        instruction.operands[index].kind != MirOperandKind::Value) {
      return std::nullopt;
    }

    MirValueId valueId = instruction.operands[index].value;
    SemanticType valueType = instruction.operands[index].type;
    std::unordered_set<MirValueId> visited;
    while (valueId != 0 && visited.size() <= body.values.size() &&
           visited.insert(valueId).second) {
      const MirValue *value = body.findValue(valueId);
      const MirBlock *definitionBlock =
          value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
      const auto definition =
          definitionBlock == nullptr
              ? std::vector<MirInstruction>::const_iterator{}
              : std::find_if(definitionBlock->instructions.begin(),
                             definitionBlock->instructions.end(),
                             [&](const MirInstruction &candidate) {
                               return candidate.id == value->definition;
                             });
      if (value == nullptr || value->info.type != valueType ||
          definitionBlock == nullptr ||
          definition == definitionBlock->instructions.end() ||
          definition->kind != MirInstructionKind::Compute ||
          definition->result != valueId || definition->info.type != valueType) {
        return std::nullopt;
      }
      if (definition->operation == MirOperation::Literal) {
        const std::uint64_t *magnitude =
            definition->literal
                ? std::get_if<std::uint64_t>(&*definition->literal)
                : nullptr;
        return definition->operands.empty() &&
                       definition->literalProvenance.kind ==
                           MirLiteralProvenanceKind::Source &&
                       definition->literalProvenance.sourceValue == 0 &&
                       magnitude != nullptr
                   ? std::optional<std::uint64_t>{*magnitude}
                   : std::nullopt;
      }
      if (definition->operation != MirOperation::Identity ||
          definition->literal ||
          definition->literalProvenance.kind !=
              MirLiteralProvenanceKind::None ||
          definition->literalProvenance.sourceValue != 0 ||
          definition->operands.size() != 1 ||
          definition->operands.front().kind != MirOperandKind::Value ||
          definition->operands.front().type != valueType) {
        return std::nullopt;
      }
      valueId = definition->operands.front().value;
    }
    return std::nullopt;
  };
  const auto operandAdmitsExecutionDomain = [&](std::size_t index,
                                                bool negativeLiteral = false) {
    const std::optional<std::uint64_t> literal =
        sourceIntegerLiteralMagnitude(index);
    if (literal) {
      return resultDomain && checkedIntegerFits({.negative = negativeLiteral,
                                                 .magnitude = *literal},
                                                *resultDomain);
    }
    const std::optional<CheckedIntegerDomain> operand = operandDomain(index);
    return resultDomain && operand &&
           (*operand == *resultDomain ||
            !integerConversionMayFail(*operand, *resultDomain));
  };
  const auto binary = [&] {
    return MirCheckedIntegerFailureContract{
        .applicable = resultDomain.has_value(),
        .validShape = resultDomain && instruction.operands.size() == 2 &&
                      operandAdmitsExecutionDomain(0) &&
                      operandAdmitsExecutionDomain(1)};
  };
  const auto shift = [&] {
    const std::optional<CheckedIntegerDomain> count = operandDomain(1);
    return MirCheckedIntegerFailureContract{
        .applicable = resultDomain.has_value(),
        .validShape = resultDomain && instruction.operands.size() == 2 &&
                      count && operandAdmitsExecutionDomain(0)};
  };

  MirCheckedIntegerFailureContract contract;
  switch (instruction.operation) {
  case MirOperation::Add:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                          .detail = DefinedFailureDetail::Addition}};
    break;
  case MirOperation::Subtract:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                          .detail = DefinedFailureDetail::Subtraction}};
    break;
  case MirOperation::Multiply:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                          .detail = DefinedFailureDetail::Multiplication}};
    break;
  case MirOperation::Divide:
    contract = binary();
    if (resultDomain && resultDomain->signedValue) {
      contract.outcomes.push_back({.code = DefinedFailureCode::IntegerOverflow,
                                   .detail = DefinedFailureDetail::Division});
    }
    contract.outcomes.push_back(
        {.code = DefinedFailureCode::DivisionByZero,
         .detail = DefinedFailureDetail::IntegerDivision});
    break;
  case MirOperation::Remainder:
    contract = binary();
    contract.outcomes = {{.code = DefinedFailureCode::ModuloByZero,
                          .detail = DefinedFailureDetail::IntegerModulo}};
    break;
  case MirOperation::ShiftLeft:
  case MirOperation::ShiftRight: {
    contract = shift();
    const DefinedFailureDetail detail =
        instruction.operation == MirOperation::ShiftLeft
            ? DefinedFailureDetail::LeftShift
            : DefinedFailureDetail::RightShift;
    const std::optional<CheckedIntegerDomain> countDomain = operandDomain(1);
    if (countDomain && countDomain->signedValue) {
      contract.outcomes.push_back(
          {.code = DefinedFailureCode::NegativeShiftCount, .detail = detail});
    }
    contract.outcomes.push_back(
        {.code = DefinedFailureCode::ShiftCountOutOfRange, .detail = detail});
    break;
  }
  case MirOperation::Negate: {
    const std::optional<std::uint64_t> sourceMagnitude =
        sourceIntegerLiteralMagnitude(0);
    const bool exactSignedMinimum =
        resultDomain && resultDomain->signedValue && sourceMagnitude &&
        resultDomain->width != 0 && resultDomain->width <= 64 &&
        *sourceMagnitude == (std::uint64_t{1} << (resultDomain->width - 1U));
    contract = {.applicable = resultDomain.has_value(),
                .validShape = resultDomain && resultDomain->signedValue &&
                              instruction.operands.size() == 1 &&
                              operandAdmitsExecutionDomain(0, true)};
    if (!instruction.programConstantSubstitution && !exactSignedMinimum) {
      contract.outcomes = {{.code = DefinedFailureCode::IntegerOverflow,
                            .detail = DefinedFailureDetail::Negation}};
    }
    break;
  }
  case MirOperation::Convert: {
    if (!resultDomain || instruction.operands.size() != 1) {
      return {};
    }
    const std::optional<CheckedIntegerDomain> sourceDomain = operandDomain(0);
    if (!sourceDomain) {
      // Float-to-integer checks and the lossless char bridge remain outside
      // this bounded integer-to-integer verifier family.
      return {};
    }
    contract.applicable = true;
    if (integerConversionMayFail(*sourceDomain, *resultDomain)) {
      contract.outcomes = {
          {.code = DefinedFailureCode::NumericConversionOutOfRange,
           .detail = DefinedFailureDetail::NumericCast}};
    }
    break;
  }
  default:
    return {};
  }

  if (!contract.applicable && !instruction.definedFailure.empty()) {
    // Floating arithmetic never selects an integer-domain record. Mark the
    // represented contract applicable-but-invalid so forged metadata cannot
    // hide behind the noninteger near-miss.
    contract.applicable = true;
    contract.validShape = false;
  }
  return contract;
}

[[nodiscard]] std::tuple<const SourceUnitId &, const std::size_t &,
                         const std::size_t &>
failureOriginRange(const FailureOriginAssignment &assignment) {
  return std::tie(assignment.sourceUnit, assignment.start, assignment.end);
}

// Finds the first failure-origin assignment covering an origin's exact
// source range. Verified failure metadata orders assignments strictly by that
// range, so a binary search finds the one candidate; metadata out of that
// order, which its own verification rejects, is searched in order instead.
class FailureOriginAssignmentLookup {
public:
  explicit FailureOriginAssignmentLookup(const FailureMetadata &metadata)
      : assignments(metadata.assignments()),
        ordered(std::adjacent_find(assignments.begin(), assignments.end(),
                                   [](const FailureOriginAssignment &left,
                                      const FailureOriginAssignment &right) {
                                     return !(failureOriginRange(left) <
                                              failureOriginRange(right));
                                   }) == assignments.end()) {}

  [[nodiscard]] const FailureOriginAssignment *
  find(const DefinedFailureOrigin &origin) const {
    const auto key = std::tie(origin.sourceUnit, origin.start, origin.end);
    const auto found =
        ordered
            ? std::lower_bound(assignments.begin(), assignments.end(), key,
                               [](const FailureOriginAssignment &candidate,
                                  const auto &target) {
                                 return failureOriginRange(candidate) < target;
                               })
            : std::find_if(assignments.begin(), assignments.end(),
                           [&](const FailureOriginAssignment &candidate) {
                             return failureOriginRange(candidate) == key;
                           });
    return found == assignments.end() || failureOriginRange(*found) != key
               ? nullptr
               : &*found;
  }

private:
  const std::vector<FailureOriginAssignment> &assignments;
  bool ordered = false;
};

[[nodiscard]] MirVerificationResult verifyMirCheckedIntegerFailureContracts(
    const FailureOriginAssignmentLookup &origins, const MirBody &body,
    std::size_t owner) {
  // Indexed on the first checked operation: full-expression roots, failure
  // records by producing instruction in body order, and the number of
  // terminal endpoints that route each record.
  std::unordered_set<HirValueId> fullExpressionRoots;
  std::unordered_map<MirInstructionId, std::vector<const MirFailureRecord *>>
      recordsByProducer;
  std::unordered_map<MirFailureRecordId, std::size_t> recordEndpoints;
  bool indexed = false;
  const auto index = [&] {
    if (indexed) {
      return;
    }
    indexed = true;
    for (const MirFullExpression &expression : body.fullExpressions) {
      fullExpressionRoots.insert(expression.roots.begin(),
                                 expression.roots.end());
    }
    for (const MirFailureRecord &record : body.failureRecords) {
      recordsByProducer[record.producerInstruction].push_back(&record);
    }
    for (const MirBlock &candidate : body.blocks) {
      const bool exactKind =
          body.kind == MirBodyKind::HostedStartup
              ? candidate.terminator.kind == MirTerminatorKind::ContainFailure
              : candidate.terminator.kind ==
                    MirTerminatorKind::PropagateFailure;
      // A record's own chain ends at its endpoint or where it enters a
      // shared cleanup continuation, which propagates it unchanged.
      const bool continuation =
          body.kind != MirBodyKind::HostedStartup &&
          candidate.terminator.kind == MirTerminatorKind::ContinueFailure &&
          candidate.activeFailure != 0 &&
          (candidate.failureParameter == 0 ||
           candidate.failureParameter == candidate.activeFailure);
      if ((exactKind &&
           candidate.activeFailure == candidate.terminator.failureRecord) ||
          continuation) {
        ++recordEndpoints[candidate.activeFailure];
      }
    }
  };
  const auto fullExpressionRoot = [&](const MirInstruction &instruction) {
    return fullExpressionRoots.contains(instruction.hirValue);
  };
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const MirCheckedIntegerFailureContract contract =
          checkedIntegerFailureContract(body, instruction);
      if (!contract.applicable) {
        continue;
      }
      index();
      if (!contract.validShape) {
        return verificationFailure(
            body, owner,
            "checked integer operation does not retain its exact "
            "fixed-width operand and result domains",
            block.id, instruction.id);
      }

      const bool exactLocalOperation =
          instruction.definedFailure.propagation ==
              FailurePropagationKind::None &&
          (contract.outcomes.empty()
               ? instruction.definedFailure.localOrigins.empty() &&
                     instruction.localFailureSites.empty()
               : instruction.definedFailure.localOrigins.size() == 1 &&
                     instruction.definedFailure.localOrigins.front().outcomes ==
                         contract.outcomes &&
                     instruction.localFailureSites.size() == 1 &&
                     instruction.localFailureSites.front() != 0);
      if (!exactLocalOperation) {
        return verificationFailure(
            body, owner,
            "checked integer operation does not retain its exact "
            "canonical local failure outcomes",
            block.id, instruction.id);
      }

      static const std::vector<const MirFailureRecord *> noRecords;
      const auto produced = recordsByProducer.find(instruction.id);
      const std::vector<const MirFailureRecord *> &records =
          produced == recordsByProducer.end() ? noRecords : produced->second;
      if (contract.outcomes.empty()) {
        if (!records.empty()) {
          return verificationFailure(
              body, owner,
              "non-failing integer conversion retains a stale "
              "failure record",
              block.id, instruction.id);
        }
        continue;
      }

      const DefinedFailureOrigin &origin =
          instruction.definedFailure.localOrigins.front();
      const FailureSiteId site = instruction.localFailureSites.front();
      const FailureOriginAssignment *assignment = origins.find(origin);
      if (assignment == nullptr || assignment->line != origin.line ||
          assignment->site != site ||
          std::any_of(contract.outcomes.begin(), contract.outcomes.end(),
                      [&](DefinedFailureOutcome outcome) {
                        return std::find(assignment->outcomes.begin(),
                                         assignment->outcomes.end(),
                                         outcome) == assignment->outcomes.end();
                      })) {
        return verificationFailure(
            body, owner,
            "checked integer operation does not retain its exact "
            "artifact-local origin and site identity",
            block.id, instruction.id);
      }

      if ((mirBodyRoutesFailureEdges(body) &&
           fullExpressionRoot(instruction)) ||
          !records.empty()) {
        if (records.size() != 1) {
          return verificationFailure(
              body, owner,
              "checked integer operation must produce exactly one "
              "fixed failure record",
              block.id, instruction.id);
        }
        const MirFailureRecord &record = *records.front();
        const MirBlock *producer = body.findBlock(record.producerBlock);
        const MirBlock *parameter = body.findBlock(record.parameterBlock);
        const auto endpoints = recordEndpoints.find(record.id);
        const std::size_t endpointCount =
            endpoints == recordEndpoints.end() ? 0 : endpoints->second;
        if (producer == nullptr || parameter == nullptr || producer != &block ||
            record.producerInstruction != instruction.id ||
            producer->terminator.kind != MirTerminatorKind::Invoke ||
            producer->terminator.invokeInstruction != instruction.id ||
            producer->terminator.failureRecord != record.id ||
            producer->terminator.elseTarget != parameter->id ||
            parameter->failureParameter != record.id ||
            parameter->activeFailure != record.id || endpointCount != 1) {
          return verificationFailure(
              body, owner,
              "checked integer operation does not retain its exact "
              "invoke, failure parameter, and terminal chain",
              block.id, instruction.id);
        }
      }
    }
  }
  return {};
}

} // namespace
namespace {
[[nodiscard]] bool hasCanonicalBodylessDefinition(
    const MirBody &body, MirBodyKind kind, const SemanticType &returnType,
    const std::vector<SemanticType> &parameterTypes,
    const std::vector<HirBindingId> &parameterBindings,
    bool neverReturns = false) {
  if (body.kind != kind || body.returnType != returnType || body.entry != 1 ||
      body.blocks.size() != 1 ||
      body.places.size() != parameterBindings.size() || !body.loans.empty() ||
      !body.fullExpressions.empty() || !body.cleanupBoundaries.empty() ||
      !body.dropObligations.empty() || !body.failureRecords.empty() ||
      !body.values.empty() || !body.valueUses.empty()) {
    return false;
  }

  MirBlock expectedBlock;
  expectedBlock.id = 1;
  expectedBlock.terminator.kind =
      !neverReturns && returnType == SemanticType::Void
          ? MirTerminatorKind::Return
          : MirTerminatorKind::Unreachable;
  expectedBlock.reachable = true;
  if (body.blocks.front() != expectedBlock) {
    return false;
  }

  std::vector<SemanticType> bindingTypes;
  for (std::size_t index = 0; index < body.places.size(); ++index) {
    const MirPlace &place = body.places[index];
    const PlaceKey expectedKey{.domain = body.placeDomain,
                               .root = place.symbol};
    if (place.id != index + 1 || place.root != MirPlaceRootKind::Binding ||
        parameterBindings[index] == 0 ||
        place.binding != parameterBindings[index] || place.symbol == 0 ||
        place.capture != 0 || place.temporary != 0 || place.value != 0 ||
        place.loan != 0 || !place.projections.empty() ||
        place.sourceValue != 0 || !place.key || *place.key != expectedKey ||
        !place.initiallyAvailable) {
      return false;
    }
    if (place.type.kind == SemanticType::TypePack &&
        (index + 1 != body.places.size() || !place.type.concretePack ||
         std::any_of(place.type.arguments.begin(), place.type.arguments.end(),
                     [](const SemanticType &element) {
                       return element.kind == SemanticType::TypePack;
                     }))) {
      return false;
    }
    bindingTypes.push_back(place.type);
  }
  return exactParameterRoles(bindingTypes, parameterTypes);
}

[[nodiscard]] bool
hasCanonicalEmptyStaticInitializerBody(const MirBody &body,
                                       const MirBody &module) {
  if (body.kind != MirBodyKind::StaticFieldInitializers ||
      body.returnType != SemanticType::Void || body.entry != 1 ||
      body.blocks.size() != 1 || !body.places.empty() || !body.loans.empty() ||
      !body.fullExpressions.empty() || !body.cleanupBoundaries.empty() ||
      !body.dropObligations.empty() || !body.failureRecords.empty() ||
      !body.programConstantSubstitutions.empty() ||
      !body.rangeIterations.empty() || !body.values.empty() ||
      !body.valueUses.empty() ||
      body.placeDomain.snapshot != module.placeDomain.snapshot ||
      body.placeDomain.body == 0 || body.placeDomain == module.placeDomain) {
    return false;
  }
  MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

[[nodiscard]] MirVerificationResult
verifyMirProgramInitialization(const MirProgram &program) {
  MirVerificationResult result;
  const MirProgramInitializationPlan &plan =
      program.programInitializationPlan();
  const MirBody &module = program.module();
  const auto add = [&](std::string message, MirBlockId block = 0,
                       MirInstructionId instruction = 0) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };

  std::unordered_set<SourceUnitId> sourceUnits;
  std::vector<ProgramInitializationStepId> inventoriedSteps;
  for (const MirProgramInitializationUnit &unit : plan.units) {
    if (unit.sourceUnit == 0 || !sourceUnits.insert(unit.sourceUnit).second) {
      add("program-initialization unit inventory is invalid");
    }
    for (const ProgramInitializationStepId stepId : unit.steps) {
      const MirProgramInitializationStep *step = plan.findStep(stepId);
      if (step == nullptr || step->sourceUnit != unit.sourceUnit) {
        add("program-initialization unit names the wrong step");
      }
      inventoriedSteps.push_back(stepId);
    }
  }
  const bool syntheticSource =
      plan.units.empty() && !plan.steps.empty() &&
      std::all_of(plan.steps.begin(), plan.steps.end(),
                  [](const auto &step) { return step.sourceUnit == 0; });
  std::vector<ProgramInitializationStepId> denseSteps;
  denseSteps.reserve(plan.steps.size());
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    denseSteps.push_back(index + 1);
  }
  if ((!syntheticSource && inventoriedSteps != denseSteps) ||
      (syntheticSource && !inventoriedSteps.empty())) {
    add("program-initialization unit/step inventory is not exact and dense");
  }

  const auto canonicalEmptyModule = [&] {
    if (module.kind != MirBodyKind::Module ||
        module.returnType != SemanticType::Void || module.entry != 1 ||
        module.blocks.size() != 1 || !module.places.empty() ||
        !module.loans.empty() || !module.fullExpressions.empty() ||
        !module.cleanupBoundaries.empty() || !module.dropObligations.empty() ||
        !module.failureRecords.empty() ||
        !module.programConstantSubstitutions.empty() ||
        !module.rangeIterations.empty() || !module.values.empty() ||
        !module.valueUses.empty()) {
      return false;
    }
    MirBlock expected;
    expected.id = 1;
    expected.terminator.kind = MirTerminatorKind::Exit;
    expected.reachable = true;
    return module.blocks.front() == expected;
  };
  if (plan.steps.empty()) {
    if (!canonicalEmptyModule()) {
      add("empty program-initialization plan has a noncanonical module body");
    }
    return result;
  }
  if (module.kind != MirBodyKind::Module ||
      module.returnType != SemanticType::Void || module.entry == 0 ||
      module.entry > module.blocks.size()) {
    add("program-initialization plan has no exact Module/0 body");
    return result;
  }

  struct LocatedInstruction {
    const MirBlock *block = nullptr;
    const MirInstruction *instruction = nullptr;
    std::size_t position = 0;
  };
  std::unordered_map<MirInstructionId, LocatedInstruction> instructions;
  std::unordered_map<HirFullExpressionId, std::vector<LocatedInstruction>>
      fullExpressionMarkers;
  std::vector<std::vector<const MirBlock *>> stepBlocks(plan.steps.size());
  ProgramInitializationStepId previousTag = 0;
  for (const MirBlock &block : module.blocks) {
    if (block.programInitializationStep == 0 ||
        block.programInitializationStep > plan.steps.size() ||
        block.programInitializationStep < previousTag ||
        block.programInitializationStep > previousTag + 1) {
      add("module block has a non-dense program-initialization tag", block.id);
    } else {
      previousTag = block.programInitializationStep;
      stepBlocks[block.programInitializationStep - 1].push_back(&block);
    }
    for (std::size_t position = 0; position < block.instructions.size();
         ++position) {
      const MirInstruction &instruction = block.instructions[position];
      instructions.emplace(instruction.id,
                           LocatedInstruction{.block = &block,
                                              .instruction = &instruction,
                                              .position = position});
      if (instruction.fullExpressionEnd != 0) {
        fullExpressionMarkers[instruction.fullExpressionEnd].push_back(
            {.block = &block,
             .instruction = &instruction,
             .position = position});
      }
    }
  }
  if (previousTag != plan.steps.size()) {
    add("module block tags do not cover every initialization step");
  }
  std::unordered_map<HirBindingId, ProgramInitializationStepId> bindingSteps;
  std::unordered_map<SymbolId, ProgramInitializationStepId> symbolSteps;
  std::unordered_set<MirPlaceId> storagePlaces;
  std::unordered_set<MirInstructionId> storageInitializations;
  std::unordered_set<HirStatementId> statements;
  std::unordered_set<HirValueId> initializerValues;
  std::unordered_set<HirFullExpressionId> fullExpressions;
  std::unordered_map<MirBlockId, std::vector<MirBlockId>> predecessors;
  for (const MirBlock &block : module.blocks) {
    for (const MirBlockId successor : terminatorSuccessors(block.terminator)) {
      if (successor != 0 && successor <= module.blocks.size()) {
        predecessors[successor].push_back(block.id);
      }
    }
  }
  const std::optional<MirDominanceInfo> dominance = computeMirDominance(module);
  if (!dominance) {
    add("program-initialization CFG has no usable dominance relation");
  }

  const auto instructionDominates = [&](const LocatedInstruction &earlier,
                                        const LocatedInstruction &later) {
    if (earlier.block == nullptr || later.block == nullptr || !dominance) {
      return false;
    }
    return earlier.block->id == later.block->id
               ? earlier.position < later.position
               : dominance->dominates(earlier.block->id, later.block->id);
  };

  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    const MirProgramInitializationStep &step = plan.steps[index];
    const MirPlace *storage = module.findPlace(step.storagePlace);
    const LocatedInstruction located =
        instructions.contains(step.storageInitialization)
            ? instructions.at(step.storageInitialization)
            : LocatedInstruction{};
    const MirInstruction *initialize = located.instruction;
    const MirBlock *entry = module.findBlock(step.entryBlock);
    const PlaceKey expectedKey{.domain = module.placeDomain,
                               .root = step.symbol};
    const bool realSource = !plan.units.empty();
    if (step.id != index + 1 ||
        (realSource ? !sourceUnits.contains(step.sourceUnit)
                    : step.sourceUnit != 0) ||
        step.symbol == 0 || step.binding == 0 ||
        !bindingSteps.emplace(step.binding, step.id).second ||
        !symbolSteps.emplace(step.symbol, step.id).second ||
        !storagePlaces.insert(step.storagePlace).second ||
        !storageInitializations.insert(step.storageInitialization).second ||
        step.requiresActiveCleanup || storage == nullptr ||
        storage->root != MirPlaceRootKind::Binding ||
        storage->binding != step.binding || storage->symbol != step.symbol ||
        storage->capture != 0 || storage->temporary != 0 ||
        storage->value != 0 || storage->loan != 0 ||
        !storage->projections.empty() ||
        storage->type == SemanticType::Unknown || storage->sourceValue != 0 ||
        !storage->key || *storage->key != expectedKey ||
        storage->initiallyAvailable || entry == nullptr ||
        entry->programInitializationStep != step.id ||
        stepBlocks[index].empty() ||
        stepBlocks[index].front()->id != step.entryBlock ||
        (index == 0 ? module.entry != step.entryBlock
                    : plan.steps[index - 1].entryBlock >= step.entryBlock)) {
      add("program-initialization step has invalid identity or storage",
          step.entryBlock, step.storageInitialization);
    }
    if (step.storageKind == ProgramStorageKind::NamespaceGlobal) {
      if (step.ownerClass != 0) {
        add("namespace-global initialization step has a class owner");
      }
    } else if (step.storageKind == ProgramStorageKind::StaticField) {
      if (step.ownerClass == 0 ||
          program.findClassInstance(step.ownerClass) == nullptr) {
        add("static-field initialization step has an invalid class owner");
      }
    } else {
      add("program-initialization step has an invalid storage kind");
    }

    const ExpressionInfo expectedInfo =
        storage == nullptr ? ExpressionInfo{}
                           : ExpressionInfo{.type = storage->type,
                                            .category = ValueCategory::Place,
                                            .access = storage->access,
                                            .traits = storage->traits};
    MirInstruction canonicalInitialize;
    canonicalInitialize.id = step.storageInitialization;
    canonicalInitialize.kind = MirInstructionKind::Initialize;
    canonicalInitialize.destination = step.storagePlace;
    canonicalInitialize.info = expectedInfo;
    const bool commonInitialize =
        initialize != nullptr && located.block != nullptr &&
        located.block->programInitializationStep == step.id;
    const std::vector<LocatedInstruction> &markers =
        fullExpressionMarkers[step.fullExpression];
    if (step.role == ProgramInitializationStepRole::DataOnly) {
      const bool implicitZero =
          step.dataInitialization ==
              MirProgramDataInitializationKind::ImplicitZero &&
          !step.dataConstant;
      const bool constant =
          step.dataInitialization ==
              MirProgramDataInitializationKind::Constant &&
          step.dataConstant && storage != nullptr &&
          constantMatchesType(*step.dataConstant, storage->type, &program);
      if ((!implicitZero && !constant) || step.statement != 0 ||
          step.initializer != 0 || step.fullExpression != 0 ||
          !commonInitialize || *initialize != canonicalInitialize ||
          stepBlocks[index].size() != 1 ||
          stepBlocks[index].front()->instructions.size() != 1 ||
          stepBlocks[index].front()->instructions.front().id !=
              step.storageInitialization) {
        add("data-only initialization step is not its exact zero/constant "
            "storage stage",
            step.entryBlock, step.storageInitialization);
      }
    } else if (step.role == ProgramInitializationStepRole::Initializer) {
      const MirFullExpression *full =
          step.fullExpression == 0 ||
                  step.fullExpression > module.fullExpressions.size()
              ? nullptr
              : &module.fullExpressions[step.fullExpression - 1];
      const auto operandMatchesInitializer = [&] {
        if (initialize == nullptr || initialize->operands.size() != 1) {
          return false;
        }
        const MirOperand &operand = initialize->operands.front();
        if (operand.kind == MirOperandKind::Value) {
          const MirValue *value = module.findValue(operand.value);
          return value != nullptr && value->sourceValue == step.initializer;
        }
        if (operand.kind == MirOperandKind::Loan) {
          const MirLoan *loan = module.findLoan(operand.loan);
          const MirPlace *source =
              loan == nullptr ? nullptr : module.findPlace(loan->source);
          return loan != nullptr && (loan->producedBy == step.initializer ||
                                     (source != nullptr &&
                                      source->sourceValue == step.initializer));
        }
        const MirPlace *place = module.findPlace(operand.place);
        return place != nullptr && place->sourceValue == step.initializer;
      };
      if (initialize != nullptr) {
        canonicalInitialize.hirStatement = step.statement;
        if (initialize->operands.size() == 1) {
          const MirOperand &operand = initialize->operands.front();
          MirOperand canonicalOperand;
          canonicalOperand.kind = operand.kind;
          canonicalOperand.type = operand.type;
          switch (operand.kind) {
          case MirOperandKind::Value:
            canonicalOperand.value = operand.value;
            break;
          case MirOperandKind::Constant:
            canonicalOperand.literal = operand.literal;
            break;
          case MirOperandKind::Address:
          case MirOperandKind::Copy:
          case MirOperandKind::Move:
          case MirOperandKind::BorrowRead:
          case MirOperandKind::BorrowWrite:
            canonicalOperand.place = operand.place;
            break;
          case MirOperandKind::Loan:
            canonicalOperand.loan = operand.loan;
            break;
          }
          canonicalInitialize.operands.push_back(std::move(canonicalOperand));
        }
        const MirDropObligation *initializerDrop = nullptr;
        for (const MirDropObligation &drop : module.dropObligations) {
          if (drop.kind != MirDropObligationKind::Value ||
              drop.value != step.initializer) {
            continue;
          }
          if (initializerDrop != nullptr) {
            initializerDrop = nullptr;
            break;
          }
          initializerDrop = &drop;
        }
        if (initializerDrop != nullptr) {
          canonicalInitialize.lifecycle.push_back(
              {.kind = MirLifecycleEventKind::TransferOut,
               .source = initializerDrop->id});
        }
      }
      MirInstruction canonicalMarker;
      if (markers.size() == 1) {
        canonicalMarker.id = markers.front().instruction->id;
        canonicalMarker.kind = MirInstructionKind::Lifecycle;
        canonicalMarker.hirValue = step.initializer;
        canonicalMarker.hirStatement = step.statement;
        canonicalMarker.fullExpressionEnd = step.fullExpression;
      }
      if (step.dataInitialization != MirProgramDataInitializationKind::None ||
          step.dataConstant || step.statement == 0 || step.initializer == 0 ||
          step.fullExpression == 0 ||
          !statements.insert(step.statement).second ||
          !initializerValues.insert(step.initializer).second ||
          !fullExpressions.insert(step.fullExpression).second ||
          !commonInitialize || !operandMatchesInitializer() ||
          *initialize != canonicalInitialize || full == nullptr ||
          full->id != step.fullExpression ||
          full->statement != step.statement ||
          full->constructorInitializer != 0 ||
          full->roots != std::vector<HirValueId>{step.initializer} ||
          markers.size() != 1 ||
          markers.front().block->programInitializationStep != step.id ||
          *markers.front().instruction != canonicalMarker ||
          !instructionDominates(located, markers.front())) {
        add("dynamic initialization step lacks its exact statement, root, "
            "storage Initialize, or ordered full-expression marker",
            step.entryBlock, step.storageInitialization);
      }
    } else {
      add("program-initialization step has an invalid role");
    }

    const std::vector<MirBlockId> &incoming = predecessors[step.entryBlock];
    const MirBlock *boundary = nullptr;
    if (index == 0) {
      if (!incoming.empty()) {
        add("first program-initialization entry has a predecessor",
            step.entryBlock);
      }
    } else {
      const MirBlock *predecessor =
          incoming.size() == 1 ? module.findBlock(incoming.front()) : nullptr;
      MirTerminator expectedTransition;
      expectedTransition.kind = MirTerminatorKind::Goto;
      expectedTransition.target = step.entryBlock;
      if (predecessor == nullptr ||
          predecessor->programInitializationStep != step.id - 1 ||
          predecessor->terminator != expectedTransition) {
        add("dense program-initialization steps lack one explicit Goto split",
            step.entryBlock);
      } else {
        boundary = predecessor;
      }
    }

    if (index + 1 == plan.steps.size()) {
      const auto finalExit = std::find_if(
          stepBlocks[index].begin(), stepBlocks[index].end(),
          [](const MirBlock *block) {
            return block->terminator.kind == MirTerminatorKind::Exit;
          });
      boundary = finalExit == stepBlocks[index].end() ? nullptr : *finalExit;
    } else {
      const std::vector<MirBlockId> &nextIncoming =
          predecessors[plan.steps[index + 1].entryBlock];
      boundary = nextIncoming.size() == 1
                     ? module.findBlock(nextIncoming.front())
                     : nullptr;
    }

    MirTerminator expectedBoundary;
    if (index + 1 == plan.steps.size()) {
      expectedBoundary.kind = MirTerminatorKind::Exit;
    } else {
      expectedBoundary.kind = MirTerminatorKind::Goto;
      expectedBoundary.target = plan.steps[index + 1].entryBlock;
    }
    if (boundary == nullptr || boundary->terminator != expectedBoundary) {
      add("program-initialization boundary is not the exact dense Goto or "
          "final Exit",
          boundary == nullptr ? step.entryBlock : boundary->id);
    }

    std::unordered_set<MirBlockId> forwardReachable;
    std::vector<MirBlockId> worklist{step.entryBlock};
    while (!worklist.empty()) {
      const MirBlockId blockId = worklist.back();
      worklist.pop_back();
      const MirBlock *block = module.findBlock(blockId);
      if (block == nullptr || block->programInitializationStep != step.id ||
          !forwardReachable.insert(blockId).second) {
        continue;
      }
      for (const MirBlockId successor :
           terminatorSuccessors(block->terminator)) {
        const MirBlock *target = module.findBlock(successor);
        if (target != nullptr && target->programInitializationStep == step.id) {
          worklist.push_back(successor);
        }
      }
    }
    if (forwardReachable.size() != stepBlocks[index].size()) {
      add("program-initialization step contains a block unreachable from its "
          "entry",
          step.entryBlock);
    }
    const auto reentersPublishedBlock = [&](MirBlockId published) {
      const MirBlock *publishedBlock = module.findBlock(published);
      if (publishedBlock == nullptr) {
        return true;
      }
      std::unordered_set<MirBlockId> visited;
      std::vector<MirBlockId> pending;
      for (const MirBlockId successor :
           terminatorSuccessors(publishedBlock->terminator)) {
        const MirBlock *target = module.findBlock(successor);
        if (target != nullptr && target->programInitializationStep == step.id) {
          pending.push_back(successor);
        }
      }
      while (!pending.empty()) {
        const MirBlockId blockId = pending.back();
        pending.pop_back();
        if (blockId == published) {
          return true;
        }
        const MirBlock *block = module.findBlock(blockId);
        if (block == nullptr || !visited.insert(blockId).second) {
          continue;
        }
        for (const MirBlockId successor :
             terminatorSuccessors(block->terminator)) {
          const MirBlock *target = module.findBlock(successor);
          if (target != nullptr &&
              target->programInitializationStep == step.id) {
            pending.push_back(successor);
          }
        }
      }
      return false;
    };
    if (located.block != nullptr && reentersPublishedBlock(located.block->id)) {
      add("program-initialization CFG can execute storage Initialize more "
          "than once",
          located.block->id, step.storageInitialization);
    }
    if (step.role == ProgramInitializationStepRole::Initializer &&
        markers.size() == 1 &&
        reentersPublishedBlock(markers.front().block->id)) {
      add("program-initialization CFG can publish a full expression more "
          "than once",
          markers.front().block->id, markers.front().instruction->id);
    }
    if (boundary == nullptr || located.block == nullptr || !dominance ||
        !dominance->dominates(located.block->id, boundary->id)) {
      add("program storage Initialize does not dominate the dense transition "
          "or final Exit",
          step.entryBlock, step.storageInitialization);
    }
    if (step.role == ProgramInitializationStepRole::Initializer &&
        (markers.size() != 1 || boundary == nullptr || !dominance ||
         !dominance->dominates(markers.front().block->id, boundary->id))) {
      add("dynamic full-expression marker does not dominate the dense "
          "transition or final Exit",
          step.entryBlock,
          markers.empty() ? 0 : markers.front().instruction->id);
    }
  }

  if (module.fullExpressions.size() != fullExpressions.size()) {
    add("module full-expression inventory is not exactly the dynamic steps");
  }
  const auto placeIsUsed = [&](MirPlaceId id) {
    if (std::any_of(module.loans.begin(), module.loans.end(),
                    [&](const MirLoan &loan) { return loan.source == id; }) ||
        std::any_of(
            module.dropObligations.begin(), module.dropObligations.end(),
            [&](const MirDropObligation &drop) { return drop.place == id; })) {
      return true;
    }
    for (const MirBlock &block : module.blocks) {
      if (block.terminator.value && block.terminator.value->place == id) {
        return true;
      }
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.destination == id ||
            (instruction.receiver && instruction.receiver->place == id) ||
            std::any_of(instruction.operands.begin(),
                        instruction.operands.end(),
                        [&](const MirOperand &operand) {
                          return operand.place == id;
                        })) {
          return true;
        }
      }
    }
    return false;
  };
  for (const MirPlace &place : module.places) {
    if (place.root == MirPlaceRootKind::This) {
      add("Module program initialization contains a This-root place");
      continue;
    }
    const bool bindingRoot = place.root == MirPlaceRootKind::Binding;
    const bool symbolRoot = place.root == MirPlaceRootKind::Symbol;
    const bool rootAxesExact =
        (bindingRoot || symbolRoot)
            ? place.capture == 0 && place.temporary == 0 && place.value == 0 &&
                  place.loan == 0 && (bindingRoot || place.binding == 0)
            : place.binding == 0 && place.symbol == 0 && place.capture == 0 &&
                  (place.root == MirPlaceRootKind::Temporary
                       ? place.value == 0 && place.loan == 0
                       : place.temporary == 0) &&
                  (place.root == MirPlaceRootKind::Value ? place.loan == 0
                                                         : place.value == 0) &&
                  (place.root == MirPlaceRootKind::Loan ? place.value == 0
                                                        : place.loan == 0);
    if (!rootAxesExact) {
      add("Module place retains identities outside its exact root kind");
    }
    if (place.root == MirPlaceRootKind::Temporary &&
        (place.sourceValue == 0 || !placeIsUsed(place.id))) {
      add("Module contains an orphan program-initialization temporary place");
    }
    if (!bindingRoot && !symbolRoot) {
      continue;
    }
    std::optional<ProgramInitializationStepId> identity;
    bool exact = true;
    const auto requireIdentity = [&](ProgramInitializationStepId candidate) {
      exact = exact && candidate != 0 && (!identity || *identity == candidate);
      if (candidate != 0 && !identity) {
        identity = candidate;
      }
    };
    if (place.root == MirPlaceRootKind::Binding) {
      const auto binding = bindingSteps.find(place.binding);
      requireIdentity(binding == bindingSteps.end() ? 0 : binding->second);
    }
    const auto symbol = symbolSteps.find(place.symbol);
    requireIdentity(symbol == symbolSteps.end() ? 0 : symbol->second);
    if (place.key) {
      const auto keyed = symbolSteps.find(place.key->root);
      requireIdentity(keyed == symbolSteps.end() ? 0 : keyed->second);
    }
    if (!exact) {
      add("module binding/symbol-root place does not name one exact planned "
          "storage step");
    }
  }

  const auto stepForPlace = [&](MirPlaceId placeId, const auto &resolve,
                                std::unordered_set<MirPlaceId> &visiting)
      -> std::optional<ProgramInitializationStepId> {
    const MirPlace *place = module.findPlace(placeId);
    if (place == nullptr || !visiting.insert(placeId).second) {
      return ProgramInitializationStepId{0};
    }
    std::optional<ProgramInitializationStepId> selected;
    const auto select = [&](ProgramInitializationStepId candidate) {
      if (candidate == 0) {
        return true;
      }
      if (selected && *selected != candidate) {
        return false;
      }
      selected = candidate;
      return true;
    };
    if (const auto found = bindingSteps.find(place->binding);
        found != bindingSteps.end() && !select(found->second)) {
      return ProgramInitializationStepId{0};
    }
    if (const auto found = symbolSteps.find(place->symbol);
        found != symbolSteps.end() && !select(found->second)) {
      return ProgramInitializationStepId{0};
    }
    if (place->key) {
      if (const auto found = symbolSteps.find(place->key->root);
          found != symbolSteps.end() && !select(found->second)) {
        return ProgramInitializationStepId{0};
      }
    }
    if (place->root == MirPlaceRootKind::Loan) {
      const MirLoan *loan = module.findLoan(place->loan);
      if (loan == nullptr) {
        return ProgramInitializationStepId{0};
      }
      const std::optional<ProgramInitializationStepId> source =
          resolve(loan->source, resolve, visiting);
      if (source && (*source == 0 || !select(*source))) {
        return ProgramInitializationStepId{0};
      }
    }
    visiting.erase(placeId);
    return selected;
  };
  const auto resolvePlaceStep = [&](MirPlaceId placeId) {
    std::unordered_set<MirPlaceId> visiting;
    return stepForPlace(placeId, stepForPlace, visiting);
  };
  const auto resolveLoanStep = [&](MirLoanId loanId) {
    const MirLoan *loan = module.findLoan(loanId);
    return loan == nullptr
               ? std::optional<
                     ProgramInitializationStepId>{ProgramInitializationStepId{
                     0}}
               : resolvePlaceStep(loan->source);
  };
  const auto legalPriorStep =
      [&](std::optional<ProgramInitializationStepId> target,
          ProgramInitializationStepId current, bool sameStepInitialization) {
        if (!target) {
          return true;
        }
        if (*target == 0) {
          return false;
        }
        return *target < current ||
               (sameStepInitialization && *target == current);
      };
  const auto legalPriorAccess = [&](MirPlaceId placeId,
                                    ProgramInitializationStepId current,
                                    bool sameStepInitialization) {
    return legalPriorStep(resolvePlaceStep(placeId), current,
                          sameStepInitialization);
  };

  std::size_t exits = 0;
  for (const MirBlock &block : module.blocks) {
    const ProgramInitializationStepId current = block.programInitializationStep;
    const MirProgramInitializationStep *step = plan.findStep(current);
    for (const MirInstruction &instruction : block.instructions) {
      const bool exactInitialization =
          step != nullptr && instruction.id == step->storageInitialization &&
          instruction.kind == MirInstructionKind::Initialize &&
          instruction.destination == step->storagePlace;
      if (instruction.destination &&
          !legalPriorAccess(*instruction.destination, current,
                            exactInitialization)) {
        add("program-storage destination is not initialized/accessed in step "
            "order",
            block.id, instruction.id);
      }
      const auto checkOperand = [&](const MirOperand &operand) {
        if (operand.place != 0 &&
            !legalPriorAccess(operand.place, current, false)) {
          add("program-storage operand/receiver is not accessed strictly "
              "after its step",
              block.id, instruction.id);
        }
        if (operand.loan != 0 &&
            !legalPriorStep(resolveLoanStep(operand.loan), current, false)) {
          add("loan operand/receiver accesses program storage before its "
              "step",
              block.id, instruction.id);
        }
      };
      if (instruction.receiver) {
        checkOperand(*instruction.receiver);
      }
      for (const MirOperand &operand : instruction.operands) {
        checkOperand(operand);
      }
      if (instruction.programConstantSubstitution &&
          (step == nullptr ||
           step->role != ProgramInitializationStepRole::Initializer ||
           instruction.kind != MirInstructionKind::Compute)) {
        add("program-constant substitution is outside an exact dynamic "
            "materialization step",
            block.id, instruction.id);
      }
    }
    if (block.terminator.value && block.terminator.value->place != 0 &&
        !legalPriorAccess(block.terminator.value->place, current, false)) {
      add("terminator accesses program storage before its step", block.id);
    }
    if (block.terminator.value && block.terminator.value->loan != 0 &&
        !legalPriorStep(resolveLoanStep(block.terminator.value->loan), current,
                        false)) {
      add("terminator loan accesses program storage before its step", block.id);
    }
    for (const MirBlockId successor : terminatorSuccessors(block.terminator)) {
      const MirBlock *target = module.findBlock(successor);
      if (target == nullptr) {
        continue;
      }
      const bool sameStep = target->programInitializationStep == current;
      const bool failureEdge =
          block.terminator.kind == MirTerminatorKind::Invoke &&
          block.terminator.elseTarget == successor;
      const bool exactNext = current < plan.steps.size() &&
                             target->programInitializationStep == current + 1 &&
                             target->id == plan.steps[current].entryBlock;
      if ((failureEdge && !sameStep) ||
          (!failureEdge && !sameStep && !exactNext)) {
        add("normal program-initialization edge skips or escapes its dense "
            "step",
            block.id);
      }
    }
    if (block.terminator.kind == MirTerminatorKind::Exit) {
      ++exits;
      if (current != plan.steps.size()) {
        add("program initialization exits before the final step", block.id);
      }
    } else if (block.terminator.kind == MirTerminatorKind::PropagateFailure ||
               block.terminator.kind ==
                   MirTerminatorKind::TerminateCleanupFailure) {
      if (block.activeFailure == 0) {
        add("program-initialization failure endpoint has no active failure",
            block.id);
      }
    } else if (block.terminator.kind == MirTerminatorKind::Return ||
               block.terminator.kind == MirTerminatorKind::Unreachable ||
               block.terminator.kind == MirTerminatorKind::ContainFailure ||
               block.terminator.kind == MirTerminatorKind::None) {
      add("program-initialization block has a non-normal terminator", block.id);
    }
  }
  if (exits != 1) {
    add("final program-initialization step must reach exactly one Exit");
  }

  for (std::size_t index = 0; index < stepBlocks.size(); ++index) {
    const ProgramInitializationStepId stepId = index + 1;
    const MirBlockId nextEntry =
        index + 1 < plan.steps.size() ? plan.steps[index + 1].entryBlock : 0;
    std::unordered_set<MirBlockId> canReachBoundary;
    bool changed = true;
    while (changed) {
      changed = false;
      for (const MirBlock *block : stepBlocks[index]) {
        if (block->activeFailure != 0 || canReachBoundary.contains(block->id)) {
          continue;
        }
        std::vector<MirBlockId> outgoing =
            terminatorSuccessors(block->terminator);
        if (block->terminator.kind == MirTerminatorKind::Invoke) {
          outgoing = {block->terminator.target};
        }
        const bool boundary =
            (nextEntry != 0 && std::find(outgoing.begin(), outgoing.end(),
                                         nextEntry) != outgoing.end()) ||
            (nextEntry == 0 &&
             block->terminator.kind == MirTerminatorKind::Exit);
        const bool reachesKnown = std::any_of(
            outgoing.begin(), outgoing.end(), [&](MirBlockId successor) {
              const MirBlock *target = module.findBlock(successor);
              return target != nullptr && target->activeFailure == 0 &&
                     target->programInitializationStep == stepId &&
                     canReachBoundary.contains(successor);
            });
        if (boundary || reachesKnown) {
          canReachBoundary.insert(block->id);
          changed = true;
        }
      }
    }
    const std::size_t normalBlocks = static_cast<std::size_t>(std::count_if(
        stepBlocks[index].begin(), stepBlocks[index].end(),
        [](const MirBlock *block) { return block->activeFailure == 0; }));
    if (canReachBoundary.size() != normalBlocks) {
      add("program-initialization step has a block that cannot reach its "
          "dense successor or final Exit",
          plan.steps[index].entryBlock);
    }
  }
  return result;
}

[[nodiscard]] MirVerificationResult
verifyMirHostedStartup(const MirProgram &program) {
  MirVerificationResult result;
  const std::optional<MirHostedStartupPlan> &optionalPlan =
      program.hostedStartupPlan();
  const MirBody *body = program.hostedStartup();
  const auto add = [&](std::string message, MirBlockId block = 0,
                       MirInstructionId instruction = 0) {
    result.errors.push_back({.bodyKind = MirBodyKind::HostedStartup,
                             .owner = optionalPlan ? optionalPlan->entry : 0,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };

  std::vector<const MirFunctionInstance *> entries;
  for (const MirFunctionInstance &function : program.functionInstances()) {
    if (function.entryKind != ProgramEntryKind::None) {
      entries.push_back(&function);
    }
  }
  if (entries.empty()) {
    if (optionalPlan || body != nullptr) {
      add("hosted-startup authority exists without a program entry");
    }
    return result;
  }
  if (entries.size() != 1) {
    add("hosted-startup authority requires one exact program entry");
    return result;
  }
  if (!optionalPlan || body == nullptr) {
    add("program entry is missing hosted-startup MIR authority");
    return result;
  }
  const MirHostedStartupPlan &plan = *optionalPlan;
  const MirFunctionInstance &entry = *entries.front();
  if (plan.entry == 0 || plan.entry != entry.id ||
      plan.kind != entry.entryKind || plan.sourceAnchor.sourceUnit == 0 ||
      plan.sourceAnchor.end <= plan.sourceAnchor.start ||
      plan.sourceAnchor.line < 1 ||
      plan.programInitializationTarget !=
          MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0} ||
      plan.exitPolicy != MirHostedStartupExitPolicy::ImmediateExit70 ||
      entry.owner || entry.staticMember ||
      entry.linkage != LanguageLinkage::Gti ||
      entry.returnType != SemanticType::Int32 ||
      entry.definitionKind != MirDefinitionKind::Source ||
      !entry.externalSymbol.empty() || entry.constexprFunction ||
      entry.virtualMethod || entry.pureVirtual || entry.overrideMethod ||
      !entry.virtualRoots.empty() || entry.overloadedOperator ||
      !entry.callableParameters.empty() ||
      entry.receiverMutability != ReceiverMutability::ReadOnly ||
      entry.returnBorrowOrigin != BorrowOriginKind::None ||
      entry.returnBorrowParameter != 0 ||
      entry.returnBorrowAccess != AccessMode::ReadOnly ||
      entry.returnBorrowPlace || body->kind != MirBodyKind::HostedStartup ||
      findMirBody(program, {.kind = MirBodyKind::HostedStartup,
                            .owner = plan.entry}) != body) {
    add("hosted-startup plan identity, source anchor, or entry is invalid");
  }
  std::size_t maximumSourceBodyDomain = 0;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    if (address.kind == MirBodyKind::HostedStartup) {
      continue;
    }
    if (const MirBody *sourceBody = findMirBody(program, address)) {
      maximumSourceBodyDomain =
          std::max(maximumSourceBodyDomain, sourceBody->placeDomain.body);
    }
  }
  if (body->placeDomain.snapshot != program.module().placeDomain.snapshot ||
      body->placeDomain.body != maximumSourceBodyDomain + 1 ||
      body->placeDomain.revision != 0 || body->entry != 1) {
    add("hosted-startup place domain is not the exact final generated domain");
  }

  const bool callsProgramInitialization = std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
  const MirDefinedFailureEffects derivedFailureEffects =
      deriveMirDefinedFailureEffects(program);
  const auto exactFunctionFailureSummary =
      [&](const MirFunctionInstance *item) {
        return item != nullptr && item->id != 0 &&
               item->id <= derivedFailureEffects.functions.size() &&
               item->mayRaiseDefinedFailure ==
                   derivedFailureEffects.functions[item->id - 1];
      };
  const auto exactConstructorFailureSummary =
      [&](const MirConstructorInstance *item) {
        return item != nullptr && item->id != 0 &&
               item->id <= derivedFailureEffects.constructors.size() &&
               item->mayRaiseDefinedFailure ==
                   derivedFailureEffects.constructors[item->id - 1];
      };
  if (!exactFunctionFailureSummary(&entry)) {
    add("hosted entry defined-failure summary is not exactly MIR-derived");
  }
  const bool moduleMayRaise = std::any_of(
      program.module().blocks.begin(), program.module().blocks.end(),
      [](const MirBlock &block) {
        return std::any_of(block.instructions.begin(), block.instructions.end(),
                           [](const MirInstruction &instruction) {
                             return !instruction.definedFailure.empty();
                           });
      });
  const auto propagation = [](FailurePropagationKind kind, bool mayRaise) {
    DefinedFailureOperation failure;
    failure.propagation = mayRaise ? kind : FailurePropagationKind::None;
    return failure;
  };
  const auto failureBehavior = [](bool mayRaise) {
    return mayRaise ? MirHostedStartupFailureBehavior::Propagate
                    : MirHostedStartupFailureBehavior::None;
  };
  const auto generatedInfo = [](const SemanticType &type,
                                ValueCategory category = ValueCategory::Value,
                                AccessMode access = AccessMode::ReadOnly) {
    return ExpressionInfo{.type = type,
                          .category = category,
                          .access = access,
                          .traits = semanticTraits(type)};
  };
  const auto instructionAt = [&](MirBlockId blockId, MirInstructionId id) {
    const MirBlock *block = body->findBlock(blockId);
    if (block == nullptr) {
      return static_cast<const MirInstruction *>(nullptr);
    }
    const auto found =
        std::find_if(block->instructions.begin(), block->instructions.end(),
                     [id](const MirInstruction &instruction) {
                       return instruction.id == id;
                     });
    return found == block->instructions.end() ? nullptr : &*found;
  };

  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    const MirHostedStartupOperation &operation = plan.operations[index];
    const bool instruction = operation.instruction != 0;
    if (operation.id != index + 1 ||
        operation.kind >= MirHostedStartupOperationKind::Count ||
        operation.failureBehavior >= MirHostedStartupFailureBehavior::Count ||
        operation.block == 0 || operation.block > body->blocks.size() ||
        instruction == operation.terminator ||
        (operation.place != 0 &&
         (operation.place > body->places.size() ||
          body->places[operation.place - 1].hostedStartupOperation !=
              operation.id)) ||
        (operation.value != 0 &&
         (operation.value > body->values.size() ||
          body->values[operation.value - 1].hostedStartupOperation !=
              operation.id)) ||
        (operation.dropObligation != 0 &&
         (operation.dropObligation > body->dropObligations.size() ||
          body->dropObligations[operation.dropObligation - 1]
                  .hostedStartupOperation != operation.id)) ||
        (operation.failureRecord != 0 &&
         (operation.failureRecord > body->failureRecords.size() ||
          body->failureRecords[operation.failureRecord - 1]
                  .hostedStartupOperation != operation.id)) ||
        (operation.cleanupBoundary != 0 &&
         (operation.cleanupBoundary > body->cleanupBoundaries.size() ||
          body->cleanupBoundaries[operation.cleanupBoundary - 1]
                  .hostedStartupOperation != operation.id))) {
      add("hosted-startup operation row has an invalid dense identity or "
          "entity reference",
          operation.block, operation.instruction);
      continue;
    }
    if (instruction) {
      const MirInstruction *record =
          instructionAt(operation.block, operation.instruction);
      if (record == nullptr || record->hostedStartupOperation != operation.id) {
        add("hosted-startup operation does not own its exact instruction",
            operation.block, operation.instruction);
      }
    } else {
      const MirBlock *block = body->findBlock(operation.block);
      if (block == nullptr ||
          block->terminator.hostedStartupOperation != operation.id) {
        add("hosted-startup operation does not own its exact terminator",
            operation.block);
      }
    }
  }
  for (const MirPlace &place : body->places) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(place.hostedStartupOperation);
    if (operation == nullptr || operation->place != place.id) {
      add("generated hosted-startup place is orphaned or multiply owned");
    }
  }
  for (const MirValue &value : body->values) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(value.hostedStartupOperation);
    if (operation == nullptr || operation->value != value.id) {
      add("generated hosted-startup value is orphaned or multiply owned");
    }
  }
  for (const MirDropObligation &drop : body->dropObligations) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(drop.hostedStartupOperation);
    if (operation == nullptr || operation->dropObligation != drop.id) {
      add("generated hosted-startup drop is orphaned or multiply owned");
    }
  }
  for (const MirFailureRecord &record : body->failureRecords) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(record.hostedStartupOperation);
    if (operation == nullptr || operation->failureRecord != record.id) {
      add("generated hosted-startup failure record is orphaned or multiply "
          "owned");
    }
  }
  for (const MirCleanupBoundary &boundary : body->cleanupBoundaries) {
    const MirHostedStartupOperation *operation =
        plan.findOperation(boundary.hostedStartupOperation);
    if (operation == nullptr || operation->cleanupBoundary != boundary.id) {
      add("generated hosted-startup cleanup boundary is orphaned or multiply "
          "owned");
    }
  }
  for (const MirBlock &block : body->blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const MirHostedStartupOperation *operation =
          plan.findOperation(instruction.hostedStartupOperation);
      if (operation == nullptr || operation->instruction != instruction.id ||
          operation->block != block.id) {
        add("generated hosted-startup instruction is orphaned or multiply "
            "owned",
            block.id, instruction.id);
      }
      for (const DefinedFailureOrigin &origin :
           instruction.definedFailure.localOrigins) {
        if (std::any_of(origin.outcomes.begin(), origin.outcomes.end(),
                        [](const DefinedFailureOutcome &outcome) {
                          return outcome.detail ==
                                 DefinedFailureDetail::HostedArguments;
                        })) {
          add("hosted-startup operation produces the reserved hosted_arguments "
              "failure detail",
              block.id, instruction.id);
        }
      }
    }
    const MirHostedStartupOperation *operation =
        plan.findOperation(block.terminator.hostedStartupOperation);
    if (operation == nullptr || !operation->terminator ||
        operation->block != block.id) {
      add("generated hosted-startup terminator is orphaned or multiply owned",
          block.id);
    }
  }

  struct TakenOperation {
    const MirHostedStartupOperation *actual = nullptr;
    MirHostedStartupOperation expected;
  };
  std::size_t nextOperation = 1;
  MirInstructionId nextInstruction = 1;
  MirPlaceId nextPlace = 1;
  MirValueId nextValue = 1;
  MirDropObligationId nextDrop = 1;
  MirFailureRecordId nextFailureRecord = 1;
  std::size_t nextCleanupBoundary = 1;
  const auto take = [&](MirHostedStartupOperationKind kind,
                        MirHostedStartupFailureBehavior behavior,
                        MirBlockId block, bool instruction, bool place,
                        bool value, bool drop, bool failureRecord = false,
                        bool cleanupBoundary = false) {
    TakenOperation taken;
    taken.expected.id = nextOperation++;
    taken.expected.kind = kind;
    taken.expected.failureBehavior = behavior;
    taken.expected.block = block;
    if (instruction) {
      taken.expected.instruction = nextInstruction++;
    } else {
      taken.expected.terminator = true;
    }
    if (place) {
      taken.expected.place = nextPlace++;
    }
    if (value) {
      taken.expected.value = nextValue++;
    }
    if (drop) {
      taken.expected.dropObligation = nextDrop++;
    }
    if (failureRecord) {
      taken.expected.failureRecord = nextFailureRecord++;
    }
    if (cleanupBoundary) {
      taken.expected.cleanupBoundary = nextCleanupBoundary++;
    }
    taken.actual = plan.findOperation(taken.expected.id);
    if (taken.actual == nullptr || *taken.actual != taken.expected) {
      add("hosted-startup dense operation sequence or per-kind entity union "
          "is not exact",
          block, taken.expected.instruction);
    }
    return taken;
  };
  const auto exactValue = [&](const TakenOperation &operation,
                              const ExpressionInfo &info) {
    if (operation.expected.value == 0 ||
        operation.expected.value > body->values.size()) {
      return;
    }
    const MirValue expected{.id = operation.expected.value,
                            .hostedStartupOperation = operation.expected.id,
                            .sourceValue = 0,
                            .info = info,
                            .definitionBlock = operation.expected.block,
                            .definition = operation.expected.instruction};
    if (body->values[operation.expected.value - 1] != expected) {
      add("hosted-startup generated value is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactPlace = [&](const TakenOperation &operation,
                              MirPlace expected) {
    if (operation.expected.place == 0 ||
        operation.expected.place > body->places.size()) {
      return;
    }
    expected.id = operation.expected.place;
    expected.hostedStartupOperation = operation.expected.id;
    if (body->places[operation.expected.place - 1] != expected) {
      add("hosted-startup generated place is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactDrop = [&](const TakenOperation &operation,
                             MirDropObligation expected) {
    if (operation.expected.dropObligation == 0 ||
        operation.expected.dropObligation > body->dropObligations.size()) {
      return;
    }
    expected.id = operation.expected.dropObligation;
    expected.hostedStartupOperation = operation.expected.id;
    expected.constructionOrder = operation.expected.dropObligation;
    if (body->dropObligations[operation.expected.dropObligation - 1] !=
        expected) {
      add("hosted-startup generated drop is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactInstruction = [&](const TakenOperation &operation,
                                    MirInstruction expected) {
    const MirInstruction *actual =
        instructionAt(operation.expected.block, operation.expected.instruction);
    expected.id = operation.expected.instruction;
    expected.hostedStartupOperation = operation.expected.id;
    if (actual == nullptr || *actual != expected) {
      add("hosted-startup generated instruction is not its exact canonical "
          "stage",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactTerminator = [&](const TakenOperation &operation,
                                   MirTerminator expected) {
    const MirBlock *block = body->findBlock(operation.expected.block);
    expected.hostedStartupOperation = operation.expected.id;
    if (block == nullptr || block->terminator != expected) {
      add("hosted-startup generated terminator is not its exact canonical "
          "stage",
          operation.expected.block);
    }
  };
  const auto exactFailureRecord = [&](const TakenOperation &operation,
                                      MirFailureRecord expected) {
    if (operation.expected.failureRecord == 0 ||
        operation.expected.failureRecord > body->failureRecords.size()) {
      return;
    }
    expected.id = operation.expected.failureRecord;
    expected.hostedStartupOperation = operation.expected.id;
    if (body->failureRecords[operation.expected.failureRecord - 1] !=
        expected) {
      add("hosted-startup generated failure record is not exact",
          operation.expected.block, operation.expected.instruction);
    }
  };
  const auto exactCleanupBoundary =
      [&](const TakenOperation &operation,
          std::vector<MirDropObligationId> obligations) {
        if (operation.expected.cleanupBoundary == 0 ||
            operation.expected.cleanupBoundary >
                body->cleanupBoundaries.size()) {
          return;
        }
        const MirCleanupBoundary expected{
            .id = operation.expected.cleanupBoundary,
            .hostedStartupOperation = operation.expected.id,
            .kind = MirCleanupBoundaryKind::Failure,
            .obligations = std::move(obligations)};
        if (body->cleanupBoundaries[operation.expected.cleanupBoundary - 1] !=
            expected) {
          add("hosted-startup generated cleanup boundary is not exact",
              operation.expected.block, operation.expected.instruction);
        }
      };
  MirBlockId nextGeneratedBlock = 2;
  const auto destructorMayRaise = [&](MirDropObligationId obligation) {
    const MirDropObligation *drop = body->findDropObligation(obligation);
    return drop != nullptr &&
           mirDropMayRaiseDefinedFailure(program, drop->dropType);
  };
  const auto takeFailureRoute = [&](MirBlockId producerBlock,
                                    MirInstructionId producerInstruction,
                                    const std::vector<MirDropObligationId>
                                        &activeDrops,
                                    MirDropObligationId successDrop = 0) {
    const MirBlockId normalBlock = nextGeneratedBlock++;
    const MirBlockId failureBlock = nextGeneratedBlock++;
    const TakenOperation route =
        take(MirHostedStartupOperationKind::RouteOperationFailure,
             MirHostedStartupFailureBehavior::None, producerBlock, false, false,
             false, false, true);
    exactFailureRecord(route, {.producerBlock = producerBlock,
                               .producerInstruction = producerInstruction,
                               .parameterBlock = failureBlock});
    MirTerminator invoke{.kind = MirTerminatorKind::Invoke,
                         .invokeInstruction = producerInstruction,
                         .failureRecord = route.expected.failureRecord,
                         .target = normalBlock,
                         .elseTarget = failureBlock};
    if (successDrop != 0) {
      invoke.successLifecycle = {
          {.kind = MirLifecycleEventKind::Initialize, .target = successDrop}};
    }
    exactTerminator(route, std::move(invoke));

    MirBlockId cleanupBlock = failureBlock;
    std::vector<MirDropObligationId> boundaryDrops;
    boundaryDrops.reserve(activeDrops.size());
    for (auto candidate = activeDrops.rbegin(); candidate != activeDrops.rend();
         ++candidate) {
      const MirDropObligation *drop = body->findDropObligation(*candidate);
      const MirPlace *place =
          drop == nullptr ? nullptr : body->findPlace(drop->place);
      const bool mayRaise = destructorMayRaise(*candidate);
      const TakenOperation cleanupDrop = take(
          MirHostedStartupOperationKind::DropFailureCleanup,
          failureBehavior(mayRaise), cleanupBlock, true, false, false, false);
      if (drop != nullptr && place != nullptr) {
        exactInstruction(
            cleanupDrop,
            {.kind = MirInstructionKind::Drop,
             .destination = drop->place,
             .definedFailure =
                 propagation(FailurePropagationKind::Destructor, mayRaise),
             .info = generatedInfo(drop->dropType.type, ValueCategory::Place,
                                   AccessMode::Mutable),
             .lifecycle = {{.kind = MirLifecycleEventKind::Drop,
                            .source = *candidate,
                            .failureCleanup = true}}});
      }
      boundaryDrops.push_back(*candidate);
      if (!mayRaise) {
        continue;
      }
      const MirBlockId cleanupNormal = nextGeneratedBlock++;
      const MirBlockId secondaryBlock = nextGeneratedBlock++;
      const TakenOperation cleanupRoute =
          take(MirHostedStartupOperationKind::RouteCleanupFailure,
               MirHostedStartupFailureBehavior::None, cleanupBlock, false,
               false, false, false, true);
      exactFailureRecord(cleanupRoute, {.producerBlock = cleanupBlock,
                                        .producerInstruction =
                                            cleanupDrop.expected.instruction,
                                        .parameterBlock = secondaryBlock});
      exactTerminator(cleanupRoute,
                      {.kind = MirTerminatorKind::Invoke,
                       .invokeInstruction = cleanupDrop.expected.instruction,
                       .failureRecord = cleanupRoute.expected.failureRecord,
                       .target = cleanupNormal,
                       .elseTarget = secondaryBlock});
      const TakenOperation terminate =
          take(MirHostedStartupOperationKind::TerminateCleanupFailure,
               MirHostedStartupFailureBehavior::None, secondaryBlock, false,
               false, false, false);
      exactTerminator(terminate,
                      {.kind = MirTerminatorKind::TerminateCleanupFailure,
                       .failureRecord = cleanupRoute.expected.failureRecord});
      cleanupBlock = cleanupNormal;
    }
    if (!boundaryDrops.empty()) {
      const TakenOperation end =
          take(MirHostedStartupOperationKind::EndFailureCleanup,
               MirHostedStartupFailureBehavior::None, cleanupBlock, true, false,
               false, false, false, true);
      exactCleanupBoundary(end, boundaryDrops);
      exactInstruction(end,
                       {.kind = MirInstructionKind::Lifecycle,
                        .cleanupBoundaryEnd = end.expected.cleanupBoundary});
    }
    const TakenOperation contain =
        take(MirHostedStartupOperationKind::ContainFailure,
             MirHostedStartupFailureBehavior::None, cleanupBlock, false, false,
             false, false);
    exactTerminator(contain, {.kind = MirTerminatorKind::ContainFailure,
                              .failureRecord = route.expected.failureRecord});
    return normalBlock;
  };

  const auto takeCThreadBoundary = [&](MirBlockId block, bool begin) {
    const auto targetId = begin ? plan.cThreadBegin : plan.cThreadEnd;
    if (!program.requiresCThreadRuntime()) {
      if (targetId != 0) {
        add("hosted C-thread boundary is present without a runtime "
            "requirement");
      }
      return;
    }
    const MirFunctionInstance *target = program.findFunctionInstance(targetId);
    if (target == nullptr ||
        target->cThreadOperation != (begin ? CThreadRuntimeOperation::Begin
                                           : CThreadRuntimeOperation::End) ||
        !exactCThreadRuntimeFunction(*target)) {
      add("hosted C-thread boundary has no exact runtime declaration");
    }
    const TakenOperation call =
        take(begin ? MirHostedStartupOperationKind::BeginCThreadRuntime
                   : MirHostedStartupOperationKind::EndCThreadRuntime,
             MirHostedStartupFailureBehavior::None, block, true, false, false,
             false);
    exactInstruction(call, {.kind = MirInstructionKind::Call,
                            .functionTarget = targetId,
                            .info = generatedInfo(SemanticType::Void)});
  };

  if (plan.kind == ProgramEntryKind::NoArguments) {
    MirBlockId currentBlock = 1;
    if (!entry.parameterTypes.empty() || plan.appendFunction != 0 ||
        plan.vectorConstructor != 0 || plan.stringConstructor != 0 ||
        plan.argumentIndexPlace != 0 || plan.argumentVectorPlace != 0 ||
        plan.stabilizedCount != 0 || plan.argumentVector != 0 ||
        !body->places.empty() || !body->dropObligations.empty()) {
      add("no-argument hosted-startup plan retains owned-argument state");
    }
    if (callsProgramInitialization) {
      const TakenOperation callBody =
          take(MirHostedStartupOperationKind::CallProgramInitialization,
               failureBehavior(moduleMayRaise), currentBlock, true, false,
               false, false);
      exactInstruction(callBody,
                       {.kind = MirInstructionKind::CallBody,
                        .definedFailure = propagation(
                            FailurePropagationKind::BodyCall, moduleMayRaise),
                        .bodyTarget = plan.programInitializationTarget,
                        .info = generatedInfo(SemanticType::Void)});
      if (moduleMayRaise) {
        currentBlock =
            takeFailureRoute(currentBlock, callBody.expected.instruction, {});
      }
    }
    takeCThreadBoundary(currentBlock, true);
    const TakenOperation callEntry =
        take(MirHostedStartupOperationKind::CallEntry,
             failureBehavior(entry.mayRaiseDefinedFailure), currentBlock, true,
             false, true, false);
    exactValue(callEntry, generatedInfo(SemanticType::Int32));
    exactInstruction(callEntry, {.kind = MirInstructionKind::Call,
                                 .result = callEntry.expected.value,
                                 .definedFailure = propagation(
                                     FailurePropagationKind::DirectCall,
                                     entry.mayRaiseDefinedFailure),
                                 .functionTarget = entry.id,
                                 .info = generatedInfo(SemanticType::Int32)});
    if (entry.mayRaiseDefinedFailure) {
      currentBlock =
          takeFailureRoute(currentBlock, callEntry.expected.instruction, {});
    }
    takeCThreadBoundary(currentBlock, false);
    const TakenOperation returnEntry =
        take(MirHostedStartupOperationKind::ReturnEntry,
             MirHostedStartupFailureBehavior::None, currentBlock, false, false,
             false, false);
    exactTerminator(returnEntry,
                    {.kind = MirTerminatorKind::Return,
                     .value = MirOperand{.kind = MirOperandKind::Value,
                                         .value = callEntry.expected.value,
                                         .type = SemanticType::Int32}});
    if (plan.entryResult != callEntry.expected.value) {
      add("no-argument hosted-startup result identity is invalid");
    }
  } else if (plan.kind == ProgramEntryKind::OwnedArguments) {
    MirBlockId currentBlock = 1;
    const bool entryShape =
        entry.parameterTypes.size() == 2 &&
        entry.parameterTypes[0] == SemanticType::Int32 &&
        entry.parameterTypes[1].kind == SemanticType::Class &&
        entry.parameterTypes[1].arguments.size() == 1 &&
        entry.parameterTypes[1].arguments.front().kind == SemanticType::Class &&
        entry.entryArgumentAppendTarget ==
            std::optional<HirFunctionInstanceId>{plan.appendFunction};
    const SemanticType vectorType =
        entryShape ? entry.parameterTypes[1] : SemanticType::Unknown;
    const SemanticType stringType =
        entryShape ? vectorType.arguments.front() : SemanticType::Unknown;
    const MirFunctionInstance *append =
        program.findFunctionInstance(plan.appendFunction);
    const MirConstructorInstance *vectorConstructor =
        program.findConstructorInstance(plan.vectorConstructor);
    const MirConstructorInstance *stringConstructor =
        program.findConstructorInstance(plan.stringConstructor);
    const MirClassInstance *vectorClass =
        vectorConstructor == nullptr
            ? nullptr
            : program.findClassInstance(vectorConstructor->owner);
    const MirClassInstance *stringClass =
        stringConstructor == nullptr
            ? nullptr
            : program.findClassInstance(stringConstructor->owner);
    const bool exactTargets =
        entryShape && append != nullptr && append->owner &&
        vectorClass != nullptr && stringClass != nullptr &&
        *append->owner == vectorClass->id && vectorClass->type == vectorType &&
        stringClass->type == stringType &&
        append->returnType == SemanticType::Void &&
        append->parameterTypes == std::vector<SemanticType>{stringType} &&
        !append->staticMember && append->entryKind == ProgramEntryKind::None &&
        append->linkage == LanguageLinkage::Gti &&
        append->externalSymbol.empty() && !append->constexprFunction &&
        append->receiverMutability == ReceiverMutability::Mutable &&
        !append->virtualMethod && !append->pureVirtual &&
        !append->overrideMethod && append->virtualRoots.empty() &&
        !append->overloadedOperator && append->callableParameters.empty() &&
        append->returnBorrowOrigin == BorrowOriginKind::None &&
        append->returnBorrowParameter == 0 &&
        append->returnBorrowAccess == AccessMode::ReadOnly &&
        !append->returnBorrowPlace &&
        append->definitionKind == MirDefinitionKind::Source &&
        exactFunctionFailureSummary(append) &&
        vectorConstructor->owner == vectorClass->id &&
        vectorConstructor->typeArguments.empty() &&
        vectorConstructor->valueArguments.empty() &&
        vectorConstructor->parameterTypes.empty() &&
        vectorConstructor->borrowOrigin == BorrowOriginKind::None &&
        vectorConstructor->borrowParameter == 0 &&
        vectorConstructor->borrowAccess == AccessMode::ReadOnly &&
        vectorConstructor->definitionKind == MirDefinitionKind::Source &&
        exactConstructorFailureSummary(vectorConstructor) &&
        stringConstructor->owner == stringClass->id &&
        stringConstructor->typeArguments.empty() &&
        stringConstructor->valueArguments.empty() &&
        stringConstructor->parameterTypes ==
            std::vector<SemanticType>{SemanticType::StringView} &&
        stringConstructor->borrowOrigin == BorrowOriginKind::None &&
        stringConstructor->borrowParameter == 0 &&
        stringConstructor->borrowAccess == AccessMode::ReadOnly &&
        stringConstructor->definitionKind == MirDefinitionKind::Source &&
        exactConstructorFailureSummary(stringConstructor) &&
        vectorClass->requiresActiveCleanup &&
        stringClass->requiresActiveCleanup;
    if (!exactTargets) {
      add("owned-argument hosted-startup targets, signatures, or CFG shape "
          "are invalid");
    }
    MirDropType vectorDropType{
        .type = vectorType,
        .classInstance = vectorClass == nullptr
                             ? std::nullopt
                             : std::optional{vectorClass->id},
        .destructor =
            vectorClass == nullptr ? std::nullopt : vectorClass->destructor,
        .requiresActiveCleanup =
            vectorClass != nullptr && vectorClass->requiresActiveCleanup};
    MirDropType stringDropType{
        .type = stringType,
        .classInstance = stringClass == nullptr
                             ? std::nullopt
                             : std::optional{stringClass->id},
        .destructor =
            stringClass == nullptr ? std::nullopt : stringClass->destructor,
        .requiresActiveCleanup =
            stringClass != nullptr && stringClass->requiresActiveCleanup};
    if (auto cleanup = mirUniqueCleanupType(program, vectorType))
      vectorDropType = *cleanup;
    if (auto cleanup = mirUniqueCleanupType(program, stringType))
      stringDropType = *cleanup;

    const DefinedFailureOrigin validateOrigin{
        .outcomes = {{.code = DefinedFailureCode::HostedRuntimeContractFailure,
                      .detail = DefinedFailureDetail::NegativeArgumentCount}},
        .sourceUnit = plan.sourceAnchor.sourceUnit,
        .start = plan.sourceAnchor.start,
        .end = plan.sourceAnchor.end,
        .line = plan.sourceAnchor.line};
    const DefinedFailureOrigin convertOrigin{
        .outcomes = {{.code = DefinedFailureCode::NumericConversionOutOfRange,
                      .detail = DefinedFailureDetail::HostedArgumentCount}},
        .sourceUnit = plan.sourceAnchor.sourceUnit,
        .start = plan.sourceAnchor.start,
        .end = plan.sourceAnchor.end,
        .line = plan.sourceAnchor.line};
    const std::optional<FailureSiteId> validateSite =
        program.failureMetadata().siteFor(validateOrigin);
    const std::optional<FailureSiteId> convertSite =
        program.failureMetadata().siteFor(convertOrigin);
    if (!validateSite || !convertSite || *validateSite == 0 ||
        *validateSite != *convertSite) {
      add("owned-argument hosted failures do not share one exact nonzero site");
    }
    const FailureSiteId hostedSite = validateSite.value_or(0);
    const FailureSiteDescriptor *hostedDescriptor =
        program.failureMetadata().findSite(hostedSite);
    const std::vector<DefinedFailureOutcome> expectedHostedOutcomes{
        {.code = DefinedFailureCode::NumericConversionOutOfRange,
         .detail = DefinedFailureDetail::HostedArgumentCount},
        {.code = DefinedFailureCode::HostedRuntimeContractFailure,
         .detail = DefinedFailureDetail::NegativeArgumentCount}};
    if (hostedDescriptor == nullptr ||
        hostedDescriptor->line != plan.sourceAnchor.line ||
        hostedDescriptor->start != plan.sourceAnchor.start ||
        hostedDescriptor->end != plan.sourceAnchor.end ||
        hostedDescriptor->outcomes != expectedHostedOutcomes) {
      add("owned-argument hosted failure site is not the exact two-outcome "
          "shared anchor");
    }
    const TakenOperation validate =
        take(MirHostedStartupOperationKind::ValidateArgumentCount,
             MirHostedStartupFailureBehavior::Detect, currentBlock, true, false,
             true, false);
    exactValue(validate, generatedInfo(SemanticType::Int64));
    exactInstruction(validate,
                     {.kind = MirInstructionKind::Load,
                      .result = validate.expected.value,
                      .definedFailure = {.localOrigins = {validateOrigin}},
                      .localFailureSites = {hostedSite},
                      .info = generatedInfo(SemanticType::Int64)});
    currentBlock =
        takeFailureRoute(currentBlock, validate.expected.instruction, {});

    const TakenOperation convert =
        take(MirHostedStartupOperationKind::ConvertArgumentCount,
             MirHostedStartupFailureBehavior::Detect, currentBlock, true, false,
             true, false);
    exactValue(convert, generatedInfo(SemanticType::Int32));
    exactInstruction(convert,
                     {.kind = MirInstructionKind::Compute,
                      .result = convert.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = validate.expected.value,
                                    .type = SemanticType::Int64}},
                      .operation = MirOperation::None,
                      .definedFailure = {.localOrigins = {convertOrigin}},
                      .localFailureSites = {hostedSite},
                      .info = generatedInfo(SemanticType::Int32)});
    currentBlock =
        takeFailureRoute(currentBlock, convert.expected.instruction, {});
    if (callsProgramInitialization) {
      const TakenOperation callBody =
          take(MirHostedStartupOperationKind::CallProgramInitialization,
               failureBehavior(moduleMayRaise), currentBlock, true, false,
               false, false);
      exactInstruction(callBody,
                       {.kind = MirInstructionKind::CallBody,
                        .definedFailure = propagation(
                            FailurePropagationKind::BodyCall, moduleMayRaise),
                        .bodyTarget = plan.programInitializationTarget,
                        .info = generatedInfo(SemanticType::Void)});
      if (moduleMayRaise) {
        currentBlock =
            takeFailureRoute(currentBlock, callBody.expected.instruction, {});
      }
    }

    takeCThreadBoundary(currentBlock, true);
    const TakenOperation vector =
        take(MirHostedStartupOperationKind::ConstructArgumentVector,
             failureBehavior(vectorConstructor != nullptr &&
                             vectorConstructor->mayRaiseDefinedFailure),
             currentBlock, true, true, true, true);
    exactValue(vector, generatedInfo(vectorType));
    exactPlace(vector, {.root = MirPlaceRootKind::Value,
                        .value = vector.expected.value,
                        .type = vectorType,
                        .access = AccessMode::Mutable,
                        .traits = semanticTraits(vectorType)});
    exactDrop(vector, {.kind = MirDropObligationKind::Value,
                       .place = vector.expected.place,
                       .generatedValue = vector.expected.value,
                       .dropType = vectorDropType});
    const bool vectorMayRaise = vectorConstructor != nullptr &&
                                vectorConstructor->mayRaiseDefinedFailure;
    MirInstruction expectedVector{
        .kind = MirInstructionKind::Construct,
        .result = vector.expected.value,
        .definedFailure =
            propagation(FailurePropagationKind::Constructor, vectorMayRaise),
        .constructorTarget = plan.vectorConstructor,
        .info = generatedInfo(vectorType)};
    if (vectorMayRaise) {
      expectedVector.successResultDrop = vector.expected.dropObligation;
    } else {
      expectedVector.lifecycle = {{.kind = MirLifecycleEventKind::Initialize,
                                   .target = vector.expected.dropObligation}};
    }
    exactInstruction(vector, std::move(expectedVector));
    if (vectorMayRaise) {
      currentBlock = takeFailureRoute(currentBlock, vector.expected.instruction,
                                      {}, vector.expected.dropObligation);
    }

    const TakenOperation initializeIndex =
        take(MirHostedStartupOperationKind::InitializeArgumentIndex,
             MirHostedStartupFailureBehavior::None, currentBlock, true, true,
             false, false);
    exactPlace(initializeIndex,
               {.root = MirPlaceRootKind::Temporary,
                .temporary = 1,
                .type = SemanticType::Int32,
                .access = AccessMode::Mutable,
                .traits = semanticTraits(SemanticType::Int32)});
    exactInstruction(
        initializeIndex,
        {.kind = MirInstructionKind::Initialize,
         .destination = initializeIndex.expected.place,
         .operands = {{.kind = MirOperandKind::Constant,
                       .literal = Literal{std::uint64_t{0}},
                       .type = SemanticType::Int32}},
         .info = generatedInfo(SemanticType::Int32, ValueCategory::Place,
                               AccessMode::Mutable)});
    const MirBlockId loopHeaderBlock = nextGeneratedBlock++;
    const TakenOperation enterLoop =
        take(MirHostedStartupOperationKind::EnterArgumentLoop,
             MirHostedStartupFailureBehavior::None, currentBlock, false, false,
             false, false);
    exactTerminator(enterLoop, {.kind = MirTerminatorKind::Goto,
                                .target = loopHeaderBlock});

    const TakenOperation loadIndex =
        take(MirHostedStartupOperationKind::LoadArgumentIndex,
             MirHostedStartupFailureBehavior::None, loopHeaderBlock, true,
             false, true, false);
    exactValue(loadIndex, generatedInfo(SemanticType::Int32));
    exactInstruction(loadIndex,
                     {.kind = MirInstructionKind::Load,
                      .result = loadIndex.expected.value,
                      .operands = {{.kind = MirOperandKind::Copy,
                                    .place = initializeIndex.expected.place,
                                    .type = SemanticType::Int32}},
                      .info = generatedInfo(SemanticType::Int32)});
    const TakenOperation testIndex =
        take(MirHostedStartupOperationKind::TestArgumentIndex,
             MirHostedStartupFailureBehavior::None, loopHeaderBlock, true,
             false, true, false);
    exactValue(testIndex, generatedInfo(SemanticType::Bool));
    exactInstruction(testIndex,
                     {.kind = MirInstructionKind::Compute,
                      .result = testIndex.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = loadIndex.expected.value,
                                    .type = SemanticType::Int32},
                                   {.kind = MirOperandKind::Value,
                                    .value = convert.expected.value,
                                    .type = SemanticType::Int32}},
                      .operation = MirOperation::Less,
                      .info = generatedInfo(SemanticType::Bool)});
    const MirBlockId loopBodyBlock = nextGeneratedBlock++;
    const MirBlockId entryCallBlock = nextGeneratedBlock++;
    const TakenOperation branchLoop =
        take(MirHostedStartupOperationKind::BranchArgumentLoop,
             MirHostedStartupFailureBehavior::None, loopHeaderBlock, false,
             false, false, false);
    exactTerminator(branchLoop,
                    {.kind = MirTerminatorKind::Branch,
                     .value = MirOperand{.kind = MirOperandKind::Value,
                                         .value = testIndex.expected.value,
                                         .type = SemanticType::Bool},
                     .target = loopBodyBlock,
                     .elseTarget = entryCallBlock});

    MirBlockId iterationBlock = loopBodyBlock;

    const TakenOperation readView =
        take(MirHostedStartupOperationKind::ReadArgumentView,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(readView, generatedInfo(SemanticType::StringView));
    exactInstruction(readView,
                     {.kind = MirInstructionKind::Compute,
                      .result = readView.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = loadIndex.expected.value,
                                    .type = SemanticType::Int32}},
                      .operation = MirOperation::None,
                      .info = generatedInfo(SemanticType::StringView)});
    const TakenOperation stringInput =
        take(MirHostedStartupOperationKind::PrepareStringConstructorArgument,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(stringInput, generatedInfo(SemanticType::StringView));
    exactInstruction(stringInput,
                     {.kind = MirInstructionKind::CallInput,
                      .callInputRole = MirCallInputRole::Argument,
                      .callInputKind = HirCallInputKind::Value,
                      .result = stringInput.expected.value,
                      .operands = {{.kind = MirOperandKind::Value,
                                    .value = readView.expected.value,
                                    .type = SemanticType::StringView}},
                      .info = generatedInfo(SemanticType::StringView)});
    const bool stringMayRaise = stringConstructor != nullptr &&
                                stringConstructor->mayRaiseDefinedFailure;
    const TakenOperation string =
        take(MirHostedStartupOperationKind::ConstructArgumentString,
             failureBehavior(stringMayRaise), iterationBlock, true, true, true,
             true);
    exactValue(string, generatedInfo(stringType));
    exactPlace(string, {.root = MirPlaceRootKind::Value,
                        .value = string.expected.value,
                        .type = stringType,
                        .access = AccessMode::Mutable,
                        .traits = semanticTraits(stringType)});
    exactDrop(string, {.kind = MirDropObligationKind::Value,
                       .place = string.expected.place,
                       .generatedValue = string.expected.value,
                       .dropType = stringDropType});
    MirInstruction expectedString{
        .kind = MirInstructionKind::Construct,
        .result = string.expected.value,
        .operands = {{.kind = MirOperandKind::Value,
                      .value = stringInput.expected.value,
                      .type = SemanticType::StringView}},
        .parameterTypes = {SemanticType::StringView},
        .definedFailure =
            propagation(FailurePropagationKind::Constructor, stringMayRaise),
        .constructorTarget = plan.stringConstructor,
        .info = generatedInfo(stringType)};
    if (stringMayRaise) {
      expectedString.successResultDrop = string.expected.dropObligation;
    } else {
      expectedString.lifecycle = {{.kind = MirLifecycleEventKind::Initialize,
                                   .target = string.expected.dropObligation}};
    }
    exactInstruction(string, std::move(expectedString));
    if (stringMayRaise) {
      iterationBlock = takeFailureRoute(
          iterationBlock, string.expected.instruction,
          {vector.expected.dropObligation}, string.expected.dropObligation);
    }

    const TakenOperation appendReceiver =
        take(MirHostedStartupOperationKind::PrepareAppendReceiver,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(appendReceiver, generatedInfo(vectorType));
    exactInstruction(appendReceiver,
                     {.kind = MirInstructionKind::CallInput,
                      .callInputRole = MirCallInputRole::Receiver,
                      .callInputKind = HirCallInputKind::MutableBorrow,
                      .result = appendReceiver.expected.value,
                      .operands = {{.kind = MirOperandKind::BorrowWrite,
                                    .place = vector.expected.place,
                                    .type = vectorType}},
                      .info = generatedInfo(vectorType)});
    const TakenOperation appendArgument =
        take(MirHostedStartupOperationKind::PrepareAppendArgumentMove,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, true,
             true, true);
    exactValue(appendArgument, generatedInfo(stringType));
    exactPlace(appendArgument, {.root = MirPlaceRootKind::Temporary,
                                .temporary = 2,
                                .type = stringType,
                                .access = AccessMode::Mutable,
                                .traits = semanticTraits(stringType)});
    exactDrop(appendArgument, {.kind = MirDropObligationKind::PreparedParameter,
                               .place = appendArgument.expected.place,
                               .generatedValue = appendArgument.expected.value,
                               .dropType = stringDropType});
    exactInstruction(
        appendArgument,
        {.kind = MirInstructionKind::CallInput,
         .callInputRole = MirCallInputRole::Argument,
         .callInputKind = HirCallInputKind::MoveValue,
         .preparedParameterDrop = appendArgument.expected.dropObligation,
         .result = appendArgument.expected.value,
         .destination = appendArgument.expected.place,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = string.expected.value,
                       .type = stringType}},
         .info = generatedInfo(stringType),
         .lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                        .source = string.expected.dropObligation,
                        .target = appendArgument.expected.dropObligation}}});
    const bool appendMayRaise =
        append != nullptr && append->mayRaiseDefinedFailure;
    const TakenOperation appendCall =
        take(MirHostedStartupOperationKind::CallAppend,
             failureBehavior(appendMayRaise), iterationBlock, true, false,
             false, false);
    exactInstruction(
        appendCall,
        {.kind = MirInstructionKind::Call,
         .receiver = MirOperand{.kind = MirOperandKind::Value,
                                .value = appendReceiver.expected.value,
                                .type = vectorType},
         .operands = {{.kind = MirOperandKind::Value,
                       .value = appendArgument.expected.value,
                       .type = stringType}},
         .parameterTypes = {stringType},
         .definedFailure =
             propagation(FailurePropagationKind::DirectCall, appendMayRaise),
         .functionTarget = plan.appendFunction,
         .info = generatedInfo(SemanticType::Void),
         .lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                        .source = appendArgument.expected.dropObligation}}});
    if (appendMayRaise) {
      iterationBlock =
          takeFailureRoute(iterationBlock, appendCall.expected.instruction,
                           {vector.expected.dropObligation});
    }
    const TakenOperation advance =
        take(MirHostedStartupOperationKind::AdvanceArgumentIndex,
             MirHostedStartupFailureBehavior::None, iterationBlock, true, false,
             true, false);
    exactValue(advance, generatedInfo(SemanticType::Int32, ValueCategory::Place,
                                      AccessMode::Mutable));
    exactInstruction(advance, {.kind = MirInstructionKind::Modify,
                               .result = advance.expected.value,
                               .destination = initializeIndex.expected.place,
                               .operation = MirOperation::PreIncrement,
                               .info = generatedInfo(SemanticType::Int32,
                                                     ValueCategory::Place,
                                                     AccessMode::Mutable)});
    const TakenOperation continueLoop =
        take(MirHostedStartupOperationKind::ContinueArgumentLoop,
             MirHostedStartupFailureBehavior::None, iterationBlock, false,
             false, false, false);
    exactTerminator(continueLoop, {.kind = MirTerminatorKind::Goto,
                                   .target = loopHeaderBlock});

    MirBlockId finalBlock = entryCallBlock;

    const TakenOperation entryCount =
        take(MirHostedStartupOperationKind::PrepareEntryCount,
             MirHostedStartupFailureBehavior::None, finalBlock, true, false,
             true, false);
    exactValue(entryCount, generatedInfo(SemanticType::Int32));
    exactInstruction(entryCount, {.kind = MirInstructionKind::CallInput,
                                  .callInputRole = MirCallInputRole::Argument,
                                  .callInputKind = HirCallInputKind::Value,
                                  .result = entryCount.expected.value,
                                  .operands = {{.kind = MirOperandKind::Value,
                                                .value = convert.expected.value,
                                                .type = SemanticType::Int32}},
                                  .info = generatedInfo(SemanticType::Int32)});
    const TakenOperation entryArguments =
        take(MirHostedStartupOperationKind::PrepareEntryArgumentsMove,
             MirHostedStartupFailureBehavior::None, finalBlock, true, true,
             true, true);
    exactValue(entryArguments, generatedInfo(vectorType));
    exactPlace(entryArguments, {.root = MirPlaceRootKind::Temporary,
                                .temporary = 3,
                                .type = vectorType,
                                .access = AccessMode::Mutable,
                                .traits = semanticTraits(vectorType)});
    exactDrop(entryArguments, {.kind = MirDropObligationKind::PreparedParameter,
                               .place = entryArguments.expected.place,
                               .generatedValue = entryArguments.expected.value,
                               .dropType = vectorDropType});
    exactInstruction(
        entryArguments,
        {.kind = MirInstructionKind::CallInput,
         .callInputRole = MirCallInputRole::Argument,
         .callInputIndex = 1,
         .callInputKind = HirCallInputKind::MoveValue,
         .preparedParameterDrop = entryArguments.expected.dropObligation,
         .result = entryArguments.expected.value,
         .destination = entryArguments.expected.place,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = vector.expected.value,
                       .type = vectorType}},
         .info = generatedInfo(vectorType),
         .lifecycle = {{.kind = MirLifecycleEventKind::Reparent,
                        .source = vector.expected.dropObligation,
                        .target = entryArguments.expected.dropObligation}}});
    const TakenOperation callEntry =
        take(MirHostedStartupOperationKind::CallEntry,
             failureBehavior(entry.mayRaiseDefinedFailure), finalBlock, true,
             false, true, false);
    exactValue(callEntry, generatedInfo(SemanticType::Int32));
    exactInstruction(
        callEntry,
        {.kind = MirInstructionKind::Call,
         .result = callEntry.expected.value,
         .operands = {{.kind = MirOperandKind::Value,
                       .value = entryCount.expected.value,
                       .type = SemanticType::Int32},
                      {.kind = MirOperandKind::Value,
                       .value = entryArguments.expected.value,
                       .type = vectorType}},
         .parameterTypes = {SemanticType::Int32, vectorType},
         .definedFailure = propagation(FailurePropagationKind::DirectCall,
                                       entry.mayRaiseDefinedFailure),
         .functionTarget = entry.id,
         .info = generatedInfo(SemanticType::Int32),
         .lifecycle = {{.kind = MirLifecycleEventKind::TransferOut,
                        .source = entryArguments.expected.dropObligation}}});
    if (entry.mayRaiseDefinedFailure) {
      finalBlock =
          takeFailureRoute(finalBlock, callEntry.expected.instruction, {});
    }
    takeCThreadBoundary(finalBlock, false);
    const TakenOperation returnEntry =
        take(MirHostedStartupOperationKind::ReturnEntry,
             MirHostedStartupFailureBehavior::None, finalBlock, false, false,
             false, false);
    exactTerminator(returnEntry,
                    {.kind = MirTerminatorKind::Return,
                     .value = MirOperand{.kind = MirOperandKind::Value,
                                         .value = callEntry.expected.value,
                                         .type = SemanticType::Int32}});

    if (plan.argumentIndexPlace != initializeIndex.expected.place ||
        plan.argumentVectorPlace != vector.expected.place ||
        plan.stabilizedCount != convert.expected.value ||
        plan.argumentVector != vector.expected.value ||
        plan.entryResult != callEntry.expected.value) {
      add("owned-argument hosted-startup summary identities are invalid");
    }
  } else {
    add("hosted-startup plan has an invalid entry kind");
  }

  if (nextOperation != plan.operations.size() + 1 ||
      nextInstruction != body->instructionCount() + 1 ||
      nextPlace != body->places.size() + 1 ||
      nextValue != body->values.size() + 1 ||
      nextDrop != body->dropObligations.size() + 1) {
    add("hosted-startup plan or body retains an extra generated entity");
  }
  return result;
}

} // namespace
namespace {

[[nodiscard]] const MirClassInstance *
uniqueMirClassInstanceForType(const MirProgram &program,
                              const SemanticType &type) {
  const MirClassInstance *result = nullptr;
  for (const MirClassInstance &instance : program.classInstances()) {
    if (instance.type != type) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &instance;
  }
  return result;
}

[[nodiscard]] const MirClassInstance *
exactMirCapabilityClass(const MirProgram &program, const SemanticType &type,
                        CompilerCapabilityTypeKind capability) {
  const MirClassInstance *instance =
      uniqueMirClassInstanceForType(program, type);
  return instance != nullptr && instance->sourceUnit != 0 &&
                 instance->declaration != 0 &&
                 instance->type.kind == SemanticType::Class &&
                 instance->type.classId == instance->declaration &&
                 instance->type.arguments.size() ==
                     (capability == CompilerCapabilityTypeKind::ErasedObject
                          ? 0u
                          : 1u) &&
                 instance->compilerCapability == capability
             ? instance
             : nullptr;
}

[[nodiscard]] bool mirPublicBaseConversion(const MirProgram &program,
                                           const SemanticType &derived,
                                           const SemanticType &base) {
  const MirClassInstance *source =
      uniqueMirClassInstanceForType(program, derived);
  if (source == nullptr) {
    return false;
  }
  std::unordered_set<HirClassInstanceId> visited;
  const auto visit = [&](const auto &self,
                         const MirClassInstance &current) -> bool {
    if (!visited.insert(current.id).second) {
      return false;
    }
    for (const HirBaseInstance &candidate : current.structuralBases) {
      if (candidate.type == base) {
        return true;
      }
      const MirClassInstance *next =
          program.findClassInstance(candidate.instance);
      if (next == nullptr || next->type != candidate.type) {
        return false;
      }
      if (self(self, *next)) {
        return true;
      }
    }
    return false;
  };
  return visit(visit, *source);
}

[[nodiscard]] std::optional<SemanticTypeTraits>
mirOwnershipErasureTraits(const MirProgram &program,
                          const SemanticType &owner) {
  if (owner.kind == SemanticType::UniqueOwner && owner.arguments.size() == 1) {
    const MirClassInstance *pointee =
        uniqueMirClassInstanceForType(program, owner.arguments.front());
    return pointee == nullptr
               ? std::nullopt
               : std::optional<SemanticTypeTraits>{pointee->traits};
  }
  const MirClassInstance *shared = exactMirCapabilityClass(
      program, owner, CompilerCapabilityTypeKind::SharedOwner);
  return shared == nullptr ? std::nullopt
                           : std::optional<SemanticTypeTraits>{shared->traits};
}

[[nodiscard]] bool
mirOwnershipErasurePreservesCapabilities(const MirProgram &program,
                                         const SemanticType &sourceOwner,
                                         const SemanticType &targetOwner) {
  const auto source = mirOwnershipErasureTraits(program, sourceOwner);
  const auto target = mirOwnershipErasureTraits(program, targetOwner);
  return source && target &&
         (!target->transferCapable || source->transferCapable) &&
         (!target->shareCapable || source->shareCapable);
}

[[nodiscard]] bool exactOwnerErasureTransfer(const MirBody &body,
                                             const MirInstruction &instruction,
                                             const MirOperand &operand) {
  const MirDropObligation *transferred = nullptr;
  for (const MirLifecycleEvent &event : instruction.lifecycle) {
    if (event.kind != MirLifecycleEventKind::TransferOut) {
      continue;
    }
    const MirDropObligation *drop = body.findDropObligation(event.source);
    const MirPlace *place =
        drop == nullptr ? nullptr : body.findPlace(drop->place);
    if (transferred != nullptr || drop == nullptr || place == nullptr ||
        drop->dropType.type != operand.type || event.target != 0 ||
        event.conditional || event.failureCleanup ||
        place->root != MirPlaceRootKind::Value ||
        place->value != operand.value || !place->projections.empty()) {
      return false;
    }
    transferred = drop;
  }
  // Result activation and other independently verified lifecycle events may
  // share the instruction.  The erasure authority itself is exactly one
  // retirement of the consumed source token.
  return transferred != nullptr;
}

[[nodiscard]] bool
exactUniqueOwnerUpcastContract(const MirProgram &program, const MirBody &body,
                               const MirInstruction &instruction) {
  if (instruction.intrinsic != IntrinsicKind::UniqueOwnerUpcast) {
    return true;
  }
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.callableInvocation ||
      !instruction.callableArguments.empty() ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      !instruction.safetyDischarges.empty() || instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      instruction.borrowArgument != 0 ||
      instruction.borrowAccess != AccessMode::ReadOnly ||
      !instruction.borrowArguments.empty() || instruction.borrowPlace ||
      !instruction.result || instruction.operands.size() != 1 ||
      instruction.parameterTypes.size() != 1 ||
      instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1 ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly ||
      instruction.operands.front().kind != MirOperandKind::Value ||
      instruction.operands.front().value == 0 ||
      instruction.operands.front().place != 0 ||
      instruction.operands.front().loan != 0 ||
      instruction.operands.front().literal ||
      instruction.operands.front().type.kind != SemanticType::UniqueOwner ||
      instruction.operands.front().type.arguments.size() != 1 ||
      instruction.parameterTypes.front() != instruction.operands.front().type ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty()) {
    return false;
  }
  const SemanticType &sourceOwner = instruction.operands.front().type;
  const SemanticType &targetOwner = instruction.info.type;
  const SemanticType &derived = sourceOwner.arguments.front();
  const SemanticType &base = targetOwner.arguments.front();
  const MirClassInstance *baseInstance =
      uniqueMirClassInstanceForType(program, base);
  return baseInstance != nullptr && baseInstance->polymorphic &&
         mirPublicBaseConversion(program, derived, base) &&
         mirOwnershipErasurePreservesCapabilities(program, sourceOwner,
                                                  targetOwner) &&
         exactOwnerErasureTransfer(body, instruction,
                                   instruction.operands.front());
}

[[nodiscard]] bool
exactUniqueOwnerSwapContract(const MirBody &body,
                             const MirInstruction &instruction) {
  if (instruction.intrinsic != IntrinsicKind::UniqueOwnerSwap) {
    return true;
  }
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.callableInvocation ||
      !instruction.callableArguments.empty() || instruction.result ||
      instruction.info.type != SemanticType::Void || instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      instruction.borrowArgument != 0 ||
      instruction.borrowAccess != AccessMode::ReadOnly ||
      !instruction.borrowArguments.empty() || instruction.borrowPlace ||
      instruction.operands.size() != 2 ||
      instruction.parameterTypes.size() != 2 ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.safetyDischarges.empty() || !instruction.lifecycle.empty()) {
    return false;
  }

  SemanticType ownerType = SemanticType::Unknown;
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    const MirOperand &operand = instruction.operands[index];
    if (instruction.parameterTypes[index] != operand.type ||
        operand.type.kind != SemanticType::Reference ||
        operand.type.referenceAccess != AccessMode::Mutable ||
        operand.type.arguments.size() != 1 ||
        operand.type.arguments.front().kind != SemanticType::UniqueOwner ||
        operand.type.arguments.front().arguments.size() != 1 ||
        operand.value != 0 || operand.literal) {
      return false;
    }
    const SemanticType &candidate = operand.type.arguments.front();
    if (index == 0) {
      ownerType = candidate;
    } else if (candidate != ownerType) {
      return false;
    }

    MirPlaceId source = 0;
    if (operand.kind == MirOperandKind::BorrowWrite) {
      if (operand.place == 0 || operand.loan != 0) {
        return false;
      }
      source = operand.place;
    } else if (operand.kind == MirOperandKind::Loan) {
      const MirLoan *loan = body.findLoan(operand.loan);
      if (operand.place != 0 || loan == nullptr ||
          loan->access != AccessMode::Mutable) {
        return false;
      }
      source = loan->source;
    } else {
      return false;
    }
    const MirPlace *place = body.findPlace(source);
    if (place == nullptr || place->type != ownerType ||
        place->access != AccessMode::Mutable) {
      return false;
    }
  }
  return ownerType != SemanticType::Unknown;
}

// This indivisible operation has no user-code/failure point between taking
// the old value and restoring its place. Ordinary call-input/lifecycle and
// loan verification still prove staging, exclusive access and result cleanup.
[[nodiscard]] bool
exactRestoringTransferContract(const MirProgram &program, const MirBody &body,
                               const MirInstruction &instruction) {
  const bool swap = instruction.intrinsic == IntrinsicKind::RestoringSwap;
  if (!swap && instruction.intrinsic != IntrinsicKind::RestoringExchange)
    return true;
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.callableInvocation ||
      !instruction.callableArguments.empty() || instruction.destination ||
      instruction.loan || instruction.borrowPlace ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      !instruction.borrowArguments.empty() ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.safetyDischarges.empty() ||
      instruction.operands.size() != 2 ||
      instruction.parameterTypes.size() != 2)
    return false;
  const SemanticType &reference = instruction.parameterTypes.front();
  if (reference.kind != SemanticType::Reference ||
      reference.referenceAccess != AccessMode::Mutable ||
      reference.arguments.size() != 1)
    return false;
  const SemanticType &type = reference.arguments.front();
  if (mirTypeRelocationKind(program, type) == RelocationKind::Unavailable ||
      !mirTypeRelocationIsDefinedFailureFree(program, type))
    return false;
  MirPlaceId first = 0;
  for (std::size_t index = 0; index != (swap ? 2u : 1u); ++index) {
    const MirOperand &operand = instruction.operands[index];
    if (operand.type != reference ||
        instruction.parameterTypes[index] != reference || operand.value ||
        operand.literal ||
        (operand.kind != MirOperandKind::BorrowWrite &&
         operand.kind != MirOperandKind::Loan))
      return false;
    const MirLoan *loan = body.findLoan(operand.loan);
    const MirPlaceId id = operand.place != 0 ? operand.place
                          : loan != nullptr  ? loan->source
                                             : 0;
    const MirPlace *place = body.findPlace(id);
    if (place == nullptr || place->type != type ||
        place->access != AccessMode::Mutable ||
        (operand.kind == MirOperandKind::Loan &&
         (loan == nullptr || loan->access != AccessMode::Mutable)) ||
        (operand.kind == MirOperandKind::BorrowWrite && operand.loan))
      return false;
    if (index == 0)
      first = id;
    else if (id == first)
      return false;
  }
  if (swap)
    return !instruction.result && instruction.info.type == SemanticType::Void &&
           instruction.lifecycle.empty();
  const MirOperand &replacement = instruction.operands.back();
  if (!instruction.result || instruction.info.type != type ||
      instruction.parameterTypes.back() != type || replacement.type != type ||
      replacement.kind != MirOperandKind::Value || !replacement.value ||
      replacement.place || replacement.loan || replacement.literal)
    return false;
  const MirValue *prepared = body.findValue(replacement.value);
  if (prepared == nullptr || prepared->info.type != type)
    return false;
  // Owning inputs must retire exactly their prepared cleanup token. The
  // result's independent token keeps the old value alive for its consumer.
  if (prepared->info.traits.drop == DropKind::Trivial ||
      exactOwnerErasureTransfer(body, instruction, replacement))
    return true;
  // Cleanup-free move-only records still have lexical binding lifetimes, but
  // lowering does not invent a temporary cleanup token for their Move. Use
  // the source's independently type-checked drop recipe to distinguish this
  // case from a lost ownership transfer for an active resource.
  const MirInstruction *move = linearDefinition(body, replacement.value);
  if (move == nullptr || move->kind != MirInstructionKind::Move ||
      move->operands.size() != 1 ||
      move->operands.front().kind != MirOperandKind::Move ||
      !move->lifecycle.empty() || !instruction.lifecycle.empty())
    return false;
  const MirPlace *source = body.findPlace(move->operands.front().place);
  const MirDropObligation *bindingDrop = nullptr;
  for (const auto &drop : body.dropObligations) {
    if (source == nullptr || drop.place != source->id)
      continue;
    if (bindingDrop != nullptr)
      return false;
    bindingDrop = &drop;
  }
  return source != nullptr && source->type == type &&
         source->root == MirPlaceRootKind::Binding &&
         source->projections.empty() && bindingDrop != nullptr &&
         bindingDrop->kind == MirDropObligationKind::Binding &&
         bindingDrop->dropType.type == type &&
         !bindingDrop->dropType.requiresActiveCleanup;
}

[[nodiscard]] const SemanticType *
exactCapabilityReferenceValue(const MirProgram &program,
                              const SemanticType &reference, AccessMode access,
                              CompilerCapabilityTypeKind capability,
                              const MirClassInstance **instance = nullptr) {
  if (reference.kind != SemanticType::Reference ||
      reference.referenceAccess != access || reference.arguments.size() != 1) {
    return nullptr;
  }
  const MirClassInstance *selected =
      exactMirCapabilityClass(program, reference.arguments.front(), capability);
  if (selected == nullptr) {
    return nullptr;
  }
  if (instance != nullptr) {
    *instance = selected;
  }
  return &selected->type.arguments.front();
}

[[nodiscard]] bool
exactRestoringIndexedSwapContract(const MirProgram &program,
                                  const MirBody &body,
                                  const MirInstruction &instruction) {
  if (instruction.intrinsic != IntrinsicKind::RestoringIndexedSwap)
    return true;
  if (instruction.kind != MirInstructionKind::Call || instruction.result ||
      instruction.destination || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.operation != MirOperation::None ||
      instruction.info.type != SemanticType::Void || instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      instruction.borrowPlace || !instruction.borrowArguments.empty() ||
      !instruction.lifecycle.empty() || instruction.operands.size() != 3 ||
      instruction.parameterTypes.size() != 3 ||
      instruction.definedFailure.localOrigins.size() != 1 ||
      instruction.localFailureSites.size() != 1 ||
      instruction.definedFailure.propagation != FailurePropagationKind::None ||
      !instruction.safetyDischarges.empty())
    return false;
  const auto &reference = instruction.parameterTypes.front();
  if (reference.kind != SemanticType::Reference ||
      reference.referenceAccess != AccessMode::Mutable ||
      reference.arguments.size() != 1)
    return false;
  const auto &storage = reference.arguments.front();
  if (storage.kind != SemanticType::Array || storage.arguments.size() != 1 ||
      mirTypeRelocationKind(program, storage.arguments.front()) ==
          RelocationKind::Unavailable ||
      !mirTypeRelocationIsDefinedFailureFree(program,
                                             storage.arguments.front()))
    return false;
  const auto &operand = instruction.operands.front();
  const auto *loan = body.findLoan(operand.loan);
  const auto *place = body.findPlace(operand.place ? operand.place
                                     : loan        ? loan->source
                                                   : 0);
  if (operand.type != reference || operand.value || operand.literal || !place ||
      place->type != storage || place->access != AccessMode::Mutable ||
      (operand.kind != MirOperandKind::BorrowWrite &&
       operand.kind != MirOperandKind::Loan) ||
      (operand.kind == MirOperandKind::BorrowWrite && operand.loan) ||
      (operand.kind == MirOperandKind::Loan &&
       (!loan || loan->access != AccessMode::Mutable)))
    return false;
  for (std::size_t index = 1; index != 3; ++index) {
    const auto &value = instruction.operands[index];
    if (instruction.parameterTypes[index] != SemanticType::UInt64 ||
        value.type != SemanticType::UInt64 ||
        value.kind != MirOperandKind::Value || !value.value || value.place ||
        value.loan || value.literal)
      return false;
  }
  const std::vector<DefinedFailureOutcome> outcomes{
      {DefinedFailureCode::IndexOutOfBounds,
       DefinedFailureDetail::PrivateStorage}};
  const auto *site =
      program.failureMetadata().findSite(instruction.localFailureSites.front());
  return site && site->outcomes == outcomes &&
         instruction.definedFailure.localOrigins.front().outcomes == outcomes;
}

[[nodiscard]] bool
exactScalarUniqueAllocationContract(const MirProgram &program,
                                    const MirBody &body,
                                    const MirInstruction &instruction) {
  if (instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1) {
    return false;
  }
  const SemanticType &pointee = instruction.info.type.arguments.front();
  if (pointee.kind == SemanticType::Class) {
    if (!instruction.constructorTarget) {
      if (!mir_detail::generatedUniqueConstruction(instruction) ||
          instruction.objectAllocation == 0 ||
          !instruction.definedFailure.empty() ||
          !instruction.localFailureSites.empty())
        return false;
      const auto owner = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const auto &instance) { return instance.type == pointee; });
      if (owner == program.classInstances().end())
        return false;
      if (instruction.constructorKind == ConstructorKind::Ordinary)
        return owner->defaultConstructor == SpecialMemberStatus::Generated &&
               !owner->generatedDefaultRequiresSchedule;
      return instruction.constructorKind == ConstructorKind::Copy
                 ? owner->copyConstructor == SpecialMemberStatus::Generated &&
                       owner->traits.copyable
                 : owner->moveConstructor == SpecialMemberStatus::Generated &&
                       owner->traits.movable;
    }
    return true; // Class construction retains its separate lifecycle contract.
  }
  bool defaultInitializable = true;
  switch (pointee.kind) {
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
  case SemanticType::RawPointer:
    break;
  case SemanticType::Enum:
    if (pointee.enumId == 0 || program.findPayloadEnum(pointee)) {
      return false;
    }
    [[fallthrough]];
  case SemanticType::NullPtr:
    defaultInitializable = false;
    break;
  case SemanticType::NativeFunction:
    if (!pointee.hasNativeFunctionShape()) {
      return false;
    }
    defaultInitializable = false;
    break;
  default:
    return false;
  }
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || !instruction.result ||
      instruction.receiver || instruction.functionTarget ||
      instruction.constructorTarget || instruction.bodyTarget ||
      instruction.lambdaTarget || instruction.callableInvocation ||
      !instruction.callableArguments.empty() || instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      instruction.successResultDrop || instruction.lifecycle.size() != 1 ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.traits.drop != DropKind::Lexical ||
      instruction.definedFailure.propagation != FailurePropagationKind::None ||
      instruction.definedFailure.localOrigins.size() != 1 ||
      instruction.localFailureSites.size() != 1 ||
      instruction.localFailureSites.front() == 0 ||
      !instruction.safetyDischarges.empty()) {
    return false;
  }
  const auto &event = instruction.lifecycle.front();
  const auto *drop = body.findDropObligation(event.target);
  if (!drop || drop->kind != MirDropObligationKind::Value ||
      drop->value != instruction.hirValue ||
      drop->dropType.type != instruction.info.type ||
      event != MirLifecycleEvent{.kind = MirLifecycleEventKind::Initialize,
                                 .target = drop->id})
    return false;
  const std::vector<DefinedFailureOutcome> expected{
      {DefinedFailureCode::AllocationFailure,
       DefinedFailureDetail::UniqueOwner}};
  if (instruction.definedFailure.localOrigins.front().outcomes != expected) {
    return false;
  }
  if (instruction.operands.empty()) {
    return defaultInitializable && instruction.parameterTypes.empty();
  }
  if (instruction.operands.size() != 1 ||
      instruction.parameterTypes != std::vector<SemanticType>{pointee}) {
    return false;
  }
  const MirOperand &operand = instruction.operands.front();
  return operand.kind == MirOperandKind::Value && operand.value != 0 &&
         operand.place == 0 && operand.loan == 0 && !operand.literal &&
         operand.type == pointee;
}

[[nodiscard]] bool
exactUniqueOwnerBorrowContract(const MirBody &body,
                               const MirInstruction &instruction) {
  const AccessMode access =
      instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.bodyTarget || instruction.lambdaTarget ||
      instruction.callableInvocation ||
      !instruction.callableArguments.empty() ||
      instruction.operands.size() != 1 || !instruction.result ||
      !instruction.lifecycle.empty() || instruction.successResultDrop ||
      instruction.successResultDestination ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      instruction.definedFailure.propagation != FailurePropagationKind::None ||
      instruction.definedFailure.localOrigins.size() != 1 ||
      instruction.localFailureSites.size() != 1 ||
      instruction.localFailureSites.front() == 0 ||
      !instruction.safetyDischarges.empty()) {
    return false;
  }
  const auto &outcomes =
      instruction.definedFailure.localOrigins.front().outcomes;
  if (outcomes.size() != 1 ||
      outcomes.front().code != DefinedFailureCode::EmptyOwnerAccess ||
      (outcomes.front().detail != DefinedFailureDetail::Dereference &&
       outcomes.front().detail != DefinedFailureDetail::MemberAccess)) {
    return false;
  }
  const MirOperand &operand = instruction.operands.front();
  if (operand.kind != MirOperandKind::Value || operand.value == 0 ||
      operand.place != 0 || operand.loan != 0 || operand.literal ||
      operand.type.kind != SemanticType::UniqueOwner ||
      operand.type.arguments.size() != 1 ||
      instruction.parameterTypes != std::vector<SemanticType>{operand.type}) {
    return false;
  }
  const MirInstruction *stage = definitionFor(body, operand.value);
  if (stage == nullptr || stage->kind != MirInstructionKind::Load ||
      stage->operands.size() != 1 ||
      stage->operands.front().kind != MirOperandKind::Copy ||
      stage->info.type != operand.type) {
    return false;
  }
  const MirPlace *owner = body.findPlace(stage->operands.front().place);
  const MirLoan *loan =
      instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
  return owner != nullptr && owner->type == operand.type &&
         (access != AccessMode::Mutable ||
          owner->access == AccessMode::Mutable) &&
         instruction.info.type == operand.type.arguments.front() &&
         instruction.info.category == ValueCategory::Place &&
         instruction.info.access == access &&
         instruction.borrowOrigin == BorrowOriginKind::Argument &&
         instruction.borrowArgument == 0 &&
         instruction.borrowAccess == access && loan != nullptr &&
         loan->kind == MirLoanKind::CallResult && loan->source == owner->id &&
         loan->access == access && loan->producedBy == instruction.hirValue &&
         !loan->entry && !loan->escapes && !loan->lifecycleBound &&
         loan->parent == 0;
}

[[nodiscard]] bool
exactSharedOwnerIntrinsicContract(const MirProgram &program,
                                  const MirBody &body,
                                  const MirInstruction &instruction) {
  const auto intrinsic = instruction.intrinsic;
  if (intrinsic < IntrinsicKind::SharedOwnerEmpty ||
      intrinsic > IntrinsicKind::SharedOwnerUpcast) {
    return true;
  }
  const bool empty = intrinsic == IntrinsicKind::SharedOwnerEmpty;
  const bool promote = intrinsic == IntrinsicKind::SharedOwnerFromUnique;
  const bool clone = intrinsic == IntrinsicKind::SharedOwnerClone;
  const bool borrow = intrinsic == IntrinsicKind::SharedOwnerBorrow;
  const bool swap = intrinsic == IntrinsicKind::SharedOwnerSwap;
  const bool upcast = intrinsic == IntrinsicKind::SharedOwnerUpcast;
  const bool creates = empty || promote || clone || upcast;
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.callableInvocation ||
      !instruction.callableArguments.empty() ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      !instruction.safetyDischarges.empty() ||
      !instruction.borrowArguments.empty() || instruction.borrowPlace ||
      instruction.operands.size() != (empty  ? 0
                                      : swap ? 2
                                             : 1) ||
      instruction.parameterTypes.size() != instruction.operands.size()) {
    return false;
  }
  const MirClassInstance *owner = nullptr;
  if (creates) {
    owner = exactMirCapabilityClass(program, instruction.info.type,
                                    CompilerCapabilityTypeKind::SharedOwner);
    if (owner == nullptr || instruction.info.category != ValueCategory::Value ||
        instruction.info.access != AccessMode::ReadOnly ||
        instruction.info.traits.ownership != OwnershipKind::Shared ||
        instruction.info.traits.drop != DropKind::Lexical ||
        instruction.info.traits.copyable ||
        instruction.info.traits.containsBorrowedState || !instruction.result) {
      return false;
    }
  }
  MirPlaceId borrowedSource = 0;
  for (std::size_t index = 0; index < instruction.operands.size(); ++index) {
    const MirOperand &operand = instruction.operands[index];
    if (instruction.parameterTypes[index] != operand.type) {
      return false;
    }
    if (promote) {
      if (operand.kind != MirOperandKind::Value || operand.value == 0 ||
          operand.place != 0 || operand.loan != 0 || operand.literal ||
          operand.type !=
              SemanticType::uniqueOwnerOf(owner->type.arguments.front()) ||
          !mirUniqueCleanupType(program, operand.type)) {
        return false;
      }
      if (!exactOwnerErasureTransfer(body, instruction, operand)) {
        return false;
      }
      continue;
    }
    if (upcast) {
      const MirClassInstance *sourceOwner = exactMirCapabilityClass(
          program, operand.type, CompilerCapabilityTypeKind::SharedOwner);
      if (operand.kind != MirOperandKind::Value || operand.value == 0 ||
          operand.place != 0 || operand.loan != 0 || operand.literal ||
          sourceOwner == nullptr || owner == nullptr ||
          sourceOwner->type.arguments.size() != 1 ||
          owner->type.arguments.size() != 1 ||
          !mirPublicBaseConversion(program, sourceOwner->type.arguments.front(),
                                   owner->type.arguments.front()) ||
          !mirOwnershipErasurePreservesCapabilities(program, operand.type,
                                                    instruction.info.type)) {
        return false;
      }
      const MirClassInstance *base = nullptr;
      for (const auto &instance : program.classInstances()) {
        if (instance.type == owner->type.arguments.front()) {
          if (base != nullptr) {
            return false;
          }
          base = &instance;
        }
      }
      if (base == nullptr || !base->polymorphic ||
          !exactOwnerErasureTransfer(body, instruction, operand)) {
        return false;
      }
      continue;
    }
    const AccessMode access = swap ? AccessMode::Mutable : AccessMode::ReadOnly;
    const MirClassInstance *argumentOwner = nullptr;
    if (exactCapabilityReferenceValue(program, operand.type, access,
                                      CompilerCapabilityTypeKind::SharedOwner,
                                      &argumentOwner) == nullptr ||
        (owner != nullptr && owner != argumentOwner)) {
      return false;
    }
    owner = argumentOwner;
    if (operand.value != 0 || operand.literal) {
      return false;
    }
    if (operand.kind ==
        (swap ? MirOperandKind::BorrowWrite : MirOperandKind::BorrowRead)) {
      if (operand.place == 0 || operand.loan != 0) {
        return false;
      }
      borrowedSource = operand.place;
    } else if (operand.kind == MirOperandKind::Loan) {
      const auto *loan = body.findLoan(operand.loan);
      if (loan == nullptr || operand.place != 0 ||
          (swap && loan->access != AccessMode::Mutable)) {
        return false;
      }
      borrowedSource = loan->source;
    } else {
      return false;
    }
    const auto *place = body.findPlace(borrowedSource);
    if (place == nullptr || (swap && place->access != AccessMode::Mutable)) {
      return false;
    }
  }
  if (!borrow &&
      (instruction.loan || instruction.borrowOrigin != BorrowOriginKind::None ||
       instruction.borrowArgument != 0 ||
       instruction.borrowAccess != AccessMode::ReadOnly)) {
    return false;
  }
  if (borrow) {
    const auto *loan =
        instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
    if (owner == nullptr || !instruction.result ||
        instruction.info.type != owner->type.arguments.front() ||
        instruction.info.category != ValueCategory::Place ||
        instruction.info.access != AccessMode::ReadOnly ||
        instruction.borrowOrigin != BorrowOriginKind::Argument ||
        instruction.borrowArgument != 0 ||
        instruction.borrowAccess != AccessMode::ReadOnly || loan == nullptr ||
        loan->kind != MirLoanKind::CallResult ||
        loan->source != borrowedSource ||
        loan->access != AccessMode::ReadOnly ||
        loan->producedBy != instruction.hirValue || loan->entry ||
        loan->escapes || loan->lifecycleBound) {
      return false;
    }
  } else if (!creates) {
    const SemanticType expected =
        swap                                            ? SemanticType::Void
        : intrinsic == IntrinsicKind::SharedOwnerIsNull ? SemanticType::Bool
                                                        : SemanticType::UInt64;
    if (instruction.info.type != expected ||
        instruction.info.category != ValueCategory::Value ||
        instruction.info.access != AccessMode::ReadOnly ||
        (expected == SemanticType::Void ? instruction.result.has_value()
                                        : !instruction.result)) {
      return false;
    }
  }
  if (!promote && !creates && !instruction.lifecycle.empty()) {
    return false;
  }
  if (upcast || !(promote || clone || borrow)) {
    return instruction.definedFailure.empty() &&
           instruction.localFailureSites.empty();
  }
  if (instruction.definedFailure.propagation != FailurePropagationKind::None ||
      instruction.definedFailure.localOrigins.size() != 1 ||
      instruction.localFailureSites.size() != 1 ||
      instruction.localFailureSites.front() == 0) {
    return false;
  }
  const auto &outcomes =
      instruction.definedFailure.localOrigins.front().outcomes;
  return outcomes.size() == 1 &&
         (borrow
              ? outcomes.front().code == DefinedFailureCode::EmptyOwnerAccess &&
                    (outcomes.front().detail ==
                         DefinedFailureDetail::Dereference ||
                     outcomes.front().detail ==
                         DefinedFailureDetail::MemberAccess)
              : outcomes.front() ==
                    DefinedFailureOutcome{DefinedFailureCode::AllocationFailure,
                                          DefinedFailureDetail::SharedOwner});
}

[[nodiscard]] bool
exactOptionalStorageIntrinsicContract(const MirProgram &program,
                                      const MirBody &body,
                                      const MirInstruction &instruction) {
  const bool optionalIntrinsic =
      instruction.intrinsic == IntrinsicKind::OptionalStorageHasValue ||
      instruction.intrinsic == IntrinsicKind::OptionalStorageConstruct ||
      instruction.intrinsic == IntrinsicKind::OptionalStorageRead ||
      instruction.intrinsic == IntrinsicKind::OptionalStorageReadMut ||
      instruction.intrinsic == IntrinsicKind::OptionalStorageReset;
  if (!optionalIntrinsic) {
    return true;
  }
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.lambdaTarget ||
      instruction.bodyTarget || instruction.callableInvocation ||
      instruction.operands.empty() ||
      instruction.parameterTypes.size() != instruction.operands.size() ||
      !std::equal(instruction.parameterTypes.begin(),
                  instruction.parameterTypes.end(),
                  instruction.operands.begin(),
                  [](const SemanticType &parameter, const MirOperand &operand) {
                    return parameter == operand.type;
                  }) ||
      !instruction.callableArguments.empty() ||
      instruction.synchronization.kind != SynchronizationOperationKind::None) {
    return false;
  }

  const MirOperand &storageOperand = instruction.operands.front();
  const MirClassInstance *storageClass =
      exactMirCapabilityClass(program, storageOperand.type,
                              CompilerCapabilityTypeKind::OptionalStorage);
  if (storageClass == nullptr || storageOperand.kind != MirOperandKind::Value ||
      storageOperand.value == 0 || storageOperand.place != 0 ||
      storageOperand.loan != 0 || storageOperand.literal) {
    return false;
  }
  const MirInstruction *stage = definitionFor(body, storageOperand.value);
  if (stage == nullptr || !stage->result ||
      *stage->result != storageOperand.value || stage->operands.size() != 1 ||
      stage->info.type != storageClass->type ||
      stage->operands.front().place == 0) {
    return false;
  }
  const MirPlace *storagePlace = body.findPlace(stage->operands.front().place);
  if (storagePlace == nullptr || storagePlace->type != storageClass->type) {
    return false;
  }
  const bool mutates =
      instruction.intrinsic == IntrinsicKind::OptionalStorageConstruct ||
      instruction.intrinsic == IntrinsicKind::OptionalStorageReadMut ||
      instruction.intrinsic == IntrinsicKind::OptionalStorageReset;
  const bool exactStage =
      (stage->kind == MirInstructionKind::Load &&
       stage->operands.front().kind == MirOperandKind::Copy) ||
      (stage->kind == MirInstructionKind::CallInput &&
       stage->operands.front().kind == (mutates ? MirOperandKind::BorrowWrite
                                                : MirOperandKind::BorrowRead));
  if (!exactStage) {
    return false;
  }

  const SemanticType &elementType = storageClass->type.arguments.front();
  const auto exactResult = [&](const SemanticType &type, ValueCategory category,
                               AccessMode access) {
    return instruction.info.type == type &&
           instruction.info.category == category &&
           instruction.info.access == access &&
           (type == SemanticType::Void ? !instruction.result
                                       : instruction.result.has_value());
  };
  const auto noLoanResult = [&] {
    return !instruction.loan &&
           instruction.borrowOrigin == BorrowOriginKind::None &&
           instruction.borrowArgument == 0 &&
           instruction.borrowAccess == AccessMode::ReadOnly;
  };
  const auto noFailure = [&] {
    return instruction.definedFailure.empty() &&
           instruction.localFailureSites.empty() &&
           instruction.safetyDischarges.empty();
  };
  const auto exactFailure = [&](std::vector<DefinedFailureOutcome> outcomes) {
    return instruction.definedFailure.propagation ==
               FailurePropagationKind::None &&
           instruction.definedFailure.localOrigins.size() == 1 &&
           instruction.definedFailure.localOrigins.front().outcomes ==
               outcomes &&
           instruction.localFailureSites.size() == 1 &&
           instruction.localFailureSites.front() != 0;
  };
  const auto exactConstructLifecycle = [&] {
    std::vector<MirDropObligationId> expectedTransfers;
    for (std::size_t index = 1; index < instruction.operands.size(); ++index) {
      const MirOperand &operand = instruction.operands[index];
      if (operand.kind != MirOperandKind::Value || operand.value == 0 ||
          operand.place != 0 || operand.loan != 0 || operand.literal) {
        return false;
      }
      const MirInstruction *producer = definitionFor(body, operand.value);
      if (producer == nullptr || !producer->result ||
          *producer->result != operand.value ||
          producer->info.traits.containsBorrowedState) {
        return false;
      }

      const MirDropObligation *transferred = nullptr;
      if (producer->preparedParameterDrop) {
        transferred = body.findDropObligation(*producer->preparedParameterDrop);
      } else {
        for (const MirDropObligation &candidate : body.dropObligations) {
          const MirPlace *place = body.findPlace(candidate.place);
          if (candidate.kind != MirDropObligationKind::Value ||
              place == nullptr || place->root != MirPlaceRootKind::Value ||
              place->value != operand.value || !place->projections.empty()) {
            continue;
          }
          if (transferred != nullptr) {
            return false;
          }
          transferred = &candidate;
        }
      }
      if (transferred == nullptr) {
        continue;
      }
      if ((transferred->kind != MirDropObligationKind::Value &&
           transferred->kind != MirDropObligationKind::PreparedParameter) ||
          transferred->dropType.type != operand.type) {
        return false;
      }
      expectedTransfers.push_back(transferred->id);
    }
    if (instruction.lifecycle.size() != expectedTransfers.size()) {
      return false;
    }
    for (std::size_t index = 0; index < expectedTransfers.size(); ++index) {
      const MirLifecycleEvent &event = instruction.lifecycle[index];
      if (event.kind != MirLifecycleEventKind::TransferOut ||
          event.source != expectedTransfers[index] || event.target != 0 ||
          event.conditional || event.failureCleanup) {
        return false;
      }
    }
    return true;
  };

  switch (instruction.intrinsic) {
  case IntrinsicKind::OptionalStorageHasValue:
    return instruction.lifecycle.empty() && instruction.operands.size() == 1 &&
           !instruction.constructorTarget &&
           exactResult(SemanticType::Bool, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() && noFailure();
  case IntrinsicKind::OptionalStorageReset:
    // Removal uses the same independently verified element cleanup recipe as
    // general raw object destruction. It has no local failure: any failure
    // retains the original destructor's identity and propagates through this
    // call.
    return instruction.lifecycle.empty() && instruction.operands.size() == 1 &&
           !instruction.constructorTarget &&
           exactResult(SemanticType::Void, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() && instruction.definedFailure.localOrigins.empty() &&
           instruction.localFailureSites.empty() &&
           instruction.safetyDischarges.empty() &&
           instruction.definedFailure.propagation ==
               (instruction.elementCleanup &&
                        mirDropMayRaiseDefinedFailure(
                            program, *instruction.elementCleanup)
                    ? FailurePropagationKind::Destructor
                    : FailurePropagationKind::None);
  case IntrinsicKind::OptionalStorageConstruct:
    // Publication transfers one already-completed value. Source constructor
    // execution and its failure channel must be separate ordinary MIR; this
    // capability cannot smuggle an arbitrary native construction through a
    // variadic operand list or a retained source-constructor target.
    return instruction.operands.size() == 2 &&
           instruction.operands[1].type == elementType &&
           !instruction.constructorTarget &&
           exactResult(SemanticType::Void, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() && exactConstructLifecycle() &&
           exactFailure(
               {{.code = DefinedFailureCode::InvalidStorageState,
                 .detail = DefinedFailureDetail::DuplicateConstruction},
                {.code = DefinedFailureCode::AllocationFailure,
                 .detail = DefinedFailureDetail::ElementConstruction}});
  case IntrinsicKind::OptionalStorageRead:
  case IntrinsicKind::OptionalStorageReadMut: {
    if (!instruction.lifecycle.empty() || instruction.operands.size() != 1 ||
        instruction.constructorTarget) {
      return false;
    }
    const AccessMode access =
        instruction.intrinsic == IntrinsicKind::OptionalStorageReadMut
            ? AccessMode::Mutable
            : AccessMode::ReadOnly;
    const MirLoan *loan =
        instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
    return exactResult(elementType, ValueCategory::Place, access) &&
           instruction.borrowOrigin == BorrowOriginKind::Argument &&
           instruction.borrowArgument == 0 &&
           instruction.borrowAccess == access && loan != nullptr &&
           loan->kind == MirLoanKind::CallResult && !loan->lifecycleBound &&
           !loan->entry && !loan->escapes && loan->parent == 0 &&
           loan->source == storagePlace->id && loan->access == access &&
           loan->producedBy == instruction.hirValue &&
           exactFailure({{.code = DefinedFailureCode::EmptyOwnerAccess,
                          .detail = DefinedFailureDetail::MemberAccess}});
  }
  default:
    return false;
  }
}

// The polymorphic slot's admitted payloads have failure-free relocation and
// inert cleanup, so none of its operations carries a failure, a cleanup
// recipe, or a source constructor. MIR re-proves the payload contract
// independently of the semantic admission that produced it.
[[nodiscard]] bool
exactPolymorphicStorageIntrinsicContract(const MirProgram &program,
                                         const MirBody &body,
                                         const MirInstruction &instruction) {
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.lambdaTarget ||
      instruction.bodyTarget || instruction.callableInvocation ||
      instruction.constructorTarget || instruction.elementCleanup ||
      instruction.operands.empty() ||
      instruction.parameterTypes.size() != instruction.operands.size() ||
      !std::equal(instruction.parameterTypes.begin(),
                  instruction.parameterTypes.end(),
                  instruction.operands.begin(),
                  [](const SemanticType &parameter, const MirOperand &operand) {
                    return parameter == operand.type;
                  }) ||
      !instruction.callableArguments.empty() ||
      instruction.synchronization.kind != SynchronizationOperationKind::None ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() ||
      !instruction.safetyDischarges.empty()) {
    return false;
  }
  const MirOperand &storageOperand = instruction.operands.front();
  const MirClassInstance *storageClass =
      exactMirCapabilityClass(program, storageOperand.type,
                              CompilerCapabilityTypeKind::PolymorphicStorage);
  if (storageClass == nullptr || storageClass->type.arguments.size() != 1 ||
      storageClass->type.valueArguments.size() != 2 ||
      storageClass->requiresActiveCleanup ||
      storageOperand.kind != MirOperandKind::Value ||
      storageOperand.value == 0 || storageOperand.place != 0 ||
      storageOperand.loan != 0 || storageOperand.literal) {
    return false;
  }
  const MirInstruction *stage = definitionFor(body, storageOperand.value);
  if (stage == nullptr || !stage->result ||
      *stage->result != storageOperand.value || stage->operands.size() != 1 ||
      stage->info.type != storageClass->type ||
      stage->operands.front().place == 0) {
    return false;
  }
  const MirPlace *storagePlace = body.findPlace(stage->operands.front().place);
  const bool mutates =
      instruction.intrinsic == IntrinsicKind::PolymorphicStorageConstruct ||
      instruction.intrinsic == IntrinsicKind::PolymorphicStorageReadMut ||
      instruction.intrinsic == IntrinsicKind::PolymorphicStorageReset;
  if (storagePlace == nullptr || storagePlace->type != storageClass->type ||
      !((stage->kind == MirInstructionKind::Load &&
         stage->operands.front().kind == MirOperandKind::Copy) ||
        (stage->kind == MirInstructionKind::CallInput &&
         stage->operands.front().kind == (mutates
                                              ? MirOperandKind::BorrowWrite
                                              : MirOperandKind::BorrowRead)))) {
    return false;
  }
  const SemanticType &interfaceType = storageClass->type.arguments.front();
  const auto exactResult = [&](const SemanticType &type, ValueCategory category,
                               AccessMode access) {
    return instruction.info.type == type &&
           instruction.info.category == category &&
           instruction.info.access == access &&
           (type == SemanticType::Void ? !instruction.result
                                       : instruction.result.has_value());
  };
  const bool noLoanResult =
      !instruction.loan && instruction.borrowOrigin == BorrowOriginKind::None &&
      instruction.borrowArgument == 0 &&
      instruction.borrowAccess == AccessMode::ReadOnly;
  // Interface must be a base of the payload's concrete instance, reached
  // through its exact recorded base instances.
  const auto derivesInterface = [&](const auto &self,
                                    const MirClassInstance &candidate,
                                    std::size_t depth) -> bool {
    if (depth > program.classInstances().size())
      return false;
    return std::any_of(candidate.bases.begin(), candidate.bases.end(),
                       [&](const HirBaseInstance &base) {
                         const MirClassInstance *instance =
                             program.findClassInstance(base.instance);
                         return instance != nullptr &&
                                instance->type == base.type &&
                                (base.type == interfaceType ||
                                 self(self, *instance, depth + 1));
                       });
  };

  switch (instruction.intrinsic) {
  case IntrinsicKind::PolymorphicStorageHasValue:
    return instruction.operands.size() == 1 && instruction.lifecycle.empty() &&
           exactResult(SemanticType::Bool, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult;
  case IntrinsicKind::PolymorphicStorageReset:
    return instruction.operands.size() == 1 && instruction.lifecycle.empty() &&
           exactResult(SemanticType::Void, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult;
  case IntrinsicKind::PolymorphicStorageConstruct: {
    // One completed payload, never an arbitrary native construction.
    if (instruction.operands.size() != 2 ||
        !exactResult(SemanticType::Void, ValueCategory::Value,
                     AccessMode::ReadOnly) ||
        !noLoanResult)
      return false;
    const MirOperand &payload = instruction.operands[1];
    const MirClassInstance *model = nullptr;
    for (const MirClassInstance &candidate : program.classInstances()) {
      if (payload.type.kind != SemanticType::Class ||
          candidate.type != payload.type)
        continue;
      if (model != nullptr)
        return false;
      model = &candidate;
    }
    const MirInstruction *producer =
        payload.kind == MirOperandKind::Value && payload.value != 0 &&
                payload.place == 0 && payload.loan == 0 && !payload.literal
            ? definitionFor(body, payload.value)
            : nullptr;
    if (model == nullptr || model->abstract || model->unionLayout ||
        model->compilerCapability != CompilerCapabilityTypeKind::None ||
        model->requiresActiveCleanup || model->requiresActiveDropState ||
        model->traits.containsBorrowedState ||
        model->moveConstructor != SpecialMemberStatus::Generated ||
        mirTypeRelocationKind(program, model->type) ==
            RelocationKind::Unavailable ||
        !derivesInterface(derivesInterface, *model, 0) || producer == nullptr ||
        !producer->result || *producer->result != payload.value)
      return false;
    // An inert payload owns no drop obligation; only a prepared argument
    // slot may hand its exact obligation to the slot.
    if (producer->preparedParameterDrop) {
      const MirDropObligation *prepared =
          body.findDropObligation(*producer->preparedParameterDrop);
      return prepared != nullptr &&
             prepared->kind == MirDropObligationKind::PreparedParameter &&
             prepared->dropType.type == payload.type &&
             instruction.lifecycle.size() == 1 &&
             instruction.lifecycle.front().kind ==
                 MirLifecycleEventKind::TransferOut &&
             instruction.lifecycle.front().source == prepared->id &&
             instruction.lifecycle.front().target == 0 &&
             !instruction.lifecycle.front().conditional &&
             !instruction.lifecycle.front().failureCleanup;
    }
    return instruction.lifecycle.empty();
  }
  case IntrinsicKind::PolymorphicStorageRead:
  case IntrinsicKind::PolymorphicStorageReadMut: {
    if (instruction.operands.size() != 1 || !instruction.lifecycle.empty())
      return false;
    const AccessMode access =
        instruction.intrinsic == IntrinsicKind::PolymorphicStorageReadMut
            ? AccessMode::Mutable
            : AccessMode::ReadOnly;
    const MirLoan *loan =
        instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
    return exactResult(interfaceType, ValueCategory::Place, access) &&
           instruction.borrowOrigin == BorrowOriginKind::Argument &&
           instruction.borrowArgument == 0 &&
           instruction.borrowAccess == access && loan != nullptr &&
           loan->kind == MirLoanKind::CallResult && !loan->lifecycleBound &&
           !loan->entry && !loan->escapes && loan->parent == 0 &&
           loan->source == storagePlace->id && loan->access == access &&
           loan->producedBy == instruction.hirValue;
  }
  default:
    return false;
  }
}

[[nodiscard]] bool
exactAtomicWaitingContract(const MirProgram &program, const MirBody &body,
                           const MirInstruction &instruction) {
  const bool wait = instruction.intrinsic == IntrinsicKind::AtomicWait;
  if (!wait && instruction.intrinsic != IntrinsicKind::AtomicNotifyOne &&
      instruction.intrinsic != IntrinsicKind::AtomicNotifyAll)
    return true;
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.result || instruction.info.type != SemanticType::Void ||
      instruction.info.category != ValueCategory::Value ||
      instruction.info.access != AccessMode::ReadOnly || instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      instruction.borrowArgument != 0 ||
      instruction.borrowAccess != AccessMode::ReadOnly ||
      !instruction.localFailureSites.empty() ||
      !instruction.definedFailure.empty() || !instruction.lifecycle.empty() ||
      instruction.operands.size() != (wait ? 2U : 1U) ||
      instruction.parameterTypes.size() != instruction.operands.size() ||
      !exactAtomicSynchronization(instruction.intrinsic,
                                  instruction.synchronization))
    return false;
  const MirOperand &storage = instruction.operands.front();
  const SemanticType *element =
      exactCapabilityReferenceValue(program, storage.type, AccessMode::ReadOnly,
                                    CompilerCapabilityTypeKind::AtomicStorage);
  const MirPlace *place = body.findPlace(storage.place);
  if (element == nullptr || storage.kind != MirOperandKind::BorrowRead ||
      storage.place == 0 || storage.value != 0 || storage.loan != 0 ||
      storage.literal || place == nullptr ||
      storage.type.arguments.front() != place->type ||
      instruction.parameterTypes.front() != storage.type)
    return false;
  if (!wait)
    return true;
  const MirOperand &old = instruction.operands[1];
  const MirValue *value = body.findValue(old.value);
  return old.kind == MirOperandKind::Value && old.value != 0 &&
         old.place == 0 && old.loan == 0 && !old.literal && value != nullptr &&
         value->info.type == *element && old.type == *element &&
         instruction.parameterTypes[1] == *element;
}

[[nodiscard]] bool
exactMutexIntrinsicContract(const MirProgram &program, const MirBody &body,
                            const MirInstruction &instruction) {
  const bool mutexIntrinsic =
      instruction.intrinsic == IntrinsicKind::MutexLock ||
      instruction.intrinsic == IntrinsicKind::MutexUnlock ||
      instruction.intrinsic == IntrinsicKind::MutexGuardActive ||
      instruction.intrinsic == IntrinsicKind::MutexGuardRead ||
      instruction.intrinsic == IntrinsicKind::MutexGuardReadMut ||
      instruction.intrinsic == IntrinsicKind::MutexLockFailure;
  if (!mutexIntrinsic) {
    return true;
  }
  if (instruction.kind != MirInstructionKind::Call ||
      instruction.operation != MirOperation::None || instruction.receiver ||
      instruction.functionTarget || instruction.constructorTarget ||
      instruction.lambdaTarget || instruction.bodyTarget ||
      instruction.parameterTypes.size() != instruction.operands.size() ||
      !std::equal(instruction.parameterTypes.begin(),
                  instruction.parameterTypes.end(),
                  instruction.operands.begin(),
                  [](const SemanticType &parameter, const MirOperand &operand) {
                    return parameter == operand.type;
                  })) {
    return false;
  }
  const auto exactBorrow = [](const MirOperand &operand, MirOperandKind kind) {
    return operand.kind == kind && operand.place != 0 && operand.value == 0 &&
           operand.loan == 0 && !operand.literal;
  };
  const auto exactResult = [&](const SemanticType &type, ValueCategory category,
                               AccessMode access) {
    return instruction.info.type == type &&
           instruction.info.category == category &&
           instruction.info.access == access &&
           (type == SemanticType::Void ? !instruction.result
                                       : instruction.result.has_value());
  };
  const auto noLoanResult = [&] {
    return !instruction.loan &&
           instruction.borrowOrigin == BorrowOriginKind::None &&
           instruction.borrowArgument == 0 &&
           instruction.borrowAccess == AccessMode::ReadOnly;
  };
  const auto emptyFailure = [&] {
    return instruction.definedFailure.empty() &&
           instruction.localFailureSites.empty();
  };

  if (instruction.intrinsic == IntrinsicKind::MutexLockFailure) {
    const std::vector<DefinedFailureOutcome> expected = {
        {.code = DefinedFailureCode::InfallibleHostOperationFailed,
         .detail = DefinedFailureDetail::MutexLock}};
    return instruction.operands.empty() &&
           exactResult(SemanticType::Void, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() &&
           instruction.synchronization.kind ==
               SynchronizationOperationKind::None &&
           instruction.definedFailure.propagation ==
               FailurePropagationKind::None &&
           instruction.definedFailure.localOrigins.size() == 1 &&
           instruction.definedFailure.localOrigins.front().outcomes ==
               expected &&
           instruction.localFailureSites.size() == 1 &&
           instruction.localFailureSites.front() != 0;
  }

  const bool twoOperand = instruction.operands.size() == 2;
  const bool oneOperand = instruction.operands.size() == 1;
  const MirOperand *storageOperand =
      twoOperand ? &instruction.operands[0] : nullptr;
  const MirOperand *tokenOperand = twoOperand   ? &instruction.operands[1]
                                   : oneOperand ? &instruction.operands[0]
                                                : nullptr;
  const MirClassInstance *storageClass = nullptr;
  const MirClassInstance *tokenClass = nullptr;
  const SemanticType *storageValue =
      storageOperand == nullptr
          ? nullptr
          : exactCapabilityReferenceValue(
                program, storageOperand->type, AccessMode::ReadOnly,
                CompilerCapabilityTypeKind::MutexStorage, &storageClass);
  const AccessMode tokenAccess =
      instruction.intrinsic == IntrinsicKind::MutexLock ||
              instruction.intrinsic == IntrinsicKind::MutexUnlock
          ? AccessMode::Mutable
          : AccessMode::ReadOnly;
  const SemanticType *tokenValue =
      tokenOperand == nullptr
          ? nullptr
          : exactCapabilityReferenceValue(
                program, tokenOperand->type, tokenAccess,
                CompilerCapabilityTypeKind::MutexLockToken, &tokenClass);
  const bool exactPair = tokenValue != nullptr &&
                         (storageValue == nullptr ||
                          (*storageValue == *tokenValue &&
                           storageClass->sourceUnit == tokenClass->sourceUnit));
  if (!exactPair || !emptyFailure()) {
    return false;
  }

  switch (instruction.intrinsic) {
  case IntrinsicKind::MutexLock:
    return twoOperand &&
           exactBorrow(*storageOperand, MirOperandKind::BorrowRead) &&
           exactBorrow(*tokenOperand, MirOperandKind::BorrowWrite) &&
           exactResult(SemanticType::Int32, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() &&
           exactMutexSynchronization(instruction.intrinsic,
                                     instruction.synchronization);
  case IntrinsicKind::MutexUnlock:
    return twoOperand &&
           exactBorrow(*storageOperand, MirOperandKind::BorrowRead) &&
           exactBorrow(*tokenOperand, MirOperandKind::BorrowWrite) &&
           exactResult(SemanticType::Void, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() &&
           exactMutexSynchronization(instruction.intrinsic,
                                     instruction.synchronization);
  case IntrinsicKind::MutexGuardActive:
    return oneOperand &&
           exactBorrow(*tokenOperand, MirOperandKind::BorrowRead) &&
           exactResult(SemanticType::Bool, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() &&
           instruction.synchronization.kind ==
               SynchronizationOperationKind::None;
  case IntrinsicKind::MutexGuardRead:
  case IntrinsicKind::MutexGuardReadMut: {
    const AccessMode resultAccess =
        instruction.intrinsic == IntrinsicKind::MutexGuardReadMut
            ? AccessMode::Mutable
            : AccessMode::ReadOnly;
    const MirLoan *loan =
        instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
    return twoOperand &&
           exactBorrow(*storageOperand, MirOperandKind::BorrowRead) &&
           exactBorrow(*tokenOperand, MirOperandKind::BorrowRead) &&
           exactResult(*storageValue, ValueCategory::Place, resultAccess) &&
           instruction.borrowOrigin == BorrowOriginKind::Argument &&
           instruction.borrowArgument == 0 &&
           instruction.borrowAccess == resultAccess && loan != nullptr &&
           loan->kind == MirLoanKind::CallResult && !loan->lifecycleBound &&
           !loan->entry && !loan->escapes && loan->parent == 0 &&
           loan->source == storageOperand->place &&
           loan->access == resultAccess &&
           loan->producedBy == instruction.hirValue &&
           instruction.synchronization.kind ==
               SynchronizationOperationKind::None;
  }
  default:
    return false;
  }
}

[[nodiscard]] MirVerificationResult
verifyLifecycleBoundLoanContracts(const MirProgram &program,
                                  const MirBody &body, std::size_t owner) {
  struct InstructionLocation {
    const MirBlock *block = nullptr;
    const MirInstruction *instruction = nullptr;
    std::size_t order = 0;
  };
  std::unordered_map<MirInstructionId, InstructionLocation> locations;
  for (const MirBlock &block : body.blocks) {
    for (std::size_t order = 0; order < block.instructions.size(); ++order) {
      locations.emplace(
          block.instructions[order].id,
          InstructionLocation{.block = &block,
                              .instruction = &block.instructions[order],
                              .order = order});
    }
  }
  const std::optional<MirDominanceInfo> dominance = computeMirDominance(body);
  if (!dominance) {
    return verificationFailure(
        body, owner,
        "lifecycle-bound loan verification requires a valid MIR CFG");
  }
  const auto precedes = [&](const InstructionLocation &before,
                            const InstructionLocation &after) {
    return before.block->id == after.block->id
               ? before.order < after.order
               : dominance->dominates(before.block->id, after.block->id);
  };
  const auto carrierClass = [&](const SemanticType &type) {
    const MirClassInstance *instance =
        uniqueMirClassInstanceForType(program, type);
    return instance != nullptr && instance->lifecycleBoundBorrowCarrier
               ? instance
               : nullptr;
  };
  // The Call, Construct or Borrow that produces each loan, absent when none
  // or several do.
  std::unordered_map<MirLoanId, std::optional<InstructionLocation>> producers;
  for (const auto &[_, location] : locations) {
    if (!location.instruction->loan ||
        (location.instruction->kind != MirInstructionKind::Call &&
         location.instruction->kind != MirInstructionKind::Construct &&
         location.instruction->kind != MirInstructionKind::Borrow)) {
      continue;
    }
    const auto [entry, first] =
        producers.try_emplace(*location.instruction->loan, location);
    if (!first) {
      entry->second.reset();
    }
  }
  const auto producerFor = [&](MirLoanId loan) {
    const auto found = producers.find(loan);
    return found == producers.end() ? std::optional<InstructionLocation>{}
                                    : found->second;
  };
  // Indexes for the per-loan checks below. Lists over instructions keep the
  // order of `locations`, so each check visits its candidates in the order a
  // scan of `locations` would.
  std::unordered_map<MirInstructionId, std::vector<const MirBlock *>>
      invokeBlocks;
  // Move, Reparent and Replace targets by source obligation.
  std::unordered_map<MirDropObligationId, std::vector<MirDropObligationId>>
      obligationTransfers;
  for (const MirBlock &block : body.blocks) {
    if (block.terminator.kind == MirTerminatorKind::Invoke) {
      invokeBlocks[block.terminator.invokeInstruction].push_back(&block);
    }
    const auto addTransfers =
        [&](const std::vector<MirLifecycleEvent> &events) {
          for (const MirLifecycleEvent &event : events) {
            if (event.kind == MirLifecycleEventKind::Move ||
                event.kind == MirLifecycleEventKind::Reparent ||
                event.kind == MirLifecycleEventKind::Replace) {
              obligationTransfers[event.source].push_back(event.target);
            }
          }
        };
    for (const MirInstruction &instruction : block.instructions) {
      addTransfers(instruction.lifecycle);
    }
    addTransfers(block.terminator.successLifecycle);
  }
  // Drops that retire, and instructions that transfer out, an obligation.
  std::unordered_map<MirDropObligationId, std::vector<InstructionLocation>>
      dropsBySource;
  std::unordered_map<MirDropObligationId, std::vector<InstructionLocation>>
      transfersOutBySource;
  // EndBorrows of a loan, and Borrows whose single operand forwards a loan.
  std::unordered_map<MirLoanId, std::vector<InstructionLocation>> loanEnds;
  std::unordered_map<MirLoanId, std::vector<InstructionLocation>> loanForwards;
  for (const auto &[_, location] : locations) {
    const MirInstruction &instruction = *location.instruction;
    for (const MirLifecycleEvent &event : instruction.lifecycle) {
      if (event.kind == MirLifecycleEventKind::Drop &&
          instruction.kind == MirInstructionKind::Drop) {
        std::vector<InstructionLocation> &drops = dropsBySource[event.source];
        if (drops.empty() || drops.back().instruction != &instruction) {
          drops.push_back(location);
        }
      }
      if (event.kind == MirLifecycleEventKind::TransferOut) {
        std::vector<InstructionLocation> &transfers =
            transfersOutBySource[event.source];
        if (transfers.empty() || transfers.back().instruction != &instruction) {
          transfers.push_back(location);
        }
      }
    }
    if (instruction.kind == MirInstructionKind::EndBorrow && instruction.loan) {
      loanEnds[*instruction.loan].push_back(location);
    }
    if (instruction.kind == MirInstructionKind::Borrow &&
        instruction.operands.size() == 1 &&
        instruction.operands.front().kind == MirOperandKind::Loan) {
      loanForwards[instruction.operands.front().loan].push_back(location);
    }
  }
  std::unordered_map<MirFailureRecordId, std::vector<const MirFailureRecord *>>
      failureRecordsById;
  for (const MirFailureRecord &record : body.failureRecords) {
    failureRecordsById[record.id].push_back(&record);
  }
  static const std::vector<InstructionLocation> noLocations;
  const auto listed =
      [&](const auto &index,
          const auto &key) -> const std::vector<InstructionLocation> & {
    const auto found = index.find(key);
    return found == index.end() ? noLocations : found->second;
  };

  for (const MirLoan &loan : body.loans) {
    const std::optional<InstructionLocation> producer = producerFor(loan.id);
    const MirPlace *source = body.findPlace(loan.source);
    const auto *function = body.kind == MirBodyKind::Function
                               ? program.findFunctionInstance(owner)
                               : nullptr;
    const auto *constructor = body.kind == MirBodyKind::Constructor
                                  ? program.findConstructorInstance(owner)
                                  : nullptr;
    if (loan.lifecycleBound && loan.storedField != 0 &&
        constructor != nullptr &&
        std::any_of(constructor->initializers.begin(),
                    constructor->initializers.end(),
                    [&](const auto &initializer) {
                      return initializer.field == loan.storedField &&
                             initializer.storesReference;
                    }))
      return verificationFailure(body, owner,
                                 "stored reference field cannot acquire its "
                                 "referent's cleanup obligation",
                                 producer ? producer->block->id : 0,
                                 producer ? producer->instruction->id : 0);
    const auto *lambda =
        body.kind == MirBodyKind::Lambda ? program.findLambda(owner) : nullptr;
    const auto *parameters = function      ? &function->parameterBindings
                             : constructor ? &constructor->parameterBindings
                             : lambda      ? &lambda->parameterBindings
                                           : nullptr;
    const auto *parameterTypes = function      ? &function->parameterTypes
                                 : constructor ? &constructor->parameterTypes
                                 : lambda      ? &lambda->parameterTypes
                                               : nullptr;
    bool ownedEntry = false;
    if (loan.entry && !loan.escapes && loan.kind == MirLoanKind::Stored &&
        !producer && loan.producedBy == 0 && loan.parent == 0 && source &&
        source->root == MirPlaceRootKind::Binding &&
        source->type.kind == SemanticType::Class &&
        std::find(loan.carriers.begin(), loan.carriers.end(),
                  source->binding) != loan.carriers.end() &&
        parameters && parameterTypes) {
      for (std::size_t index = 0;
           index < parameters->size() && index < parameterTypes->size();
           ++index) {
        if ((*parameters)[index] != source->binding)
          continue;
        const auto &formal = (*parameterTypes)[index];
        if (source->projections.empty()) {
          ownedEntry = ownedEntry || formal == source->type;
        } else if (source->projections.size() == 1 &&
                   source->projections.front().kind ==
                       MirProjectionKind::PackElement &&
                   source->projections.front().constantIndex &&
                   index + 1 == parameters->size()) {
          const auto element = *source->projections.front().constantIndex;
          const auto pack = std::find_if(
              body.places.begin(), body.places.end(), [&](const auto &place) {
                return place.root == MirPlaceRootKind::Binding &&
                       place.binding == source->binding &&
                       place.projections.empty() &&
                       place.type.kind == SemanticType::TypePack &&
                       place.type.concretePack;
              });
          const std::vector<SemanticType> remaining(
              parameterTypes->begin() + index, parameterTypes->end());
          ownedEntry =
              ownedEntry || (pack != body.places.end() &&
                             element < pack->type.arguments.size() &&
                             pack->type.arguments[element] == source->type &&
                             exactParameterRoles({pack->type}, remaining));
        }
      }
    }
    const SemanticType carrierType = ownedEntry ? source->type
                                     : producer
                                         ? producer->instruction->info.type
                                         : SemanticType::Unknown;
    const auto *producingInstruction =
        producer ? producer->instruction : nullptr;
    const bool owningPublication =
        producingInstruction &&
        ((loan.kind == MirLoanKind::Stored && !loan.escapes &&
          producingInstruction->result &&
          (producingInstruction->kind == MirInstructionKind::Call ||
           producingInstruction->kind == MirInstructionKind::Construct)) ||
         (((loan.kind == MirLoanKind::Return &&
            body.returnType == carrierType) ||
           (loan.kind == MirLoanKind::Stored && loan.storedField != 0 &&
            producingInstruction->operands.size() == 1 &&
            producingInstruction->operands.front().value != 0)) &&
          producingInstruction->kind == MirInstructionKind::Borrow &&
          producingInstruction->operands.size() == 1 &&
          producingInstruction->operands.front().kind == MirOperandKind::Loan));
    if (!loan.lifecycleBound) {
      // Receiver/reference shell loans do not own the carrier. Only exact
      // by-value entries and owning result/publication records inherit its
      // mandatory destructor-bound retention; clearing the marker cannot
      // downgrade them to an ordinary last-use loan.
      if ((ownedEntry || owningPublication) && carrierClass(carrierType))
        return verificationFailure(
            body, owner,
            "owned cleanup-bound carrier dependency lost lifecycle retention",
            producer ? producer->block->id : 0,
            producer ? producer->instruction->id : 0);
      continue;
    }
    if (source == nullptr || loan.twoPhase ||
        (!ownedEntry && (!producer || loan.producedBy == 0 ||
                         producer->instruction->hirValue != loan.producedBy)) ||
        carrierClass(carrierType) == nullptr) {
      return verificationFailure(
          body, owner,
          "lifecycle-bound loan is not produced by one exact cleanup-bound "
          "borrowed carrier",
          producer ? producer->block->id : 0,
          producer ? producer->instruction->id : 0);
    }

    if (loan.kind == MirLoanKind::Return) {
      const bool exactReturnProducer =
          loan.escapes && !loan.entry && loan.carriers.empty() &&
          loan.storedField == 0 &&
          producer->instruction->kind == MirInstructionKind::Borrow &&
          !producer->instruction->result &&
          producer->instruction->operands.size() == 1 &&
          producer->instruction->operands.front().kind == MirOperandKind::Loan;
      const MirLoan *sourceLoan =
          exactReturnProducer
              ? body.findLoan(producer->instruction->operands.front().loan)
              : nullptr;
      std::size_t returns = 0;
      MirValueId returnedValue = 0;
      for (const MirBlock &block : body.blocks) {
        if (block.terminator.kind == MirTerminatorKind::Return &&
            block.terminator.returnLoan == loan.id && block.terminator.value &&
            block.terminator.value->kind == MirOperandKind::Value &&
            block.terminator.value->type == producer->instruction->info.type) {
          ++returns;
          returnedValue = block.terminator.value->value;
        }
      }
      if (!exactReturnProducer || sourceLoan == nullptr ||
          sourceLoan->kind != MirLoanKind::Stored ||
          !sourceLoan->lifecycleBound || sourceLoan->source != loan.source ||
          sourceLoan->access != loan.access ||
          sourceLoan->designatedFields != loan.designatedFields ||
          sourceLoan->additionalDependencies != loan.additionalDependencies ||
          returns != 1) {
        return verificationFailure(
            body, owner,
            "lifecycle-bound return loan is not the exact escaping transfer "
            "of one stored cleanup-bound dependency",
            producer->block->id, producer->instruction->id);
      }
      // The exported loan bridges cleanup between evaluating an owning result
      // and its final return. On a failed local destructor, that result is
      // still owned here: its dependency may retire only after its exact
      // pending-result destruction, never merely because return was abandoned.
      for (const auto &[_, end] : locations) {
        if (end.instruction->kind != MirInstructionKind::EndBorrow ||
            end.instruction->loan != loan.id)
          continue;
        const bool afterResultDrop =
            end.block->activeFailure != 0 &&
            std::any_of(
                locations.begin(), locations.end(), [&](const auto &item) {
                  const auto &drop = *item.second.instruction;
                  if (drop.kind != MirInstructionKind::Drop ||
                      !precedes(item.second, end) ||
                      drop.info.type != producer->instruction->info.type)
                    return false;
                  return std::any_of(
                      drop.lifecycle.begin(), drop.lifecycle.end(),
                      [&](const MirLifecycleEvent &event) {
                        const auto *obligation =
                            body.findDropObligation(event.source);
                        const auto *place =
                            obligation ? body.findPlace(obligation->place)
                                       : nullptr;
                        return event.kind == MirLifecycleEventKind::Drop &&
                               event.failureCleanup && !event.conditional &&
                               event.target == 0 && obligation && place &&
                               obligation->kind ==
                                   MirDropObligationKind::ReturnResult &&
                               obligation->dropType.type ==
                                   producer->instruction->info.type &&
                               drop.destination == place->id &&
                               place->root == MirPlaceRootKind::Value &&
                               place->value == returnedValue &&
                               place->projections.empty();
                      });
                });
        if (!afterResultDrop)
          return verificationFailure(body, owner,
                                     "cleanup-bound return dependency ends "
                                     "before its exact pending result drop",
                                     end.block->id, end.instruction->id);
      }
      continue;
    }

    // The constructor summary proves the exact field, moved parameter and
    // dependency set. Cleanup-bearing publication must additionally reparent
    // that same value's obligation into this exact field's rollback slot.
    // Merely destroying or transferring an unrelated same-typed value cannot
    // satisfy the publication contract.
    if (loan.kind == MirLoanKind::Stored && loan.escapes &&
        loan.storedField != 0 && constructor != nullptr) {
      const auto checked =
          verifyMirConstructorBorrowSummary(program, *constructor);
      if (!checked.valid())
        return checked;
      const auto &publication = *producer->instruction;
      const auto valueId = publication.operands.size() == 1
                               ? publication.operands.front().value
                               : 0;
      const bool staged =
          valueId != 0 &&
          std::any_of(
              locations.begin(), locations.end(), [&](const auto &item) {
                const auto &stage = *item.second.instruction;
                const auto *destination =
                    stage.destination ? body.findPlace(*stage.destination)
                                      : nullptr;
                if (stage.kind != MirInstructionKind::Initialize ||
                    !precedes(item.second, *producer) ||
                    stage.info.type != carrierType ||
                    stage.operands.size() != 1 ||
                    stage.operands.front().kind != MirOperandKind::Value ||
                    stage.operands.front().value != valueId || !destination ||
                    destination->root != MirPlaceRootKind::This ||
                    destination->projections.size() != 1 ||
                    destination->projections.front().kind !=
                        MirProjectionKind::Field ||
                    destination->projections.front().field != loan.storedField)
                  return false;
                return std::any_of(
                    stage.lifecycle.begin(), stage.lifecycle.end(),
                    [&](const MirLifecycleEvent &event) {
                      const auto *from = body.findDropObligation(event.source);
                      const auto *to = body.findDropObligation(event.target);
                      const auto *valuePlace =
                          from ? body.findPlace(from->place) : nullptr;
                      return event.kind == MirLifecycleEventKind::Reparent &&
                             !event.conditional && !event.failureCleanup &&
                             from && to &&
                             from->kind == MirDropObligationKind::Value &&
                             !from->initiallyActive &&
                             from->dropType.type == carrierType && valuePlace &&
                             valuePlace->root == MirPlaceRootKind::Value &&
                             valuePlace->value == valueId &&
                             valuePlace->projections.empty() &&
                             to->kind ==
                                 MirDropObligationKind::ConstructionRollback &&
                             !to->initiallyActive &&
                             to->place == destination->id &&
                             to->dropType == from->dropType;
                    });
              });
      if (!staged)
        return verificationFailure(
            body, owner,
            "cleanup-bound field publication lacks its exact value-to-field "
            "rollback transfer",
            producer->block->id, publication.id);
      continue;
    }

    const bool exactStored =
        ownedEntry ||
        (loan.kind == MirLoanKind::Stored && !loan.entry && !loan.escapes &&
         loan.storedField == 0 &&
         (producer->instruction->kind == MirInstructionKind::Call ||
          producer->instruction->kind == MirInstructionKind::Construct) &&
         producer->instruction->result &&
         producer->instruction->borrowAccess == loan.access &&
         (producer->instruction->borrowOrigin == BorrowOriginKind::Receiver ||
          producer->instruction->borrowOrigin == BorrowOriginKind::Argument));
    if (!exactStored) {
      return verificationFailure(
          body, owner,
          "lifecycle-bound stored loan does not retain the exact local "
          "cleanup-bound result shape",
          producer->block->id, producer->instruction->id);
    }

    std::unordered_set<MirDropObligationId> carrierObligations;
    const auto seed = [&](MirDropObligationId obligation) {
      const MirDropObligation *drop = body.findDropObligation(obligation);
      if (drop != nullptr && drop->dropType.type == carrierType) {
        carrierObligations.insert(obligation);
      }
    };
    if (ownedEntry) {
      // The exact by-value formal owns this initially active drop; a checked
      // reference parameter or abstract receiver cannot synthesize one.
      for (const MirDropObligation &drop : body.dropObligations)
        if (drop.kind == MirDropObligationKind::Binding &&
            drop.binding == source->binding && drop.place == source->id &&
            drop.initiallyActive)
          seed(drop.id);
    }
    if (producer && producer->instruction->successResultDrop) {
      seed(*producer->instruction->successResultDrop);
    }
    if (producer)
      for (const MirLifecycleEvent &event : producer->instruction->lifecycle) {
        if (event.kind == MirLifecycleEventKind::Initialize) {
          seed(event.target);
        }
      }
    if (const auto invokes = producer
                                 ? invokeBlocks.find(producer->instruction->id)
                                 : invokeBlocks.end();
        invokes != invokeBlocks.end()) {
      for (const MirBlock *block : invokes->second) {
        for (const MirLifecycleEvent &event :
             block->terminator.successLifecycle) {
          if (event.kind == MirLifecycleEventKind::Initialize) {
            seed(event.target);
          }
        }
      }
    }
    // Close the carrier obligations under Move, Reparent and Replace.
    std::vector<MirDropObligationId> pendingObligations(
        carrierObligations.begin(), carrierObligations.end());
    while (!pendingObligations.empty()) {
      const MirDropObligationId source = pendingObligations.back();
      pendingObligations.pop_back();
      const auto targets = obligationTransfers.find(source);
      if (targets == obligationTransfers.end()) {
        continue;
      }
      for (const MirDropObligationId target : targets->second) {
        const std::size_t before = carrierObligations.size();
        seed(target);
        if (carrierObligations.size() != before) {
          pendingObligations.push_back(target);
        }
      }
    }
    if (carrierObligations.empty()) {
      return verificationFailure(
          body, owner,
          "lifecycle-bound borrowed carrier has no exact drop obligation",
          producer ? producer->block->id : 0,
          producer ? producer->instruction->id : 0);
    }

    if (ownedEntry) {
      // Local carriers may retain an entry dependency only by moving that
      // formal's exact owned obligation. A same-typed unrelated local is not
      // evidence of a transfer and must not acquire the formal's provenance.
      for (const auto binding : loan.carriers) {
        const bool transferred = std::any_of(
            carrierObligations.begin(), carrierObligations.end(),
            [&](MirDropObligationId id) {
              const auto *drop = body.findDropObligation(id);
              const auto *place = drop ? body.findPlace(drop->place) : nullptr;
              return drop && drop->kind == MirDropObligationKind::Binding &&
                     drop->binding == binding && place &&
                     place->root == MirPlaceRootKind::Binding &&
                     place->binding == binding &&
                     (place->id == source->id || place->projections.empty());
            });
        if (!transferred)
          return verificationFailure(
              body, owner,
              "owned entry carrier lacks an exact lifecycle transfer from "
              "its formal parameter");
      }
    }

    // Drops of the carrier type that retire a carrier obligation, and calls
    // that transfer one out; only the latter can complete an owned handoff.
    std::vector<InstructionLocation> carrierDrops;
    std::vector<InstructionLocation> carrierTransfersOut;
    {
      std::unordered_set<const MirInstruction *> seenDrops;
      std::unordered_set<const MirInstruction *> seenTransfers;
      for (const MirDropObligationId obligation : carrierObligations) {
        for (const InstructionLocation &location :
             listed(dropsBySource, obligation)) {
          if (location.instruction->info.type == carrierType &&
              seenDrops.insert(location.instruction).second) {
            carrierDrops.push_back(location);
          }
        }
        for (const InstructionLocation &location :
             listed(transfersOutBySource, obligation)) {
          if (seenTransfers.insert(location.instruction).second) {
            carrierTransfersOut.push_back(location);
          }
        }
      }
    }

    const std::vector<InstructionLocation> &ends = listed(loanEnds, loan.id);
    if (ends.empty()) {
      return verificationFailure(
          body, owner,
          "local lifecycle-bound borrowed carrier loan has no explicit end",
          producer ? producer->block->id : 0,
          producer ? producer->instruction->id : 0);
    }
    for (const InstructionLocation &end : ends) {
      const auto activeRecords =
          failureRecordsById.find(end.block->activeFailure);
      const bool producerFailure =
          activeRecords != failureRecordsById.end() &&
          std::any_of(activeRecords->second.begin(),
                      activeRecords->second.end(),
                      [&](const MirFailureRecord *record) {
                        return producer &&
                               record->producerBlock == producer->block->id &&
                               record->producerInstruction ==
                                   producer->instruction->id &&
                               dominance->dominates(record->parameterBlock,
                                                    end.block->id);
                      });
      const bool afterCarrierDrop = std::any_of(
          carrierDrops.begin(), carrierDrops.end(),
          [&](const InstructionLocation &drop) { return precedes(drop, end); });
      const bool afterOwnedCall = std::any_of(
          carrierTransfersOut.begin(), carrierTransfersOut.end(),
          [&](const InstructionLocation &callLocation) {
            const auto &call = *callLocation.instruction;
            // Generated movement has no callee/CallInput: the already-moved
            // SSA object hands its exact obligation and every dependency to
            // a new result. Ending the old loan is allowed only after that
            // complete handoff, not merely after any same-typed construction.
            const auto *movedClass = carrierClass(carrierType);
            if (call.kind == MirInstructionKind::Construct &&
                call.constructorKind == ConstructorKind::Move &&
                !call.constructorTarget && !call.functionTarget &&
                !call.receiver && call.callSite == 0 &&
                call.intrinsic == IntrinsicKind::None && call.result &&
                call.definedFailure.empty() && call.info.type == carrierType &&
                call.operands.size() == 1 &&
                call.operands.front().kind == MirOperandKind::Value &&
                call.operands.front().type == carrierType &&
                call.parameterTypes.size() == 1 &&
                call.parameterTypes.front().kind == SemanticType::Reference &&
                call.parameterTypes.front().arguments ==
                    std::vector<SemanticType>{carrierType} &&
                movedClass &&
                movedClass->moveConstructor == SpecialMemberStatus::Generated &&
                call.loan && *call.loan != loan.id &&
                precedes(callLocation, end)) {
              const auto *resultLoan = body.findLoan(*call.loan);
              const auto *input =
                  definitionFor(body, call.operands.front().value);
              if (!resultLoan || resultLoan->kind != MirLoanKind::Stored ||
                  !resultLoan->lifecycleBound || resultLoan->entry ||
                  resultLoan->escapes || resultLoan->storedField != 0 ||
                  resultLoan->producedBy != call.hirValue ||
                  resultLoan->source != loan.source ||
                  resultLoan->access != loan.access ||
                  resultLoan->parent != loan.parent ||
                  resultLoan->storedDependency != loan.storedDependency ||
                  resultLoan->designatedFields != loan.designatedFields ||
                  resultLoan->additionalDependencies !=
                      loan.additionalDependencies ||
                  !input || input->kind != MirInstructionKind::Move ||
                  input->info.type != carrierType ||
                  input->lifecycle.size() != 1 ||
                  input->lifecycle.front().kind !=
                      MirLifecycleEventKind::Move ||
                  !carrierObligations.contains(
                      input->lifecycle.front().source) ||
                  call.lifecycle.size() != 2)
                return false;
              const auto *inputDrop =
                  body.findDropObligation(input->lifecycle.front().target);
              const auto *inputPlace =
                  inputDrop ? body.findPlace(inputDrop->place) : nullptr;
              if (!inputDrop ||
                  inputDrop->kind != MirDropObligationKind::Value ||
                  inputDrop->value != input->hirValue ||
                  inputDrop->dropType.type != carrierType || !inputPlace ||
                  inputPlace->root != MirPlaceRootKind::Value ||
                  inputPlace->value != call.operands.front().value ||
                  !inputPlace->projections.empty())
                return false;
              const auto &transfer = call.lifecycle.front();
              const auto &initialize = call.lifecycle.back();
              const auto *resultDrop =
                  body.findDropObligation(initialize.target);
              const auto *resultPlace =
                  resultDrop ? body.findPlace(resultDrop->place) : nullptr;
              return transfer.kind == MirLifecycleEventKind::TransferOut &&
                     transfer.source == inputDrop->id && transfer.target == 0 &&
                     !transfer.conditional && !transfer.failureCleanup &&
                     initialize.kind == MirLifecycleEventKind::Initialize &&
                     initialize.source == 0 && !initialize.conditional &&
                     !initialize.failureCleanup && resultDrop && resultPlace &&
                     resultDrop->kind == MirDropObligationKind::Value &&
                     resultDrop->value == call.hirValue &&
                     resultDrop->dropType == inputDrop->dropType &&
                     resultPlace->root == MirPlaceRootKind::Value &&
                     resultPlace->value == *call.result &&
                     resultPlace->projections.empty();
            }
            if ((call.kind != MirInstructionKind::Call &&
                 call.kind != MirInstructionKind::Construct) ||
                (!call.functionTarget && !call.constructorTarget) ||
                !precedes(callLocation, end))
              return false;
            for (std::size_t index = 0; index < call.operands.size() &&
                                        index < call.parameterTypes.size();
                 ++index) {
              const auto &operand = call.operands[index];
              if (call.parameterTypes[index] != carrierType ||
                  operand.kind != MirOperandKind::Value)
                continue;
              const auto *input = definitionFor(body, operand.value);
              if (!input || input->kind != MirInstructionKind::CallInput ||
                  input->callSite != call.callSite ||
                  !input->preparedParameterDrop ||
                  !carrierObligations.contains(*input->preparedParameterDrop))
                continue;
              if (std::any_of(call.lifecycle.begin(), call.lifecycle.end(),
                              [&](const MirLifecycleEvent &event) {
                                return event.kind ==
                                           MirLifecycleEventKind::TransferOut &&
                                       event.source ==
                                           *input->preparedParameterDrop &&
                                       event.target == 0 &&
                                       !event.conditional &&
                                       !event.failureCleanup;
                              }))
                return true;
            }
            return false;
          });
      const bool publishedField =
          constructor != nullptr &&
          verifyMirConstructorBorrowSummary(program, *constructor).valid() &&
          std::any_of(
              listed(loanForwards, loan.id).begin(),
              listed(loanForwards, loan.id).end(),
              [&](const InstructionLocation &location) {
                const auto &publication = *location.instruction;
                if (!precedes(location, end) ||
                    publication.kind != MirInstructionKind::Borrow ||
                    !publication.loan || publication.operands.size() != 1 ||
                    publication.operands.front().kind != MirOperandKind::Loan ||
                    publication.operands.front().loan != loan.id)
                  return false;
                const auto *fieldLoan = body.findLoan(*publication.loan);
                return fieldLoan && fieldLoan->lifecycleBound &&
                       fieldLoan->escapes &&
                       fieldLoan->kind == MirLoanKind::Stored &&
                       fieldLoan->storedField != 0 &&
                       fieldLoan->source == loan.source &&
                       fieldLoan->access == loan.access;
              });
      const bool returned = std::any_of(
          listed(loanForwards, loan.id).begin(),
          listed(loanForwards, loan.id).end(),
          [&](const InstructionLocation &location) {
            const auto &candidate = *location.instruction;
            if (candidate.kind != MirInstructionKind::Borrow ||
                !precedes(location, end) || !candidate.loan ||
                candidate.operands.size() != 1 ||
                candidate.operands.front().kind != MirOperandKind::Loan ||
                candidate.operands.front().loan != loan.id) {
              return false;
            }
            const MirLoan *transferred = body.findLoan(*candidate.loan);
            if (transferred == nullptr || !transferred->lifecycleBound ||
                transferred->kind != MirLoanKind::Return)
              return false;
            return std::any_of(
                body.blocks.begin(), body.blocks.end(),
                [&](const MirBlock &exit) {
                  const auto &term = exit.terminator;
                  if (term.kind != MirTerminatorKind::Return ||
                      term.returnLoan != transferred->id || !term.value ||
                      term.value->kind != MirOperandKind::Value ||
                      term.value->type != carrierType)
                    return false;
                  return std::any_of(
                      carrierObligations.begin(), carrierObligations.end(),
                      [&](MirDropObligationId id) {
                        const auto *drop = body.findDropObligation(id);
                        const auto *place =
                            drop ? body.findPlace(drop->place) : nullptr;
                        return place &&
                               place->root == MirPlaceRootKind::Value &&
                               place->value == term.value->value &&
                               place->projections.empty();
                      });
                });
          });
      if (!producerFailure && !afterCarrierDrop && !afterOwnedCall &&
          !publishedField && !returned) {
        return verificationFailure(body, owner,
                                   "lifecycle-bound borrowed carrier loan ends "
                                   "before carrier drop or its "
                                   "exact return transfer",
                                   end.block->id, end.instruction->id);
      }
    }
  }
  return {};
}

[[nodiscard]] std::optional<SemanticType>
mirFieldProjectionType(const MirProgram &program, const SemanticType &receiver,
                       SymbolId symbol) {
  if (receiver.kind == SemanticType::Lambda) {
    const auto instance = std::find_if(
        program.lambdaInstances().begin(), program.lambdaInstances().end(),
        [&](const auto &candidate) { return candidate.type == receiver; });
    if (instance == program.lambdaInstances().end() ||
        instance->captureSymbols.size() != instance->captureTypes.size())
      return std::nullopt;
    for (std::size_t index = 0; index < instance->captureSymbols.size();
         ++index)
      if (instance->captureSymbols[index] == symbol)
        return instance->captureTypes[index];
    return std::nullopt;
  }
  const MirClassInstance *root =
      uniqueMirClassInstanceForType(program, receiver);
  if (root == nullptr || symbol == 0) {
    return std::nullopt;
  }
  std::optional<SemanticType> result;
  bool ambiguous = false;
  std::unordered_set<HirClassInstanceId> seen;
  const auto visit = [&](const auto &self,
                         const MirClassInstance &instance) -> void {
    if (ambiguous || !seen.insert(instance.id).second) {
      return;
    }
    for (const MirClassFieldInfo &field : instance.declaredFields) {
      if (field.symbol != symbol) {
        continue;
      }
      if (result) {
        ambiguous = true;
        return;
      }
      result = field.type;
    }
    for (const HirBaseInstance &base : instance.structuralBases) {
      const MirClassInstance *baseInstance =
          program.findClassInstance(base.instance);
      if (baseInstance == nullptr) {
        ambiguous = true;
        return;
      }
      self(self, *baseInstance);
    }
  };
  visit(visit, *root);
  return ambiguous ? std::nullopt : result;
}

[[nodiscard]] std::optional<SemanticType>
projectedMirType(const MirProgram &program, SemanticType current,
                 const std::vector<MirPlaceProjection> &projections) {
  for (const MirPlaceProjection &projection : projections) {
    switch (projection.kind) {
    case MirProjectionKind::Base: {
      const MirClassInstance *base =
          program.findClassInstance(projection.selection);
      const auto parent = std::find_if(program.classInstances().begin(),
                                       program.classInstances().end(),
                                       [&](const MirClassInstance &candidate) {
                                         return candidate.type == current;
                                       });
      if (base == nullptr || parent == program.classInstances().end() ||
          std::none_of(parent->bases.begin(), parent->bases.end(),
                       [&](const HirBaseInstance &candidate) {
                         return !candidate.interface &&
                                candidate.instance == base->id &&
                                candidate.type == base->type;
                       })) {
        return std::nullopt;
      }
      current = base->type;
      break;
    }
    case MirProjectionKind::Field: {
      std::optional<SemanticType> field =
          mirFieldProjectionType(program, current, projection.field);
      if (!field) {
        return std::nullopt;
      }
      current = std::move(*field);
      break;
    }
    case MirProjectionKind::Index:
      if (current.kind != SemanticType::Array ||
          current.arguments.size() != 1) {
        return std::nullopt;
      }
      {
        SemanticType element = current.arguments.front();
        current = std::move(element);
      }
      break;
    case MirProjectionKind::Dereference:
      if ((current.kind != SemanticType::Reference &&
           current.kind != SemanticType::UniqueOwner &&
           current.kind != SemanticType::SharedPointer) ||
          current.arguments.size() != 1) {
        return std::nullopt;
      }
      {
        SemanticType pointee = current.arguments.front();
        current = std::move(pointee);
      }
      break;
    case MirProjectionKind::RawIndex:
    case MirProjectionKind::RawDereference:
      if (current.kind != SemanticType::RawPointer ||
          current.arguments.size() != 1) {
        return std::nullopt;
      }
      {
        SemanticType pointee = current.arguments.front();
        current = std::move(pointee);
      }
      break;
    case MirProjectionKind::PackElement:
      if (current.kind != SemanticType::TypePack || !current.concretePack ||
          !projection.constantIndex ||
          *projection.constantIndex >= current.arguments.size()) {
        return std::nullopt;
      }
      {
        SemanticType element =
            current
                .arguments[static_cast<std::size_t>(*projection.constantIndex)];
        current = std::move(element);
      }
      break;
    }
  }
  return current;
}

[[nodiscard]] MirVerificationResult
verifyOwnedPointeeProjection(const MirProgram &program,
                             const MirFunctionInstance &function) {
  if (!function.ownedPointeeProjection)
    return {};
  const auto reject = [&] {
    return verificationFailure(
        function.body, function.id,
        "owned-pointee projection summary does not prove one exact "
        "unique-owner field borrow and return");
  };
  const OwnedPointeeProjection &summary = *function.ownedPointeeProjection;
  const bool mutableBorrow =
      summary.borrowIntrinsic == IntrinsicKind::UniqueOwnerBorrowMut;
  const AccessMode access =
      mutableBorrow ? AccessMode::Mutable : AccessMode::ReadOnly;
  if (summary.fields.size() != 1 || summary.fields.front() == 0 ||
      (!mutableBorrow &&
       summary.borrowIntrinsic != IntrinsicKind::UniqueOwnerBorrow) ||
      !function.owner || function.staticMember || function.virtualMethod ||
      function.pureVirtual || function.overrideMethod ||
      !function.virtualRoots.empty() ||
      function.definitionKind != MirDefinitionKind::Source ||
      !function.parameterTypes.empty() || !function.parameterBindings.empty() ||
      function.receiverMutability == ReceiverMutability::Consuming ||
      (mutableBorrow &&
       function.receiverMutability != ReceiverMutability::Mutable) ||
      function.returnType.kind != SemanticType::Reference ||
      function.returnType.arguments.size() != 1 ||
      function.returnType.referenceAccess != access ||
      function.returnBorrowOrigin != BorrowOriginKind::Receiver ||
      function.returnBorrowParameter != 0 || function.returnBorrowPlace ||
      function.returnBorrowAccess != access || !function.mayRaiseDefinedFailure)
    return reject();
  const MirClassInstance *owner = program.findClassInstance(*function.owner);
  if (owner == nullptr)
    return reject();
  const auto field =
      std::find_if(owner->declaredFields.begin(), owner->declaredFields.end(),
                   [&](const auto &candidate) {
                     return candidate.symbol == summary.fields.front();
                   });
  if (field == owner->declaredFields.end() ||
      (mutableBorrow && field->access != AccessMode::Mutable) ||
      field->type.kind != SemanticType::UniqueOwner ||
      field->type.arguments.size() != 1 ||
      field->type.arguments.front() != function.returnType.arguments.front())
    return reject();

  // Deliberately bounded to the source single-return intrinsic accessor. The
  // only branch is the intrinsic's checked empty-owner failure; ordinary
  // calls, writes, conditional return origins, and borrowed forwarding are
  // not interchangeable with this owning provenance.
  const MirBody &body = function.body;
  const MirBlock *entry = body.findBlock(body.entry);
  if (body.blocks.size() != 3 || !body.dropObligations.empty() ||
      body.loans.size() != 2 || body.fullExpressions.size() != 1 ||
      entry == nullptr || entry->instructions.size() != 2 ||
      entry->terminator.kind != MirTerminatorKind::Invoke)
    return reject();
  const MirInstruction &load = entry->instructions.front();
  const MirInstruction &call = entry->instructions.back();
  if (load.kind != MirInstructionKind::Load || !load.result ||
      load.operands.size() != 1 ||
      load.operands.front().kind != MirOperandKind::Copy ||
      !load.lifecycle.empty() || !load.definedFailure.empty() ||
      call.intrinsic != summary.borrowIntrinsic ||
      !exactUniqueOwnerBorrowContract(body, call) ||
      call.operands.front().value != *load.result ||
      entry->terminator.invokeInstruction != call.id ||
      entry->terminator.target == entry->terminator.elseTarget ||
      !entry->terminator.successLifecycle.empty())
    return reject();
  const MirPlace *place = body.findPlace(load.operands.front().place);
  if (place == nullptr || place->root != MirPlaceRootKind::This ||
      place->type != field->type || place->projections.size() != 1 ||
      place->projections.front().kind != MirProjectionKind::Field ||
      place->projections.front().field != summary.fields.front() ||
      !place->key || !place->key->receiver || place->key->root != 0 ||
      place->key->projections.size() != 1 ||
      place->key->projections.front().kind != PlaceProjectionKind::Field ||
      place->key->projections.front().field != summary.fields.front())
    return reject();
  const MirBlock *success = body.findBlock(entry->terminator.target);
  const MirBlock *failure = body.findBlock(entry->terminator.elseTarget);
  if (success == nullptr || failure == nullptr || success == entry ||
      failure == entry || success->instructions.size() != 3 ||
      failure->instructions.size() != 1 ||
      success->terminator.kind != MirTerminatorKind::Return ||
      !success->terminator.returnLoan || !success->terminator.value ||
      failure->terminator.kind != MirTerminatorKind::PropagateFailure ||
      failure->terminator.failureRecord != entry->terminator.failureRecord)
    return reject();
  const MirLoan *returned = body.findLoan(*success->terminator.returnLoan);
  if (returned == nullptr || returned->kind != MirLoanKind::Return ||
      !returned->escapes || returned->entry || returned->parent != 0 ||
      returned->lifecycleBound || returned->twoPhase ||
      !returned->carriers.empty() || returned->source != place->id ||
      returned->access != access || returned->producedBy != call.hirValue ||
      success->terminator.value->kind != MirOperandKind::Loan ||
      success->terminator.value->loan != returned->id ||
      success->terminator.value->type != field->type.arguments.front())
    return reject();
  const MirInstruction &borrow = success->instructions[0];
  const MirInstruction &end = success->instructions[1];
  const MirInstruction &boundary = success->instructions[2];
  const MirInstruction &failedEnd = failure->instructions.front();
  if (borrow.kind != MirInstructionKind::Borrow ||
      borrow.loan != returned->id || borrow.operands.size() != 1 ||
      borrow.operands.front().kind != MirOperandKind::Loan ||
      borrow.operands.front().loan != *call.loan ||
      borrow.hirValue != call.hirValue || !borrow.lifecycle.empty() ||
      !borrow.definedFailure.empty() ||
      end.kind != MirInstructionKind::EndBorrow || end.loan != call.loan ||
      !end.lifecycle.empty() || !end.definedFailure.empty() ||
      failedEnd.kind != MirInstructionKind::EndBorrow ||
      failedEnd.loan != call.loan || !failedEnd.lifecycle.empty() ||
      !failedEnd.definedFailure.empty() ||
      boundary.kind != MirInstructionKind::Lifecycle ||
      !boundary.lifecycle.empty() || !boundary.definedFailure.empty() ||
      boundary.fullExpressionEnd != body.fullExpressions.front().id ||
      boundary.cleanupBoundaryEnd != 0 ||
      body.fullExpressions.front().roots !=
          std::vector<HirValueId>{call.hirValue})
    return reject();
  return {};
}

} // namespace

static MirVerificationResult
verifyMirProgramContracts(const MirProgram &program) {
  MirVerificationResult result;
  if (!program.valid()) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .message = "MIR program is marked invalid"});
  }
  const FailureMetadataVerificationResult failureMetadata =
      verifyFailureMetadata(program.failureMetadata());
  for (const std::string &error : failureMetadata.errors) {
    result.errors.push_back({.bodyKind = MirBodyKind::Module,
                             .message = "invalid failure metadata: " + error});
  }

  const std::optional<MirEmbeddingPlan> &embedding = program.embeddingPlan();
  const auto addEmbeddingError = [&](std::string message,
                                     HirFunctionInstanceId function = 0) {
    result.errors.push_back({.bodyKind = function == 0 ? MirBodyKind::Module
                                                       : MirBodyKind::Function,
                             .owner = function,
                             .message = std::move(message)});
  };
  if (program.programKind() == ProgramKind::Executable && embedding) {
    addEmbeddingError("executable MIR carries a host embedding plan");
  }
  if (program.programKind() == ProgramKind::Library && !embedding) {
    addEmbeddingError("library MIR lacks its host embedding plan");
  }
  if (embedding) {
    if (program.programKind() != ProgramKind::Library ||
        embedding->abiVersion != embeddingAbiVersionV1 ||
        embedding->exports.empty()) {
      addEmbeddingError("MIR embedding plan has an invalid kind or ABI");
    }
    std::string previousSymbol;
    std::unordered_set<HirFunctionInstanceId> selectedFunctions;
    for (std::size_t index = 0; index < embedding->exports.size(); ++index) {
      const MirEmbeddingExport &selected = embedding->exports[index];
      const MirFunctionInstance *function =
          program.findFunctionInstance(selected.function);
      const bool sorted = index == 0 || previousSymbol < selected.symbol;
      const bool uniqueFunction =
          selectedFunctions.insert(selected.function).second;
      if (!sorted || !uniqueFunction || selected.declaration == 0 ||
          selected.function == 0 || selected.qualifiedName.empty() ||
          selected.symbol.empty() || selected.requestSource.source.empty() ||
          function == nullptr || function->id != selected.function ||
          function->declaration != selected.declaration || function->owner ||
          !function->typeArguments.empty() ||
          !function->valueArguments.empty() ||
          function->returnType != selected.returnType ||
          function->parameterTypes != selected.parameterTypes ||
          function->entryKind != ProgramEntryKind::None ||
          function->linkage != LanguageLinkage::Gti ||
          function->definitionKind != MirDefinitionKind::Source) {
        addEmbeddingError(
            "MIR embedding export has no exact source function root",
            selected.function);
      }
      previousSymbol = selected.symbol;
    }
  }
  if (program.programKind() == ProgramKind::Library &&
      (program.hostedStartupPlan() || program.hostedStartup() != nullptr ||
       !program.nativeCallbackAdapters().empty())) {
    addEmbeddingError(
        "library MIR retains hosted startup or process-lifetime callbacks");
  }

  const FailureOriginAssignmentLookup failureOrigins(program.failureMetadata());
  const auto verifyBody = [&](const MirBody &body, std::size_t owner) {
    const auto verifyStorageType = [&](const SemanticType &type) {
      if (!storageOwnershipTypeValid(program, type))
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .message = "compiler-owned storage payload retains borrowed state "
                        "without an owner-dependency contract"});
    };
    for (const auto &place : body.places)
      verifyStorageType(place.type);
    for (const auto &value : body.values)
      verifyStorageType(value.info.type);
    verifyStorageType(body.returnType);
    if (body.kind == MirBodyKind::Function) {
      if (const auto *function = program.findFunctionInstance(owner))
        for (const auto &parameter : function->parameterTypes)
          verifyStorageType(parameter);
    } else if (body.kind == MirBodyKind::Constructor) {
      if (const auto *constructor = program.findConstructorInstance(owner))
        for (const auto &parameter : constructor->parameterTypes)
          verifyStorageType(parameter);
    }
    appendVerification(result, verifyMirCheckedIntegerFailureContracts(
                                   failureOrigins, body, owner));
    appendVerification(result,
                       verifyLifecycleBoundLoanContracts(program, body, owner));
    const std::vector<HirBindingId> *parameters = nullptr;
    const std::vector<SemanticType> *parameterTypes = nullptr;
    if (body.kind == MirBodyKind::Function) {
      const auto *function = program.findFunctionInstance(owner);
      if (function != nullptr &&
          function->definitionKind == MirDefinitionKind::Source) {
        parameters = &function->parameterBindings;
        parameterTypes = &function->parameterTypes;
      }
    } else if (body.kind == MirBodyKind::Constructor) {
      const auto *constructor = program.findConstructorInstance(owner);
      if (constructor != nullptr &&
          constructor->definitionKind == MirDefinitionKind::Source) {
        parameters = &constructor->parameterBindings;
        parameterTypes = &constructor->parameterTypes;
      }
    } else if (body.kind == MirBodyKind::Lambda) {
      if (const auto *lambda = program.findLambda(owner)) {
        parameters = &lambda->parameterBindings;
        parameterTypes = &lambda->parameterTypes;
      }
    }
    if (parameters != nullptr) {
      // Native formal transport consumes these resolved copy/move facts. A
      // binding cannot independently grant copying, forbid an available move,
      // or claim movability contrary to its concrete MIR type. Reference and
      // pack roots have distinct carrier/flattening contracts below and do not
      // represent a by-value native object prologue.
      std::vector<SemanticType> bindingTypes;
      for (const HirBindingId binding : *parameters) {
        const auto formal = std::find_if(
            body.places.begin(), body.places.end(), [&](const MirPlace &place) {
              return place.root == MirPlaceRootKind::Binding &&
                     place.binding == binding && place.projections.empty();
            });
        if (binding == 0 || formal == body.places.end()) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .message = "source formal has no exact binding root"});
          continue;
        }
        bindingTypes.push_back(formal->type);
        if (formal->type.kind == SemanticType::Reference ||
            formal->type.kind == SemanticType::TypePack)
          continue;
        const auto traits = concreteMirTypeTraits(program, formal->type);
        for (const MirPlace &place : body.places) {
          if (place.root != MirPlaceRootKind::Binding ||
              place.binding != binding || !place.projections.empty())
            continue;
          if (!traits || place.type != formal->type ||
              place.traits.copyable != traits->copyable ||
              place.traits.movable != traits->movable)
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .message = "by-value formal copy/move facts do not match its "
                            "concrete MIR type"});
        }
      }
      if (parameterTypes != nullptr &&
          !exactParameterRoles(bindingTypes, *parameterTypes))
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .message = "source formal bindings do not match its parameter "
                        "roles"});
      for (const HirBindingId binding : *parameters) {
        const auto pack = std::find_if(
            body.places.begin(), body.places.end(), [&](const MirPlace &place) {
              return place.root == MirPlaceRootKind::Binding &&
                     place.binding == binding && place.projections.empty() &&
                     place.type.kind == SemanticType::TypePack &&
                     place.type.concretePack;
            });
        if (pack == body.places.end())
          continue;
        // The element a return-origin contract names seeds its own Parameter
        // entry loan, exactly as a whole read-only reference formal does.
        std::optional<std::size_t> summarizedElement;
        {
          std::optional<mir_detail::MirReturnOriginFormal> formal;
          if (const auto *function = program.findFunctionInstance(owner);
              body.kind == MirBodyKind::Function && function != nullptr &&
              function->returnBorrowOrigin == BorrowOriginKind::Argument)
            formal = mir_detail::returnOriginFormal(
                function->parameterTypes, function->parameterBindings,
                function->returnBorrowParameter);
          else if (const auto *lambda = program.findLambda(owner);
                   body.kind == MirBodyKind::Lambda && lambda != nullptr &&
                   lambda->returnBorrowOrigin == BorrowOriginKind::Argument)
            formal = mir_detail::returnOriginFormal(
                lambda->parameterTypes, lambda->parameterBindings,
                lambda->returnBorrowParameter);
          if (formal && formal->binding == binding)
            summarizedElement = formal->elementWithinBinding;
        }
        std::size_t previousOrder = 0;
        for (std::size_t element = 0; element < pack->type.arguments.size();
             ++element) {
          const SemanticType &type = pack->type.arguments[element];
          const bool summarizedOrigin =
              summarizedElement == element &&
              type.kind == SemanticType::Reference &&
              type.referenceAccess == AccessMode::ReadOnly;
          const bool mutableReference =
              type.kind == SemanticType::Reference &&
              type.referenceAccess == AccessMode::Mutable;
          // An opaque checked reference carries its hidden owners even when
          // read-only. Preserve the exact semantic per-element entry witness.
          const bool erasedReference =
              type.kind == SemanticType::Reference &&
              type.arguments.size() == 1 &&
              exactMirCapabilityClass(program, type.arguments.front(),
                                      CompilerCapabilityTypeKind::ErasedObject);
          const auto storedPaths = findStoredCarrierReferences(program, type);
          const bool storedCarrier =
              type.kind == SemanticType::Class && !storedPaths.empty();
          std::size_t entryLoans = 0;
          for (const MirLoan &loan : body.loans) {
            const MirPlace *source = body.findPlace(loan.source);
            if (!loan.entry || source == nullptr ||
                source->root != MirPlaceRootKind::Binding ||
                source->binding != binding || source->projections.size() != 1 ||
                source->projections.front().kind !=
                    MirProjectionKind::PackElement ||
                source->projections.front().constantIndex != element)
              continue;
            ++entryLoans;
            const bool exactAccess =
                ((mutableReference || erasedReference || summarizedOrigin) &&
                 loan.kind == MirLoanKind::Parameter &&
                 loan.access == type.referenceAccess) ||
                (storedCarrier && loan.kind == MirLoanKind::Stored &&
                 loan.access == storedPaths.front().reference.referenceAccess);
            if (!exactAccess || (loan.semanticLoan == 0 && !summarizedOrigin) ||
                loan.escapes || loan.parent != 0 ||
                loan.carriers != std::vector<HirBindingId>{binding} ||
                source->type != type || !source->initiallyAvailable) {
              result.errors.push_back(
                  {.bodyKind = body.kind,
                   .owner = owner,
                   .message =
                       "borrowed pack element has invalid entry capability"});
            }
          }
          if (entryLoans != (mutableReference || erasedReference ||
                                     storedCarrier || summarizedOrigin
                                 ? 1u
                                 : 0u)) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .message = "concrete pack parameter lacks exact per-element "
                            "entry loans"});
          }
          const auto traits = concreteMirTypeTraits(program, type);
          if (!traits) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .message =
                     "concrete pack parameter lacks exact element traits"});
            continue;
          }
          std::size_t count = 0;
          for (const MirDropObligation &drop : body.dropObligations) {
            if (drop.kind != MirDropObligationKind::Binding ||
                drop.binding != binding)
              continue;
            const MirPlace *place = body.findPlace(drop.place);
            if (place == nullptr || place->projections.size() != 1 ||
                place->projections.front().kind !=
                    MirProjectionKind::PackElement ||
                place->projections.front().constantIndex != element)
              continue;
            ++count;
            if (!drop.initiallyActive || !place->initiallyAvailable ||
                drop.dropType.type != type ||
                drop.constructionOrder <= previousOrder) {
              result.errors.push_back(
                  {.bodyKind = body.kind,
                   .owner = owner,
                   .message = "owning pack element has invalid entry lifetime "
                              "or cleanup order"});
            }
            previousOrder = drop.constructionOrder;
          }
          if (count != (traits->drop == DropKind::Lexical ? 1u : 0u)) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .message = "concrete pack parameter lacks exact per-element "
                            "cleanup coverage"});
          }
        }
      }
    }
    for (const MirPlace &place : body.places) {
      const bool resolvedPackElement =
          (place.type.kind == SemanticType::Class ||
           place.type.kind == SemanticType::Lambda ||
           place.type.kind == SemanticType::Enum) &&
          place.projections.size() == 1 &&
          place.projections.front().kind == MirProjectionKind::PackElement;
      if (!resolvedPackElement) {
        continue;
      }
      const auto traits = concreteMirTypeTraits(program, place.type);
      if (!traits || place.traits != *traits) {
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .message = "concrete pack element traits do not match its "
                        "resolved type"});
      }
      if (place.traits.drop == DropKind::Lexical &&
          std::none_of(body.dropObligations.begin(), body.dropObligations.end(),
                       [&](const MirDropObligation &drop) {
                         return drop.kind == MirDropObligationKind::Binding &&
                                drop.binding == place.binding &&
                                drop.place == place.id && drop.initiallyActive;
                       })) {
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .message = "owning pack projection does not name its entry "
                        "lifetime place"});
      }
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const bool atomicIntrinsic =
            instruction.intrinsic == IntrinsicKind::AtomicWait ||
            instruction.intrinsic == IntrinsicKind::AtomicNotifyOne ||
            instruction.intrinsic == IntrinsicKind::AtomicNotifyAll ||
            instruction.intrinsic == IntrinsicKind::AtomicLoad ||
            instruction.intrinsic == IntrinsicKind::AtomicStore ||
            instruction.intrinsic == IntrinsicKind::AtomicExchange ||
            instruction.intrinsic == IntrinsicKind::AtomicCompareExchange;
        const bool layoutIntrinsic =
            instruction.intrinsic == IntrinsicKind::ObjectSize ||
            instruction.intrinsic == IntrinsicKind::ObjectAlignment;
        if ((instruction.intrinsic == IntrinsicKind::ObjectDestroy ||
             instruction.intrinsic == IntrinsicKind::ObjectRelocate ||
             instruction.intrinsic == IntrinsicKind::ObjectRelocateRange ||
             instruction.intrinsic == IntrinsicKind::ObjectConstruct ||
             instruction.unsafeOperation ==
                 UnsafeOperationKind::ObjectLifetime) &&
            !((instruction.intrinsic == IntrinsicKind::ObjectRelocate ||
               instruction.intrinsic == IntrinsicKind::ObjectRelocateRange)
                  ? mir_detail::objectRelocate(program, instruction)
              : instruction.intrinsic == IntrinsicKind::ObjectConstruct
                  ? mir_detail::objectConstruct(program, instruction)
                  : mir_detail::objectDestroy(program, instruction))) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .instruction = instruction.id,
               .message =
                   "object lifetime operation loses its unsafe permission or "
                   "exact writable complete-object pointer"});
        }
        if ((layoutIntrinsic &&
             !mir_detail::objectLayoutQuery(program, instruction)) ||
            (!layoutIntrinsic && instruction.objectLayoutType)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .instruction = instruction.id,
               .message = "runtime object layout query loses its exact "
                          "complete type or pure zero-input contract"});
        }
        if (instruction.intrinsic == IntrinsicKind::EnumUnderlyingValue) {
          const MirValue *argument =
              instruction.operands.size() == 1 &&
                      instruction.operands.front().kind == MirOperandKind::Value
                  ? body.findValue(instruction.operands.front().value)
                  : nullptr;
          const MirIntegralEnum *enumeration =
              argument != nullptr &&
                      argument->info.type.kind == SemanticType::Enum
                  ? program.findIntegralEnum(argument->info.type)
                  : nullptr;
          if (instruction.kind != MirInstructionKind::Call ||
              !instruction.result || instruction.receiver ||
              instruction.functionTarget || enumeration == nullptr ||
              instruction.info.type != enumeration->underlyingType ||
              instruction.parameterTypes !=
                  std::vector<SemanticType>{argument->info.type} ||
              !instruction.localFailureSites.empty()) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .instruction = instruction.id,
                 .message = "enum extraction does not preserve the exact "
                            "integral-enum underlying type"});
          }
        }
        if (((instruction.intrinsic == IntrinsicKind::FunctionPointerFromItem ||
              instruction.intrinsic == IntrinsicKind::FunctionPointerInvoke) &&
             !mir_detail::functionPointerOperation(program, instruction)) ||
            (instruction.intrinsic != IntrinsicKind::FunctionPointerFromItem &&
             (instruction.functionPointerFunctionTarget ||
              instruction.functionPointerLambdaTarget)))
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .instruction = instruction.id,
               .message = "GTI invocation pointer loses its exact safe "
                          "signature, stateless target or failure contract"});
        const bool restoringIntrinsic =
            instruction.intrinsic == IntrinsicKind::RestoreReference ||
            instruction.intrinsic == IntrinsicKind::RestoreReferenceMut;
        if (((instruction.restoresReferenceResult || restoringIntrinsic) &&
             !verifiedRestoreCall(program, body, instruction)) ||
            (!instruction.restoresReferenceResult &&
             !instruction.restoredBorrowFields.empty()))
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .instruction = instruction.id,
               .message = "restored reference loses its verified type, fields, "
                          "access or opaque witness"});
        if ((instruction.intrinsic == IntrinsicKind::RawBorrow ||
             instruction.intrinsic == IntrinsicKind::RawBorrowMut) &&
            mir_detail::rawBorrowResultLoan(body, instruction) == nullptr) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .instruction = instruction.id,
               .message = "raw borrow loses its typed address, explicit owner "
                          "witness, access or loan identity"});
        }
        if ((instruction.intrinsic == IntrinsicKind::EraseReference ||
             instruction.intrinsic == IntrinsicKind::EraseReferenceMut ||
             instruction.intrinsic == IntrinsicKind::RestoreReference ||
             instruction.intrinsic == IntrinsicKind::RestoreReferenceMut) &&
            !mir_detail::erasedReferenceResultLoan(program, body, instruction))
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .instruction = instruction.id,
               .message = "checked reference erasure loses its exact opaque "
                          "type, access, origin or restoration permission"});
        if ((instruction.intrinsic == IntrinsicKind::TextViewFromAddress ||
             instruction.intrinsic == IntrinsicKind::CStringFromAddress) &&
            !mir_detail::textAddressResultLoan(body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .instruction = instruction.id,
               .message =
                   "raw text assertion loses its exact character address, "
                   "count, ordered owner witness or checked loan identity"});
        }
        const auto cThreadSync = cThreadSynchronization(program, instruction);
        const bool nativeThreadClaim =
            instruction.intrinsic == IntrinsicKind::None &&
            (instruction.synchronization.kind ==
                 SynchronizationOperationKind::ThreadSpawn ||
             instruction.synchronization.kind ==
                 SynchronizationOperationKind::ThreadScope ||
             instruction.synchronization.kind ==
                 SynchronizationOperationKind::ThreadJoin ||
             instruction.synchronization.kind ==
                 SynchronizationOperationKind::MutexLock ||
             instruction.synchronization.kind ==
                 SynchronizationOperationKind::MutexUnlock ||
             instruction.synchronization.kind ==
                 SynchronizationOperationKind::ConditionWait ||
             instruction.synchronization.kind ==
                 SynchronizationOperationKind::ConditionNotify);
        if ((cThreadSync != SynchronizationOperationKind::None ||
             nativeThreadClaim) &&
            (instruction.synchronization.kind != cThreadSync ||
             !validSynchronizationOperation(instruction.synchronization))) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message =
                   "C-thread call has no exact synchronization contract"});
        }
        if (atomicIntrinsic &&
            (!exactAtomicSynchronization(instruction.intrinsic,
                                         instruction.synchronization) ||
             !exactAtomicWaitingContract(program, body, instruction))) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "atomic intrinsic does not retain its exact "
                          "operation-specific synchronization contract"});
        }
        if (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
            !exactScalarUniqueAllocationContract(program, body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message =
                   "scalar unique allocation does not retain its exact "
                   "pointee, initializer, result, and failure contract"});
        }
        if (!exactUniqueOwnerUpcastContract(program, body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "unique-owner upcast does not retain its exact "
                          "public-base, capability-preserving ownership "
                          "transfer contract"});
        }
        if (!exactUniqueOwnerSwapContract(body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "unique-owner swap does not retain its exact "
                          "matching mutable-owner contract"});
        }
        if (!exactRestoringTransferContract(program, body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message =
                   "restoring transfer lost its exact initialized-place, "
                   "prepared replacement, or failure-free relocation "
                   "contract"});
        }
        if (!exactRestoringIndexedSwapContract(program, body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message =
                   "indexed restoring swap lost its exclusive initialized "
                   "storage, complete bounds preflight, or relocation "
                   "contract"});
        }
        if ((instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
             instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut) &&
            !exactUniqueOwnerBorrowContract(body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "unique-owner borrow does not retain its exact "
                          "operand, result, loan, and failure contract"});
        }
        const bool mutexIntrinsic =
            instruction.intrinsic == IntrinsicKind::MutexLock ||
            instruction.intrinsic == IntrinsicKind::MutexUnlock ||
            instruction.intrinsic == IntrinsicKind::MutexGuardActive ||
            instruction.intrinsic == IntrinsicKind::MutexGuardRead ||
            instruction.intrinsic == IntrinsicKind::MutexGuardReadMut ||
            instruction.intrinsic == IntrinsicKind::MutexLockFailure;
        if (mutexIntrinsic &&
            !exactMutexIntrinsicContract(program, body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "mutex intrinsic does not retain its exact "
                          "capability, operand, result, loan, failure, and "
                          "synchronization contract"});
        }
        const bool optionalStorageIntrinsic =
            instruction.intrinsic == IntrinsicKind::OptionalStorageHasValue ||
            instruction.intrinsic == IntrinsicKind::OptionalStorageConstruct ||
            instruction.intrinsic == IntrinsicKind::OptionalStorageRead ||
            instruction.intrinsic == IntrinsicKind::OptionalStorageReadMut ||
            instruction.intrinsic == IntrinsicKind::OptionalStorageReset;
        const bool polymorphicStorageIntrinsic =
            instruction.intrinsic ==
                IntrinsicKind::PolymorphicStorageHasValue ||
            instruction.intrinsic ==
                IntrinsicKind::PolymorphicStorageConstruct ||
            instruction.intrinsic == IntrinsicKind::PolymorphicStorageRead ||
            instruction.intrinsic == IntrinsicKind::PolymorphicStorageReadMut ||
            instruction.intrinsic == IntrinsicKind::PolymorphicStorageReset;
        if (polymorphicStorageIntrinsic &&
            !exactPolymorphicStorageIntrinsicContract(program, body,
                                                      instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "polymorphic storage intrinsic does not retain its "
                          "exact capability, staged place, inert payload, "
                          "result, and loan contract"});
        }
        if (optionalStorageIntrinsic && !exactOptionalStorageIntrinsicContract(
                                            program, body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "optional storage intrinsic does not retain its "
                          "exact capability, staged place, result, loan, and "
                          "failure contract"});
        }
        if (!exactSharedOwnerIntrinsicContract(program, body, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message =
                   "shared owner intrinsic lost its exact capability, "
                   "ownership transfer, read-only loan, or failure contract"});
        }
        if (instruction.intrinsic == IntrinsicKind::ThreadScope) {
          bool exact =
              instruction.kind == MirInstructionKind::Call &&
              instruction.result && !instruction.receiver &&
              !instruction.threadTaskAdapter &&
              instruction.info.type == SemanticType::Int32 &&
              instruction.parameterTypes.size() ==
                  instruction.operands.size() &&
              instruction.scopedThreadTasks.size() ==
                  instruction.operands.size() &&
              instruction.synchronization.kind ==
                  SynchronizationOperationKind::ThreadScope &&
              validSynchronizationOperation(instruction.synchronization) &&
              instruction.definedFailure.propagation ==
                  FailurePropagationKind::TaskJoin &&
              program.executionProfile() == ExecutionProfile::Concurrent;
          for (std::size_t index = 0;
               index < instruction.scopedThreadTasks.size(); ++index) {
            const auto *adapter = program.findThreadTaskAdapter(
                instruction.scopedThreadTasks[index].adapter);
            exact = exact && adapter != nullptr && adapter->scoped &&
                    adapter->scopeArgument == index &&
                    adapter->spawnValue == instruction.hirValue &&
                    adapter->spawnInstruction == instruction.id &&
                    adapter->sourceBody.kind == body.kind &&
                    adapter->sourceBody.owner == owner &&
                    index < instruction.operands.size() &&
                    adapter->taskType == instruction.operands[index].type &&
                    index < instruction.parameterTypes.size() &&
                    adapter->taskType == instruction.parameterTypes[index];
          }
          if (!exact)
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .block = block.id,
                 .instruction = instruction.id,
                 .message =
                     "parallel scope lost its exact join-all task group"});
        } else if (!instruction.scopedThreadTasks.empty()) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message =
                   "non-scope instruction carries parallel task records"});
        }
        if (instruction.intrinsic == IntrinsicKind::ThreadSpawn &&
            !instruction.threadTaskAdapter) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "thread spawn instruction has no task adapter "
                          "reference"});
        }
        if (instruction.nativeCallbackAdapter) {
          const MirNativeCallbackAdapter *adapter =
              program.findNativeCallbackAdapter(
                  *instruction.nativeCallbackAdapter);
          if (adapter == nullptr ||
              instruction.operation != MirOperation::NativeCallback ||
              instruction.info.type != adapter->type) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .block = block.id,
                 .instruction = instruction.id,
                 .message = "native callback instruction has no exact "
                            "program adapter"});
          }
        }
        if (instruction.operation == MirOperation::FunctionItem) {
          const MirFunctionInstance *target =
              instruction.functionTarget
                  ? program.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          const SemanticType *returnType =
              instruction.info.type.functionItemReturnType();
          const std::span<const SemanticType> parameters =
              instruction.info.type.functionItemParameterTypes();
          if (target == nullptr || target->owner || target->requiresUnsafe ||
              target->definitionKind != MirDefinitionKind::Source ||
              target->linkage != LanguageLinkage::Gti ||
              instruction.info.type.functionId != target->declaration ||
              returnType == nullptr || *returnType != target->returnType ||
              parameters.size() != target->parameterTypes.size() ||
              !std::equal(parameters.begin(), parameters.end(),
                          target->parameterTypes.begin())) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .block = block.id,
                 .instruction = instruction.id,
                 .message = "function-item instruction has no exact GTI "
                            "free-function target"});
          }
        }
        if (instruction.threadTaskAdapter) {
          const MirThreadTaskAdapter *adapter =
              program.findThreadTaskAdapter(*instruction.threadTaskAdapter);
          if (adapter == nullptr || adapter->scoped ||
              instruction.intrinsic != IntrinsicKind::ThreadSpawn ||
              instruction.synchronization.kind !=
                  SynchronizationOperationKind::ThreadSpawn ||
              instruction.hirValue != adapter->spawnValue ||
              instruction.id != adapter->spawnInstruction ||
              body.kind != adapter->sourceBody.kind ||
              owner != adapter->sourceBody.owner) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .block = block.id,
                 .instruction = instruction.id,
                 .message = "thread spawn instruction has no exact program "
                            "task adapter"});
          }
        }
        for (std::size_t originIndex = 0;
             originIndex < instruction.definedFailure.localOrigins.size();
             ++originIndex) {
          const std::optional<FailureSiteId> expected =
              program.failureMetadata().siteFor(
                  instruction.definedFailure.localOrigins[originIndex]);
          if (!expected ||
              originIndex >= instruction.localFailureSites.size() ||
              instruction.localFailureSites[originIndex] != *expected) {
            result.errors.push_back(
                {.bodyKind = body.kind,
                 .owner = owner,
                 .block = block.id,
                 .instruction = instruction.id,
                 .message = "defined-failure origin does not retain its exact "
                            "artifact-local site"});
          }
        }
      }
    }
    bool hasSynchronization = false;
    bool exactTrustedSynchronization = true;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.synchronization.kind ==
            SynchronizationOperationKind::None) {
          continue;
        }
        hasSynchronization = true;
        const bool exactPrivateSynchronizationIntrinsic =
            (cThreadSynchronization(program, instruction) !=
                 SynchronizationOperationKind::None &&
             instruction.synchronization.kind ==
                 cThreadSynchronization(program, instruction) &&
             validSynchronizationOperation(instruction.synchronization)) ||
            (instruction.synchronization.kind ==
                 SynchronizationOperationKind::ThreadSpawn &&
             instruction.intrinsic == IntrinsicKind::ThreadSpawn) ||
            (instruction.synchronization.kind ==
                 SynchronizationOperationKind::ThreadScope &&
             instruction.intrinsic == IntrinsicKind::ThreadScope) ||
            (instruction.synchronization.kind ==
                 SynchronizationOperationKind::ThreadJoin &&
             (instruction.intrinsic == IntrinsicKind::ThreadJoin ||
              instruction.intrinsic == IntrinsicKind::ThreadAutoJoin)) ||
            exactMutexSynchronization(instruction.intrinsic,
                                      instruction.synchronization) ||
            exactAtomicSynchronization(instruction.intrinsic,
                                       instruction.synchronization);
        exactTrustedSynchronization =
            exactTrustedSynchronization && exactPrivateSynchronizationIntrinsic;
        if (program.executionProfile() != ExecutionProfile::SingleThreaded ||
            body.compilerTrustedSynchronization) {
          continue;
        }
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .block = block.id,
             .instruction = instruction.id,
             .message = "synchronization operation is unavailable in the "
                        "single-threaded execution profile"});
      }
    }
    if (body.compilerTrustedSynchronization &&
        (!hasSynchronization || !exactTrustedSynchronization ||
         body.kind == MirBodyKind::Module ||
         body.kind == MirBodyKind::HostedStartup)) {
      result.errors.push_back(
          {.bodyKind = body.kind,
           .owner = owner,
           .message = "compiler-trusted synchronization marker is stale or "
                      "does not name exact private synchronization "
                      "intrinsics"});
    }
  };

  for (std::size_t index = 0; index < program.nativeCallbackAdapters().size();
       ++index) {
    const MirNativeCallbackAdapter &adapter =
        program.nativeCallbackAdapters()[index];
    const MirFunctionInstance *target =
        program.findFunctionInstance(adapter.target);
    const SemanticType *returnType = adapter.type.nativeFunctionReturnType();
    const std::span<const SemanticType> parameters =
        adapter.type.nativeFunctionParameterTypes();
    if (adapter.id != index + 1 || target == nullptr || target->owner ||
        target->requiresUnsafe ||
        target->definitionKind != MirDefinitionKind::Source ||
        target->linkage != LanguageLinkage::Gti || returnType == nullptr ||
        target->returnType != *returnType ||
        target->parameterTypes.size() != parameters.size() ||
        !std::equal(target->parameterTypes.begin(),
                    target->parameterTypes.end(), parameters.begin()) ||
        adapter.targetMayRaiseDefinedFailure !=
            target->mayRaiseDefinedFailure ||
        adapter.failurePolicy !=
            MirNativeCallbackFailurePolicy::TerminateInvocation ||
        !adapter.catchesNativeExceptions ||
        adapter.cThreadCallback !=
            (program.executionProfile() == ExecutionProfile::Concurrent
                 ? (target->threadConfinedCallback
                        ? CThreadCallbackKind::ThreadConfined
                        : adapter.type.cThreadCallbackKind())
                 : CThreadCallbackKind::None) ||
        (program.executionProfile() == ExecutionProfile::Concurrent &&
         adapter.cThreadCallback == CThreadCallbackKind::None)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = adapter.target,
           .message = "native callback adapter contract is invalid"});
    }
  }

  for (const MirFunctionInstance &function : program.functionInstances()) {
    appendVerification(result, verifyOwnedPointeeProjection(program, function));
    if (!exactCThreadRuntimeFunction(function)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = function.id,
           .message = "C-thread runtime declaration contract is invalid"});
    }
    if (!exactProcessMemoryRuntimeFunction(program, function)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = function.id,
           .message =
               "process-memory runtime declaration contract is invalid"});
    }
  }

  for (std::size_t index = 0; index < program.threadTaskAdapters().size();
       ++index) {
    const MirThreadTaskAdapter &adapter = program.threadTaskAdapters()[index];
    const MirBody *body = findMirBody(program, adapter.sourceBody);
    const MirInstruction *spawn = nullptr;
    if (body != nullptr) {
      for (const MirBlock &block : body->blocks) {
        const auto found =
            std::find_if(block.instructions.begin(), block.instructions.end(),
                         [&](const MirInstruction &instruction) {
                           return instruction.id == adapter.spawnInstruction;
                         });
        if (found != block.instructions.end()) {
          spawn = &*found;
          break;
        }
      }
    }
    const bool functionEntry = adapter.functionTarget.has_value();
    const bool lambdaEntry = adapter.lambdaTarget.has_value();
    SemanticType spawnResult = SemanticType::Unknown;
    if (adapter.scoped && spawn != nullptr) {
      spawnResult = SemanticType::Void;
    } else if (spawn != nullptr && spawn->operands.size() == 2 &&
               spawn->operands.front().type.kind == SemanticType::Reference &&
               spawn->operands.front().type.arguments.size() == 1) {
      const SemanticType &handleType =
          spawn->operands.front().type.arguments.front();
      if (handleType.kind == SemanticType::Class &&
          handleType.arguments.size() <= 1) {
        spawnResult = handleType.arguments.empty()
                          ? SemanticType::Void
                          : handleType.arguments.front();
      }
    }
    bool exactEntry = functionEntry != lambdaEntry;
    if (functionEntry) {
      const MirFunctionInstance *target =
          program.findFunctionInstance(*adapter.functionTarget);
      const MirClassInstance *owner =
          target == nullptr || !target->owner
              ? nullptr
              : program.findClassInstance(*target->owner);
      const bool functionItemEntry = adapter.taskType.hasFunctionItemShape();
      exactEntry =
          exactEntry && target != nullptr && !target->requiresUnsafe &&
          target->definitionKind == MirDefinitionKind::Source &&
          target->linkage == LanguageLinkage::Gti &&
          target->returnType == adapter.resultType &&
          target->parameterTypes.empty() && !target->pureVirtual &&
          (functionItemEntry
               ? owner == nullptr &&
                     adapter.taskType.functionId == target->declaration &&
                     adapter.targetInvocation ==
                         CallableInvocationCapability::Read
               : owner != nullptr && owner->type == adapter.taskType &&
                     target->overloadedOperator ==
                         std::optional{OverloadedOperator::Call} &&
                     adapter.targetInvocation ==
                         callableInvocationCapability(
                             target->receiverMutability));
    }
    if (lambdaEntry) {
      const MirLambdaInstance *target =
          program.findLambda(*adapter.lambdaTarget);
      exactEntry =
          exactEntry && target != nullptr && target->type == adapter.taskType &&
          target->returnType == adapter.resultType &&
          target->parameterTypes.empty() &&
          adapter.targetInvocation == CallableInvocationCapability::Read;
    }
    const bool exactSpawn =
        spawn != nullptr &&
        (adapter.scoped
             ? spawn->intrinsic == IntrinsicKind::ThreadScope &&
                   spawn->synchronization.kind ==
                       SynchronizationOperationKind::ThreadScope &&
                   adapter.scopeArgument < spawn->scopedThreadTasks.size() &&
                   spawn->scopedThreadTasks[adapter.scopeArgument].adapter ==
                       adapter.id
             : spawn->intrinsic == IntrinsicKind::ThreadSpawn &&
                   spawn->threadTaskAdapter == adapter.id &&
                   spawn->synchronization.kind ==
                       SynchronizationOperationKind::ThreadSpawn &&
                   adapter.scopeArgument == 0);
    if (adapter.id != index + 1 || adapter.taskType == SemanticType::Unknown ||
        adapter.resultType == SemanticType::Unknown ||
        adapter.resultType != spawnResult || adapter.spawnValue == 0 ||
        adapter.spawnInstruction == 0 ||
        adapter.invocation != CallableInvocationCapability::Once ||
        !adapter.catchesWorkerFailure || !adapter.catchesNativeExceptions ||
        !exactEntry || body == nullptr || spawn == nullptr ||
        spawn->hirValue != adapter.spawnValue || !exactSpawn ||
        program.executionProfile() != ExecutionProfile::Concurrent) {
      result.errors.push_back(
          {.bodyKind = adapter.sourceBody.kind,
           .owner = adapter.sourceBody.owner,
           .instruction = adapter.spawnInstruction,
           .message = "managed thread task adapter contract is invalid"});
    }
  }

  appendVerification(result, verifyMirProgramInitialization(program));
  appendVerification(result, verifyMirHostedStartup(program));
  if (program.hostedStartupPlan() && program.hostedStartup() != nullptr) {
    appendVerification(result,
                       verifyMirBody(*program.hostedStartup(),
                                     program.hostedStartupPlan()->entry));
  }
  verifyBody(program.module(), 0);
  const auto typeRequiresActiveCleanup =
      [&](const SemanticType &type, const auto &query) -> std::optional<bool> {
    switch (type.kind) {
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
      return true;
    case SemanticType::Array:
    case SemanticType::Unexpected:
      return type.arguments.size() == 1 ? query(type.arguments.front(), query)
                                        : std::nullopt;
    case SemanticType::Expected: {
      bool unknown = false;
      for (const SemanticType &argument : type.arguments) {
        if (argument == SemanticType::Void) {
          continue;
        }
        const std::optional<bool> active = query(argument, query);
        if (active && *active) {
          return true;
        }
        unknown = unknown || !active;
      }
      return unknown ? std::nullopt : std::optional<bool>{false};
    }
    case SemanticType::Enum: {
      const MirPayloadEnum *enumeration = program.findPayloadEnum(type);
      return enumeration == nullptr
                 ? std::optional<bool>{false}
                 : std::optional<bool>{enumeration->requiresActiveCleanup};
    }
    case SemanticType::Class: {
      const auto instance = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const MirClassInstance &candidate) {
            return candidate.type == type;
          });
      return instance == program.classInstances().end()
                 ? std::nullopt
                 : std::optional<bool>{instance->requiresActiveCleanup};
    }
    case SemanticType::Lambda:
      if (type.lambdaId == 0) {
        return std::nullopt;
      }
      {
        std::optional<bool> result;
        for (const MirLambdaInstance &lambda : program.lambdaInstances()) {
          if (lambda.type != type) {
            continue;
          }
          if (lambda.captureTypes.size() !=
              lambda.captureRequiresActiveCleanup.size()) {
            return std::nullopt;
          }
          const bool active =
              std::any_of(lambda.captureRequiresActiveCleanup.begin(),
                          lambda.captureRequiresActiveCleanup.end(),
                          [](bool capture) { return capture; });
          if (result && *result != active) {
            return std::nullopt;
          }
          result = active;
        }
        return result;
      }
    default:
      return false;
    }
  };
  for (const auto &adapter : program.threadTaskAdapters()) {
    for (const SemanticType *type : {&adapter.taskType, &adapter.resultType}) {
      const auto active =
          typeRequiresActiveCleanup(*type, typeRequiresActiveCleanup);
      const auto cleanup = active && *active
                               ? mirUniqueCleanupType(program, *type)
                               : std::optional<MirDropType>{};
      if (!active ||
          (*active && (!cleanup || !cleanup->requiresActiveCleanup))) {
        result.errors.push_back(
            {.bodyKind = adapter.sourceBody.kind,
             .owner = adapter.sourceBody.owner,
             .instruction = adapter.spawnInstruction,
             .message = "managed thread active task or result has no exact MIR "
                        "cleanup recipe"});
      }
    }
  }
  std::vector<SemanticType> integralEnumIds;
  for (const MirIntegralEnum &enumeration : program.integralEnums()) {
    if (enumeration.declaration == 0 || enumeration.sourceUnit == 0 ||
        enumeration.type.kind != SemanticType::Enum ||
        enumeration.type.enumId != enumeration.declaration ||
        std::find(integralEnumIds.begin(), integralEnumIds.end(),
                  enumeration.type) != integralEnumIds.end() ||
        program.findPayloadEnum(enumeration.type) != nullptr ||
        !constantIntegerDomain(enumeration.underlyingType)) {
      result.errors.push_back(
          {.message = "integral-enum metadata has no unique exact integer "
                      "representation"});
    }
    integralEnumIds.push_back(enumeration.type);
  }
  appendVerification(result, verifyMirPayloadEnums(program));
  for (std::size_t index = 0; index < program.classInstances().size();
       ++index) {
    const MirClassInstance &instance = program.classInstances()[index];
    if (instance.compilerCapability ==
        CompilerCapabilityTypeKind::PolymorphicStorage) {
      const auto erased = instance.type.arguments.size() == 1
                              ? conditionalCapabilities(
                                    program, instance.type.arguments.front())
                              : std::nullopt;
      if (!erased ||
          instance.traits.transferCapable != erased->transferCapable ||
          instance.traits.shareCapable != erased->shareCapable) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message =
                 "polymorphic storage capabilities contradict its erased "
                 "interface contract"});
      }
    }
    if (instance.generatedDefaultRequiresSchedule &&
        instance.defaultConstructor != SpecialMemberStatus::Generated) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "generated-default schedule obligation requires an "
                      "available generated constructor"});
    }
    for (const bool transfer : {true, false}) {
      const auto policy =
          transfer ? instance.transferPolicy : instance.sharePolicy;
      const auto &condition =
          transfer ? instance.transferCondition : instance.shareCondition;
      bool exact = (policy == ConcurrencyCapabilityPolicy::UnsafeConditional) ==
                   condition.has_value();
      switch (policy) {
      case ConcurrencyCapabilityPolicy::Structural:
      case ConcurrencyCapabilityPolicy::Denied:
      case ConcurrencyCapabilityPolicy::Required:
      case ConcurrencyCapabilityPolicy::UnsafeAsserted:
      case ConcurrencyCapabilityPolicy::UnsafeConditional:
        break;
      default:
        exact = false;
        break;
      }
      if (condition) {
        const auto required = conditionalCapabilities(program, *condition);
        exact =
            exact && required && instance.kind != ClassKind::Interface &&
            instance.compilerCapability == CompilerCapabilityTypeKind::None &&
            (transfer
                 ? instance.traits.transferCapable == required->transferCapable
                 : instance.traits.shareCapable == required->shareCapable);
      }
      if (!exact) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "class conditional unsafe capability contradicts its "
                        "concrete condition"});
      }
    }
    if (instance.id != index + 1 || instance.sourceUnit == 0 ||
        instance.declaration == 0 ||
        instance.type.kind != SemanticType::Class ||
        instance.type.classId != instance.declaration) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class instance source, identity, declaration, or "
                      "nominal type is invalid"});
    }
    const auto validSpecialMemberStatus = [](SpecialMemberStatus status) {
      return status == SpecialMemberStatus::Declared ||
             status == SpecialMemberStatus::Generated ||
             status == SpecialMemberStatus::Deleted;
    };
    if (!validSpecialMemberStatus(instance.defaultConstructor) ||
        !validSpecialMemberStatus(instance.copyConstructor) ||
        !validSpecialMemberStatus(instance.moveConstructor) ||
        !validSpecialMemberStatus(instance.copyAssignment) ||
        !validSpecialMemberStatus(instance.moveAssignment) ||
        !validSpecialMemberStatus(instance.destructorStatus) ||
        ((instance.destructorStatus == SpecialMemberStatus::Declared) !=
         instance.destructor.has_value())) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class special-member metadata is invalid or does not "
                      "match its declared destructor"});
    }
    if ((instance.copyConstructor == SpecialMemberStatus::Generated &&
         !instance.traits.copyable) ||
        (instance.moveConstructor == SpecialMemberStatus::Generated &&
         !instance.traits.movable) ||
        (instance.copyAssignment == SpecialMemberStatus::Generated &&
         !instance.traits.copyAssignable) ||
        (instance.moveAssignment == SpecialMemberStatus::Generated &&
         !instance.traits.moveAssignable)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "generated class special member contradicts its concrete "
                      "semantic traits"});
    }
    if (instance.requiresActiveDropState != instance.destructor.has_value()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class active-drop state does not match its destructor"});
    }
    if (instance.traits.relocation !=
        mirTypeRelocationKind(program, instance.type)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class relocation traits do not match its structural "
                      "move, borrow, base, field, and cleanup metadata"});
    }
    if (instance.requiresActiveDropState && !instance.requiresActiveCleanup) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message =
               "class active-drop state is missing active-cleanup metadata"});
    }
    if (instance.compilerCapability ==
            CompilerCapabilityTypeKind::ErasedObject ||
        instance.compilerCapability ==
            CompilerCapabilityTypeKind::FunctionPointer) {
      const bool pointer = instance.compilerCapability ==
                           CompilerCapabilityTypeKind::FunctionPointer;
      const SemanticTypeTraits expected{
          .ownership = OwnershipKind::Value,
          .drop = DropKind::Trivial,
          .copyable = pointer,
          .movable = pointer,
          .copyAssignable = pointer,
          .moveAssignable = pointer,
          .containsBorrowedState = false,
          .transferCapable = false,
          .shareCapable = false,
          .relocation =
              pointer ? RelocationKind::Bitwise : RelocationKind::Unavailable};
      if (instance.traits != expected || !instance.bases.empty() ||
          !instance.structuralBases.empty() ||
          !instance.declaredFields.empty() || !instance.fields.empty() ||
          !instance.fieldDropOrder.empty() || instance.requiresActiveCleanup ||
          instance.requiresActiveDropState || instance.destructor ||
          instance.polymorphic || instance.cAbiRecord || instance.cAbiLayout ||
          instance.unionLayout ||
          (pointer
               ? !mir_detail::functionPointerSignature(program, instance.type)
               : !instance.type.arguments.empty()))
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "opaque referent or invocation-pointer capability "
                        "loses its exact private shape and traits"});
    }
    if (instance.compilerCapability ==
            CompilerCapabilityTypeKind::MutexStorage ||
        instance.compilerCapability ==
            CompilerCapabilityTypeKind::MutexLockToken) {
      const bool storageCapability = instance.compilerCapability ==
                                     CompilerCapabilityTypeKind::MutexStorage;
      const std::optional<bool> payloadRequiresActiveCleanup =
          instance.type.arguments.size() == 1
              ? typeRequiresActiveCleanup(instance.type.arguments.front(),
                                          typeRequiresActiveCleanup)
              : std::nullopt;
      const auto payloadCapabilities =
          instance.type.arguments.size() == 1
              ? conditionalCapabilities(program,
                                        instance.type.arguments.front())
              : std::nullopt;
      const bool mayCrossThreads = storageCapability && payloadCapabilities &&
                                   payloadCapabilities->transferCapable;
      const bool exactMutexCapability =
          instance.type.arguments.size() == 1 &&
          instance.type.arguments.front() != SemanticType::Void &&
          instance.type.arguments.front().kind != SemanticType::Reference &&
          instance.type.arguments.front().kind != SemanticType::RawPointer &&
          !instance.lifecycleBoundBorrowCarrier && instance.bases.empty() &&
          instance.structuralBases.empty() && instance.declaredFields.empty() &&
          instance.fields.empty() && instance.fieldDropOrder.empty() &&
          !instance.abstract && !instance.polymorphic && !instance.cAbiRecord &&
          !instance.cAbiLayout && !instance.unionLayout &&
          !instance.destructor && !instance.requiresActiveDropState &&
          payloadCapabilities &&
          instance.traits.transferCapable == mayCrossThreads &&
          instance.traits.shareCapable == mayCrossThreads &&
          payloadRequiresActiveCleanup &&
          instance.requiresActiveCleanup ==
              (storageCapability && *payloadRequiresActiveCleanup);
      if (!exactMutexCapability) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "compiler-owned mutex capability class does not "
                        "retain its exact passive native-storage shape"});
      }
    }
    if (instance.compilerCapability ==
        CompilerCapabilityTypeKind::UninitializedStorage) {
      const auto payload =
          instance.type.arguments.size() == 1
              ? storagePayloadTraits(program, instance.type.arguments.front())
              : std::nullopt;
      const bool movable =
          payload &&
          mirTypeRelocationKind(program, instance.type.arguments.front()) ==
              RelocationKind::Bitwise;
      const bool copyable = movable && payload->copyable;
      const auto status = [](bool available) {
        return available ? SpecialMemberStatus::Generated
                         : SpecialMemberStatus::Deleted;
      };
      const bool exact =
          payload &&
          mir_detail::completeObjectType(program,
                                         instance.type.arguments.front()) &&
          !payload->containsBorrowedState &&
          instance.kind == ClassKind::Class &&
          instance.type.kind == SemanticType::Class &&
          instance.type.classId == instance.declaration &&
          instance.traits.ownership == OwnershipKind::Value &&
          instance.traits.drop == DropKind::Trivial &&
          instance.traits.copyable == copyable &&
          instance.traits.movable == movable &&
          instance.traits.copyAssignable == copyable &&
          instance.traits.moveAssignable == movable &&
          instance.traits.transferCapable == payload->transferCapable &&
          instance.traits.shareCapable == payload->shareCapable &&
          instance.traits.relocation == (movable
                                             ? RelocationKind::Bitwise
                                             : RelocationKind::Unavailable) &&
          !instance.traits.containsBorrowedState && instance.bases.empty() &&
          instance.structuralBases.empty() && instance.declaredFields.empty() &&
          instance.fields.empty() && instance.fieldDropOrder.empty() &&
          !instance.destructor && !instance.requiresActiveCleanup &&
          !instance.requiresActiveDropState &&
          !instance.lifecycleBoundBorrowCarrier && !instance.abstract &&
          !instance.polymorphic && !instance.cAbiRecord &&
          !instance.cAbiLayout && !instance.cOpaqueHandle &&
          !instance.unionLayout &&
          instance.defaultConstructor == SpecialMemberStatus::Generated &&
          instance.copyConstructor == status(copyable) &&
          instance.moveConstructor == status(movable) &&
          instance.copyAssignment == status(copyable) &&
          instance.moveAssignment == status(movable) &&
          instance.destructorStatus == SpecialMemberStatus::Generated;
      if (!exact)
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "uninitialized storage lost its exact layout-only "
                        "lifetime contract"});
    }
    if (instance.compilerCapability ==
        CompilerCapabilityTypeKind::OptionalStorage) {
      const auto payloadTraits =
          instance.type.arguments.size() == 1
              ? storagePayloadTraits(program, instance.type.arguments.front())
              : std::nullopt;
      const std::optional<bool> payloadRequiresActiveCleanup =
          instance.type.arguments.size() == 1
              ? typeRequiresActiveCleanup(instance.type.arguments.front(),
                                          typeRequiresActiveCleanup)
              : std::nullopt;
      const bool exactOptionalCapability =
          instance.kind == ClassKind::Class &&
          instance.type.kind == SemanticType::Class &&
          instance.type.classId == instance.declaration &&
          instance.type.arguments.size() == 1 &&
          instance.type.arguments.front() != SemanticType::Void &&
          instance.type.arguments.front().kind != SemanticType::Reference &&
          instance.type.arguments.front().kind != SemanticType::RawPointer &&
          payloadTraits && payloadTraits->movable &&
          !payloadTraits->containsBorrowedState &&
          !instance.traits.containsBorrowedState &&
          !instance.lifecycleBoundBorrowCarrier && instance.bases.empty() &&
          instance.structuralBases.empty() && instance.declaredFields.empty() &&
          instance.fields.empty() && instance.fieldDropOrder.empty() &&
          !instance.abstract && !instance.polymorphic && !instance.cAbiRecord &&
          !instance.cAbiLayout && !instance.unionLayout &&
          instance.defaultConstructor == SpecialMemberStatus::Declared &&
          instance.copyConstructor == SpecialMemberStatus::Deleted &&
          instance.moveConstructor == SpecialMemberStatus::Generated &&
          instance.copyAssignment == SpecialMemberStatus::Deleted &&
          instance.moveAssignment == SpecialMemberStatus::Deleted &&
          instance.destructorStatus == SpecialMemberStatus::Generated &&
          !instance.destructor && !instance.requiresActiveDropState &&
          instance.traits.ownership == OwnershipKind::Value &&
          instance.traits.drop == DropKind::Lexical &&
          !instance.traits.copyable && instance.traits.movable &&
          !instance.traits.copyAssignable && !instance.traits.moveAssignable &&
          payloadRequiresActiveCleanup &&
          instance.requiresActiveCleanup == *payloadRequiresActiveCleanup;
      if (!exactOptionalCapability) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "compiler-owned optional storage capability does not "
                        "retain its exact move-only inline-slot shape"});
      }
    }
    if (instance.compilerCapability ==
        CompilerCapabilityTypeKind::SharedOwner) {
      const bool exactSharedCapability =
          instance.kind == ClassKind::Class &&
          instance.type.arguments.size() == 1 &&
          mirUniqueCleanupType(program, instance.type).has_value() &&
          instance.type.arguments.front() != SemanticType::Void &&
          instance.type.arguments.front().kind != SemanticType::Reference &&
          instance.type.arguments.front().kind != SemanticType::Array &&
          !instance.lifecycleBoundBorrowCarrier && instance.bases.empty() &&
          instance.structuralBases.empty() && instance.declaredFields.empty() &&
          instance.fields.empty() && instance.fieldDropOrder.empty() &&
          !instance.abstract && !instance.polymorphic && !instance.cAbiRecord &&
          !instance.cAbiLayout && !instance.unionLayout &&
          instance.defaultConstructor == SpecialMemberStatus::Generated &&
          instance.copyConstructor == SpecialMemberStatus::Deleted &&
          instance.moveConstructor == SpecialMemberStatus::Generated &&
          instance.copyAssignment == SpecialMemberStatus::Deleted &&
          instance.moveAssignment == SpecialMemberStatus::Generated &&
          instance.destructorStatus == SpecialMemberStatus::Generated &&
          !instance.destructor && !instance.requiresActiveDropState &&
          instance.traits.ownership == OwnershipKind::Shared &&
          instance.traits.drop == DropKind::Lexical &&
          !instance.traits.copyable && instance.traits.movable &&
          !instance.traits.copyAssignable && instance.traits.moveAssignable &&
          !instance.traits.containsBorrowedState &&
          instance.requiresActiveCleanup;
      if (!exactSharedCapability) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "compiler-owned shared owner capability does not "
                        "retain its exact move-only strong-handle shape"});
      }
    }
    if (instance.lifecycleBoundBorrowCarrier !=
        (instance.traits.containsBorrowedState &&
         instance.requiresActiveCleanup)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class cleanup-bound dependency retention does not match "
                      "its borrowed state and executable cleanup"});
    }
    if (instance.mutexGuardBorrowCarrier) {
      const MirClassFieldInfo *storageField = nullptr;
      const MirClassFieldInfo *tokenField = nullptr;
      const MirClassInstance *storageClass = nullptr;
      const MirClassInstance *tokenClass = nullptr;
      for (const MirClassFieldInfo &field : instance.declaredFields) {
        if (exactCapabilityReferenceValue(
                program, field.type, AccessMode::ReadOnly,
                CompilerCapabilityTypeKind::MutexStorage, &storageClass)) {
          storageField = storageField == nullptr ? &field : nullptr;
          continue;
        }
        const MirClassInstance *candidate = exactMirCapabilityClass(
            program, field.type, CompilerCapabilityTypeKind::MutexLockToken);
        if (candidate != nullptr) {
          tokenField = tokenField == nullptr ? &field : nullptr;
          tokenClass = candidate;
        }
      }
      const bool exactFields =
          storageField != nullptr && tokenField != nullptr &&
          storageClass != nullptr && tokenClass != nullptr &&
          instance.declaredFields.size() == 2 &&
          instance.type.arguments.size() == 1 &&
          storageClass->sourceUnit == instance.sourceUnit &&
          tokenClass->sourceUnit == instance.sourceUnit &&
          storageClass->type.arguments.front() ==
              instance.type.arguments.front() &&
          tokenClass->type.arguments.front() ==
              instance.type.arguments.front() &&
          storageField->dropKind == DropKind::Trivial &&
          tokenField->dropKind == DropKind::Lexical &&
          instance.fields.size() == 1 &&
          instance.fields.front().field == tokenField->field &&
          instance.fields.front().symbol == tokenField->symbol &&
          instance.fields.front().type == tokenField->type &&
          instance.fieldDropOrder.size() == 1 &&
          instance.fieldDropOrder.front().field == tokenField->field &&
          instance.fieldDropOrder.front().symbol == tokenField->symbol &&
          instance.fieldDropOrder.front().type == tokenField->type;
      const bool exactCarrier =
          instance.lifecycleBoundBorrowCarrier &&
          instance.compilerCapability == CompilerCapabilityTypeKind::None &&
          instance.kind == ClassKind::Class && exactFields &&
          instance.bases.empty() && instance.structuralBases.empty() &&
          !instance.abstract && !instance.polymorphic && !instance.cAbiRecord &&
          !instance.cAbiLayout && !instance.unionLayout &&
          instance.copyConstructor == SpecialMemberStatus::Deleted &&
          instance.moveConstructor != SpecialMemberStatus::Deleted &&
          instance.copyAssignment == SpecialMemberStatus::Deleted &&
          instance.moveAssignment == SpecialMemberStatus::Deleted &&
          instance.destructorStatus == SpecialMemberStatus::Declared &&
          instance.destructor && instance.requiresActiveDropState &&
          instance.requiresActiveCleanup &&
          instance.traits.drop == DropKind::Lexical &&
          !instance.traits.copyable && instance.traits.movable &&
          !instance.traits.copyAssignable && !instance.traits.moveAssignable &&
          instance.traits.containsBorrowedState &&
          !instance.traits.transferCapable && !instance.traits.shareCapable;
      if (!exactCarrier) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "lifecycle-bound borrow carrier is not the exact "
                        "same-unit mutex guard storage/token owner"});
      }
    }
    for (const auto &field : instance.declaredFields) {
      if (!storageOwnershipTypeValid(program, field.type))
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "compiler-owned storage field payload retains borrowed "
                        "state without an owner-dependency contract"});
    }
    const bool exactBaseProjection =
        instance.bases.size() == instance.structuralBases.size() &&
        std::equal(
            instance.bases.begin(), instance.bases.end(),
            instance.structuralBases.begin(),
            [](const HirBaseInstance &actual, const HirBaseInstance &expected) {
              return actual.instance == expected.instance &&
                     actual.type == expected.type &&
                     actual.interface == expected.interface;
            });
    if (!exactBaseProjection) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class base metadata omits or changes a structural "
                      "base"});
    }
    for (const HirBaseInstance &base : instance.structuralBases) {
      const MirClassInstance *baseInstance =
          program.findClassInstance(base.instance);
      if (base.instance == instance.id || baseInstance == nullptr ||
          baseInstance->type != base.type ||
          base.interface != (baseInstance->kind == ClassKind::Interface)) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message =
                 "class base metadata does not name its exact instance"});
      }
    }
    std::unordered_set<HirBindingId> declaredClassFields;
    std::unordered_set<SymbolId> declaredClassSymbols;
    bool validDeclaredFields = true;
    for (const MirClassFieldInfo &field : instance.declaredFields) {
      const std::optional<bool> fieldRequiresActiveCleanup =
          typeRequiresActiveCleanup(field.type, typeRequiresActiveCleanup);
      validDeclaredFields =
          validDeclaredFields && field.field != 0 && field.symbol != 0 &&
          declaredClassFields.insert(field.field).second &&
          declaredClassSymbols.insert(field.symbol).second &&
          field.type != SemanticType::Unknown &&
          field.type.kind != SemanticType::FunctionSignature &&
          fieldRequiresActiveCleanup &&
          *fieldRequiresActiveCleanup == field.requiresActiveCleanup;
    }
    const std::vector<MirClassFieldInfo> declaredLifecycleFields = [&] {
      std::vector<MirClassFieldInfo> fields;
      std::copy_if(instance.declaredFields.begin(),
                   instance.declaredFields.end(), std::back_inserter(fields),
                   [](const MirClassFieldInfo &field) {
                     return field.dropKind == DropKind::Lexical;
                   });
      return fields;
    }();
    validDeclaredFields =
        validDeclaredFields &&
        declaredLifecycleFields.size() == instance.fields.size() &&
        std::equal(declaredLifecycleFields.begin(),
                   declaredLifecycleFields.end(), instance.fields.begin(),
                   [](const MirClassFieldInfo &declared,
                      const MirClassFieldLifecycle &lifecycle) {
                     return declared.field == lifecycle.field &&
                            declared.symbol == lifecycle.symbol &&
                            declared.type == lifecycle.type &&
                            declared.dropKind == lifecycle.dropKind &&
                            declared.requiresActiveCleanup ==
                                lifecycle.requiresActiveCleanup;
                   });
    if (!validDeclaredFields) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class declared-field metadata is invalid or does not "
                      "match its lifecycle projection"});
    }
    std::unordered_set<HirBindingId> classFields;
    for (const MirClassFieldLifecycle &field : instance.fields) {
      const std::optional<bool> fieldRequiresActiveCleanup =
          typeRequiresActiveCleanup(field.type, typeRequiresActiveCleanup);
      if (field.field == 0 || field.symbol == 0 ||
          !classFields.insert(field.field).second ||
          field.type == SemanticType::Unknown ||
          field.dropKind != DropKind::Lexical || !fieldRequiresActiveCleanup ||
          *fieldRequiresActiveCleanup != field.requiresActiveCleanup) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "class field active-cleanup metadata does not match "
                        "its type"});
      }
    }
    std::vector<MirFieldDrop> exactFieldDropOrder;
    for (auto field = instance.fields.rbegin(); field != instance.fields.rend();
         ++field) {
      if (field->dropKind == DropKind::Lexical) {
        exactFieldDropOrder.push_back(
            {.field = field->field,
             .symbol = field->symbol,
             .type = field->type,
             .requiresActiveCleanup = field->requiresActiveCleanup});
      }
    }
    const bool exactFieldDropProjection =
        instance.fieldDropOrder.size() == exactFieldDropOrder.size() &&
        std::equal(
            instance.fieldDropOrder.begin(), instance.fieldDropOrder.end(),
            exactFieldDropOrder.begin(),
            [](const MirFieldDrop &actual, const MirFieldDrop &expected) {
              return actual.field == expected.field &&
                     actual.symbol == expected.symbol &&
                     actual.type == expected.type &&
                     actual.requiresActiveCleanup ==
                         expected.requiresActiveCleanup;
            });
    if (!exactFieldDropProjection) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class field-drop order is not the exact reverse "
                      "projection of its lifecycle fields"});
    }
    const bool structurallyRequiresActiveCleanup =
        instance.destructor.has_value() ||
        instance.compilerCapability ==
            CompilerCapabilityTypeKind::SharedOwner ||
        ((instance.compilerCapability ==
              CompilerCapabilityTypeKind::MutexStorage ||
          instance.compilerCapability ==
              CompilerCapabilityTypeKind::OptionalStorage) &&
         instance.type.arguments.size() == 1 &&
         typeRequiresActiveCleanup(instance.type.arguments.front(),
                                   typeRequiresActiveCleanup)
             .value_or(false)) ||
        std::any_of(instance.structuralBases.begin(),
                    instance.structuralBases.end(),
                    [&](const HirBaseInstance &base) {
                      const MirClassInstance *baseInstance =
                          program.findClassInstance(base.instance);
                      return !base.interface && baseInstance != nullptr &&
                             baseInstance->requiresActiveCleanup;
                    }) ||
        std::any_of(instance.fields.begin(), instance.fields.end(),
                    [](const MirClassFieldLifecycle &field) {
                      return field.requiresActiveCleanup;
                    });
    if (instance.requiresActiveCleanup != structurallyRequiresActiveCleanup) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message =
               "class active-cleanup metadata does not match its structure"});
    }
    if (instance.destructor) {
      const MirDestructorInstance *destructor =
          program.findDestructorInstance(*instance.destructor);
      if (destructor == nullptr || destructor->owner != instance.id) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message =
                 "class cleanup descriptor names an invalid destructor"});
      }
    }
    if (instance.cAbiRecord) {
      if (instance.kind != ClassKind::Struct || !instance.bases.empty() ||
          instance.abstract || instance.polymorphic || !instance.cAbiLayout ||
          instance.destructor || instance.requiresActiveDropState ||
          instance.requiresActiveCleanup || !instance.fields.empty() ||
          !instance.fieldDropOrder.empty()) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "C ABI record class metadata is structurally invalid"});
      } else {
        const MirCAbiRecordLayout &layout = *instance.cAbiLayout;
        bool validLayout = layout.sizeBytes != 0 &&
                           layout.abiAlignmentBytes != 0 &&
                           layout.sizeBytes % layout.abiAlignmentBytes == 0 &&
                           !layout.fields.empty();
        std::uint64_t previousEnd = 0;
        for (const MirCAbiRecordFieldLayout &field : layout.fields) {
          const bool fieldEndValid =
              field.offsetBytes <=
              std::numeric_limits<std::uint64_t>::max() - field.sizeBytes;
          const std::uint64_t fieldEnd =
              fieldEndValid ? field.offsetBytes + field.sizeBytes : 0;
          validLayout = validLayout && field.field != 0 &&
                        field.type != SemanticType::Unknown &&
                        field.sizeBytes != 0 && field.abiAlignmentBytes != 0 &&
                        field.offsetBytes % field.abiAlignmentBytes == 0 &&
                        field.offsetBytes >= previousEnd && fieldEndValid &&
                        fieldEnd <= layout.sizeBytes;
          previousEnd = fieldEnd;
        }
        if (!validLayout) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::FieldInitializers,
               .owner = instance.id,
               .message = "C ABI record layout metadata is invalid"});
        }
      }
    } else if (instance.cAbiLayout) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "ordinary class carries C ABI record layout metadata"});
    }
    if (instance.kind == ClassKind::Union) {
      bool validLayout =
          instance.unionLayout.has_value() && instance.bases.empty() &&
          !instance.abstract && !instance.polymorphic && !instance.cAbiRecord &&
          !instance.cAbiLayout && !instance.destructor &&
          !instance.requiresActiveDropState &&
          !instance.requiresActiveCleanup && instance.fields.empty() &&
          instance.fieldDropOrder.empty();
      if (instance.unionLayout) {
        const MirUnionLayout &layout = *instance.unionLayout;
        validLayout = validLayout && layout.sizeBytes != 0 &&
                      layout.abiAlignmentBytes != 0 &&
                      layout.sizeBytes % layout.abiAlignmentBytes == 0 &&
                      !layout.fields.empty() &&
                      layout.fields.size() == instance.declaredFields.size();
        for (const MirUnionFieldLayout &field : layout.fields) {
          validLayout = validLayout && field.field != 0 &&
                        field.type != SemanticType::Unknown &&
                        field.sizeBytes != 0 && field.abiAlignmentBytes != 0 &&
                        field.sizeBytes <= layout.sizeBytes &&
                        layout.abiAlignmentBytes % field.abiAlignmentBytes == 0;
        }
      }
      if (!validLayout) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::FieldInitializers,
             .owner = instance.id,
             .message = "union layout metadata is structurally invalid"});
      }
    } else if (instance.unionLayout) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "non-union class carries union layout metadata"});
    }
    verifyBody(instance.fieldInitializers, instance.id);
    verifyBody(instance.staticFieldInitializers, instance.id);
    const bool hasMergedStaticInitialization = std::any_of(
        program.programInitializationPlan().steps.begin(),
        program.programInitializationPlan().steps.end(),
        [&](const MirProgramInitializationStep &step) {
          return step.storageKind == ProgramStorageKind::StaticField &&
                 step.ownerClass == instance.id;
        });
    if (hasMergedStaticInitialization &&
        !hasCanonicalEmptyStaticInitializerBody(
            instance.staticFieldInitializers, program.module())) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::StaticFieldInitializers,
           .owner = instance.id,
           .message = "legacy static-field initializer body is not the "
                      "canonical empty migration shell"});
    }
  }
  const auto verifyCallableBodySignature = [&](const MirBody &body,
                                               MirBodyKind kind,
                                               const SemanticType &returnType,
                                               std::size_t owner) {
    if (body.kind != kind || body.returnType != returnType) {
      result.errors.push_back(
          {.bodyKind = kind,
           .owner = owner,
           .message = "callable body kind or return type does not match its "
                      "instance signature"});
    }
  };
  std::size_t entryPoints = 0;
  for (std::size_t index = 0; index < program.functionInstances().size();
       ++index) {
    const MirFunctionInstance &instance = program.functionInstances()[index];
    verifyCallableBodySignature(instance.body, MirBodyKind::Function,
                                instance.returnType, instance.id);
    if (instance.body.hasCallerLocation != instance.tracksCaller) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "function body caller parameter does not match its "
                      "declaration contract"});
    }
    if (instance.staticMember &&
        (!instance.owner ||
         program.findClassInstance(*instance.owner) == nullptr ||
         instance.virtualMethod || instance.pureVirtual ||
         instance.overrideMethod || !instance.virtualRoots.empty() ||
         instance.receiverMutability != ReceiverMutability::ReadOnly ||
         std::any_of(instance.body.places.begin(), instance.body.places.end(),
                     [](const MirPlace &place) {
                       return place.root == MirPlaceRootKind::This;
                     }))) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "static method must retain one exact owner and no "
                      "virtual or receiver contract"});
    }
    if (instance.neverReturns &&
        instance.definitionKind == MirDefinitionKind::Source &&
        std::any_of(instance.body.blocks.begin(), instance.body.blocks.end(),
                    [](const MirBlock &block) {
                      return block.reachable &&
                             block.terminator.kind == MirTerminatorKind::Return;
                    })) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "terminal source function has a reachable return"});
    }
    if (instance.requiresUnsafe &&
        (instance.overloadedOperator || instance.virtualMethod ||
         instance.pureVirtual || instance.overrideMethod ||
         instance.entryKind != ProgramEntryKind::None ||
         instance.linkage != LanguageLinkage::Gti ||
         instance.definitionKind == MirDefinitionKind::RuntimeBinding)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message =
               "unsafe function contract has an invalid declaration kind"});
    }
    if (instance.id != index + 1 || instance.declaration == 0) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "function instance identity or declaration is invalid"});
    }
    switch (instance.definitionKind) {
    case MirFunctionInstance::DefinitionKind::Source:
    case MirFunctionInstance::DefinitionKind::RuntimeBinding:
    case MirFunctionInstance::DefinitionKind::Declaration:
      break;
    default:
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "function definition kind is invalid"});
      break;
    }
    if (instance.definitionKind != MirDefinitionKind::Source &&
        !hasCanonicalBodylessDefinition(
            instance.body, MirBodyKind::Function, instance.returnType,
            instance.parameterTypes, instance.parameterBindings,
            instance.neverReturns)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "bodyless function declaration or runtime binding has "
                      "noncanonical MIR body"});
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
    if (instance.cArrayCountParameter) {
      const std::size_t count = *instance.cArrayCountParameter;
      const SemanticType *countType = count < instance.parameterTypes.size()
                                          ? &instance.parameterTypes[count]
                                          : nullptr;
      const bool validCount =
          countType != nullptr && countType->kind == SemanticType::RawPointer &&
          countType->pointerAccess == AccessMode::Mutable &&
          countType->arguments.size() == 1 &&
          constantIntegerDomain(countType->arguments.front()).has_value();
      if (instance.linkage != LanguageLinkage::C ||
          instance.returnType.kind != SemanticType::RawPointer || !validCount) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Function,
             .owner = instance.id,
             .message = "c_array function metadata is invalid"});
      }
    }
    bool validCallableContracts = true;
    std::string callableContractError;
    const auto rejectCallableContract = [&](std::string message) {
      validCallableContracts = false;
      if (callableContractError.empty()) {
        callableContractError = std::move(message);
      }
    };
    std::size_t previousCallableParameter = 0;
    bool firstCallableParameter = true;
    for (const MirCallableParameter &parameter : instance.callableParameters) {
      const bool borrowed =
          parameter.callableType.kind == SemanticType::Reference &&
          parameter.callableType.arguments.size() == 1;
      const SemanticType &callableType =
          borrowed ? parameter.callableType.arguments.front()
                   : parameter.callableType;
      const CallableInvocationCapability parameterCapability =
          callableInvocationCapability(parameter.access);
      if (parameter.parameterIndex >= instance.parameterTypes.size() ||
          parameter.callableType !=
              instance.parameterTypes[parameter.parameterIndex]) {
        rejectCallableContract("parameter identity or type is invalid");
      }
      const bool exactReturn = parameter.ownedTransport &&
                               parameter.ownedTransport->kind ==
                                   CallableOwnedTransportKind::ExactReturn;
      const bool repeatableSignatures =
          std::none_of(parameter.signatures.begin(), parameter.signatures.end(),
                       [](const MirCallableSignature &signature) {
                         return signature.requiredCapability ==
                                CallableInvocationCapability::Once;
                       });
      if (borrowed &&
          (parameter.access != parameter.callableType.referenceAccess ||
           parameter.boundary != CallableBoundary::Confined ||
           parameter.ownedTransport || !repeatableSignatures)) {
        rejectCallableContract(
            "borrowed callable access or confinement is invalid");
      }
      if (parameter.boundary == CallableBoundary::Confined) {
        if (parameter.ownedTransport &&
            (!exactReturn ||
             parameter.ownedTransport->destinationType ==
                 SemanticType::Unknown ||
             parameter.signatures.empty() || !parameter.forwardings.empty() ||
             !repeatableSignatures)) {
          rejectCallableContract(
              "confined exact-return parameter contract is invalid");
        }
      } else if (!parameter.ownedTransport ||
                 parameter.ownedTransport->destinationType ==
                     SemanticType::Unknown ||
                 !parameter.signatures.empty() ||
                 !parameter.forwardings.empty() ||
                 (parameter.access != AccessMode::ReadOnly &&
                  parameter.ownedTransport->kind !=
                      CallableOwnedTransportKind::Value)) {
        rejectCallableContract("owned parameter contract is invalid");
      }
      if (parameter.ownedTransport) {
        if (const std::optional<std::string> failure =
                ownedTransportFailure(program, instance, parameter)) {
          rejectCallableContract(*failure);
        }
      }
      if (!firstCallableParameter &&
          previousCallableParameter >= parameter.parameterIndex) {
        rejectCallableContract("parameter descriptors are not ordered");
      }
      firstCallableParameter = false;
      previousCallableParameter = parameter.parameterIndex;

      for (std::size_t signatureIndex = 0;
           signatureIndex < parameter.signatures.size(); ++signatureIndex) {
        const MirCallableSignature &signature =
            parameter.signatures[signatureIndex];
        const bool hasTarget =
            int(signature.functionTarget.has_value()) +
                int(signature.lambdaTarget.has_value()) +
                int(signature.nativeCalleeType.has_value()) ==
            1;
        const std::string prefix =
            "signature " + std::to_string(signatureIndex) + " ";
        if (!hasTarget) {
          rejectCallableContract(prefix +
                                 "does not name exactly one selected target");
        }
        if (signature.requiredCapability !=
                CallableInvocationCapability::Once &&
            signature.requiredCapability != parameterCapability) {
          rejectCallableContract(
              prefix + "does not match the callable parameter access");
        }
        if (!signature.selectedCapability) {
          rejectCallableContract(prefix + "has no selected capability");
        } else if (!callableCapabilitySatisfies(*signature.selectedCapability,
                                                signature.requiredCapability)) {
          rejectCallableContract(prefix +
                                 "does not satisfy the required capability");
        }
        if (!hasTarget || !signature.selectedCapability) {
          continue;
        }

        CallableInvocationCapability expected =
            CallableInvocationCapability::Read;
        if (signature.functionTarget) {
          if (*signature.functionTarget == 0 ||
              *signature.functionTarget > program.functionInstances().size()) {
            rejectCallableContract(prefix + "has an invalid function target");
            continue;
          }
          const MirFunctionInstance &target =
              program.functionInstances().at(*signature.functionTarget - 1);
          const bool functionItem = callableType.hasFunctionItemShape();
          if ((functionItem
                   ? target.owner.has_value() ||
                         target.overloadedOperator.has_value() ||
                         callableType.functionId != target.declaration
                   : target.overloadedOperator != OverloadedOperator::Call ||
                         !target.owner || target.staticMember) ||
              signature.returnType != target.returnType ||
              signature.parameterTypes != target.parameterTypes) {
            rejectCallableContract(prefix +
                                   "does not match an exact callable target");
          }
          expected =
              functionItem
                  ? CallableInvocationCapability::Read
                  : callableInvocationCapability(target.receiverMutability);
        } else if (signature.lambdaTarget) {
          const MirLambdaInstance *target =
              program.findLambda(*signature.lambdaTarget);
          if (target == nullptr || target->returnType != signature.returnType ||
              target->parameterTypes != signature.parameterTypes ||
              target->type != callableType) {
            rejectCallableContract(prefix +
                                   "does not match an exact lambda target");
          }
          if (target != nullptr)
            expected = target->capability;
        } else if (signature.nativeCalleeType) {
          const auto &type = *signature.nativeCalleeType;
          const auto parameters = type.nativeFunctionParameterTypes();
          if (type != callableType || !type.hasNativeFunctionShape() ||
              signature.returnType != *type.nativeFunctionReturnType() ||
              signature.parameterTypes.size() != parameters.size() ||
              !std::equal(parameters.begin(), parameters.end(),
                          signature.parameterTypes.begin())) {
            rejectCallableContract(prefix +
                                   "does not match an exact native signature");
          }
        }
        if (*signature.selectedCapability != expected) {
          rejectCallableContract(
              prefix + "disagrees with the selected target capability");
        }
      }

      for (std::size_t forwardingIndex = 0;
           forwardingIndex < parameter.forwardings.size(); ++forwardingIndex) {
        const MirCallableForwarding &forwarding =
            parameter.forwardings[forwardingIndex];
        const std::string prefix =
            "forwarding " + std::to_string(forwardingIndex) + " ";
        const bool duplicate = std::any_of(
            parameter.forwardings.begin(),
            parameter.forwardings.begin() +
                static_cast<std::ptrdiff_t>(forwardingIndex),
            [&](const MirCallableForwarding &candidate) {
              return candidate.functionTarget == forwarding.functionTarget &&
                     candidate.parameterIndex == forwarding.parameterIndex;
            });
        if (duplicate) {
          rejectCallableContract(prefix + "is duplicated");
        }
        if (!forwarding.functionTarget || *forwarding.functionTarget == 0 ||
            *forwarding.functionTarget > program.functionInstances().size()) {
          rejectCallableContract(prefix + "has an invalid function target");
          continue;
        }

        const MirFunctionInstance &target =
            program.functionInstances().at(*forwarding.functionTarget - 1);
        const auto targetContract = std::find_if(
            target.callableParameters.begin(), target.callableParameters.end(),
            [&](const MirCallableParameter &candidate) {
              return candidate.parameterIndex == forwarding.parameterIndex;
            });
        const SemanticType *forwardedType =
            forwarding.parameterIndex < target.parameterTypes.size()
                ? &target.parameterTypes[forwarding.parameterIndex]
                : nullptr;
        if (forwardedType != nullptr &&
            forwardedType->kind == SemanticType::Reference &&
            forwardedType->arguments.size() == 1) {
          if (forwardedType->referenceAccess == AccessMode::Mutable &&
              parameter.access != AccessMode::Mutable)
            rejectCallableContract(prefix +
                                   "upgrades borrowed callable access");
          forwardedType = &forwardedType->arguments.front();
        }
        if (forwarding.parameterIndex >= target.parameterTypes.size() ||
            targetContract == target.callableParameters.end() ||
            targetContract->boundary != CallableBoundary::Confined ||
            forwardedType == nullptr || *forwardedType != callableType) {
          rejectCallableContract(prefix +
                                 "does not match a confined target parameter");
        }

        const std::optional<HirBindingId> sourceBinding =
            parameter.parameterIndex < instance.parameterBindings.size()
                ? std::optional<HirBindingId>{instance.parameterBindings
                                                  [parameter.parameterIndex]}
                : std::nullopt;
        const bool targetRequiresOnce = callableContractRequiresOnce(
            program, *forwarding.functionTarget, forwarding.parameterIndex);
        bool exactCallEdge = false;
        bool exactOnceMoveProof = true;
        if (sourceBinding) {
          for (const MirBlock &block : instance.body.blocks) {
            for (const MirInstruction &instruction : block.instructions) {
              if (instruction.kind != MirInstructionKind::Call ||
                  instruction.functionTarget != forwarding.functionTarget ||
                  forwarding.parameterIndex >= instruction.operands.size()) {
                continue;
              }
              const MirOperand &operand =
                  instruction.operands[forwarding.parameterIndex];
              const bool matchingEdge =
                  callableSourceBindingForOperand(instance.body, operand) ==
                      sourceBinding &&
                  std::any_of(instruction.callableArguments.begin(),
                              instruction.callableArguments.end(),
                              [&](const CallableArgumentBoundary &argument) {
                                return argument.parameterIndex ==
                                           forwarding.parameterIndex &&
                                       argument.boundary ==
                                           CallableBoundary::Confined;
                              });
              if (!matchingEdge) {
                continue;
              }
              exactCallEdge = true;
              exactOnceMoveProof =
                  exactOnceMoveProof &&
                  (!targetRequiresOnce ||
                   receiverIsConsumed(instance.body, operand,
                                      MirValueUseKind::InstructionOperand,
                                      instruction.id,
                                      forwarding.parameterIndex));
            }
          }
        }
        if (!exactCallEdge) {
          rejectCallableContract(prefix +
                                 "does not match a concrete call edge");
        } else if (!exactOnceMoveProof) {
          rejectCallableContract(
              prefix + "into a once-confined target is not rooted in an "
                       "exact ownership move");
        }
      }
    }
    if (!validCallableContracts) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "function callable capability metadata is invalid: " +
                      callableContractError});
    }
    if (instance.entryKind != ProgramEntryKind::None) {
      ++entryPoints;
      if (instance.owner || instance.staticMember ||
          instance.linkage != LanguageLinkage::Gti) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Function,
             .owner = instance.id,
             .message = "program entry point must be a free GTI function"});
      }
    }
    if (instance.entryKind == ProgramEntryKind::None &&
        instance.entryArgumentAppendTarget) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "non-entry function has a program-argument adapter"});
    } else if (instance.entryKind == ProgramEntryKind::NoArguments &&
               (instance.returnType != SemanticType::Int32 ||
                !instance.parameterTypes.empty() ||
                instance.entryArgumentAppendTarget)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "no-argument entry point has invalid adapter metadata"});
    } else if (instance.entryKind == ProgramEntryKind::OwnedArguments) {
      const bool validEntryShape =
          instance.returnType == SemanticType::Int32 &&
          instance.parameterTypes.size() == 2 &&
          instance.parameterTypes[0] == SemanticType::Int32 &&
          instance.parameterTypes[1].kind == SemanticType::Class &&
          instance.parameterTypes[1].arguments.size() == 1 &&
          instance.parameterTypes[1].arguments.front().kind ==
              SemanticType::Class &&
          instance.entryArgumentAppendTarget &&
          *instance.entryArgumentAppendTarget != instance.id &&
          *instance.entryArgumentAppendTarget <=
              program.functionInstances().size();
      bool validAppendTarget = false;
      if (validEntryShape) {
        const MirFunctionInstance &append = program.functionInstances().at(
            *instance.entryArgumentAppendTarget - 1);
        validAppendTarget =
            append.owner && *append.owner <= program.classInstances().size() &&
            program.classInstances().at(*append.owner - 1).type ==
                instance.parameterTypes[1] &&
            append.returnType == SemanticType::Void &&
            append.parameterTypes.size() == 1 &&
            append.parameterTypes.front() ==
                instance.parameterTypes[1].arguments.front() &&
            !append.staticMember &&
            append.entryKind == ProgramEntryKind::None &&
            append.linkage == LanguageLinkage::Gti;
      }
      if (!validEntryShape || !validAppendTarget) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Function,
             .owner = instance.id,
             .message = "owned-argument entry point has invalid adapter "
                        "metadata"});
      }
    }
    verifyBody(instance.body, instance.id);
  }
  if (entryPoints > 1) {
    result.errors.push_back(
        {.bodyKind = MirBodyKind::Module,
         .message = "MIR program contains multiple entry points"});
  }
  const MirDefinedFailureEffects derivedFailureEffects =
      deriveMirDefinedFailureEffects(program);
  if (derivedFailureEffects.functions.size() !=
          program.functionInstances().size() ||
      derivedFailureEffects.constructors.size() !=
          program.constructorInstances().size() ||
      derivedFailureEffects.destructors.size() !=
          program.destructorInstances().size()) {
    result.errors.push_back(
        {.bodyKind = MirBodyKind::Module,
         .message = "defined-failure effect summary has the wrong size"});
  } else {
    for (const MirFunctionInstance &function : program.functionInstances()) {
      if (function.id == 0 ||
          function.id > derivedFailureEffects.functions.size() ||
          function.mayRaiseDefinedFailure ||
          !derivedFailureEffects.functions[function.id - 1]) {
        continue;
      }
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = function.id,
           .message = "failure-free function summary is not proved by an "
                      "acyclic closed scalar MIR call graph"});
    }
  }
  for (std::size_t index = 0; index < program.constructorInstances().size();
       ++index) {
    const MirConstructorInstance &instance =
        program.constructorInstances()[index];
    verifyCallableBodySignature(instance.body, MirBodyKind::Constructor,
                                SemanticType::Void, instance.id);
    if (instance.id != index + 1 || instance.owner == 0 ||
        instance.owner > program.classInstances().size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor instance identity or owner is invalid"});
    }
    if (instance.definitionKind != MirDefinitionKind::Source &&
        instance.definitionKind != MirDefinitionKind::Declaration) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor definition kind is invalid"});
    }
    if (instance.definitionKind == MirDefinitionKind::Declaration &&
        !hasCanonicalBodylessDefinition(
            instance.body, MirBodyKind::Constructor, SemanticType::Void,
            instance.parameterTypes, instance.parameterBindings)) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "bodyless constructor declaration has noncanonical MIR "
                      "body"});
    }
    if (instance.id != 0 &&
        instance.id <= derivedFailureEffects.constructors.size() &&
        !instance.mayRaiseDefinedFailure &&
        derivedFailureEffects.constructors[instance.id - 1]) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "failure-free constructor summary is not proved by "
                      "bounded MIR construction effects"});
    }
    const MirClassInstance *constructorOwner =
        program.findClassInstance(instance.owner);
    if (instance.generatedDefault) {
      bool exact = constructorOwner != nullptr &&
                   constructorOwner->generatedDefaultRequiresSchedule &&
                   constructorOwner->defaultConstructor ==
                       SpecialMemberStatus::Generated &&
                   instance.definitionKind == MirDefinitionKind::Source &&
                   instance.parameterTypes.empty() &&
                   instance.parameterBindings.empty() &&
                   instance.typeArguments.empty() &&
                   instance.valueArguments.empty() &&
                   !instance.constexprConstructor;
      const HirBaseInstance *expectedBase = nullptr;
      if (constructorOwner != nullptr) {
        const auto base = std::find_if(
            constructorOwner->bases.begin(), constructorOwner->bases.end(),
            [](const auto &candidate) { return !candidate.interface; });
        if (base != constructorOwner->bases.end())
          expectedBase = &*base;
      }
      std::size_t fieldIndex = 0;
      std::size_t baseCount = 0;
      for (const auto &initializer : instance.initializers) {
        if (initializer.kind == ConstructorInitializerTargetKind::Base) {
          ++baseCount;
          const auto *baseOwner =
              expectedBase != nullptr
                  ? program.findClassInstance(expectedBase->instance)
                  : nullptr;
          const auto *target = initializer.constructorTarget
                                   ? program.findConstructorInstance(
                                         *initializer.constructorTarget)
                                   : nullptr;
          exact &=
              fieldIndex == 0 && baseCount == 1 && expectedBase != nullptr &&
              baseOwner != nullptr &&
              initializer.base == expectedBase->instance &&
              initializer.targetType == expectedBase->type &&
              initializer.field == 0 && !initializer.storesReference &&
              !initializer.ownedParameter &&
              initializer.explicitArgumentCount == 0 &&
              initializer.generatedDefault == (baseOwner->defaultConstructor ==
                                               SpecialMemberStatus::Generated);
          if (target != nullptr)
            exact &= expectedBase != nullptr &&
                     target->owner == expectedBase->instance &&
                     target->definitionKind == MirDefinitionKind::Source &&
                     target->generatedDefault == initializer.generatedDefault;
          else
            exact &= !initializer.constructorTarget &&
                     initializer.generatedDefault &&
                     initializer.arguments.empty() && baseOwner != nullptr &&
                     !baseOwner->generatedDefaultRequiresSchedule;
          continue;
        }
        exact &= constructorOwner != nullptr &&
                 fieldIndex < constructorOwner->declaredFields.size() &&
                 initializer.field ==
                     constructorOwner->declaredFields[fieldIndex].symbol &&
                 initializer.targetType ==
                     constructorOwner->declaredFields[fieldIndex].type &&
                 initializer.arguments.size() == 1 &&
                 initializer.explicitArgumentCount == 1 &&
                 !initializer.storesReference &&
                 !initializer.generatedDefault && !initializer.ownedParameter &&
                 !initializer.constructorTarget;
        ++fieldIndex;
      }
      exact &= constructorOwner != nullptr &&
               fieldIndex == constructorOwner->declaredFields.size() &&
               baseCount == (expectedBase != nullptr ? 1U : 0U);
      if (!exact)
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Constructor,
             .owner = instance.id,
             .message = "generated constructor lost its exact default "
                        "initialization schedule"});
    }
    bool exactDefaultFieldRollbackEntry = constructorOwner != nullptr;
    std::unordered_map<SymbolId, const MirDropObligation *>
        defaultFieldRollbackSources;
    if (constructorOwner != nullptr) {
      for (const MirDropObligation &rollback :
           constructorOwner->fieldInitializers.dropObligations) {
        if (rollback.kind != MirDropObligationKind::ConstructionRollback) {
          continue;
        }
        const MirPlace *place =
            constructorOwner->fieldInitializers.findPlace(rollback.place);
        const auto field =
            std::find_if(constructorOwner->declaredFields.begin(),
                         constructorOwner->declaredFields.end(),
                         [&](const MirClassFieldInfo &candidate) {
                           return place != nullptr &&
                                  candidate.field == place->binding &&
                                  candidate.symbol == place->symbol;
                         });
        if (place == nullptr || place->root != MirPlaceRootKind::Binding ||
            !place->projections.empty() || rollback.initiallyActive ||
            field == constructorOwner->declaredFields.end() ||
            place->type != field->type ||
            rollback.dropType.type != field->type ||
            !defaultFieldRollbackSources.emplace(field->symbol, &rollback)
                 .second) {
          exactDefaultFieldRollbackEntry = false;
        }
      }
    }

    std::unordered_set<SymbolId> explicitlyInitializedFields;
    std::size_t previousFieldOrder = 0;
    bool exactSelectedFieldOrder = true;
    for (const MirConstructorInitializer &initializer : instance.initializers) {
      if (initializer.kind == ConstructorInitializerTargetKind::Base &&
          initializer.base && initializer.arguments.empty() &&
          initializer.explicitArgumentCount == 0) {
        const auto *base = program.findClassInstance(*initializer.base);
        const auto *target = initializer.constructorTarget
                                 ? program.findConstructorInstance(
                                       *initializer.constructorTarget)
                                 : nullptr;
        if (base != nullptr && base->generatedDefaultRequiresSchedule &&
            (!initializer.generatedDefault || target == nullptr ||
             target->owner != base->id || !target->generatedDefault ||
             !target->parameterTypes.empty() ||
             initializer.targetType != base->type)) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Constructor,
               .owner = instance.id,
               .message = "generated base initialization is missing its "
                          "required exact schedule"});
        }
      }
      if (initializer.kind == ConstructorInitializerTargetKind::Field &&
          initializer.field != 0) {
        if (!explicitlyInitializedFields.insert(initializer.field).second)
          exactSelectedFieldOrder = false;
        if (constructorOwner != nullptr) {
          const auto field =
              std::find_if(constructorOwner->declaredFields.begin(),
                           constructorOwner->declaredFields.end(),
                           [&](const MirClassFieldInfo &candidate) {
                             return candidate.symbol == initializer.field;
                           });
          const std::size_t order =
              std::size_t(field - constructorOwner->declaredFields.begin()) + 1;
          if (field == constructorOwner->declaredFields.end() ||
              field->type != initializer.targetType ||
              order <= previousFieldOrder)
            exactSelectedFieldOrder = false;
          previousFieldOrder = order;
        }
      } else if (initializer.kind == ConstructorInitializerTargetKind::Base &&
                 !explicitlyInitializedFields.empty()) {
        exactSelectedFieldOrder = false;
      }
    }
    if (!exactSelectedFieldOrder) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message =
               "constructor selected fields must be unique, exactly typed, "
               "and in declaration order after the base"});
    }

    std::vector<SymbolId> expectedDefaultRollbackOrder;
    if (constructorOwner != nullptr &&
        instance.definitionKind == MirDefinitionKind::Source) {
      for (const MirClassFieldInfo &field : constructorOwner->declaredFields) {
        if (field.dropKind == DropKind::Lexical &&
            field.requiresActiveCleanup &&
            !explicitlyInitializedFields.contains(field.symbol) &&
            defaultFieldRollbackSources.contains(field.symbol)) {
          expectedDefaultRollbackOrder.push_back(field.symbol);
        }
      }
    }

    std::vector<SymbolId> actualDefaultRollbackOrder;
    std::unordered_set<SymbolId> actualDefaultRollbackFields;
    for (const MirDropObligation &rollback : instance.body.dropObligations) {
      if (rollback.kind != MirDropObligationKind::ConstructionRollback ||
          !rollback.initiallyActive) {
        continue;
      }
      const MirPlace *place = instance.body.findPlace(rollback.place);
      const MirClassFieldInfo *field = nullptr;
      if (constructorOwner != nullptr && place != nullptr &&
          place->projections.size() == 1) {
        const SymbolId symbol = place->projections.front().field;
        const auto found =
            std::find_if(constructorOwner->declaredFields.begin(),
                         constructorOwner->declaredFields.end(),
                         [&](const MirClassFieldInfo &candidate) {
                           return candidate.symbol == symbol;
                         });
        if (found != constructorOwner->declaredFields.end()) {
          field = &*found;
        }
      }
      const auto source = field == nullptr
                              ? defaultFieldRollbackSources.end()
                              : defaultFieldRollbackSources.find(field->symbol);
      if (place == nullptr || place->root != MirPlaceRootKind::This ||
          place->projections.size() != 1 ||
          place->projections.front().kind != MirProjectionKind::Field ||
          field == nullptr || place->type != field->type ||
          field->dropKind != DropKind::Lexical ||
          !field->requiresActiveCleanup ||
          explicitlyInitializedFields.contains(field->symbol) ||
          source == defaultFieldRollbackSources.end() ||
          source->second->dropType != rollback.dropType ||
          !actualDefaultRollbackFields.insert(field->symbol).second) {
        exactDefaultFieldRollbackEntry = false;
        continue;
      }
      actualDefaultRollbackOrder.push_back(field->symbol);
    }
    exactDefaultFieldRollbackEntry =
        exactDefaultFieldRollbackEntry &&
        actualDefaultRollbackOrder == expectedDefaultRollbackOrder;
    if (!exactDefaultFieldRollbackEntry) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor entry rollback obligations do not exactly "
                      "match completed default field initializers"});
    }
    const auto cleanupFreeInitializerType = [&](const SemanticType &type) {
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
      case SemanticType::RawPointer:
        return true;
      case SemanticType::Array: {
        const auto traits = concreteMirTypeTraits(program, type);
        return traits && !traits->containsBorrowedState;
      }
      case SemanticType::Expected: {
        const auto traits = concreteMirTypeTraits(program, type);
        return traits && !traits->containsBorrowedState;
      }
      case SemanticType::Enum:
        return program.findPayloadEnum(type) == nullptr;
      case SemanticType::Class: {
        const auto traits = concreteMirTypeTraits(program, type);
        return traits &&
               (!traits->containsBorrowedState ||
                findStoredCarrierReference(program, type).has_value());
      }
      case SemanticType::Lambda: {
        // A closure environment stored by value is a borrow-carrying field
        // like a class carrier: its borrowed state is its reference
        // captures, published through the same explicit field stage. A
        // closure without borrowed state publishes through the constructor
        // record instead and takes no stage.
        const auto traits = concreteMirTypeTraits(program, type);
        return traits.has_value() && traits->containsBorrowedState;
      }
      default:
        return false;
      }
    };
    std::vector<std::size_t> initializerStageCounts(
        instance.initializers.size());
    std::vector<std::size_t> initializerBoundaryCounts(
        instance.initializers.size());
    const auto stagedBaseInitializer =
        [&](const MirConstructorInitializer &initializer) {
          const MirClassInstance *base =
              initializer.base ? program.findClassInstance(*initializer.base)
                               : nullptr;
          const MirConstructorInstance *target =
              initializer.constructorTarget
                  ? program.findConstructorInstance(
                        *initializer.constructorTarget)
                  : nullptr;
          const bool selectedConstructor =
              target != nullptr && base != nullptr &&
              target->owner == base->id &&
              target->generatedDefault == initializer.generatedDefault &&
              target->parameterTypes.size() == initializer.arguments.size() &&
              std::all_of(
                  target->parameterTypes.begin(), target->parameterTypes.end(),
                  [&](const SemanticType &type) {
                    const auto traits = concreteMirTypeTraits(program, type);
                    return traits && traits->drop == DropKind::Trivial &&
                           !traits->containsBorrowedState &&
                           type.kind != SemanticType::Reference;
                  });
          const bool selectedGeneratedDefault =
              !initializer.constructorTarget && initializer.generatedDefault &&
              initializer.arguments.empty() && base != nullptr &&
              base->defaultConstructor == SpecialMemberStatus::Generated &&
              !base->generatedDefaultRequiresSchedule &&
              !base->traits.containsBorrowedState &&
              std::none_of(base->bases.begin(), base->bases.end(),
                           [](const HirBaseInstance &nested) {
                             return !nested.interface;
                           }) &&
              std::all_of(base->declaredFields.begin(),
                          base->declaredFields.end(),
                          [](const MirClassFieldInfo &field) {
                            return !field.requiresActiveCleanup &&
                                   field.dropKind == DropKind::Trivial;
                          }) &&
              std::all_of(base->fieldInitializers.blocks.begin(),
                          base->fieldInitializers.blocks.end(),
                          [](const MirBlock &block) {
                            return std::all_of(
                                block.instructions.begin(),
                                block.instructions.end(),
                                [](const MirInstruction &instruction) {
                                  return instruction.definedFailure.empty() &&
                                         !instruction.functionTarget &&
                                         !instruction.constructorTarget &&
                                         !instruction.lambdaTarget;
                                });
                          });
          return constructorOwner != nullptr &&
                 mirBodyRoutesFailureEdges(instance.body) &&
                 std::none_of(
                     instance.body.dropObligations.begin(),
                     instance.body.dropObligations.end(),
                     [](const MirDropObligation &drop) {
                       return drop.kind ==
                                  MirDropObligationKind::ConstructionRollback &&
                              drop.initiallyActive;
                     }) &&
                 initializer.kind == ConstructorInitializerTargetKind::Base &&
                 !initializer.storesReference && !initializer.ownedParameter &&
                 (selectedConstructor || selectedGeneratedDefault) &&
                 !base->traits.containsBorrowedState &&
                 base->type == initializer.targetType &&
                 std::any_of(constructorOwner->bases.begin(),
                             constructorOwner->bases.end(),
                             [&](const HirBaseInstance &candidate) {
                               return !candidate.interface &&
                                      candidate.instance == base->id &&
                                      candidate.type == base->type;
                             });
        };
    bool exactInitializerStages = constructorOwner != nullptr;
    const MirInstruction *baseStage = nullptr;
    MirBlockId baseStageBlock = 0;
    for (const MirBlock &block : instance.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.constructorInitializer == 0) {
          continue;
        }
        const std::size_t stage = instruction.constructorInitializer;
        if (stage > instance.initializers.size()) {
          exactInitializerStages = false;
          continue;
        }
        ++initializerStageCounts[stage - 1];
        const MirConstructorInitializer &initializer =
            instance.initializers[stage - 1];
        if (instruction.kind == MirInstructionKind::InitializeBase) {
          if (baseStage != nullptr)
            exactInitializerStages = false;
          baseStage = &instruction;
          baseStageBlock = block.id;
          const MirPlace *destination =
              instruction.destination
                  ? instance.body.findPlace(*instruction.destination)
                  : nullptr;
          const MirDropObligation *rollback =
              instruction.successResultDrop
                  ? instance.body.findDropObligation(
                        *instruction.successResultDrop)
                  : nullptr;
          const MirClassInstance *baseOwner =
              initializer.base ? program.findClassInstance(*initializer.base)
                               : nullptr;
          const MirConstructorInstance *baseConstructor =
              initializer.constructorTarget
                  ? program.findConstructorInstance(
                        *initializer.constructorTarget)
                  : nullptr;
          bool exactArguments =
              instruction.operands.size() == initializer.arguments.size() &&
              instruction.parameterTypes.size() == initializer.arguments.size();
          const auto liveForwardedBorrow = [&](const MirValue &value) {
            if (!value.info.traits.containsBorrowedState)
              return true;
            // The bounded passive forwarding form observes a formal whose
            // entry loan is live through this call. A value-level dependency
            // is not borrowed state retained by the constructed base. Do not
            // admit arbitrary borrowed SSA values merely because their type
            // is passive: retain an independently checkable lifetime witness.
            const auto *load = linearDefinition(instance.body, value.id);
            const auto *source =
                load && load->kind == MirInstructionKind::Load &&
                        load->operands.size() == 1 &&
                        load->operands.front().kind == MirOperandKind::Copy
                    ? instance.body.findPlace(load->operands.front().place)
                    : nullptr;
            if (block.id != instance.body.entry ||
                value.definitionBlock != block.id || !source ||
                source->root != MirPlaceRootKind::Binding ||
                !source->projections.empty() || !source->initiallyAvailable ||
                source->type != value.info.type ||
                std::find(instance.parameterBindings.begin(),
                          instance.parameterBindings.end(),
                          source->binding) == instance.parameterBindings.end())
              return false;
            const auto loan = std::find_if(
                instance.body.loans.begin(), instance.body.loans.end(),
                [&](const MirLoan &candidate) {
                  return candidate.entry && candidate.parent == 0 &&
                         candidate.source == source->id &&
                         candidate.access == AccessMode::ReadOnly &&
                         std::find(candidate.carriers.begin(),
                                   candidate.carriers.end(),
                                   source->binding) != candidate.carriers.end();
                });
            if (loan == instance.body.loans.end())
              return false;
            bool loaded = false;
            for (const auto &prefix : block.instructions) {
              if (&prefix == &instruction)
                return loaded;
              if (&prefix == load)
                loaded = true;
              if (prefix.kind == MirInstructionKind::EndBorrow &&
                  prefix.loan == loan->id)
                return false;
              if (prefix.loan) {
                const auto *child = instance.body.findLoan(*prefix.loan);
                if (child && child->parent == loan->id)
                  return false;
              }
            }
            return false;
          };
          for (std::size_t index = 0;
               exactArguments && index < initializer.arguments.size();
               ++index) {
            const auto &operand = instruction.operands[index];
            const MirValue *value = operand.kind == MirOperandKind::Value
                                        ? instance.body.findValue(operand.value)
                                        : nullptr;
            exactArguments =
                value != nullptr &&
                value->sourceValue == initializer.arguments[index] &&
                value->info.type == operand.type &&
                value->info.traits.drop == DropKind::Trivial &&
                liveForwardedBorrow(*value) &&
                value->info.type.kind != SemanticType::Reference &&
                operand.type == instruction.parameterTypes[index] &&
                baseConstructor != nullptr &&
                index < baseConstructor->parameterTypes.size() &&
                operand.type == baseConstructor->parameterTypes[index];
          }
          const bool fallible = baseConstructor != nullptr &&
                                baseConstructor->mayRaiseDefinedFailure;
          exactInitializerStages =
              exactInitializerStages && stagedBaseInitializer(initializer) &&
              std::none_of(
                  instance.body.dropObligations.begin(),
                  instance.body.dropObligations.end(),
                  [](const MirDropObligation &drop) {
                    return drop.kind ==
                               MirDropObligationKind::ConstructionRollback &&
                           drop.initiallyActive;
                  }) &&
              exactArguments &&
              instruction.constructorTarget == initializer.constructorTarget &&
              instruction.info.type == initializer.targetType &&
              instruction.definedFailure.propagation ==
                  (fallible ? FailurePropagationKind::Constructor
                            : FailurePropagationKind::None) &&
              destination != nullptr &&
              destination->root == MirPlaceRootKind::This &&
              destination->projections.size() == 1 &&
              destination->projections.front().kind ==
                  MirProjectionKind::Base &&
              destination->projections.front().selection == initializer.base &&
              destination->type == initializer.targetType &&
              rollback != nullptr && rollback->place == destination->id &&
              rollback->dropType.classInstance == initializer.base &&
              baseOwner != nullptr &&
              rollback->dropType.requiresActiveCleanup ==
                  baseOwner->requiresActiveCleanup &&
              rollback->dropType.destructor == baseOwner->destructor &&
              !rollback->initiallyActive;
          continue;
        }
        const MirClassFieldInfo *field =
            constructorOwner == nullptr ? nullptr
                                        : [&]() -> const MirClassFieldInfo * {
          const auto found =
              std::find_if(constructorOwner->declaredFields.begin(),
                           constructorOwner->declaredFields.end(),
                           [&](const MirClassFieldInfo &candidate) {
                             return candidate.symbol == initializer.field;
                           });
          return found == constructorOwner->declaredFields.end() ? nullptr
                                                                 : &*found;
        }();
        const MirPlace *destination =
            instruction.destination
                ? instance.body.findPlace(*instruction.destination)
                : nullptr;
        const MirValue *sourceValue =
            instruction.operands.size() == 1 &&
                    instruction.operands.front().kind == MirOperandKind::Value
                ? instance.body.findValue(instruction.operands.front().value)
                : nullptr;
        exactInitializerStages =
            exactInitializerStages &&
            initializer.kind == ConstructorInitializerTargetKind::Field &&
            initializer.arguments.size() == 1 && field != nullptr &&
            field->type == initializer.targetType &&
            instruction.kind == MirInstructionKind::Initialize &&
            !instruction.result && instruction.destination &&
            instruction.hirValue == initializer.arguments.front() &&
            instruction.operands.size() == 1 && sourceValue != nullptr &&
            sourceValue->sourceValue == initializer.arguments.front() &&
            sourceValue->info.type == instruction.operands.front().type &&
            (instruction.operands.front().type == initializer.targetType ||
             isImplicitScalarInitializationConversion(
                 initializer.targetType, instruction.operands.front().type) ||
             isImplicitExpectedInitializationConversion(
                 initializer.targetType, instruction.operands.front().type)) &&
            instruction.info.type == initializer.targetType &&
            destination != nullptr &&
            destination->root == MirPlaceRootKind::This &&
            destination->projections.size() == 1 &&
            destination->projections.front().kind == MirProjectionKind::Field &&
            destination->projections.front().field == initializer.field &&
            destination->sourceValue == initializer.arguments.front() &&
            destination->type == initializer.targetType;
      }
    }
    if (baseStage != nullptr) {
      const auto dominance = computeMirDominance(instance.body);
      const auto *baseBlock = instance.body.findBlock(baseStageBlock);
      const MirBlockId completed =
          baseStage->definedFailure.empty()
              ? baseStageBlock
              : (baseBlock != nullptr &&
                         baseBlock->terminator.kind ==
                             MirTerminatorKind::Invoke &&
                         baseBlock->terminator.invokeInstruction ==
                             baseStage->id
                     ? baseBlock->terminator.target
                     : 0);
      // Publication alone is insufficient: an effectful argument may already
      // have run before the base while its Initialize remains after it. Follow
      // the existing SSA/place dependencies of each selected field argument,
      // including scalar joins and their controlling conditions. No source
      // expression reconstruction or effect inference is needed here.
      std::vector<MirValueId> pendingValues;
      std::vector<MirPlaceId> pendingPlaces;
      std::vector<MirBlockId> pendingControls;
      std::unordered_set<MirValueId> seenValues;
      std::unordered_set<MirPlaceId> seenPlaces;
      std::unordered_set<MirBlockId> seenControls;
      std::unordered_set<MirInstructionId> fieldArgumentDefinitions;
      const auto enqueueOperand = [&](const MirOperand &operand) {
        if (operand.kind == MirOperandKind::Value)
          pendingValues.push_back(operand.value);
        if (operand.place != 0)
          pendingPlaces.push_back(operand.place);
      };
      for (const auto &initializer : instance.initializers) {
        if (initializer.kind != ConstructorInitializerTargetKind::Field)
          continue;
        for (const auto argument : initializer.arguments) {
          bool found = false;
          for (const auto &value : instance.body.values)
            if (value.sourceValue == argument) {
              pendingValues.push_back(value.id);
              found = true;
            }
          exactInitializerStages &= found;
        }
      }
      while (!pendingValues.empty() || !pendingPlaces.empty() ||
             !pendingControls.empty()) {
        if (!pendingPlaces.empty()) {
          const auto id = pendingPlaces.back();
          pendingPlaces.pop_back();
          if (!seenPlaces.insert(id).second)
            continue;
          const auto *place = instance.body.findPlace(id);
          if (place == nullptr) {
            exactInitializerStages = false;
            continue;
          }
          if (place->value != 0)
            pendingValues.push_back(place->value);
          for (const auto &projection : place->projections)
            if (projection.index != 0)
              pendingValues.push_back(projection.index);
          if (const auto *loan = instance.body.findLoan(place->loan))
            pendingPlaces.push_back(loan->source);
          continue;
        }
        if (!pendingControls.empty()) {
          const auto id = pendingControls.back();
          pendingControls.pop_back();
          if (!seenControls.insert(id).second || id == completed ||
              id == baseStageBlock)
            continue;
          for (const auto &predecessor : instance.body.blocks) {
            const auto successors =
                terminatorSuccessors(predecessor.terminator);
            if (std::find(successors.begin(), successors.end(), id) ==
                successors.end())
              continue;
            if (predecessor.terminator.value)
              enqueueOperand(*predecessor.terminator.value);
            pendingControls.push_back(predecessor.id);
          }
          continue;
        }
        const auto id = pendingValues.back();
        pendingValues.pop_back();
        if (!seenValues.insert(id).second)
          continue;
        const auto *value = instance.body.findValue(id);
        const auto *block =
            value == nullptr ? nullptr
                             : instance.body.findBlock(value->definitionBlock);
        if (value == nullptr || block == nullptr) {
          exactInitializerStages = false;
          continue;
        }
        if (const auto *definition = linearDefinition(instance.body, id)) {
          fieldArgumentDefinitions.insert(definition->id);
          if (definition->receiver)
            enqueueOperand(*definition->receiver);
          for (const auto &operand : definition->operands)
            enqueueOperand(operand);
          if (dominance && dominance->dominates(completed, block->id))
            pendingControls.push_back(block->id);
          continue;
        }
        // A block parameter is evaluated at entry, not after an instruction
        // in that block. Only a join reached after base completion qualifies.
        const auto parameter =
            std::find_if(block->parameters.begin(), block->parameters.end(),
                         [&](const MirBlockParameter &candidate) {
                           return candidate.value == id;
                         });
        if (parameter == block->parameters.end() || !dominance ||
            !dominance->dominates(completed, block->id) ||
            block->id == baseStageBlock) {
          exactInitializerStages = false;
          continue;
        }
        const auto ordinal =
            static_cast<std::size_t>(parameter - block->parameters.begin());
        const auto enqueueEdge = [&](MirBlockId target,
                                     const std::vector<MirOperand> &arguments) {
          if (target != block->id)
            return;
          if (ordinal >= arguments.size())
            exactInitializerStages = false;
          else
            enqueueOperand(arguments[ordinal]);
        };
        for (const auto &predecessor : instance.body.blocks) {
          const auto &terminator = predecessor.terminator;
          enqueueEdge(terminator.target, terminator.targetArguments);
          enqueueEdge(terminator.elseTarget, terminator.elseArguments);
          for (const auto &target : terminator.switchTargets)
            enqueueEdge(target.target, target.arguments);
        }
        pendingControls.push_back(block->id);
      }
      for (const auto &block : instance.body.blocks) {
        bool afterBase = block.id != baseStageBlock;
        for (const auto &instruction : block.instructions) {
          if (&instruction == baseStage) {
            afterBase = true;
            continue;
          }
          const auto *destination =
              instruction.destination
                  ? instance.body.findPlace(*instruction.destination)
                  : nullptr;
          const bool publishesField =
              destination != nullptr &&
              destination->root == MirPlaceRootKind::This &&
              !destination->projections.empty() &&
              destination->projections.front().kind ==
                  MirProjectionKind::Field &&
              (instruction.kind == MirInstructionKind::Initialize ||
               instruction.kind == MirInstructionKind::Assign ||
               instruction.kind == MirInstructionKind::Construct);
          if (instruction.constructorInitializer != 0 || publishesField)
            exactInitializerStages =
                exactInitializerStages && dominance && completed != 0 &&
                dominance->dominates(completed, block.id) && afterBase;
          if (fieldArgumentDefinitions.contains(instruction.id)) {
            const auto *parameter =
                instruction.kind == MirInstructionKind::Load &&
                        instruction.operands.size() == 1 &&
                        instruction.operands.front().kind ==
                            MirOperandKind::Copy
                    ? instance.body.findPlace(
                          instruction.operands.front().place)
                    : nullptr;
            const bool scalarParameterLoad =
                parameter != nullptr &&
                parameter->root == MirPlaceRootKind::Binding &&
                parameter->projections.empty() &&
                parameter->initiallyAvailable &&
                isMirBlockParameterType(parameter->type) &&
                std::find(instance.parameterBindings.begin(),
                          instance.parameterBindings.end(),
                          parameter->binding) !=
                    instance.parameterBindings.end();
            const bool harmless =
                instruction.definedFailure.empty() &&
                instruction.lifecycle.empty() &&
                (scalarParameterLoad ||
                 (instruction.kind == MirInstructionKind::Compute &&
                  isMirBlockParameterType(instruction.info.type) &&
                  effects(instruction).speculatable));
            exactInitializerStages =
                exactInitializerStages &&
                (harmless ||
                 (dominance && completed != 0 &&
                  dominance->dominates(completed, block.id) && afterBase));
          }
        }
      }
    }
    for (const MirFullExpression &full : instance.body.fullExpressions) {
      if (full.constructorInitializer == 0) {
        continue;
      }
      if (full.constructorInitializer > instance.initializers.size()) {
        exactInitializerStages = false;
        continue;
      }
      ++initializerBoundaryCounts[full.constructorInitializer - 1];
      exactInitializerStages =
          exactInitializerStages &&
          full.roots ==
              instance.initializers[full.constructorInitializer - 1].arguments;
    }
    for (std::size_t initializerIndex = 0;
         initializerIndex < instance.initializers.size(); ++initializerIndex) {
      const MirConstructorInitializer &initializer =
          instance.initializers[initializerIndex];
      if (stagedBaseInitializer(initializer)) {
        exactInitializerStages =
            exactInitializerStages &&
            initializerStageCounts[initializerIndex] == 1 &&
            initializerBoundaryCounts[initializerIndex] <= 1;
        continue;
      }
      if (initializer.explicitArgumentCount > initializer.arguments.size()) {
        exactInitializerStages = false;
      }
      const auto field =
          constructorOwner == nullptr
              ? std::vector<MirClassFieldInfo>::const_iterator{}
              : std::find_if(constructorOwner->declaredFields.begin(),
                             constructorOwner->declaredFields.end(),
                             [&](const MirClassFieldInfo &candidate) {
                               return candidate.symbol == initializer.field;
                             });
      const bool explicitCleanupFreeField =
          constructorOwner != nullptr &&
          field != constructorOwner->declaredFields.end() &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          !initializer.storesReference && !initializer.constructorTarget &&
          !initializer.generatedDefault && initializer.arguments.size() == 1 &&
          field->type == initializer.targetType &&
          cleanupFreeInitializerType(initializer.targetType) &&
          (field->dropKind == DropKind::Trivial ||
           initializer.targetType.kind == SemanticType::Class ||
           initializer.targetType.kind == SemanticType::Lambda ||
           initializer.targetType.kind == SemanticType::Array ||
           initializer.targetType.kind == SemanticType::Expected) &&
          !field->requiresActiveCleanup;
      // A lexical field completed from one owning argument may publish an
      // explicit rollback-armed stage; whether it does depends on the
      // argument's lowering-time ownership, so its stage count is at most
      // one rather than exactly one. This is deliberately based on the
      // field's lifecycle contract rather than compiler-known type kinds:
      // storage and source-defined owning classes obey the same rule.
      const bool explicitOwningField =
          constructorOwner != nullptr &&
          field != constructorOwner->declaredFields.end() &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          !initializer.storesReference && !initializer.generatedDefault &&
          initializer.arguments.size() == 1 &&
          field->type == initializer.targetType &&
          field->dropKind == DropKind::Lexical;
      // Constructor full-expression boundaries are shared lifecycle facts,
      // not scalar-stage markers: reference, nested-construction, and
      // generated-default initializers can legitimately publish one. Only the
      // non-borrowing cleanup-free form requires a matching Initialize stage.
      if ((explicitCleanupFreeField &&
           (initializerStageCounts[initializerIndex] != 1 ||
            initializerBoundaryCounts[initializerIndex] != 1)) ||
          (!explicitCleanupFreeField && explicitOwningField &&
           (initializerStageCounts[initializerIndex] > 1 ||
            initializerBoundaryCounts[initializerIndex] > 1)) ||
          (!explicitCleanupFreeField && !explicitOwningField &&
           (initializerStageCounts[initializerIndex] != 0 ||
            initializerBoundaryCounts[initializerIndex] > 1))) {
        exactInitializerStages = false;
      }
    }
    if (!exactInitializerStages) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Constructor,
           .owner = instance.id,
           .message = "constructor initializer stages do not exactly cover "
                      "their cleanup-free or owning field initializers"});
    }
    for (const MirConstructorInitializer &initializer : instance.initializers) {
      if (!initializer.ownedParameter) {
        continue;
      }
      const std::size_t parameter = *initializer.ownedParameter;
      const bool exactField =
          constructorOwner != nullptr &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          initializer.field != 0 && initializer.arguments.size() == 1 &&
          parameter < instance.parameterTypes.size() &&
          parameter < instance.parameterBindings.size() &&
          initializer.targetType == instance.parameterTypes[parameter] &&
          std::any_of(
              constructorOwner->declaredFields.begin(),
              constructorOwner->declaredFields.end(),
              [&](const MirClassFieldInfo &field) {
                return field.symbol == initializer.field &&
                       field.type == initializer.targetType;
              });
      const HirValueId ownedSource =
          exactField ? initializer.arguments.front() : 0;
      const auto value = std::find_if(
          instance.body.values.begin(), instance.body.values.end(),
          [&](const MirValue &candidate) {
            return ownedSource != 0 && candidate.sourceValue == ownedSource;
          });
      if (!exactField || value == instance.body.values.end() ||
          std::count_if(instance.body.values.begin(),
                        instance.body.values.end(),
                        [&](const MirValue &item) {
                          return item.sourceValue == ownedSource;
                        }) != 1 ||
          !exactMoveFromBinding(instance.body,
                                MirOperand{.kind = MirOperandKind::Value,
                                           .value = value->id,
                                           .type = value->info.type},
                                instance.parameterBindings[parameter]) ||
          !valueTransferredOut(instance.body,
                               MirOperand{.kind = MirOperandKind::Value,
                                          .value = value->id,
                                          .type = value->info.type},
                               true)) {
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Constructor,
             .owner = instance.id,
             .message = "owned field initializer is not rooted in the exact "
                        "constructor parameter move"});
      }
    }
    verifyBody(instance.body, instance.id);
  }
  for (std::size_t index = 0; index < program.destructorInstances().size();
       ++index) {
    const MirDestructorInstance &instance =
        program.destructorInstances()[index];
    verifyCallableBodySignature(instance.body, MirBodyKind::Destructor,
                                SemanticType::Void, instance.id);
    if (instance.id != index + 1 || instance.owner == 0 ||
        instance.owner > program.classInstances().size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "destructor instance identity or owner is invalid"});
    }
    if (instance.definitionKind != MirDefinitionKind::Source &&
        instance.definitionKind != MirDefinitionKind::Declaration) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "destructor definition kind is invalid"});
    }
    if (instance.definitionKind == MirDefinitionKind::Declaration &&
        !hasCanonicalBodylessDefinition(instance.body, MirBodyKind::Destructor,
                                        SemanticType::Void, {}, {})) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "bodyless destructor declaration has noncanonical MIR "
                      "body"});
    }
    if (instance.id != 0 &&
        instance.id <= derivedFailureEffects.destructors.size() &&
        !instance.mayRaiseDefinedFailure &&
        derivedFailureEffects.destructors[instance.id - 1]) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Destructor,
           .owner = instance.id,
           .message = "failure-free destructor summary is not proved by "
                      "bounded MIR cleanup effects"});
    }
    verifyBody(instance.body, instance.id);
  }
  for (std::size_t index = 0; index < program.lambdaInstances().size();
       ++index) {
    const MirLambdaInstance &instance = program.lambdaInstances()[index];
    verifyCallableBodySignature(instance.body, MirBodyKind::Lambda,
                                instance.returnType, instance.id);
    if (instance.id != index + 1 || instance.declaration == 0) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Lambda,
           .owner = instance.id,
           .message = "lambda instance identity or declaration is invalid"});
    }
    const std::span<const SemanticType> exactParameters =
        instance.type.lambdaParameterTypes();
    const std::span<const SemanticType> exactCaptures =
        instance.type.lambdaCaptureTypes();
    const SemanticType *exactReturn = instance.type.lambdaReturnType();
    if (instance.type.kind != SemanticType::Lambda ||
        instance.type.lambdaId != instance.declaration ||
        !instance.type.hasLambdaShape() || exactReturn == nullptr ||
        *exactReturn != instance.returnType ||
        exactParameters.size() != instance.parameterTypes.size() ||
        !std::equal(exactParameters.begin(), exactParameters.end(),
                    instance.parameterTypes.begin()) ||
        exactCaptures.size() != instance.captureTypes.size() ||
        !std::equal(exactCaptures.begin(), exactCaptures.end(),
                    instance.captureTypes.begin())) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Lambda,
           .owner = instance.id,
           .message = "lambda instance type does not match its declaration, "
                      "signature, or captures"});
    }
    if (instance.captureTypes.size() != instance.captureModes.size() ||
        instance.captureTypes.size() != instance.captureSymbols.size() ||
        instance.captureTypes.size() !=
            instance.captureRequiresActiveCleanup.size()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Lambda,
           .owner = instance.id,
           .message = "lambda capture ownership metadata has the wrong size"});
    } else {
      std::unordered_set<SymbolId> exactCaptureSymbols;
      const auto expectedCapability =
          std::find(instance.captureModes.begin(), instance.captureModes.end(),
                    LambdaCaptureMode::BorrowMut) != instance.captureModes.end()
              ? CallableInvocationCapability::Mutable
              : CallableInvocationCapability::Read;
      if (instance.capability != expectedCapability)
        result.errors.push_back(
            {.bodyKind = MirBodyKind::Lambda,
             .owner = instance.id,
             .message =
                 "lambda invocation capability does not match its captures"});
      for (std::size_t capture = 0; capture < instance.captureTypes.size();
           ++capture) {
        if ((instance.captureModes[capture] != LambdaCaptureMode::Copy &&
             instance.captureModes[capture] != LambdaCaptureMode::Move &&
             !isBorrowingCapture(instance.captureModes[capture])) ||
            instance.captureSymbols[capture] == 0 ||
            !exactCaptureSymbols.insert(instance.captureSymbols[capture])
                 .second) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Lambda,
               .owner = instance.id,
               .message = "lambda capture mode or symbol identity is "
                          "invalid or duplicated"});
        }
        const auto mode = instance.captureModes[capture];
        const auto &type = instance.captureTypes[capture];
        if (isBorrowingCapture(mode) &&
            (type.kind != SemanticType::Reference ||
             type.arguments.size() != 1 ||
             type.referenceAccess != (mode == LambdaCaptureMode::BorrowMut
                                          ? AccessMode::Mutable
                                          : AccessMode::ReadOnly))) {
          result.errors.push_back({.bodyKind = MirBodyKind::Lambda,
                                   .owner = instance.id,
                                   .message = "borrowed capture mode does not "
                                              "match its reference access"});
        }
        const std::optional<bool> exactActiveCleanup =
            typeRequiresActiveCleanup(instance.captureTypes[capture],
                                      typeRequiresActiveCleanup);
        if (!exactActiveCleanup ||
            *exactActiveCleanup !=
                instance.captureRequiresActiveCleanup[capture]) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Lambda,
               .owner = instance.id,
               .message =
                   "lambda capture cleanup metadata does not match its type"});
        }
      }
      for (const MirPlace &place : instance.body.places) {
        const auto capture =
            std::find(instance.captureSymbols.begin(),
                      instance.captureSymbols.end(), place.symbol);
        const std::size_t exactCapture =
            capture == instance.captureSymbols.end()
                ? 0
                : static_cast<std::size_t>(
                      std::distance(instance.captureSymbols.begin(), capture)) +
                      1;
        const std::optional<SemanticType> exactPlaceType =
            exactCapture == 0
                ? std::nullopt
                : projectedMirType(program,
                                   instance.captureTypes[exactCapture - 1],
                                   place.projections);
        if (place.capture != exactCapture ||
            (exactCapture != 0 &&
             (place.root != MirPlaceRootKind::Symbol || !exactPlaceType ||
              place.type != *exactPlaceType ||
              (place.access == AccessMode::Mutable &&
               instance.captureModes[exactCapture - 1] !=
                   LambdaCaptureMode::BorrowMut)))) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Lambda,
               .owner = instance.id,
               .message = "lambda capture place does not match its exact "
                          "environment field"});
        }
      }
    }
    verifyBody(instance.body, instance.id);
  }
  const auto exactActiveCleanup = [&](const SemanticType &type) {
    return typeRequiresActiveCleanup(type, typeRequiresActiveCleanup);
  };
  appendVerification(result, verifyMirProgramBorrowContracts(program));
  appendVerification(result, verifyMirDropTargets(program, program.module(), 0,
                                                  exactActiveCleanup));
  for (const MirClassInstance &instance : program.classInstances()) {
    appendVerification(result,
                       verifyMirDropTargets(program, instance.fieldInitializers,
                                            instance.id, exactActiveCleanup));
    appendVerification(
        result, verifyMirDropTargets(program, instance.staticFieldInitializers,
                                     instance.id, exactActiveCleanup));
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    appendVerification(result,
                       verifyMirDropTargets(program, instance.body, instance.id,
                                            exactActiveCleanup));
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    appendVerification(result,
                       verifyMirDropTargets(program, instance.body, instance.id,
                                            exactActiveCleanup));
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    appendVerification(result,
                       verifyMirDropTargets(program, instance.body, instance.id,
                                            exactActiveCleanup));
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    appendVerification(result,
                       verifyMirDropTargets(program, instance.body, instance.id,
                                            exactActiveCleanup));
    // A lambda's reference result carries the same owner-dependent return
    // contract as a free function's: verify it through that summary over a
    // static shell naming the lambda's parameters, type and body.
    if (instance.returnBorrowOrigin != BorrowOriginKind::None ||
        instance.returnType.kind == SemanticType::Reference) {
      MirFunctionInstance shell{
          .id = instance.id,
          .returnType = instance.returnType,
          .parameterTypes = instance.parameterTypes,
          .parameterBindings = instance.parameterBindings,
          .staticMember = true,
          .returnBorrowOrigin = instance.returnBorrowOrigin,
          .returnBorrowParameter = instance.returnBorrowParameter,
          .returnBorrowAccess = instance.returnBorrowAccess,
          .body = instance.body};
      appendVerification(result,
                         verifyMirFunctionBorrowSummary(program, shell));
    }
  }
  if (const MirBody *hosted = program.hostedStartup()) {
    appendVerification(result,
                       verifyMirDropTargets(program, *hosted,
                                            program.hostedStartupPlan()->entry,
                                            exactActiveCleanup));
  }
  return result;
}

MirVerificationResult verifyMirProgram(const MirProgram &program,
                                       MirVerificationComponents components) {
  MirVerificationResult result;
  if (components != requiredMirVerificationComponents) {
    result.errors.push_back(
        {.bodyKind = MirBodyKind::Module,
         .message = "MIR verification component set is incomplete or invalid"});
    return result;
  }

  const auto validScalarIdentity = [&](const auto &self,
                                       const SemanticType &type) -> bool {
    if (type.nativeCScalar != NativeCScalarKind::None &&
        SemanticType::nativeCScalarType(type.nativeCScalar, program.target()) !=
            type) {
      return false;
    }
    const auto validArguments = [&](const auto &arguments) {
      return std::all_of(
          arguments.begin(), arguments.end(),
          [&](const auto &argument) { return self(self, argument); });
    };
    return validArguments(type.arguments) &&
           validArguments(type.lambdaEnclosingClassTypes) &&
           validArguments(type.lambdaEnclosingFunctionTypes);
  };
  const auto validEnumIdentity = [&](const auto &self,
                                     const SemanticType &type) -> bool {
    if (type.kind == SemanticType::Enum &&
        program.findPayloadEnum(type) == nullptr &&
        program.findIntegralEnum(type) == nullptr)
      return false;
    return std::all_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const SemanticType &argument) { return self(self, argument); });
  };
  const auto checkProgramType = [&](const SemanticType &type) {
    if (!validEnumIdentity(validEnumIdentity, type))
      result.errors.push_back(
          {.message = "MIR enum type has no exact concrete enum catalog row"});
    if (!validScalarIdentity(validScalarIdentity, type)) {
      result.errors.push_back(
          {.message = "MIR native C scalar identity disagrees with the "
                      "selected target"});
    }
  };
  const auto checkTypes = [&](const auto &types) {
    for (const SemanticType &type : types) {
      checkProgramType(type);
    }
  };
  for (const auto &function : program.functionInstances()) {
    checkProgramType(function.returnType);
    checkTypes(function.typeArguments);
    checkTypes(function.parameterTypes);
    for (const auto &parameter : function.callableParameters) {
      checkProgramType(parameter.callableType);
      for (const auto &signature : parameter.signatures) {
        if (signature.nativeCalleeType) {
          checkProgramType(*signature.nativeCalleeType);
        }
        checkProgramType(signature.returnType);
        checkTypes(signature.parameterTypes);
      }
    }
  }
  for (const auto &instance : program.classInstances()) {
    checkProgramType(instance.type);
    std::unordered_set<TypeAliasId> aliases;
    const auto closedAlias = [&](const auto &self,
                                 const SemanticType &type) -> bool {
      if (type.kind == SemanticType::Unknown ||
          type.kind == SemanticType::TypeParameter ||
          (type.kind == SemanticType::TypePack && !type.concretePack) ||
          type.arrayLengthParameterId != 0)
        return false;
      const auto valuesClosed = [](const auto &values) {
        return std::all_of(values.begin(), values.end(), [](const auto &value) {
          return value.kind == CompileTimeValue::UInt64;
        });
      };
      const auto typesClosed = [&](const auto &types) {
        return std::all_of(
            types.begin(), types.end(),
            [&](const auto &argument) { return self(self, argument); });
      };
      return valuesClosed(type.valueArguments) &&
             valuesClosed(type.lambdaEnclosingClassValues) &&
             valuesClosed(type.lambdaEnclosingFunctionValues) &&
             typesClosed(type.arguments) &&
             typesClosed(type.lambdaEnclosingClassTypes) &&
             typesClosed(type.lambdaEnclosingFunctionTypes);
    };
    for (const auto &alias : instance.typeAliases) {
      checkProgramType(alias.type);
      if (alias.declaration == 0 || !aliases.insert(alias.declaration).second ||
          !closedAlias(closedAlias, alias.type))
        result.errors.push_back({.bodyKind = MirBodyKind::FieldInitializers,
                                 .owner = instance.id,
                                 .message = "class alias has duplicate/missing "
                                            "identity or open concrete type"});
    }
    for (const auto &field : instance.declaredFields) {
      checkProgramType(field.type);
    }
    for (const auto &field : instance.fields) {
      checkProgramType(field.type);
    }
    for (const auto &field : instance.fieldDropOrder) {
      checkProgramType(field.type);
    }
    if (instance.cAbiLayout) {
      for (const auto &field : instance.cAbiLayout->fields) {
        checkProgramType(field.type);
      }
    }
    if (instance.unionLayout) {
      for (const auto &field : instance.unionLayout->fields) {
        checkProgramType(field.type);
      }
    }
  }
  for (const auto &constructor : program.constructorInstances()) {
    checkTypes(constructor.typeArguments);
    checkTypes(constructor.parameterTypes);
    for (const auto &initializer : constructor.initializers) {
      checkProgramType(initializer.targetType);
    }
  }
  for (const auto &lambda : program.lambdaInstances()) {
    checkProgramType(lambda.type);
    checkProgramType(lambda.returnType);
    checkTypes(lambda.parameterTypes);
    checkTypes(lambda.captureTypes);
  }
  for (const auto &adapter : program.nativeCallbackAdapters()) {
    checkProgramType(adapter.type);
  }
  for (const auto &adapter : program.threadTaskAdapters()) {
    checkProgramType(adapter.taskType);
    checkProgramType(adapter.resultType);
  }
  for (const auto &enumeration : program.integralEnums()) {
    checkProgramType(enumeration.type);
    checkProgramType(enumeration.underlyingType);
  }
  for (const auto &enumeration : program.payloadEnums()) {
    checkProgramType(enumeration.type);
    for (const auto &variant : enumeration.variants) {
      checkTypes(variant.payloadTypes);
    }
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      result.errors.push_back(
          {.bodyKind = address.kind,
           .owner = address.owner,
           .message = "MIR body inventory contains an unresolved address"});
      continue;
    }
    appendVerification(result, verifyMirBody(*body, address.owner));
    const auto check = [&](const SemanticType &type,
                           MirInstructionId instruction = 0) {
      if (!validEnumIdentity(validEnumIdentity, type))
        result.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .instruction = instruction,
             .message =
                 "MIR enum type has no exact concrete enum catalog row"});
      if (!validScalarIdentity(validScalarIdentity, type)) {
        result.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .instruction = instruction,
             .message = "MIR native C scalar identity disagrees with the "
                        "selected target"});
      }
    };
    const auto checkDrop = [&](const auto &self,
                               const MirDropType &drop) -> void {
      check(drop.type);
      if (drop.fixedArray) {
        check(drop.fixedArray->leafType);
      }
      for (const auto &child : drop.aggregate) {
        self(self, child);
      }
    };
    check(body->returnType);
    for (const auto &value : body->values) {
      check(value.info.type);
    }
    for (const auto &iteration : body->rangeIterations) {
      check(iteration.rangeType);
      check(iteration.iteratorType);
      check(iteration.sentinelType);
      check(iteration.elementType);
      if (!iteration.materializesElementValue) {
        const auto *element = body->findLoan(iteration.elementLoan);
        if (iteration.strategy != RangeIterationStrategy::Structural ||
            iteration.elementType.kind != SemanticType::Class || !element ||
            element->kind != MirLoanKind::CallResult ||
            element->source != iteration.iterator)
          continue;
        const MirInstruction *producer = nullptr;
        for (const auto &block : body->blocks)
          for (const auto &instruction : block.instructions)
            if (instruction.kind == MirInstructionKind::Call &&
                instruction.loan == element->id)
              producer = &instruction;
        const auto *selected =
            producer && producer->functionTarget
                ? program.findFunctionInstance(*producer->functionTarget)
                : nullptr;
        const auto nominal = std::find_if(
            program.classInstances().begin(), program.classInstances().end(),
            [&](const auto &instance) {
              return instance.type == iteration.elementType;
            });
        // Body verification owns exact cursor and producer identities; the
        // program catalog independently owns the nominal result's traits and
        // selected declaration. Falsifying those traits cannot turn a physical
        // inline proxy into the abstract Stored-parent element profile.
        if (!selected ||
            selected->declaration != iteration.dereferenceFunction ||
            selected->overloadedOperator != OverloadedOperator::Dereference ||
            !selected->parameterTypes.empty() ||
            selected->returnType !=
                SemanticType::referenceTo(iteration.elementType,
                                          iteration.elementAccess) ||
            selected->returnBorrowOrigin != BorrowOriginKind::Receiver ||
            nominal == program.classInstances().end() ||
            producer->info.traits != nominal->traits)
          result.errors.push_back(
              {.bodyKind = address.kind,
               .owner = address.owner,
               .instruction = producer == nullptr ? 0 : producer->id,
               .message = "reference iteration element has no exact nominal "
                          "traits and selected dereference declaration"});
        continue;
      }
      const auto *value = body->findValue(iteration.elementAccessValue);
      const MirInstruction *producer = nullptr;
      if (value)
        if (const auto *block = body->findBlock(value->definitionBlock))
          for (const auto &instruction : block->instructions)
            if (instruction.id == value->definition)
              producer = &instruction;
      const auto *selected =
          producer && producer->functionTarget
              ? program.findFunctionInstance(*producer->functionTarget)
              : nullptr;
      if (!selected || selected->declaration != iteration.dereferenceFunction ||
          selected->overloadedOperator != OverloadedOperator::Dereference ||
          selected->receiverMutability != ReceiverMutability::ReadOnly ||
          !selected->parameterTypes.empty() ||
          selected->returnType != iteration.elementType ||
          selected->returnBorrowOrigin != BorrowOriginKind::None)
        result.errors.push_back(
            {.bodyKind = address.kind,
             .owner = address.owner,
             .instruction = producer == nullptr ? 0 : producer->id,
             .message = "materialized iteration value has no exact independent "
                        "readonly dereference declaration"});
    }
    for (const auto &obligation : body->dropObligations) {
      checkDrop(checkDrop, obligation.dropType);
    }
    for (const MirPlace &place : body->places) {
      check(place.type);
    }
    for (const MirBlock &block : body->blocks) {
      for (const auto &parameter : block.parameters) {
        check(parameter.type);
      }
      for (const MirInstruction &instruction : block.instructions) {
        check(instruction.info.type, instruction.id);
        check(instruction.dispatchOwner, instruction.id);
        if (instruction.nativeCalleeType) {
          check(*instruction.nativeCalleeType, instruction.id);
        }
        if (instruction.objectLayoutType) {
          check(*instruction.objectLayoutType, instruction.id);
        }
        if (instruction.receiver) {
          check(instruction.receiver->type, instruction.id);
        }
        for (const auto &operand : instruction.operands) {
          check(operand.type, instruction.id);
        }
        for (const auto &type : instruction.parameterTypes) {
          check(type, instruction.id);
        }
        for (const auto &type : instruction.closureCaptureTypes) {
          check(type, instruction.id);
        }
        if (instruction.elementCleanup) {
          checkDrop(checkDrop, *instruction.elementCleanup);
        }
        if (instruction.replacementCleanup) {
          checkDrop(checkDrop, *instruction.replacementCleanup);
        }
      }
      const auto &terminator = block.terminator;
      if (terminator.value) {
        check(terminator.value->type);
      }
      for (const auto &operand : terminator.targetArguments) {
        check(operand.type);
      }
      for (const auto &operand : terminator.elseArguments) {
        check(operand.type);
      }
      for (const auto &target : terminator.switchTargets) {
        if (target.value) {
          check(target.value->type);
        }
        for (const auto &operand : target.arguments) {
          check(operand.type);
        }
      }
    }
  }
  appendVerification(result, verifyMirProgramContracts(program));
  appendVerification(result, verifyMirEffectSummaries(program));
  return result;
}

} // namespace lang
