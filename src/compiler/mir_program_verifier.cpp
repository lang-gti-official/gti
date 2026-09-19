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

using mir_detail::appendVerification;
using mir_detail::canonicalBorrowOrigin;
using mir_detail::CanonicalPlace;
using mir_detail::constantMatchesType;
using mir_detail::freshOwnedValueInto;
using mir_detail::linearDefinition;
using mir_detail::movedValueInto;
using mir_detail::receiverIsConsumed;
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
  if (depth > body.values.size() + body.instructionCount() ||
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
  return definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Identity &&
         exactMoveFromBinding(body, definition->operands.front(), binding,
                              depth + 1);
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
      if (bookkeepingPlace &&
          ((instruction.destination &&
            *instruction.destination == *bookkeepingPlace) ||
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
  if (depth > body.values.size() + body.instructionCount() ||
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

[[nodiscard]] std::optional<std::string>
ownedTransportFailure(const MirProgram &program,
                      const MirFunctionInstance &instance,
                      const MirCallableParameter &parameter) {
  if (!parameter.ownedTransport ||
      parameter.parameterIndex >= instance.parameterBindings.size() ||
      parameter.parameterIndex >= instance.parameterTypes.size()) {
    return "owned transport does not name an exact formal parameter";
  }
  const CallableOwnedTransport &transport = *parameter.ownedTransport;
  const SemanticType &callableType =
      instance.parameterTypes[parameter.parameterIndex];
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
  if (depth > body.places.size() + body.values.size() + body.loans.size() +
                  body.instructionCount()) {
    return std::nullopt;
  }
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  if (place->root == MirPlaceRootKind::Binding && place->projections.empty()) {
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
  if (depth > body.places.size() + body.values.size() + body.loans.size() +
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
       !instruction.functionTarget);
  return sameTarget && signature.selectedCapability &&
         instruction.callableInvocation == signature.selectedCapability &&
         callResultMatches(instruction.info, signature.returnType) &&
         signature.parameterTypes == instruction.parameterTypes;
}

[[nodiscard]] MirVerificationResult
verifyMirCallableMetadata(const MirProgram &program, const MirBody &body,
                          std::size_t owner,
                          const MirFunctionInstance *caller = nullptr) {
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
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
      const MirInstruction *receiverInput =
          instruction.receiver &&
                  instruction.receiver->kind == MirOperandKind::Value
              ? definitionFor(body, instruction.receiver->value)
              : nullptr;
      const bool temporaryReceiver =
          receiverInput != nullptr &&
          (receiverInput->callInputKind ==
               HirCallInputKind::ReadTemporaryBorrow ||
           receiverInput->callInputKind ==
               HirCallInputKind::MutableTemporaryBorrow);
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
        const bool mutableInput = receiverInput->callInputKind ==
                                  HirCallInputKind::MutableTemporaryBorrow;
        const bool exactReceiverQualifier =
            functionTarget != nullptr &&
            ((mutableInput && functionTarget->receiverMutability ==
                                  ReceiverMutability::Mutable) ||
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
              producerOwner->type == receiverPlace->type &&
              producer->constructorKind == ConstructorKind::Ordinary;
        } else if (producer != nullptr &&
                   producer->kind == MirInstructionKind::Call) {
          const MirFunctionInstance *target =
              producer->functionTarget
                  ? program.findFunctionInstance(*producer->functionTarget)
                  : nullptr;
          exactProducer = target != nullptr &&
                          target->linkage == LanguageLinkage::Gti &&
                          target->returnType == receiverPlace->type &&
                          target->returnBorrowOrigin == BorrowOriginKind::None;
        }
        if (functionTarget == nullptr || receiverPlace == nullptr ||
            ownerClass == nullptr || functionTarget->staticMember ||
            functionTarget->virtualMethod ||
            functionTarget->linkage != LanguageLinkage::Gti ||
            instruction.dispatch != CallDispatch::Static ||
            ownerClass->type != receiverPlace->type ||
            !exactReceiverQualifier || !exactProducer ||
            receiverObligations != 1 ||
            functionTarget->returnType.kind == SemanticType::Reference ||
            functionTarget->returnBorrowOrigin != BorrowOriginKind::None ||
            instruction.info.traits.containsBorrowedState) {
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
            functionTarget != nullptr &&
            functionTarget->returnType.kind != SemanticType::Reference &&
            functionTarget->returnType.kind != SemanticType::StringView &&
            functionTarget->returnType.kind != SemanticType::CString &&
            functionTarget->returnType.kind != SemanticType::RawPointer &&
            functionTarget->returnType.kind != SemanticType::NativeFunction &&
            functionTarget->returnBorrowOrigin == BorrowOriginKind::None &&
            !instruction.info.traits.containsBorrowedState;
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
          isCallOperatorTarget(functionTarget) && !instruction.lambdaTarget;
      const bool exactLambdaCallable =
          lambdaTarget != nullptr && !instruction.functionTarget;
      const bool exactCallableTarget =
          exactFunctionCallable != exactLambdaCallable;

      if (instruction.callableInvocation.has_value() != exactCallableTarget) {
        return verificationFailure(
            body, owner,
            "callable invocation capability does not match an "
            "exact lambda or operator() target",
            block.id, instruction.id);
      }
      if (instruction.callableInvocation) {
        const CallableInvocationCapability expected =
            exactFunctionCallable ? callableInvocationCapability(
                                        functionTarget->receiverMutability)
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
            if (operandIndex >= instruction.operands.size() ||
                operandIndex >= functionTarget->parameterTypes.size() ||
                instruction.operands[operandIndex].type !=
                    functionTarget->parameterTypes[operandIndex] ||
                (!movedValueInto(body, instruction.operands[operandIndex],
                                 instruction.id, operandIndex) &&
                 !receiverIsConsumed(body, instruction.operands[operandIndex],
                                     MirValueUseKind::InstructionOperand,
                                     instruction.id, operandIndex) &&
                 !freshOwnedValueInto(body, instruction.operands[operandIndex],
                                      instruction.id, operandIndex))) {
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

[[nodiscard]] MirVerificationResult
verifyMirBorrowProducers(const MirProgram &program, const MirBody &body,
                         std::size_t owner) {
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
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
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
            const bool exactMode =
                lambda->captureModes[index] == LambdaCaptureMode::Copy
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
        }
        if (instruction.borrowOrigin != BorrowOriginKind::None) {
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
          !instruction.functionTarget && !instruction.lambdaTarget) {
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
      if (instruction.functionTarget) {
        target = program.findFunctionInstance(*instruction.functionTarget);
        if (target == nullptr) {
          return verificationFailure(
              body, owner,
              "call borrow contract references an invalid function "
              "target",
              block.id, instruction.id);
        }
        if (instruction.borrowOrigin != target->returnBorrowOrigin ||
            instruction.borrowArgument != target->returnBorrowParameter ||
            instruction.borrowAccess != target->returnBorrowAccess ||
            instruction.borrowPlace != target->returnBorrowPlace) {
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
        std::optional<std::size_t> inPlaceArgumentOffset;
        SemanticType::Kind inPlaceStorageKind = SemanticType::Unknown;
        if (instruction.kind == MirInstructionKind::Call) {
          switch (instruction.intrinsic) {
          case IntrinsicKind::StorageConstruct:
            inPlaceArgumentOffset = 2;
            inPlaceStorageKind = SemanticType::Storage;
            break;
          case IntrinsicKind::PrefixStorageAppend:
            inPlaceArgumentOffset = 1;
            inPlaceStorageKind = SemanticType::PrefixStorage;
            break;
          case IntrinsicKind::PrefixStorageInsert:
            inPlaceArgumentOffset = 2;
            inPlaceStorageKind = SemanticType::PrefixStorage;
            break;
          case IntrinsicKind::OptionalStorageConstruct:
            inPlaceArgumentOffset = 1;
            inPlaceStorageKind = SemanticType::Class;
            break;
          default:
            break;
          }
        }
        if (inPlaceArgumentOffset) {
          const MirClassInstance *constructorOwner =
              program.findClassInstance(constructorTarget->owner);
          const bool hasIndex = *inPlaceArgumentOffset == 2;
          const bool exactStorageSurface =
              instruction.parameterTypes.size() >= *inPlaceArgumentOffset &&
              !instruction.parameterTypes.empty() &&
              instruction.parameterTypes.front().kind == inPlaceStorageKind &&
              instruction.parameterTypes.front().arguments.size() == 1 &&
              (!hasIndex ||
               instruction.parameterTypes[1] == SemanticType::UInt64);
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
        if (instruction.kind == MirInstructionKind::Construct &&
            (instruction.borrowOrigin != constructorTarget->borrowOrigin ||
             instruction.borrowArgument != constructorTarget->borrowParameter ||
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
        const bool staticStringViewResult =
            instruction.kind == MirInstructionKind::Call &&
            instruction.borrowOrigin == BorrowOriginKind::Argument &&
            instruction.borrowAccess == AccessMode::ReadOnly &&
            !instruction.borrowPlace &&
            instruction.info.type.kind == SemanticType::StringView &&
            !instruction.info.traits.containsBorrowedState &&
            instruction.borrowArgument < instruction.parameterTypes.size() &&
            instruction.parameterTypes[instruction.borrowArgument].kind ==
                SemanticType::StringView &&
            argumentInput != nullptr &&
            argumentInput->kind == MirInstructionKind::CallInput &&
            argumentInput->callInputRole == MirCallInputRole::Argument &&
            argumentInput->callInputIndex == instruction.borrowArgument &&
            argumentInput->callInputKind == HirCallInputKind::Value &&
            argumentInput->info.type.kind == SemanticType::StringView &&
            !argumentInput->info.traits.containsBorrowedState;
        if (staticStringViewResult) {
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
      const CanonicalPlace expectedPlace =
          expectedSource
              ? canonicalBorrowOrigin(body, *expectedSource, bindingLoans)
              : CanonicalPlace{.ambiguous = true};
      const CanonicalPlace actualPlace =
          canonicalBorrowOrigin(body, loan->source, bindingLoans);
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
    }
  }
  return {};
}

struct MirStoredCarrierReference {
  SymbolId field = 0;
  SemanticType reference = SemanticType::Unknown;
};

[[nodiscard]] std::optional<MirStoredCarrierReference>
findStoredCarrierReference(const MirProgram &program,
                           const SemanticType &type) {
  if (type.kind != SemanticType::Class) {
    return std::nullopt;
  }
  const auto instance = std::find_if(program.classInstances().begin(),
                                     program.classInstances().end(),
                                     [&](const MirClassInstance &candidate) {
                                       return candidate.type == type;
                                     });
  if (instance == program.classInstances().end() ||
      !instance->traits.containsBorrowedState) {
    return std::nullopt;
  }
  std::optional<MirStoredCarrierReference> reference;
  for (const MirClassFieldInfo &field : instance->declaredFields) {
    if (field.type.kind != SemanticType::Reference) {
      continue;
    }
    if (reference) {
      return std::nullopt;
    }
    reference = MirStoredCarrierReference{.field = field.symbol,
                                          .reference = field.type};
  }
  if (!reference || reference->field == 0 ||
      reference->reference.arguments.size() != 1) {
    return std::nullopt;
  }
  return reference;
}

[[nodiscard]] bool exactStoredCarrierValueTransfer(
    const MirBody &body, MirValueId valueId, const MirLoan &entry,
    const SemanticType &carrierType, const MirInstruction &transfer) {
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
         place->root == MirPlaceRootKind::Binding &&
         place->projections.empty() && place->type == carrierType &&
         std::find(entry.carriers.begin(), entry.carriers.end(),
                   place->binding) != entry.carriers.end();
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
  std::optional<MirStoredCarrierReference> storedCarrierReference;
  const MirLoan *storedCarrierEntryLoan = nullptr;
  bool dynamicStringViewCarrier = false;
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
    break;
  case BorrowOriginKind::Argument: {
    if (instance.returnBorrowParameter >= instance.parameterTypes.size() ||
        instance.returnBorrowParameter >= instance.parameterBindings.size() ||
        instance.parameterBindings[instance.returnBorrowParameter] == 0) {
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
      storedCarrierReference = findStoredCarrierReference(program, parameter);
      const bool exactReference =
          parameter.kind == SemanticType::Reference &&
          parameter.arguments.size() == 1 &&
          parameter.referenceAccess == instance.returnBorrowAccess;
      const bool exactCarrier =
          storedCarrierReference &&
          storedCarrierReference->reference.referenceAccess ==
              instance.returnBorrowAccess;
      dynamicStringViewCarrier =
          parameter.kind == SemanticType::StringView &&
          instance.returnType.kind == SemanticType::StringView &&
          instance.returnBorrowAccess == AccessMode::ReadOnly;
      if (!exactReference && !exactCarrier && !dynamicStringViewCarrier) {
        return invalidSummary(
            "argument return dependency does not identify one exact "
            "access-compatible reference, stored-reference carrier, or dynamic "
            "string-view parameter");
      }
      if (exactCarrier || dynamicStringViewCarrier) {
        std::optional<SemanticType> resultReferent;
        if (dynamicStringViewCarrier) {
          resultReferent = SemanticType::StringView;
        } else if (instance.returnType.kind == SemanticType::Reference &&
                   instance.returnType.arguments.size() == 1) {
          resultReferent = instance.returnType.arguments.front();
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
            (!dynamicStringViewCarrier &&
             *resultReferent !=
                 storedCarrierReference->reference.arguments.front())) {
          return invalidSummary(
              "stored-carrier return dependency type does not match its "
              "exact external referent type");
        }

        const HirBindingId parameterBinding =
            instance.parameterBindings[instance.returnBorrowParameter];
        for (const MirLoan &loan : body.loans) {
          const MirPlace *source = body.findPlace(loan.source);
          const bool exactEntry =
              loan.kind == MirLoanKind::Stored && loan.entry && !loan.escapes &&
              loan.access == instance.returnBorrowAccess && source != nullptr &&
              source->root == MirPlaceRootKind::Binding &&
              source->projections.empty() &&
              source->binding == parameterBinding &&
              loan.designatedField == (dynamicStringViewCarrier
                                           ? 0
                                           : storedCarrierReference->field) &&
              std::find(loan.carriers.begin(), loan.carriers.end(),
                        parameterBinding) != loan.carriers.end();
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
    const std::optional<MirPlaceId> canonicalSource =
        borrowSourceForPlace(body, loan->source, 0);
    const MirPlace *source =
        canonicalSource ? body.findPlace(*canonicalSource) : nullptr;
    if (source == nullptr) {
      return verificationFailure(
          body, instance.id,
          "owner-dependent return loan has no source identity", block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Receiver &&
        source->root != MirPlaceRootKind::This) {
      return verificationFailure(
          body, instance.id,
          "receiver-dependent return does not originate from this", block.id);
    }
    if (instance.returnBorrowOrigin == BorrowOriginKind::Argument &&
        (instance.returnBorrowParameter >= instance.parameterBindings.size() ||
         source->root != MirPlaceRootKind::Binding ||
         source->binding !=
             instance.parameterBindings[instance.returnBorrowParameter])) {
      return verificationFailure(
          body, instance.id,
          "argument-dependent return does not originate from the "
          "summarized formal parameter",
          block.id);
    }
    if (storedCarrierReference || dynamicStringViewCarrier) {
      const SemanticType expectedPayload =
          instance.returnType.kind == SemanticType::Reference
              ? instance.returnType.arguments.front()
              : instance.returnType;
      const MirInstruction *transfer = nullptr;
      for (const MirBlock &candidateBlock : body.blocks) {
        for (const MirInstruction &instruction : candidateBlock.instructions) {
          if (instruction.loan != loan->id) {
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
          loan->source == storedCarrierEntryLoan->source && loan->parent == 0 &&
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
      if (instance.returnType.kind == SemanticType::Reference) {
        const MirOperand &published = transfer->operands.front();
        const MirPlace *runtime = body.findPlace(published.place);
        const bool exactDesignatedReferent =
            published.value == 0 && runtime != nullptr &&
            runtime->root == MirPlaceRootKind::Binding &&
            std::find(storedCarrierEntryLoan->carriers.begin(),
                      storedCarrierEntryLoan->carriers.end(),
                      runtime->binding) !=
                storedCarrierEntryLoan->carriers.end() &&
            runtime->projections.size() == 2 &&
            runtime->projections[0].kind == MirProjectionKind::Field &&
            runtime->projections[0].field ==
                storedCarrierEntryLoan->designatedField &&
            runtime->projections[1].kind == MirProjectionKind::Dereference &&
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
      } else {
        const MirOperand &published = transfer->operands.front();
        const bool exactCarrier =
            published.place == 0 && published.value != 0 &&
            block.terminator.value->kind == MirOperandKind::Value &&
            block.terminator.value->value == published.value &&
            exactStoredCarrierValueTransfer(body, published.value,
                                            *storedCarrierEntryLoan,
                                            instance.returnType, *transfer);
        if (!exactCarrier) {
          return verificationFailure(
              body, instance.id,
              "stored-carrier value return does not transfer one "
              "exact runtime carrier of its entry dependency",
              block.id);
        }
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
  case BorrowOriginKind::Argument: {
    if (instance.borrowParameter >= instance.parameterTypes.size()) {
      return invalidSummary(
          "constructor borrow dependency is outside the formal parameters");
    }
    const SemanticType &borrowParameter =
        instance.parameterTypes[instance.borrowParameter];
    const MirClassInstance *constructedOwner =
        program.findClassInstance(instance.owner);
    const bool mutexGuardInteriorBorrow = [&] {
      if (constructedOwner == nullptr ||
          !constructedOwner->lifecycleBoundBorrowCarrier ||
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
    if (instance.parameterBindings[instance.borrowParameter] == 0 ||
        borrowParameter.kind != SemanticType::Reference ||
        (borrowParameter.referenceAccess != instance.borrowAccess &&
         !mutexGuardInteriorBorrow)) {
      return invalidSummary(
          "constructor borrow dependency does not identify an exact "
          "reference formal parameter");
    }
    break;
  }
  case BorrowOriginKind::Global:
    return invalidSummary("constructor result cannot depend on global storage");
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
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop) {
        continue;
      }
      const MirDropObligation *obligation =
          body.findDropObligation(instruction.lifecycle.front().source);
      const MirDestructorInstance *destructor =
          obligation != nullptr && obligation->dropType.destructor
              ? program.findDestructorInstance(*obligation->dropType.destructor)
              : nullptr;
      const FailurePropagationKind expected =
          destructor != nullptr && destructor->mayRaiseDefinedFailure &&
                  !mirDropFollowsDefiniteMove(block, obligation->id,
                                              instruction.id)
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
  std::unordered_set<EnumId> declarations;
  const auto typeRequiresActiveCleanup =
      [&](const SemanticType &type, const auto &query) -> std::optional<bool> {
    switch (type.kind) {
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
    case SemanticType::PrefixStorage:
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
      const MirPayloadEnum *enumeration = program.findPayloadEnum(type.enumId);
      return enumeration == nullptr
                 ? std::optional<bool>{false}
                 : std::optional<bool>{enumeration->requiresActiveCleanup};
    }
    case SemanticType::Lambda: {
      const MirLambdaInstance *lambda =
          type.lambdaId == 0 ? nullptr : program.findLambda(type.lambdaId);
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
                 declarations.insert(enumeration.declaration).second &&
                 enumeration.type.kind == SemanticType::Enum &&
                 enumeration.type.enumId == enumeration.declaration &&
                 !enumeration.variants.empty();
    bool active = false;
    for (std::size_t variantIndex = 0;
         variantIndex < enumeration.variants.size(); ++variantIndex) {
      const MirPayloadEnumVariant &variant = enumeration.variants[variantIndex];
      exact = exact && variant.variantIndex == variantIndex &&
              variant.payloadTypes.size() ==
                  variant.payloadRequiresActiveCleanup.size();
      for (std::size_t fieldIndex = 0; fieldIndex < variant.payloadTypes.size();
           ++fieldIndex) {
        const SemanticType &field = variant.payloadTypes[fieldIndex];
        const std::optional<bool> fieldActive =
            typeRequiresActiveCleanup(field, typeRequiresActiveCleanup);
        exact =
            exact && field != SemanticType::Unknown &&
            field != SemanticType::Void && fieldActive &&
            *fieldActive == variant.payloadRequiresActiveCleanup[fieldIndex];
        // L-SUM-01 deliberately admits only concrete class values as active
        // payload fields. Recursive cleanup carriers need their own
        // partial-state representation before they can be forged into MIR.
        exact = exact && (!variant.payloadRequiresActiveCleanup[fieldIndex] ||
                          field.kind == SemanticType::Class);
        active = active || variant.payloadRequiresActiveCleanup[fieldIndex];
      }
    }
    exact = exact && active == enumeration.requiresActiveCleanup &&
            (!active || enumeration.traits.drop == DropKind::Lexical);
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
                ? program.findPayloadEnum(*instruction.enumOwner)
                : nullptr;
        const MirPayloadEnumVariant *variant =
            enumeration != nullptr && instruction.enumVariant &&
                    *instruction.enumVariant < enumeration->variants.size()
                ? &enumeration->variants[*instruction.enumVariant]
                : nullptr;
        bool exact = enumeration != nullptr && variant != nullptr &&
                     variant->variantIndex == *instruction.enumVariant;
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

[[nodiscard]] MirVerificationResult verifyMirCheckedIntegerFailureContracts(
    const MirProgram &program, const MirBody &body, std::size_t owner) {
  const auto fullExpressionRoot = [&](const MirInstruction &instruction) {
    return std::any_of(
        body.fullExpressions.begin(), body.fullExpressions.end(),
        [&](const MirFullExpression &expression) {
          return std::find(expression.roots.begin(), expression.roots.end(),
                           instruction.hirValue) != expression.roots.end();
        });
  };
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      const MirCheckedIntegerFailureContract contract =
          checkedIntegerFailureContract(body, instruction);
      if (!contract.applicable) {
        continue;
      }
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

      std::vector<const MirFailureRecord *> records;
      for (const MirFailureRecord &record : body.failureRecords) {
        if (record.producerInstruction == instruction.id) {
          records.push_back(&record);
        }
      }
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
      const auto assignment =
          std::find_if(program.failureMetadata().assignments().begin(),
                       program.failureMetadata().assignments().end(),
                       [&](const FailureOriginAssignment &candidate) {
                         return candidate.sourceUnit == origin.sourceUnit &&
                                candidate.start == origin.start &&
                                candidate.end == origin.end;
                       });
      if (assignment == program.failureMetadata().assignments().end() ||
          assignment->line != origin.line || assignment->site != site ||
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
        const auto endpointCount = static_cast<std::size_t>(std::count_if(
            body.blocks.begin(), body.blocks.end(),
            [&](const MirBlock &candidate) {
              const bool exactKind =
                  body.kind == MirBodyKind::HostedStartup
                      ? candidate.terminator.kind ==
                            MirTerminatorKind::ContainFailure
                      : candidate.terminator.kind ==
                            MirTerminatorKind::PropagateFailure;
              return exactKind && candidate.activeFailure == record.id &&
                     candidate.terminator.failureRecord == record.id;
            }));
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
    const std::vector<HirBindingId> &parameterBindings) {
  if (body.kind != kind || body.returnType != returnType || body.entry != 1 ||
      body.blocks.size() != 1 || body.places.size() != parameterTypes.size() ||
      parameterBindings.size() != parameterTypes.size() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }

  MirBlock expectedBlock;
  expectedBlock.id = 1;
  expectedBlock.terminator.kind = returnType == SemanticType::Void
                                      ? MirTerminatorKind::Return
                                      : MirTerminatorKind::Unreachable;
  expectedBlock.reachable = true;
  if (body.blocks.front() != expectedBlock) {
    return false;
  }

  for (std::size_t index = 0; index < body.places.size(); ++index) {
    const MirPlace &place = body.places[index];
    const PlaceKey expectedKey{.domain = body.placeDomain,
                               .root = place.symbol};
    if (place.id != index + 1 || place.root != MirPlaceRootKind::Binding ||
        parameterBindings[index] == 0 ||
        place.binding != parameterBindings[index] || place.symbol == 0 ||
        place.capture != 0 || place.temporary != 0 || place.value != 0 ||
        place.loan != 0 || !place.projections.empty() ||
        place.type != parameterTypes[index] || place.sourceValue != 0 ||
        !place.key || *place.key != expectedKey || !place.initiallyAvailable) {
      return false;
    }
  }
  return true;
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
    const MirDestructorInstance *destructor =
        drop != nullptr && drop->dropType.destructor
            ? program.findDestructorInstance(*drop->dropType.destructor)
            : nullptr;
    return destructor != nullptr && destructor->mayRaiseDefinedFailure;
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
        vectorConstructor->parameterTypes.empty() &&
        vectorConstructor->borrowOrigin == BorrowOriginKind::None &&
        vectorConstructor->borrowParameter == 0 &&
        vectorConstructor->borrowAccess == AccessMode::ReadOnly &&
        vectorConstructor->definitionKind == MirDefinitionKind::Source &&
        exactConstructorFailureSummary(vectorConstructor) &&
        stringConstructor->owner == stringClass->id &&
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
    const MirDropType vectorDropType{
        .type = vectorType,
        .classInstance = vectorClass == nullptr
                             ? std::nullopt
                             : std::optional{vectorClass->id},
        .destructor =
            vectorClass == nullptr ? std::nullopt : vectorClass->destructor,
        .requiresActiveCleanup =
            vectorClass != nullptr && vectorClass->requiresActiveCleanup};
    const MirDropType stringDropType{
        .type = stringType,
        .classInstance = stringClass == nullptr
                             ? std::nullopt
                             : std::optional{stringClass->id},
        .destructor =
            stringClass == nullptr ? std::nullopt : stringClass->destructor,
        .requiresActiveCleanup =
            stringClass != nullptr && stringClass->requiresActiveCleanup};

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
                 instance->type.arguments.size() == 1 &&
                 instance->compilerCapability == capability
             ? instance
             : nullptr;
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
exactScalarUniqueAllocationContract(const MirProgram &program,
                                    const MirInstruction &instruction) {
  if (instruction.info.type.kind != SemanticType::UniqueOwner ||
      instruction.info.type.arguments.size() != 1) {
    return false;
  }
  const SemanticType &pointee = instruction.info.type.arguments.front();
  if (pointee.kind == SemanticType::Class) {
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
    if (pointee.enumId == 0 || program.findPayloadEnum(pointee.enumId)) {
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
      !instruction.lifecycle.empty() ||
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
          *producer->result != operand.value) {
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
    return instruction.lifecycle.empty() && instruction.operands.size() == 1 &&
           !instruction.constructorTarget &&
           exactResult(SemanticType::Void, ValueCategory::Value,
                       AccessMode::ReadOnly) &&
           noLoanResult() && noFailure();
  case IntrinsicKind::OptionalStorageConstruct:
    return exactResult(SemanticType::Void, ValueCategory::Value,
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
  const auto producerFor = [&](MirLoanId loan) {
    std::optional<InstructionLocation> result;
    for (const auto &[_, location] : locations) {
      if (location.instruction->loan != loan ||
          (location.instruction->kind != MirInstructionKind::Call &&
           location.instruction->kind != MirInstructionKind::Construct &&
           location.instruction->kind != MirInstructionKind::Borrow)) {
        continue;
      }
      if (result) {
        return std::optional<InstructionLocation>{};
      }
      result = location;
    }
    return result;
  };

  for (const MirLoan &loan : body.loans) {
    if (!loan.lifecycleBound) {
      continue;
    }
    const std::optional<InstructionLocation> producer = producerFor(loan.id);
    const MirPlace *source = body.findPlace(loan.source);
    if (!producer || source == nullptr || loan.access != AccessMode::Mutable ||
        loan.parent != 0 || loan.twoPhase || loan.producedBy == 0 ||
        producer->instruction->hirValue != loan.producedBy ||
        producer->instruction->info.access != AccessMode::ReadOnly ||
        carrierClass(producer->instruction->info.type) == nullptr) {
      return verificationFailure(
          body, owner,
          "lifecycle-bound loan is not produced by one exact movable mutex "
          "guard carrier",
          producer ? producer->block->id : 0,
          producer ? producer->instruction->id : 0);
    }

    if (loan.kind == MirLoanKind::Return) {
      const bool exactReturnProducer =
          loan.escapes && !loan.entry && loan.carriers.empty() &&
          loan.storedField == 0 && loan.designatedField == 0 &&
          producer->instruction->kind == MirInstructionKind::Borrow &&
          !producer->instruction->result &&
          producer->instruction->operands.size() == 1 &&
          producer->instruction->operands.front().kind == MirOperandKind::Loan;
      const MirLoan *sourceLoan =
          exactReturnProducer
              ? body.findLoan(producer->instruction->operands.front().loan)
              : nullptr;
      std::size_t returns = 0;
      for (const MirBlock &block : body.blocks) {
        if (block.terminator.kind == MirTerminatorKind::Return &&
            block.terminator.returnLoan == loan.id && block.terminator.value &&
            block.terminator.value->type == producer->instruction->info.type) {
          ++returns;
        }
      }
      if (!exactReturnProducer || sourceLoan == nullptr ||
          sourceLoan->kind != MirLoanKind::Stored ||
          !sourceLoan->lifecycleBound || sourceLoan->source != loan.source ||
          sourceLoan->access != loan.access || returns != 1) {
        return verificationFailure(
            body, owner,
            "lifecycle-bound return loan is not the exact escaping transfer "
            "of one stored mutex-guard dependency",
            producer->block->id, producer->instruction->id);
      }
      continue;
    }

    const bool exactStored =
        loan.kind == MirLoanKind::Stored && !loan.entry && !loan.escapes &&
        loan.storedField == 0 && loan.designatedField == 0 &&
        (producer->instruction->kind == MirInstructionKind::Call ||
         producer->instruction->kind == MirInstructionKind::Construct) &&
        producer->instruction->result &&
        producer->instruction->borrowAccess == AccessMode::Mutable &&
        (producer->instruction->borrowOrigin == BorrowOriginKind::Receiver ||
         producer->instruction->borrowOrigin == BorrowOriginKind::Argument);
    if (!exactStored) {
      return verificationFailure(
          body, owner,
          "lifecycle-bound stored loan does not retain the exact local "
          "mutex-guard result shape",
          producer->block->id, producer->instruction->id);
    }

    std::unordered_set<MirDropObligationId> carrierObligations;
    const auto seed = [&](MirDropObligationId obligation) {
      const MirDropObligation *drop = body.findDropObligation(obligation);
      if (drop != nullptr &&
          drop->dropType.type == producer->instruction->info.type) {
        carrierObligations.insert(obligation);
      }
    };
    if (producer->instruction->successResultDrop) {
      seed(*producer->instruction->successResultDrop);
    }
    for (const MirLifecycleEvent &event : producer->instruction->lifecycle) {
      if (event.kind == MirLifecycleEventKind::Initialize) {
        seed(event.target);
      }
    }
    for (const MirBlock &block : body.blocks) {
      if (block.terminator.kind != MirTerminatorKind::Invoke ||
          block.terminator.invokeInstruction != producer->instruction->id) {
        continue;
      }
      for (const MirLifecycleEvent &event : block.terminator.successLifecycle) {
        if (event.kind == MirLifecycleEventKind::Initialize) {
          seed(event.target);
        }
      }
    }
    for (bool changed = true; changed;) {
      changed = false;
      const auto follow = [&](const std::vector<MirLifecycleEvent> &events) {
        for (const MirLifecycleEvent &event : events) {
          if ((event.kind == MirLifecycleEventKind::Move ||
               event.kind == MirLifecycleEventKind::Reparent ||
               event.kind == MirLifecycleEventKind::Replace) &&
              carrierObligations.contains(event.source)) {
            const std::size_t before = carrierObligations.size();
            seed(event.target);
            changed = changed || carrierObligations.size() != before;
          }
        }
      };
      for (const MirBlock &block : body.blocks) {
        for (const MirInstruction &instruction : block.instructions) {
          follow(instruction.lifecycle);
        }
        follow(block.terminator.successLifecycle);
      }
    }
    if (carrierObligations.empty()) {
      return verificationFailure(
          body, owner,
          "lifecycle-bound mutex guard has no exact drop obligation",
          producer->block->id, producer->instruction->id);
    }

    std::vector<InstructionLocation> carrierDrops;
    for (const auto &[_, location] : locations) {
      if (location.instruction->kind != MirInstructionKind::Drop ||
          location.instruction->info.type != producer->instruction->info.type) {
        continue;
      }
      if (std::any_of(location.instruction->lifecycle.begin(),
                      location.instruction->lifecycle.end(),
                      [&](const MirLifecycleEvent &event) {
                        return event.kind == MirLifecycleEventKind::Drop &&
                               carrierObligations.contains(event.source);
                      })) {
        carrierDrops.push_back(location);
      }
    }

    std::vector<InstructionLocation> ends;
    for (const auto &[_, location] : locations) {
      if (location.instruction->kind == MirInstructionKind::EndBorrow &&
          location.instruction->loan == loan.id) {
        ends.push_back(location);
      }
    }
    if (ends.empty()) {
      return verificationFailure(
          body, owner,
          "local lifecycle-bound mutex guard loan has no explicit end",
          producer->block->id, producer->instruction->id);
    }
    for (const InstructionLocation &end : ends) {
      const bool producerFailure = std::any_of(
          body.failureRecords.begin(), body.failureRecords.end(),
          [&](const MirFailureRecord &record) {
            return record.producerBlock == producer->block->id &&
                   record.producerInstruction == producer->instruction->id &&
                   record.parameterBlock == end.block->id;
          });
      const bool afterCarrierDrop = std::any_of(
          carrierDrops.begin(), carrierDrops.end(),
          [&](const InstructionLocation &drop) { return precedes(drop, end); });
      const bool returned = std::any_of(
          end.block->instructions.begin(),
          end.block->instructions.begin() +
              static_cast<std::ptrdiff_t>(end.order),
          [&](const MirInstruction &candidate) {
            if (candidate.kind != MirInstructionKind::Borrow ||
                !candidate.loan || candidate.operands.size() != 1 ||
                candidate.operands.front().kind != MirOperandKind::Loan ||
                candidate.operands.front().loan != loan.id) {
              return false;
            }
            const MirLoan *transferred = body.findLoan(*candidate.loan);
            return transferred != nullptr && transferred->lifecycleBound &&
                   transferred->kind == MirLoanKind::Return &&
                   end.block->terminator.kind == MirTerminatorKind::Return &&
                   end.block->terminator.returnLoan == transferred->id;
          });
      if (!producerFailure && !afterCarrierDrop && !returned) {
        return verificationFailure(
            body, owner,
            "lifecycle-bound mutex guard loan ends before guard drop or its "
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

  const auto verifyBody = [&](const MirBody &body, std::size_t owner) {
    appendVerification(
        result, verifyMirCheckedIntegerFailureContracts(program, body, owner));
    appendVerification(result,
                       verifyLifecycleBoundLoanContracts(program, body, owner));
    for (const MirPlace &place : body.places) {
      const bool classPackElement =
          place.type.kind == SemanticType::Class &&
          place.projections.size() == 1 &&
          place.projections.front().kind == MirProjectionKind::PackElement;
      if (!classPackElement) {
        continue;
      }
      const auto instance = std::find_if(
          program.classInstances().begin(), program.classInstances().end(),
          [&](const MirClassInstance &candidate) {
            return candidate.type == place.type;
          });
      if (instance == program.classInstances().end() ||
          place.traits != instance->traits) {
        result.errors.push_back(
            {.bodyKind = body.kind,
             .owner = owner,
             .message = "concrete class pack element traits do not match its "
                        "class instance"});
      }
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        const bool atomicIntrinsic =
            instruction.intrinsic == IntrinsicKind::AtomicLoad ||
            instruction.intrinsic == IntrinsicKind::AtomicStore ||
            instruction.intrinsic == IntrinsicKind::AtomicExchange ||
            instruction.intrinsic == IntrinsicKind::AtomicCompareExchange;
        if (atomicIntrinsic &&
            !exactAtomicSynchronization(instruction.intrinsic,
                                        instruction.synchronization)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message = "atomic intrinsic does not retain its exact "
                          "operation-specific synchronization contract"});
        }
        if (instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
            !exactScalarUniqueAllocationContract(program, instruction)) {
          result.errors.push_back(
              {.bodyKind = body.kind,
               .owner = owner,
               .block = block.id,
               .instruction = instruction.id,
               .message =
                   "scalar unique allocation does not retain its exact "
                   "pointee, initializer, result, and failure contract"});
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
          if (target == nullptr || target->owner ||
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
          if (adapter == nullptr ||
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
            (instruction.synchronization.kind ==
                 SynchronizationOperationKind::ThreadSpawn &&
             instruction.intrinsic == IntrinsicKind::ThreadSpawn) ||
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
        program.executionProfile() != ExecutionProfile::SingleThreaded) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = adapter.target,
           .message = "native callback adapter contract is invalid"});
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
    if (spawn != nullptr && spawn->operands.size() == 2 &&
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
          exactEntry && target != nullptr &&
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
    if (adapter.id != index + 1 || adapter.taskType == SemanticType::Unknown ||
        adapter.resultType == SemanticType::Unknown ||
        adapter.resultType != spawnResult || adapter.spawnValue == 0 ||
        adapter.spawnInstruction == 0 ||
        adapter.invocation != CallableInvocationCapability::Once ||
        !adapter.catchesWorkerFailure || !adapter.catchesNativeExceptions ||
        !exactEntry || body == nullptr || spawn == nullptr ||
        spawn->hirValue != adapter.spawnValue ||
        spawn->intrinsic != IntrinsicKind::ThreadSpawn ||
        spawn->threadTaskAdapter != adapter.id ||
        spawn->synchronization.kind !=
            SynchronizationOperationKind::ThreadSpawn ||
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
    case SemanticType::Storage:
    case SemanticType::PrefixStorage:
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
      const MirPayloadEnum *enumeration = program.findPayloadEnum(type.enumId);
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
  appendVerification(result, verifyMirPayloadEnums(program));
  for (std::size_t index = 0; index < program.classInstances().size();
       ++index) {
    const MirClassInstance &instance = program.classInstances()[index];
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
    if (instance.requiresActiveDropState != instance.destructor.has_value()) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message = "class active-drop state does not match its destructor"});
    }
    if (instance.requiresActiveDropState && !instance.requiresActiveCleanup) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::FieldInitializers,
           .owner = instance.id,
           .message =
               "class active-drop state is missing active-cleanup metadata"});
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
        CompilerCapabilityTypeKind::OptionalStorage) {
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
    if (instance.lifecycleBoundBorrowCarrier) {
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
          field.type != SemanticType::Unknown && fieldRequiresActiveCleanup &&
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
  std::size_t entryPoints = 0;
  for (std::size_t index = 0; index < program.functionInstances().size();
       ++index) {
    const MirFunctionInstance &instance = program.functionInstances()[index];
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
            instance.parameterTypes, instance.parameterBindings)) {
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
    const bool nestedPointerReturn =
        instance.returnType.kind == SemanticType::RawPointer &&
        instance.returnType.arguments.size() == 1 &&
        instance.returnType.arguments.front().kind == SemanticType::RawPointer;
    if (nestedPointerReturn && !instance.cArrayCountParameter) {
      result.errors.push_back(
          {.bodyKind = MirBodyKind::Function,
           .owner = instance.id,
           .message = "two-level pointer return lacks c_array metadata"});
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
                 parameter.access != AccessMode::ReadOnly) {
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
        const bool hasTarget = signature.functionTarget.has_value() !=
                               signature.lambdaTarget.has_value();
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
          const bool functionItem =
              parameter.callableType.hasFunctionItemShape();
          if ((functionItem
                   ? target.owner.has_value() ||
                         target.overloadedOperator.has_value() ||
                         parameter.callableType.functionId != target.declaration
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
              target->type != parameter.callableType) {
            rejectCallableContract(prefix +
                                   "does not match an exact lambda target");
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
        if (forwarding.parameterIndex >= target.parameterTypes.size() ||
            targetContract == target.callableParameters.end() ||
            targetContract->boundary != CallableBoundary::Confined ||
            target.parameterTypes[forwarding.parameterIndex] !=
                parameter.callableType) {
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
    for (const MirConstructorInitializer &initializer : instance.initializers) {
      if (initializer.kind == ConstructorInitializerTargetKind::Field &&
          initializer.field != 0) {
        explicitlyInitializedFields.insert(initializer.field);
      }
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
    const auto scalarInitializerType = [](const SemanticType &type) {
      switch (type.kind) {
      case SemanticType::Int8:
      case SemanticType::Int16:
      case SemanticType::Int32:
      case SemanticType::Int64:
      case SemanticType::UInt8:
      case SemanticType::UInt16:
      case SemanticType::UInt32:
      case SemanticType::UInt64:
      case SemanticType::Bool:
      case SemanticType::Char:
        return true;
      default:
        return false;
      }
    };
    std::vector<std::size_t> initializerStageCounts(
        instance.initializers.size());
    std::vector<std::size_t> initializerBoundaryCounts(
        instance.initializers.size());
    bool exactInitializerStages = constructorOwner != nullptr;
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
            instruction.operands.front().type == initializer.targetType &&
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
      const bool explicitScalarField =
          constructorOwner != nullptr &&
          field != constructorOwner->declaredFields.end() &&
          initializer.kind == ConstructorInitializerTargetKind::Field &&
          !initializer.storesReference && !initializer.constructorTarget &&
          !initializer.generatedDefault && initializer.arguments.size() == 1 &&
          field->type == initializer.targetType &&
          scalarInitializerType(initializer.targetType) &&
          field->dropKind == DropKind::Trivial && !field->requiresActiveCleanup;
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
      // bounded scalar form requires a matching Initialize stage.
      if ((explicitScalarField &&
           (initializerStageCounts[initializerIndex] != 1 ||
            initializerBoundaryCounts[initializerIndex] != 1)) ||
          (!explicitScalarField && explicitOwningField &&
           (initializerStageCounts[initializerIndex] > 1 ||
            initializerBoundaryCounts[initializerIndex] > 1)) ||
          (!explicitScalarField && !explicitOwningField &&
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
                      "their scalar or owning field initializers"});
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
      for (std::size_t capture = 0; capture < instance.captureTypes.size();
           ++capture) {
        if ((instance.captureModes[capture] != LambdaCaptureMode::Copy &&
             instance.captureModes[capture] != LambdaCaptureMode::Move) ||
            instance.captureSymbols[capture] == 0 ||
            !exactCaptureSymbols.insert(instance.captureSymbols[capture])
                 .second) {
          result.errors.push_back(
              {.bodyKind = MirBodyKind::Lambda,
               .owner = instance.id,
               .message = "lambda capture mode or symbol identity is "
                          "invalid or duplicated"});
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
              place.access != AccessMode::ReadOnly))) {
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
  }
  appendVerification(result, verifyMirProgramContracts(program));
  appendVerification(result, verifyMirEffectSummaries(program));
  return result;
}

} // namespace lang
