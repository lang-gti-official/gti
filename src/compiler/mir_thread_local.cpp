#include "mir_thread_local.h"

#include "gti/mir_dominance.h"
#include "gti/mir_effects.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace lang {
namespace {

bool exactDataInitializer(const MirThreadLocalStorage &storage) {
  if (!storage.dataInitializer || storage.requiresActiveCleanup ||
      storage.traits.containsBorrowedState ||
      storage.traits.drop != DropKind::Trivial)
    return false;
  const MirBody &body = storage.initializer;
  const MirTerminator *returned = nullptr;
  std::unordered_map<MirValueId, const MirInstruction *> definitions;
  for (const auto &block : body.blocks) {
    if (block.terminator.kind != MirTerminatorKind::Return || returned)
      return false;
    returned = &block.terminator;
    for (const auto &instruction : block.instructions) {
      if (instruction.tombstone)
        continue;
      // Full-expression endpoints carry only checked proof metadata. They do
      // not turn a literal initializer into executable native initialization.
      if (instruction.kind == MirInstructionKind::Lifecycle &&
          instruction.fullExpressionEnd != 0 &&
          instruction.cleanupBoundaryEnd == 0 &&
          instruction.lifecycle.empty() && instruction.definedFailure.empty())
        continue;
      if (instruction.kind != MirInstructionKind::Compute ||
          !instruction.result || !instruction.definedFailure.empty() ||
          !instruction.lifecycle.empty() ||
          (instruction.operation != MirOperation::Literal &&
           instruction.operation != MirOperation::Convert &&
           instruction.operation != MirOperation::Identity))
        return false;
      definitions.emplace(*instruction.result, &instruction);
    }
  }
  if (!returned || !returned->value ||
      returned->value->kind != MirOperandKind::Value ||
      returned->value->type != storage.type)
    return false;
  MirValueId value = returned->value->value;
  std::unordered_set<MirValueId> seen;
  while (seen.insert(value).second) {
    const auto found = definitions.find(value);
    if (found == definitions.end())
      return false;
    const auto &instruction = *found->second;
    if (instruction.operation == MirOperation::Literal)
      return instruction.literal == storage.dataInitializer;
    if (instruction.operands.size() != 1 ||
        instruction.operands.front().kind != MirOperandKind::Value)
      return false;
    value = instruction.operands.front().value;
  }
  return false;
}

} // namespace

MirVerificationResult verifyMirThreadLocalStorage(const MirProgram &program) {
  MirVerificationResult result;
  const auto add = [&](MirBodyAddress address, MirBlockId block,
                       MirInstructionId instruction, std::string message) {
    result.errors.push_back({.bodyKind = address.kind,
                             .owner = address.owner,
                             .block = block,
                             .instruction = instruction,
                             .message = std::move(message)});
  };
  std::unordered_map<SymbolId, const MirThreadLocalStorage *> inventory;
  for (const auto &storage : program.threadLocalStorage()) {
    const MirBodyAddress address{.kind = MirBodyKind::ThreadLocalInitializer,
                                 .owner = storage.symbol};
    bool borrowedRepresentation = storage.type.kind == SemanticType::Reference;
    for (const auto &instance : program.classInstances())
      if (instance.type == storage.type)
        borrowedRepresentation |= instance.traits.containsBorrowedState;
    if (const auto *enumeration = program.findPayloadEnum(storage.type))
      borrowedRepresentation |= enumeration->traits.containsBorrowedState;
    if ((storage.type.kind == SemanticType::StringView ||
         storage.type.kind == SemanticType::CString) &&
        !storage.dataInitializer)
      borrowedRepresentation = true;
    if (!storage.symbol ||
        !inventory.emplace(storage.symbol, &storage).second ||
        program.findForeignStorage(storage.symbol) != nullptr ||
        storage.type == SemanticType::Unknown ||
        storage.type == SemanticType::Void ||
        storage.type.kind == SemanticType::Reference ||
        storage.traits.containsBorrowedState || borrowedRepresentation ||
        storage.initializer.kind != address.kind ||
        storage.initializer.returnType != storage.type) {
      add(address, 0, 0,
          "thread-local inventory has no exact owning initializer");
    }
    auto cleanup = mirUniqueCleanupType(program, storage.type);
    if (!cleanup)
      cleanup = MirDropType{.type = storage.type};
    if (*cleanup != storage.cleanup ||
        storage.cleanup.requiresActiveCleanup != storage.requiresActiveCleanup)
      add(address, 0, 0,
          "thread-local cleanup differs from its exact type recipe");
    if (storage.dataInitializer && !exactDataInitializer(storage))
      add(address, 0, 0,
          "thread-local data initializer is not proved by its MIR");
    for (const auto &step : program.programInitializationPlan().steps)
      if (step.symbol == storage.symbol)
        add(address, 0, 0,
            "thread-local storage appears in eager initialization");
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (!body)
      continue;
    const auto dominance = computeMirDominance(*body);
    if (!dominance)
      continue; // Structural verification reports the unusable CFG.
    for (const auto &place : body->places) {
      if (place.root != MirPlaceRootKind::Symbol || place.capture != 0)
        continue;
      const auto found = inventory.find(place.symbol);
      if (found == inventory.end())
        continue;
      const auto &storage = *found->second;
      if (place.binding != 0 || place.temporary != 0 || place.value != 0 ||
          place.loan != 0 || place.foreignStorage ||
          (place.key && place.key->root != storage.symbol) ||
          (place.projections.empty() &&
           (place.type != storage.type || place.traits != storage.traits ||
            (storage.access == AccessMode::ReadOnly &&
             place.access == AccessMode::Mutable))))
        add(address, 0, 0,
            "thread-local place diverges from its exact storage identity or "
            "type");
    }
    struct Guard {
      MirBlockId block = 0;
      std::size_t position = 0;
      bool invokes = false;
    };
    std::unordered_map<SymbolId, std::vector<Guard>> guards;
    for (const auto &block : body->blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &instruction = block.instructions[index];
        if (instruction.tombstone ||
            instruction.kind != MirInstructionKind::EnsureThreadLocal)
          continue;
        const auto found = inventory.find(instruction.threadLocalStorage);
        if (found == inventory.end() || !instruction.bodyTarget ||
            *instruction.bodyTarget !=
                MirBodyAddress{.kind = MirBodyKind::ThreadLocalInitializer,
                               .owner = instruction.threadLocalStorage}) {
          add(address, block.id, instruction.id,
              "thread-local guard has no exact initializer target");
          continue;
        }
        const bool data = found->second->dataInitializer.has_value();
        if (data ? !instruction.definedFailure.empty()
                 : (instruction.definedFailure.propagation !=
                        FailurePropagationKind::BodyCall ||
                    block.terminator.kind != MirTerminatorKind::Invoke ||
                    block.terminator.invokeInstruction != instruction.id)) {
          add(address, block.id, instruction.id,
              "thread-local guard loses its checked first-use failure edge");
          continue;
        }
        if (!data) {
          const auto admits = [&](DefinedFailureDetail detail) {
            return std::any_of(
                instruction.definedFailure.localOrigins.begin(),
                instruction.definedFailure.localOrigins.end(),
                [&](const DefinedFailureOrigin &origin) {
                  return std::any_of(
                      origin.outcomes.begin(), origin.outcomes.end(),
                      [&](DefinedFailureOutcome outcome) {
                        return outcome.code ==
                                   DefinedFailureCode::
                                       HostedRuntimeContractFailure &&
                               outcome.detail == detail;
                      });
                });
          };
          if (!admits(
                  DefinedFailureDetail::RecursiveThreadLocalInitialization) ||
              !admits(
                  DefinedFailureDetail::ThreadLocalOutsideAttachedLifetime) ||
              !admits(DefinedFailureDetail::
                          ThreadLocalInitializationDuringShutdown))
            add(address, block.id, instruction.id,
                "thread-local guard omits initialization-state failure "
                "outcomes");
        }
        guards[instruction.threadLocalStorage].push_back(
            {.block = data ? block.id : block.terminator.target,
             .position = data ? index : 0,
             .invokes = !data});
      }
    }
    const auto check = [&](MirPlaceId id, MirBlockId block,
                           std::size_t position, MirInstructionId instruction) {
      const auto *place = body->findPlace(id);
      if (!place || place->root != MirPlaceRootKind::Symbol ||
          place->capture != 0 || !inventory.contains(place->symbol))
        return;
      const auto found = guards.find(place->symbol);
      const bool dominated =
          found != guards.end() &&
          std::any_of(found->second.begin(), found->second.end(),
                      [&](const Guard &guard) {
                        return dominance->dominates(guard.block, block) &&
                               (guard.block != block || guard.invokes ||
                                guard.position < position);
                      });
      if (!dominated)
        add(address, block, instruction,
            "thread-local access is not dominated by successful "
            "initialization");
    };
    const auto checkOperand = [&](const MirOperand &operand, MirBlockId block,
                                  std::size_t position,
                                  MirInstructionId instruction) {
      if (operand.place != 0)
        check(operand.place, block, position, instruction);
      if (operand.kind == MirOperandKind::Loan)
        if (const auto *loan = body->findLoan(operand.loan))
          check(loan->source, block, position, instruction);
    };
    for (const auto &block : body->blocks) {
      for (std::size_t index = 0; index < block.instructions.size(); ++index) {
        const auto &instruction = block.instructions[index];
        if (instruction.tombstone)
          continue;
        if ((instruction.kind == MirInstructionKind::DrainThreadLocals ||
             instruction.kind == MirInstructionKind::BeginThreadLocals) &&
            (address.kind != MirBodyKind::HostedStartup || inventory.empty()))
          add(address, block.id, instruction.id,
              "thread-local drain is outside its exact hosted shutdown "
              "schedule");
        if (instruction.destination)
          check(*instruction.destination, block.id, index, instruction.id);
        if (instruction.receiver)
          checkOperand(*instruction.receiver, block.id, index, instruction.id);
        for (const auto &operand : instruction.operands)
          checkOperand(operand, block.id, index, instruction.id);
      }
      if (block.terminator.value)
        checkOperand(*block.terminator.value, block.id,
                     block.instructions.size(), 0);
    }
  }
  return result;
}
} // namespace lang
