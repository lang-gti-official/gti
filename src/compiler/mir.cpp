#include "gti/mir.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/coverage.h"
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

bool isMirNativeIndirectCall(const MirProgram &program, const MirBody &body,
                             const MirInstruction &instruction) {
  if (!instruction.nativeCalleeType ||
      !instruction.nativeCalleeType->hasNativeFunctionShape())
    return false;
  const SemanticType &signature = *instruction.nativeCalleeType;
  const auto abiType = [&](const auto &self, const SemanticType &type,
                           bool result, std::size_t depth) -> bool {
    if (depth > 256)
      return false;
    switch (type.kind) {
    case SemanticType::Void:
      return result;
    case SemanticType::Bool:
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
    case SemanticType::CString:
      return true;
    case SemanticType::StringView:
      return !result;
    case SemanticType::RawPointer:
      if (type.arguments.size() != 1)
        return false;
      if (type.arguments.front().kind == SemanticType::Class) {
        return std::any_of(program.classInstances().begin(),
                           program.classInstances().end(), [&](const auto &c) {
                             return c.type == type.arguments.front() &&
                                    (c.cAbiRecord || c.cOpaqueHandle);
                           });
      }
      return self(self, type.arguments.front(), true, depth + 1);
    case SemanticType::Class:
      return std::any_of(program.classInstances().begin(),
                         program.classInstances().end(), [&](const auto &c) {
                           return c.type == type && c.cAbiRecord &&
                                  c.traits.copyable &&
                                  !c.traits.containsBorrowedState;
                         });
    case SemanticType::NativeFunction:
      if (!type.hasNativeFunctionShape() ||
          !self(self, *type.nativeFunctionReturnType(), true, depth + 1))
        return false;
      return std::all_of(
          type.nativeFunctionParameterTypes().begin(),
          type.nativeFunctionParameterTypes().end(),
          [&](const auto &p) { return self(self, p, false, depth + 1); });
    default:
      return false;
    }
  };
  const auto parameters = signature.nativeFunctionParameterTypes();
  if (!abiType(abiType, signature, false, 0) ||
      instruction.kind != MirInstructionKind::Call ||
      instruction.unsafeOperation != UnsafeOperationKind::ForeignPointerCall ||
      instruction.rawMemoryAccess || instruction.callSite == 0 ||
      instruction.callInputRole || instruction.destination ||
      !instruction.receiver || instruction.functionTarget ||
      instruction.constructorTarget || instruction.lambdaTarget ||
      instruction.bodyTarget || instruction.nativeCallbackAdapter ||
      instruction.intrinsic != IntrinsicKind::None ||
      instruction.dispatch != CallDispatch::Static ||
      instruction.dispatchOwner != SemanticType::Unknown ||
      instruction.callableInvocation != CallableInvocationCapability::Read ||
      !instruction.callableArguments.empty() || instruction.loan ||
      instruction.borrowOrigin != BorrowOriginKind::None ||
      !instruction.definedFailure.empty() ||
      !instruction.localFailureSites.empty() || instruction.neverReturns ||
      instruction.tracksCaller ||
      instruction.parameterTypes.size() != parameters.size() ||
      !std::equal(parameters.begin(), parameters.end(),
                  instruction.parameterTypes.begin()) ||
      instruction.operands.size() != parameters.size() ||
      instruction.info.type != *signature.nativeFunctionReturnType() ||
      (instruction.info.type == SemanticType::Void
           ? instruction.result.has_value()
           : !instruction.result))
    return false;
  const auto staged = [&](const MirOperand &operand, MirCallInputRole role,
                          std::size_t index, const SemanticType &type) -> bool {
    const auto *value = operand.kind == MirOperandKind::Value
                            ? body.findValue(operand.value)
                            : nullptr;
    if (!value || operand.type != type || value->info.type != type)
      return false;
    for (const auto &block : body.blocks)
      for (const auto &input : block.instructions)
        if (input.id == value->definition)
          return input.kind == MirInstructionKind::CallInput &&
                 input.callSite == instruction.callSite &&
                 input.callInputRole == role && input.callInputIndex == index &&
                 input.info.type == type &&
                 (role != MirCallInputRole::Receiver ||
                  (input.callInputKind == HirCallInputKind::Value &&
                   input.operands.size() == 1 &&
                   input.operands.front().type == type));
    return false;
  };
  if (!staged(*instruction.receiver, MirCallInputRole::Receiver, 0, signature))
    return false;
  for (std::size_t index = 0; index < parameters.size(); ++index)
    if (!staged(instruction.operands[index], MirCallInputRole::Argument, index,
                parameters[index]))
      return false;
  return true;
}

std::vector<MirPlaceId> mirLoanProtectedSources(const MirBody &body,
                                                MirLoanId id) {
  const MirLoan *loan = body.findLoan(id);
  if (loan == nullptr || loan->id != id)
    return {};
  std::vector<MirPlaceId> sources;
  const auto append = [&](MirPlaceId source) {
    if (body.findPlace(source) == nullptr)
      return false;
    if (std::find(sources.begin(), sources.end(), source) == sources.end())
      sources.push_back(source);
    return true;
  };
  const auto appendLoan = [&](const MirLoan &record) {
    return append(record.source) &&
           std::all_of(record.additionalDependencies.begin(),
                       record.additionalDependencies.end(),
                       [&](const auto &dependency) {
                         return dependency.source == 0 ||
                                append(dependency.source);
                       });
  };
  std::unordered_set<MirLoanId> visited;
  while (loan != nullptr) {
    if (!visited.insert(loan->id).second || !appendLoan(*loan))
      return {};
    const MirLoanId dependency = loan->storedDependency;
    if (dependency == 0)
      break;
    loan = body.findLoan(dependency);
    if (loan == nullptr || loan->id != dependency ||
        loan->kind != MirLoanKind::Stored)
      return {};
  }
  return sources;
}

const std::vector<MirValueUse> &MirBody::usesOf(MirValueId id) const {
  static const std::vector<MirValueUse> empty;
  return id == 0 || id > valueUses.size() ? empty : valueUses[id - 1];
}

bool mirCallInputAbandonedByTerminal(const MirBody &body,
                                     const MirInstruction &input) {
  if (input.kind != MirInstructionKind::CallInput || !input.result ||
      !body.usesOf(*input.result).empty())
    return false;
  const MirValue *value = body.findValue(*input.result);
  const MirBlock *start =
      value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
  if (start == nullptr || value->definition != input.id)
    return false;
  const auto position = std::find_if(
      start->instructions.begin(), start->instructions.end(),
      [&](const MirInstruction &candidate) { return &candidate == &input; });
  if (position == start->instructions.end())
    return false;
  std::unordered_set<MirBlockId> visiting;
  std::unordered_map<MirBlockId, bool> complete;
  const auto visit = [&](const auto &self, const MirBlock &block,
                         std::size_t begin) -> bool {
    if (begin == 0) {
      if (const auto found = complete.find(block.id); found != complete.end())
        return found->second;
    }
    if (!visiting.insert(block.id).second)
      return false;
    const auto finish = [&](bool result) {
      visiting.erase(block.id);
      if (begin == 0)
        complete.emplace(block.id, result);
      return result;
    };
    for (std::size_t index = begin; index < block.instructions.size();
         ++index) {
      const auto &instruction = block.instructions[index];
      if (instruction.tombstone)
        continue;
      // A later, unrelated statement's panic cannot excuse a lost call input.
      if (instruction.fullExpressionEnd != 0)
        return finish(false);
      if (instruction.kind == MirInstructionKind::Call &&
          instruction.neverReturns)
        return finish(true);
    }
    const auto edge = [&](MirBlockId id) {
      const auto *target = body.findBlock(id);
      return target != nullptr && self(self, *target, 0);
    };
    const auto &term = block.terminator;
    switch (term.kind) {
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Invoke:
      // Invoke's failure edge already owns cleanup; only its normal successor
      // could reach an invocation that consumes the prepared input.
      return finish(edge(term.target));
    case MirTerminatorKind::Branch:
      return finish(edge(term.target) && edge(term.elseTarget));
    case MirTerminatorKind::Switch:
      return finish(term.target != 0 && edge(term.target) &&
                    std::all_of(term.switchTargets.begin(),
                                term.switchTargets.end(),
                                [&](const MirSwitchTarget &target) {
                                  return edge(target.target);
                                }));
    default:
      // Neither a bare Unreachable nor a return is a terminal-call witness.
      return finish(false);
    }
  };
  return visit(
      visit, *start,
      static_cast<std::size_t>(position - start->instructions.begin()) + 1);
}

std::size_t MirBody::instructionCount() const {
  std::size_t result = 0;
  for (const MirBlock &block : blocks) {
    result += block.instructions.size();
  }
  return result;
}

std::size_t MirProgram::blockCount() const {
  std::size_t result = 0;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(*this)) {
    if (const MirBody *body = findMirBody(*this, address)) {
      result += body->blocks.size();
    }
  }
  return result;
}

std::vector<MirBodyAddress>
enumerateMirBodyAddresses(const MirProgram &program) {
  std::vector<MirBodyAddress> result;
  result.reserve(1 + program.classInstances().size() * 2 +
                 program.functionInstances().size() +
                 program.constructorInstances().size() +
                 program.destructorInstances().size() +
                 program.lambdaInstances().size() +
                 (program.hostedStartup() == nullptr ? 0 : 1));
  result.push_back({.kind = MirBodyKind::Module});
  for (const MirClassInstance &instance : program.classInstances()) {
    result.push_back(
        {.kind = MirBodyKind::FieldInitializers, .owner = instance.id});
    result.push_back(
        {.kind = MirBodyKind::StaticFieldInitializers, .owner = instance.id});
  }
  for (const MirFunctionInstance &instance : program.functionInstances()) {
    result.push_back({.kind = MirBodyKind::Function, .owner = instance.id});
  }
  for (const MirConstructorInstance &instance :
       program.constructorInstances()) {
    result.push_back({.kind = MirBodyKind::Constructor, .owner = instance.id});
  }
  for (const MirDestructorInstance &instance : program.destructorInstances()) {
    result.push_back({.kind = MirBodyKind::Destructor, .owner = instance.id});
  }
  for (const MirLambdaInstance &instance : program.lambdaInstances()) {
    result.push_back({.kind = MirBodyKind::Lambda, .owner = instance.id});
  }
  if (const std::optional<MirHostedStartupPlan> &hosted =
          program.hostedStartupPlan();
      hosted && program.hostedStartup() != nullptr) {
    result.push_back(
        {.kind = MirBodyKind::HostedStartup, .owner = hosted->entry});
  }
  return result;
}

const MirBody *findMirBody(const MirProgram &program, MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Module:
    return address.owner == 0 ? &program.module() : nullptr;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    if (const MirClassInstance *instance =
            program.findClassInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return address.kind == MirBodyKind::FieldInitializers
                 ? &instance->fieldInitializers
                 : &instance->staticFieldInitializers;
    }
    return nullptr;
  case MirBodyKind::Function:
    if (const MirFunctionInstance *instance =
            program.findFunctionInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Constructor:
    if (const MirConstructorInstance *instance =
            program.findConstructorInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Destructor:
    if (const MirDestructorInstance *instance =
            program.findDestructorInstance(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::Lambda:
    if (const MirLambdaInstance *instance = program.findLambda(address.owner);
        instance != nullptr && instance->id == address.owner) {
      return &instance->body;
    }
    return nullptr;
  case MirBodyKind::HostedStartup:
    return address.owner != 0 && program.hostedStartupPlan() &&
                   program.hostedStartupPlan()->entry == address.owner
               ? program.hostedStartup()
               : nullptr;
  }
  return nullptr;
}

MirBody *findMirBody(MirProgram &program, MirBodyAddress address) {
  return const_cast<MirBody *>(findMirBody(std::as_const(program), address));
}

bool mirTransformRecordLess(const MirTransformRecord &left,
                            const MirTransformRecord &right) {
  const auto key = [](const MirTransformRecord &record) {
    return std::tuple{
        static_cast<std::size_t>(record.body.kind),
        record.body.owner,
        record.block,
        record.instruction,
        record.place,
        static_cast<std::size_t>(record.kind),
        static_cast<std::size_t>(record.proof),
        record.sourceValue,
        record.sourceStatement,
        record.pass,
        record.safetyDischarge.has_value(),
        record.safetyDischarge
            ? static_cast<std::size_t>(record.safetyDischarge->family)
            : 0,
        record.safetyDischarge ? record.safetyDischarge->origin : 0,
        record.safetyDischarge
            ? static_cast<std::size_t>(record.safetyDischarge->outcome.code)
            : 0,
        record.safetyDischarge
            ? static_cast<std::size_t>(record.safetyDischarge->outcome.detail)
            : 0,
        record.relatedBlocks,
        record.relatedInstructions,
        record.relatedValues,
        record.relatedPlaces};
  };
  if (key(left) != key(right)) {
    return key(left) < key(right);
  }
  return std::tie(left.requiredAnalyses.valueUses,
                  left.requiredAnalyses.effects,
                  left.requiredAnalyses.reachability,
                  left.requiredAnalyses.dominance,
                  left.requiredAnalyses.scalarPromotion,
                  left.requiredAnalyses.safetyChecks,
                  left.requiredAnalyses.sparseConstants) <
         std::tie(right.requiredAnalyses.valueUses,
                  right.requiredAnalyses.effects,
                  right.requiredAnalyses.reachability,
                  right.requiredAnalyses.dominance,
                  right.requiredAnalyses.scalarPromotion,
                  right.requiredAnalyses.safetyChecks,
                  right.requiredAnalyses.sparseConstants);
}

bool hasMirSafetyDischarge(const MirInstruction &instruction,
                           MirSafetyDischarge discharge) {
  return std::find(instruction.safetyDischarges.begin(),
                   instruction.safetyDischarges.end(),
                   discharge) != instruction.safetyDischarges.end();
}

bool isMirFailureOutcomeDischarged(const MirInstruction &instruction,
                                   std::size_t origin,
                                   DefinedFailureOutcome outcome) {
  return std::any_of(
      instruction.safetyDischarges.begin(), instruction.safetyDischarges.end(),
      [&](const MirSafetyDischarge &discharge) {
        return discharge.origin == origin && discharge.outcome == outcome;
      });
}

bool allMirLocalFailuresDischarged(const MirInstruction &instruction) {
  if (instruction.definedFailure.localOrigins.empty()) {
    return false;
  }
  bool sawOutcome = false;
  for (std::size_t origin = 0;
       origin < instruction.definedFailure.localOrigins.size(); ++origin) {
    for (const DefinedFailureOutcome outcome :
         instruction.definedFailure.localOrigins[origin].outcomes) {
      sawOutcome = true;
      if (!isMirFailureOutcomeDischarged(instruction, origin, outcome)) {
        return false;
      }
    }
  }
  return sawOutcome;
}

bool isMirBlockParameterType(const SemanticType &type) {
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
  case SemanticType::CString:
  case SemanticType::NullPtr:
  case SemanticType::RawPointer:
  case SemanticType::Enum:
    return true;
  case SemanticType::Unknown:
  case SemanticType::Void:
  case SemanticType::StringView:
  case SemanticType::Array:
  case SemanticType::Class:
  case SemanticType::Reference:
  case SemanticType::UniqueOwner:
  case SemanticType::SharedPointer:
  case SemanticType::TypeParameter:
  case SemanticType::TypePack:
  case SemanticType::TypeName:
  case SemanticType::Function:
  case SemanticType::FunctionSignature:
  case SemanticType::FunctionItem:
  case SemanticType::NativeFunction:
  case SemanticType::Lambda:
  case SemanticType::Expected:
  case SemanticType::Unexpected:
  case SemanticType::PrefixStorage:
    return false;
  }
  return false;
}

MirInstruction makeMirInstructionTombstone(const MirInstruction &instruction) {
  MirInstruction result;
  result.id = instruction.id;
  result.kind = MirInstructionKind::Compute;
  result.tombstone = true;
  result.hirValue = instruction.hirValue;
  result.hirStatement = instruction.hirStatement;
  result.result = instruction.result;
  result.info = instruction.info;
  return result;
}

MirTemporaryMaterialization
findMirTemporaryMaterialization(const MirBody &body, const MirPlace &place,
                                HirValueId sourceValue) {
  if (sourceValue == 0 || place.sourceValue != sourceValue ||
      !place.projections.empty() ||
      (place.root != MirPlaceRootKind::Value &&
       place.root != MirPlaceRootKind::Temporary)) {
    return {};
  }
  if (place.root == MirPlaceRootKind::Value &&
      (place.value == 0 || place.temporary != 0)) {
    return {};
  }
  if (place.root == MirPlaceRootKind::Temporary &&
      (place.temporary == 0 || place.value != 0)) {
    return {};
  }

  MirTemporaryMaterialization result;
  for (const MirValue &candidate : body.values) {
    if (candidate.sourceValue != sourceValue ||
        (place.root == MirPlaceRootKind::Value &&
         candidate.id != place.value)) {
      continue;
    }
    const MirBlock *block = body.findBlock(candidate.definitionBlock);
    if (block == nullptr) {
      continue;
    }
    const auto definition = std::find_if(
        block->instructions.begin(), block->instructions.end(),
        [&](const MirInstruction &instruction) {
          return instruction.id == candidate.definition &&
                 instruction.result == candidate.id &&
                 instruction.hirValue == sourceValue &&
                 (instruction.kind == MirInstructionKind::Call ||
                  instruction.kind == MirInstructionKind::Construct ||
                  (place.root == MirPlaceRootKind::Temporary &&
                   (instruction.kind == MirInstructionKind::Move ||
                    instruction.kind == MirInstructionKind::Load) &&
                   instruction.operands.size() == 1 &&
                   instruction.operands.front().place == place.id &&
                   instruction.operands.front().kind ==
                       (instruction.kind == MirInstructionKind::Move
                            ? MirOperandKind::Move
                            : MirOperandKind::Copy) &&
                   !instruction.ownership && instruction.lifecycle.empty()));
        });
    if (definition == block->instructions.end()) {
      continue;
    }
    if (result) {
      return {};
    }
    result = {.value = &candidate, .producer = &*definition};
  }
  return result;
}

MirTemporaryCarrierDependency
findMirTemporaryCarrierDependency(const MirBody &body, const MirPlace &place) {
  if ((place.root != MirPlaceRootKind::Value &&
       place.root != MirPlaceRootKind::Temporary) ||
      place.type.kind != SemanticType::Class ||
      !place.traits.containsBorrowedState ||
      place.access != AccessMode::Mutable) {
    return {};
  }
  const auto materialization =
      findMirTemporaryMaterialization(body, place, place.sourceValue);
  const auto *producer = materialization.producer;
  const auto *loan = producer != nullptr && producer->loan
                         ? body.findLoan(*producer->loan)
                         : nullptr;
  if (!materialization || producer->info.type != place.type ||
      !producer->info.traits.containsBorrowedState ||
      producer->info.category != ValueCategory::Value || loan == nullptr ||
      loan->kind != MirLoanKind::Stored || loan->entry || loan->escapes ||
      !loan->carriers.empty() || loan->storedDependency != 0 ||
      loan->producedBy != place.sourceValue || loan->storedField != 0) {
    return {};
  }
  const auto stableOwner = [&](MirPlaceId id) {
    const auto *owner = body.findPlace(id);
    return owner != nullptr && owner->root != MirPlaceRootKind::Value &&
           owner->root != MirPlaceRootKind::Temporary;
  };
  if (!stableOwner(loan->source) ||
      std::any_of(loan->additionalDependencies.begin(),
                  loan->additionalDependencies.end(),
                  [&](const auto &dependency) {
                    return !stableOwner(dependency.source);
                  })) {
    return {};
  }
  // Cleanup-bearing temporaries retain the same exact producer/value/drop
  // identity as passive carriers. Their Stored dependency is lifecycle-bound;
  // the separate CallArgument shell is not. Lifecycle and loan dataflow still
  // require the owning value's cleanup before any retained dependency ends.
  const MirDropObligation *drop = nullptr;
  for (const auto &candidate : body.dropObligations) {
    if (candidate.kind != MirDropObligationKind::Value ||
        candidate.value != place.sourceValue) {
      continue;
    }
    if (drop != nullptr || candidate.place != place.id ||
        candidate.dropType.type != place.type ||
        candidate.hirFullExpression == 0) {
      return {};
    }
    drop = &candidate;
  }
  return drop == nullptr
             ? MirTemporaryCarrierDependency{}
             : MirTemporaryCarrierDependency{materialization, loan, drop};
}

bool mirLoanRetainsDependency(const MirBody &body, MirLoanId loan,
                              MirLoanId dependency) {
  if (dependency == 0 || body.findLoan(dependency) == nullptr)
    return false;
  std::vector<MirLoanId> pending{loan};
  std::unordered_set<MirLoanId> visited;
  while (!pending.empty()) {
    const MirLoanId candidate = pending.back();
    pending.pop_back();
    if (candidate == 0 || !visited.insert(candidate).second)
      continue;
    const MirLoan *record = body.findLoan(candidate);
    if (record == nullptr)
      continue;
    if (candidate == dependency)
      return true;
    pending.push_back(record->parent);
    pending.push_back(record->storedDependency);
  }
  return false;
}

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
    case MirTerminatorKind::Invoke:
      enqueue(terminator.target);
      enqueue(terminator.elseTarget);
      break;
    case MirTerminatorKind::None:
    case MirTerminatorKind::Return:
    case MirTerminatorKind::PropagateFailure:
    case MirTerminatorKind::Unreachable:
    case MirTerminatorKind::Exit:
    case MirTerminatorKind::ContainFailure:
    case MirTerminatorKind::TerminateCleanupFailure:
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

[[nodiscard]] bool literalMatchesType(const Literal &literal,
                                      const SemanticType &type) {
  if (const auto *integer = std::get_if<std::uint64_t>(&literal)) {
    (void)integer;
    // The lexical magnitude of a signed minimum literal is one greater than
    // the positive range and becomes valid only when consumed by unary
    // negation. Semantic analysis owns that contextual proof; MIR verifies the
    // literal alternative and integer result domain without rejecting the
    // intermediate lexical magnitude.
    return type == SemanticType::Unknown ||
           constantIntegerDomain(type).has_value();
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&literal)) {
    return validBinaryFloat(*floating) &&
           semanticFloatFormat(type) == floating->format;
  }
  if (std::holds_alternative<CharacterLiteral>(literal)) {
    return type == SemanticType::Char;
  }
  if (std::holds_alternative<std::string>(literal)) {
    return type == SemanticType::StringView;
  }
  if (std::holds_alternative<bool>(literal)) {
    return type == SemanticType::Bool;
  }
  if (std::holds_alternative<std::nullptr_t>(literal)) {
    return type == SemanticType::NullPtr;
  }
  return false;
}

[[nodiscard]] std::optional<Literal>
programConstantLiteral(const ConstantValue &constant) {
  if (const auto *integer = std::get_if<ConstantInteger>(&constant)) {
    return Literal{integer->magnitude};
  }
  if (const auto *enumerator = std::get_if<ConstantEnum>(&constant)) {
    return Literal{enumerator->value.magnitude};
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&constant)) {
    return Literal{*floating};
  }
  if (const auto *character = std::get_if<CharacterLiteral>(&constant)) {
    return Literal{*character};
  }
  if (const auto *string = std::get_if<std::string>(&constant)) {
    return Literal{*string};
  }
  if (const auto *boolean = std::get_if<bool>(&constant)) {
    return Literal{*boolean};
  }
  if (std::holds_alternative<NullConstant>(constant)) {
    return Literal{nullptr};
  }
  return std::nullopt;
}

[[nodiscard]] bool
programConstantMatchesType(const ConstantValue &constant,
                           const SemanticType &type,
                           const MirProgram *program = nullptr) {
  if (const auto *integer = std::get_if<ConstantInteger>(&constant)) {
    if (type.kind == SemanticType::Enum) {
      return type.enumId != 0;
    }
    const std::optional<CheckedIntegerDomain> domain =
        constantIntegerDomain(type);
    return domain && *domain == integer->domain;
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&constant)) {
    return semanticFloatFormat(type) == floating->format;
  }
  if (std::holds_alternative<CharacterLiteral>(constant)) {
    return type == SemanticType::Char;
  }
  if (std::holds_alternative<std::string>(constant)) {
    return type == SemanticType::StringView;
  }
  if (std::holds_alternative<bool>(constant)) {
    return type == SemanticType::Bool;
  }
  if (std::holds_alternative<NullConstant>(constant)) {
    return type == SemanticType::NullPtr;
  }
  if (const auto *enumerator = std::get_if<ConstantEnum>(&constant)) {
    return type.kind == SemanticType::Enum && type.enumId != 0 &&
           type.enumId == enumerator->enumId;
  }
  if (const auto *aggregate = std::get_if<ConstantAggregateValue>(&constant)) {
    if (aggregate->value == nullptr || aggregate->value->exactType == nullptr ||
        *aggregate->value->exactType != type) {
      return false;
    }
    if (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
        type.arrayLengthParameterId == 0 &&
        aggregate->value->kind == ConstantAggregateKind::Array &&
        aggregate->value->elements.size() == type.arrayLength) {
      return std::all_of(aggregate->value->elements.begin(),
                         aggregate->value->elements.end(),
                         [&](const ConstantValue &element) {
                           return programConstantMatchesType(
                               element, type.arguments.front(), program);
                         });
    }
    if (type.kind != SemanticType::Class || type.classId == 0 ||
        aggregate->value->kind != ConstantAggregateKind::Object ||
        aggregate->value->identity != type.classId) {
      return false;
    }
    if (program == nullptr) {
      return true;
    }
    const auto instance = std::find_if(
        program->classInstances().begin(), program->classInstances().end(),
        [&](const MirClassInstance &candidate) {
          return candidate.declaration == type.classId &&
                 candidate.type == type;
        });
    if (instance == program->classInstances().end() ||
        instance->declaredFields.size() != aggregate->value->fields.size()) {
      return false;
    }
    for (std::size_t index = 0; index < instance->declaredFields.size();
         ++index) {
      const MirClassFieldInfo &expected = instance->declaredFields[index];
      const ConstantAggregateField &field = aggregate->value->fields[index];
      if (field.symbol != expected.symbol ||
          !programConstantMatchesType(field.value, expected.type, program)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

enum class MirLoanFlowState : std::uint8_t {
  Inactive = 1,
  Active = 2,
  Suspended = 4,
  Reserved = 8,
  // Joins may carry these sets only for exact conditional temporary-carrier
  // dependencies and their ancestors. No executable access accepts a set.
  MaybeActive = 3,
  MaybeSuspended = 5,
  ActiveOrSuspended = 6,
  MaybeActiveOrSuspended = 7,
};

[[nodiscard]] bool loanFlowContains(MirLoanFlowState state,
                                    MirLoanFlowState member) {
  return (static_cast<unsigned>(state) & static_cast<unsigned>(member)) != 0;
}

using MirLoanState = std::vector<MirLoanFlowState>;

struct MirCanonicalPayloadSelection {
  // Inserted at this position among the ordinary field/dereference path.
  std::size_t projectionOffset = 0;
  SemanticType type = SemanticType::Unknown;
  std::size_t variant = 0;
  std::size_t field = 0;

  friend bool operator==(const MirCanonicalPayloadSelection &,
                         const MirCanonicalPayloadSelection &) = default;
};

struct MirCanonicalPlace {
  MirPlaceRootKind root = MirPlaceRootKind::Value;
  HirBindingId binding = 0;
  SymbolId symbol = 0;
  MirTemporaryId temporary = 0;
  MirValueId value = 0;
  std::vector<MirPlaceProjection> projections;
  std::vector<MirCanonicalPayloadSelection> payloadSelections;
  MirLoanId throughLoan = 0;
  bool ambiguous = false;
};

[[nodiscard]] std::optional<MirPlaceId>
borrowSourceForValue(const MirBody &body, MirValueId valueId,
                     std::size_t depth) {
  return mir_detail::borrowSourceForValue(body, valueId, depth);
}

[[nodiscard]] MirCanonicalPlace
canonicalPlace(const MirBody &body, MirPlaceId placeId,
               const std::unordered_map<HirBindingId, std::vector<MirLoanId>>
                   &bindingLoans,
               const MirLoanState &state) {
  const auto carrierLoan = [&](HirBindingId binding) {
    const auto found = bindingLoans.find(binding);
    if (found == bindingLoans.end()) {
      return MirLoanId{0};
    }
    MirLoanId candidate = 0;
    for (const MirLoanId loan : found->second) {
      if (state[loan - 1] == MirLoanFlowState::Inactive) {
        continue;
      }
      if (candidate != 0) {
        return MirLoanId{0};
      }
      candidate = loan;
    }
    if (candidate == 0 && found->second.size() == 1) {
      candidate = found->second.front();
    }
    return candidate;
  };

  const auto resolve = [&](const auto &self, MirPlaceId id,
                           std::size_t depth) -> MirCanonicalPlace {
    const MirPlace *place = body.findPlace(id);
    if (place == nullptr ||
        depth > body.places.size() + body.values.size() + body.loans.size()) {
      return {.ambiguous = true};
    }

    if (place->root == MirPlaceRootKind::Value) {
      const MirValue *value = body.findValue(place->value);
      // A raw dereference designates pointee storage, not storage containing
      // the pointer. Following a pointer load's borrow origin here would
      // equate that unsafe pointee with the checked carrier shell and apply
      // the shell's access permissions to unrelated memory.
      const bool rawPointee = std::any_of(
          place->projections.begin(), place->projections.end(),
          [](const MirPlaceProjection &projection) {
            return projection.kind == MirProjectionKind::RawDereference ||
                   projection.kind == MirProjectionKind::RawIndex;
          });
      if (value != nullptr && value->info.category == ValueCategory::Place &&
          !rawPointee) {
        // Storage identity must retain the executable input's projections.
        // A dependency-origin query can deliberately normalize a borrowed
        // carrier back to its owner, losing a nested field in that carrier.
        // Load and borrowed payload projection are storage-alias operations.
        // A Call's argument/receiver does not prove its returned address: keep
        // calls on the separately recorded result-origin path instead.
        std::optional<MirPlaceId> source;
        const MirInstruction *definition =
            mir_detail::definitionFor(body, place->value);
        if (definition != nullptr && definition->operands.size() == 1 &&
            (definition->kind == MirInstructionKind::Load ||
             (definition->kind == MirInstructionKind::Compute &&
              definition->operation == MirOperation::PayloadExtract))) {
          const MirOperand &operand = definition->operands.front();
          if (operand.place != 0 &&
              (operand.kind == MirOperandKind::Copy ||
               operand.kind == MirOperandKind::BorrowRead ||
               operand.kind == MirOperandKind::BorrowWrite))
            source = operand.place;
        }
        if (!source)
          source = borrowSourceForValue(body, place->value, depth + 1);
        if (source && *source != id) {
          auto result = self(self, *source, depth + 1);
          if (definition != nullptr &&
              definition->kind == MirInstructionKind::Compute &&
              definition->operation == MirOperation::PayloadExtract) {
            // The operand is the enum subject, not the selected field's
            // address. Retain the exact typed selector already owned by MIR;
            // program verification separately proves its catalog row and
            // active-variant edge before this identity can authorize code.
            if (!definition->enumOwner || !definition->enumVariant ||
                !definition->payloadIndex || definition->operands.size() != 1 ||
                definition->operands.front().type.kind != SemanticType::Enum ||
                definition->operands.front().type.enumId !=
                    *definition->enumOwner)
              return {.ambiguous = true};
            result.payloadSelections.push_back(
                {.projectionOffset = result.projections.size(),
                 .type = definition->operands.front().type,
                 .variant = *definition->enumVariant,
                 .field = *definition->payloadIndex});
          }
          result.projections.insert(result.projections.end(),
                                    place->projections.begin(),
                                    place->projections.end());
          return result;
        }
      }
    }

    MirLoanId throughLoan = 0;
    std::size_t projectionOffset = 0;
    // A local carrier's reference field designates its recorded external
    // dependency, not storage physically nested below the carrier shell.
    // Preserve the executable field/dereference place, but normalize its
    // overlap identity through the exact field-to-owner publication contract.
    if (place->root == MirPlaceRootKind::Binding) {
      const auto carrier = carrierLoan(place->binding);
      const auto *loan = body.findLoan(carrier);
      if (loan != nullptr && loan->kind == MirLoanKind::Stored &&
          !loan->entry) {
        const auto project = [&](MirPlaceId source,
                                 const std::vector<SymbolId> &fields)
            -> std::optional<MirCanonicalPlace> {
          if (source == 0 || fields.empty() ||
              place->projections.size() <= fields.size())
            return std::nullopt;
          for (std::size_t index = 0; index < fields.size(); ++index)
            if (place->projections[index].kind != MirProjectionKind::Field ||
                place->projections[index].field != fields[index])
              return std::nullopt;
          if (place->projections[fields.size()].kind !=
              MirProjectionKind::Dereference)
            return std::nullopt;
          auto result = self(self, source, depth + 1);
          result.throughLoan = carrier;
          result.projections.insert(result.projections.end(),
                                    place->projections.begin() + fields.size() +
                                        1,
                                    place->projections.end());
          return result;
        };
        if (auto result = project(loan->source, loan->designatedFields))
          return *result;
        for (const auto &dependency : loan->additionalDependencies)
          if (auto result =
                  project(dependency.source, dependency.designatedFields))
            return *result;
      }
    }
    if (place->root == MirPlaceRootKind::Loan) {
      throughLoan = place->loan;
    } else if (place->root == MirPlaceRootKind::Binding &&
               !place->projections.empty() &&
               place->projections.front().kind ==
                   MirProjectionKind::Dereference) {
      throughLoan = carrierLoan(place->binding);
      projectionOffset = throughLoan == 0 ? 0 : 1;
    }

    if (throughLoan != 0) {
      const MirLoan *loan = body.findLoan(throughLoan);
      if (loan == nullptr) {
        return {.ambiguous = true};
      }
      if (loan->source == id) {
        // A by-value borrowed-state parameter is an opaque owner-loan root:
        // MIR cannot name the caller's owner place, but the parameter's
        // carrier object is the exact stable root inside this body.  Copies
        // of that carrier resolve back to this same root.  Other self cycles
        // remain malformed rather than being silently accepted.
        const bool opaqueStoredEntry =
            loan->kind == MirLoanKind::Stored && loan->entry &&
            place->root == MirPlaceRootKind::Binding &&
            place->traits.containsBorrowedState &&
            std::find(loan->carriers.begin(), loan->carriers.end(),
                      place->binding) != loan->carriers.end();
        if (!opaqueStoredEntry) {
          return {.ambiguous = true};
        }
        return {.root = place->root,
                .binding = place->binding,
                .symbol = place->symbol,
                .temporary = place->temporary,
                .value = place->value,
                .projections = place->projections};
      }
      MirCanonicalPlace result = self(self, loan->source, depth + 1);
      result.throughLoan = throughLoan;
      result.projections.insert(
          result.projections.end(),
          place->projections.begin() +
              static_cast<std::ptrdiff_t>(projectionOffset),
          place->projections.end());
      return result;
    }

    return {.root = place->root,
            .binding = place->binding,
            .symbol = place->symbol,
            .temporary = place->temporary,
            .value = place->value,
            .projections = place->projections,
            .ambiguous = place->root == MirPlaceRootKind::Loan};
  };
  return resolve(resolve, placeId, 0);
}

[[nodiscard]] MirCanonicalPlace canonicalBorrowOriginPlace(
    const MirBody &body, MirPlaceId placeId,
    const std::unordered_map<HirBindingId, std::vector<MirLoanId>>
        &bindingLoans,
    bool physicalCarrier = false) {
  const auto carrierLoan = [&](HirBindingId binding) {
    const auto found = bindingLoans.find(binding);
    if (found == bindingLoans.end() || found->second.size() != 1) {
      return MirLoanId{0};
    }
    return found->second.front();
  };
  const auto resolve = [&](const auto &self, MirPlaceId id,
                           std::size_t depth) -> MirCanonicalPlace {
    const MirPlace *place = body.findPlace(id);
    if (place == nullptr ||
        depth > body.places.size() + body.values.size() + body.loans.size()) {
      return {.ambiguous = true};
    }

    MirLoanId throughLoan = 0;
    std::size_t projectionOffset = 0;
    if (place->root == MirPlaceRootKind::Loan) {
      throughLoan = place->loan;
    } else if (place->root == MirPlaceRootKind::Binding) {
      const MirLoanId carrier = carrierLoan(place->binding);
      const bool referenceProjection =
          !place->projections.empty() &&
          place->projections.front().kind == MirProjectionKind::Dereference;
      if (carrier != 0 &&
          (referenceProjection ||
           (!physicalCarrier && place->type.kind != SemanticType::Reference &&
            place->traits.containsBorrowedState))) {
        throughLoan = carrier;
        projectionOffset = referenceProjection ? 1 : 0;
      }
    }
    if (place->root == MirPlaceRootKind::Value) {
      const std::optional<MirPlaceId> source =
          borrowSourceForValue(body, place->value, depth + 1);
      if (source && *source != id) {
        MirCanonicalPlace result = self(self, *source, depth + 1);
        result.projections.insert(result.projections.end(),
                                  place->projections.begin(),
                                  place->projections.end());
        return result;
      }
    }
    if (throughLoan != 0) {
      const MirLoan *loan = body.findLoan(throughLoan);
      if (loan == nullptr) {
        return {.ambiguous = true};
      }
      if (loan->source == id) {
        // A by-value borrowed-state parameter is an opaque owner-loan root:
        // MIR cannot name the caller's owner place, but the parameter's
        // carrier object is the exact stable root inside this body.  Copies
        // of that carrier resolve back to this same root.  Other self cycles
        // remain malformed rather than being silently accepted.
        const bool opaqueStoredEntry =
            loan->kind == MirLoanKind::Stored && loan->entry &&
            place->root == MirPlaceRootKind::Binding &&
            place->traits.containsBorrowedState &&
            std::find(loan->carriers.begin(), loan->carriers.end(),
                      place->binding) != loan->carriers.end();
        if (!opaqueStoredEntry) {
          return {.ambiguous = true};
        }
        return {.root = place->root,
                .binding = place->binding,
                .symbol = place->symbol,
                .temporary = place->temporary,
                .value = place->value,
                .projections = place->projections};
      }
      MirCanonicalPlace result = self(self, loan->source, depth + 1);
      result.throughLoan = throughLoan;
      result.projections.insert(
          result.projections.end(),
          place->projections.begin() +
              static_cast<std::ptrdiff_t>(projectionOffset),
          place->projections.end());
      return result;
    }
    return {.root = place->root,
            .binding = place->binding,
            .symbol = place->symbol,
            .temporary = place->temporary,
            .value = place->value,
            .projections = place->projections,
            .ambiguous = place->root == MirPlaceRootKind::Loan};
  };
  return resolve(resolve, placeId, 0);
}

[[nodiscard]] bool sameCanonicalRoot(const MirCanonicalPlace &left,
                                     const MirCanonicalPlace &right) {
  if (left.ambiguous || right.ambiguous) {
    return true;
  }
  if (left.root != right.root) {
    return false;
  }
  switch (left.root) {
  case MirPlaceRootKind::Binding:
    return left.binding == right.binding;
  case MirPlaceRootKind::Symbol:
    return left.symbol == right.symbol;
  case MirPlaceRootKind::This:
    return true;
  case MirPlaceRootKind::Temporary:
    return left.temporary == right.temporary;
  case MirPlaceRootKind::Value:
    return left.value == right.value;
  case MirPlaceRootKind::Loan:
    return true;
  }
  return true;
}

[[nodiscard]] bool sameCanonicalPlace(const MirCanonicalPlace &left,
                                      const MirCanonicalPlace &right) {
  if (left.ambiguous || right.ambiguous || !sameCanonicalRoot(left, right) ||
      left.projections.size() != right.projections.size() ||
      left.payloadSelections != right.payloadSelections) {
    return false;
  }
  return std::equal(
      left.projections.begin(), left.projections.end(),
      right.projections.begin(),
      [](const MirPlaceProjection &lhs, const MirPlaceProjection &rhs) {
        return lhs.kind == rhs.kind && lhs.field == rhs.field &&
               lhs.index == rhs.index &&
               lhs.constantIndex == rhs.constantIndex &&
               lhs.selection == rhs.selection;
      });
}

[[nodiscard]] bool sameGlobalBorrowPlace(const BorrowOriginPlace &expected,
                                         const MirCanonicalPlace &actual) {
  if (!expected.valid() || actual.ambiguous ||
      actual.root != MirPlaceRootKind::Symbol ||
      actual.symbol != expected.root ||
      actual.projections.size() != expected.projections.size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected.projections.size(); ++index) {
    const PlaceProjection &semantic = expected.projections[index];
    const MirPlaceProjection &mir = actual.projections[index];
    switch (semantic.kind) {
    case PlaceProjectionKind::Field:
      if (mir.kind != MirProjectionKind::Field || mir.field != semantic.field) {
        return false;
      }
      break;
    case PlaceProjectionKind::Dereference:
      if (mir.kind != MirProjectionKind::Dereference &&
          mir.kind != MirProjectionKind::RawDereference) {
        return false;
      }
      break;
    case PlaceProjectionKind::ConstantIndex:
      if (mir.kind != MirProjectionKind::Index ||
          mir.constantIndex != semantic.index) {
        return false;
      }
      break;
    case PlaceProjectionKind::DynamicIndex:
      if (mir.kind != MirProjectionKind::Index || mir.constantIndex ||
          mir.selection != semantic.selection) {
        return false;
      }
      break;
    }
  }
  return true;
}

[[nodiscard]] bool canonicalPlacesOverlap(const MirCanonicalPlace &left,
                                          const MirCanonicalPlace &right) {
  if (left.ambiguous || right.ambiguous) {
    return true;
  }
  if (!sameCanonicalRoot(left, right)) {
    return false;
  }
  // Keep overlap conservative at the matched enum subject. Refining exact
  // containment must not expand acceptance of sibling/alternative borrows.
  const auto protectedPrefix = [](const MirCanonicalPlace &place) {
    return place.payloadSelections.empty()
               ? place.projections.size()
               : std::min(place.projections.size(),
                          place.payloadSelections.front().projectionOffset);
  };
  const std::size_t common =
      std::min(protectedPrefix(left), protectedPrefix(right));
  for (std::size_t index = 0; index < common; ++index) {
    const MirPlaceProjection &lhs = left.projections[index];
    const MirPlaceProjection &rhs = right.projections[index];
    if (lhs.kind == MirProjectionKind::Field &&
        rhs.kind == MirProjectionKind::Field && lhs.field != 0 &&
        rhs.field != 0 && lhs.field != rhs.field) {
      return false;
    }
    if ((lhs.kind == MirProjectionKind::Index ||
         lhs.kind == MirProjectionKind::PackElement) &&
        lhs.kind == rhs.kind && lhs.constantIndex && rhs.constantIndex &&
        lhs.constantIndex != rhs.constantIndex) {
      return false;
    }
    if (lhs.kind != rhs.kind) {
      return true;
    }
  }
  return true;
}

[[nodiscard]] bool canonicalPlaceContains(const MirCanonicalPlace &parent,
                                          const MirCanonicalPlace &child) {
  if (parent.ambiguous || child.ambiguous ||
      !sameCanonicalRoot(parent, child) ||
      parent.projections.size() > child.projections.size() ||
      parent.payloadSelections.size() > child.payloadSelections.size()) {
    return false;
  }
  if (!std::equal(parent.payloadSelections.begin(),
                  parent.payloadSelections.end(),
                  child.payloadSelections.begin()))
    return false;
  if (child.payloadSelections.size() > parent.payloadSelections.size() &&
      child.payloadSelections[parent.payloadSelections.size()]
              .projectionOffset < parent.projections.size())
    return false;
  for (std::size_t index = 0; index < parent.projections.size(); ++index) {
    const MirPlaceProjection &outer = parent.projections[index];
    const MirPlaceProjection &inner = child.projections[index];
    if (outer.kind != inner.kind) {
      return false;
    }
    if (outer.kind == MirProjectionKind::Field &&
        (outer.field == 0 || outer.field != inner.field)) {
      return false;
    }
    if ((outer.kind == MirProjectionKind::Index ||
         outer.kind == MirProjectionKind::RawIndex ||
         outer.kind == MirProjectionKind::PackElement) &&
        (outer.kind == MirProjectionKind::RawIndex ||
         (outer.kind != MirProjectionKind::PackElement &&
          !outer.constantIndex && outer.selection == 0))) {
      if (outer.index == 0 || outer.index != inner.index) {
        return false;
      }
    }
    if (outer.kind == MirProjectionKind::Index ||
        outer.kind == MirProjectionKind::PackElement) {
      if (outer.constantIndex != inner.constantIndex ||
          outer.selection != inner.selection) {
        return false;
      }
    }
  }
  return true;
}

[[nodiscard]] std::optional<PlaceKey>
structuralPlaceKey(const MirBody &body, const MirPlace &place) {
  PlaceKey result{.domain = body.placeDomain};
  switch (place.root) {
  case MirPlaceRootKind::Binding:
    if (place.symbol == 0) {
      return std::nullopt;
    }
    result.root = place.symbol;
    break;
  case MirPlaceRootKind::Symbol:
    if (place.symbol == 0) {
      return std::nullopt;
    }
    result.root = place.symbol;
    break;
  case MirPlaceRootKind::This:
    result.receiver = true;
    break;
  case MirPlaceRootKind::Temporary:
  case MirPlaceRootKind::Value:
  case MirPlaceRootKind::Loan:
    return std::nullopt;
  }

  for (const MirPlaceProjection &projection : place.projections) {
    switch (projection.kind) {
    case MirProjectionKind::Field:
      if (projection.field == 0) {
        return std::nullopt;
      }
      result.projections.push_back(
          {.kind = PlaceProjectionKind::Field, .field = projection.field});
      break;
    case MirProjectionKind::Index:
      if (projection.constantIndex) {
        result.projections.push_back(
            {.kind = PlaceProjectionKind::ConstantIndex,
             .index = *projection.constantIndex});
      } else {
        const std::size_t selection =
            projection.selection != 0 ? projection.selection : projection.index;
        if (selection == 0) {
          return std::nullopt;
        }
        result.projections.push_back({.kind = PlaceProjectionKind::DynamicIndex,
                                      .selection = selection});
      }
      break;
    case MirProjectionKind::Dereference:
      result.projections.push_back({.kind = PlaceProjectionKind::Dereference});
      break;
    case MirProjectionKind::PackElement:
      if (!projection.constantIndex) {
        return std::nullopt;
      }
      result.projections.push_back({.kind = PlaceProjectionKind::ConstantIndex,
                                    .index = *projection.constantIndex});
      break;
    case MirProjectionKind::RawIndex:
    case MirProjectionKind::RawDereference:
    case MirProjectionKind::Base:
      return std::nullopt;
    }
  }
  return result;
}

[[nodiscard]] std::optional<PlaceKey> ownershipPlaceKey(const MirBody &body,
                                                        MirPlaceId placeId) {
  const MirPlace *place = body.findPlace(placeId);
  if (place == nullptr) {
    return std::nullopt;
  }
  return place->key ? place->key : structuralPlaceKey(body, *place);
}

struct MirOwnershipPlaceState {
  PlaceKey place;
  OwnershipStateSet state = OwnershipStateSet::Available;
};

using MirOwnershipFlowState = std::vector<MirOwnershipPlaceState>;

[[nodiscard]] OwnershipStateSet
ownershipStateAt(const MirOwnershipFlowState &state, const PlaceKey &place) {
  const auto found = std::find_if(state.begin(), state.end(),
                                  [&](const MirOwnershipPlaceState &candidate) {
                                    return candidate.place == place;
                                  });
  return found == state.end() ? OwnershipStateSet::Available : found->state;
}

void setOwnershipState(MirOwnershipFlowState &state, const PlaceKey &place,
                       OwnershipStateSet value) {
  const auto found = std::find_if(state.begin(), state.end(),
                                  [&](const MirOwnershipPlaceState &candidate) {
                                    return candidate.place == place;
                                  });
  if (value == OwnershipStateSet::Available) {
    if (found != state.end()) {
      state.erase(found);
    }
    return;
  }
  if (found == state.end()) {
    state.push_back({.place = place, .state = value});
  } else {
    found->state = value;
  }
}

[[nodiscard]] bool sameOwnershipState(const MirOwnershipFlowState &left,
                                      const MirOwnershipFlowState &right) {
  return left.size() == right.size() &&
         std::all_of(left.begin(), left.end(),
                     [&](const MirOwnershipPlaceState &candidate) {
                       return ownershipStateAt(right, candidate.place) ==
                              candidate.state;
                     });
}

[[nodiscard]] MirOwnershipFlowState
joinOwnershipState(const MirOwnershipFlowState &left,
                   const MirOwnershipFlowState &right) {
  MirOwnershipFlowState result = left;
  for (MirOwnershipPlaceState &candidate : result) {
    candidate.state =
        candidate.state | ownershipStateAt(right, candidate.place);
  }
  for (const MirOwnershipPlaceState &candidate : right) {
    if (std::none_of(result.begin(), result.end(),
                     [&](const MirOwnershipPlaceState &existing) {
                       return existing.place == candidate.place;
                     })) {
      const OwnershipStateSet joined =
          OwnershipStateSet::Available | candidate.state;
      if (joined != OwnershipStateSet::Available) {
        result.push_back({.place = candidate.place, .state = joined});
      }
    }
  }
  std::erase_if(result, [](const MirOwnershipPlaceState &candidate) {
    return candidate.state == OwnershipStateSet::Available;
  });
  return result;
}

[[nodiscard]] const MirOwnershipPlaceState *
unavailableOwnershipState(const MirOwnershipFlowState &state,
                          const PlaceKey &place,
                          bool includeIndirectDescendants = true) {
  const auto found = std::find_if(
      state.begin(), state.end(), [&](const MirOwnershipPlaceState &candidate) {
        if (candidate.state == OwnershipStateSet::Available) {
          return false;
        }
        const PlaceRelationResult relation =
            placeRelation(candidate.place, place);
        // Observing an owner handle does not read its pointee. This matches
        // semantic projected-place availability and permits checked access to
        // restore a moved pointee field. Whole-owner movement still includes
        // those descendants, and accessing the pointee itself does too.
        if (!includeIndirectDescendants && relation.compatibleDomain &&
            relation.relation == PlaceRelation::RightStrictPrefix &&
            candidate.place.projections[place.projections.size()].kind ==
                PlaceProjectionKind::Dereference) {
          return false;
        }
        return !relation.compatibleDomain ||
               relation.relation != PlaceRelation::Disjoint;
      });
  return found == state.end() ? nullptr : &*found;
}

[[nodiscard]] bool hasDynamicIndex(const PlaceKey &place) {
  return std::any_of(place.projections.begin(), place.projections.end(),
                     [](const PlaceProjection &projection) {
                       return projection.kind ==
                              PlaceProjectionKind::DynamicIndex;
                     });
}

[[nodiscard]] PlaceKey ownershipRoot(PlaceKey place) {
  place.projections.clear();
  return place;
}

[[nodiscard]] bool tracksOwnershipRoot(std::span<const PlaceKey> roots,
                                       const PlaceKey &place) {
  const PlaceKey root = ownershipRoot(place);
  return std::find(roots.begin(), roots.end(), root) != roots.end();
}

void restoreOwnershipPlace(MirOwnershipFlowState &state,
                           const PlaceKey &place) {
  std::erase_if(state, [&](const MirOwnershipPlaceState &candidate) {
    const PlaceRelationResult relation = placeRelation(place, candidate.place);
    return relation.compatibleDomain &&
           (relation.relation == PlaceRelation::Equal ||
            relation.relation == PlaceRelation::LeftStrictPrefix);
  });
}

[[nodiscard]] std::vector<MirBlockId>
ownershipSuccessors(const MirBody &body, const MirTerminator &terminator) {
  std::vector<MirBlockId> result;
  const auto append = [&](MirBlockId target) {
    if (target != 0 &&
        std::find(result.begin(), result.end(), target) == result.end()) {
      result.push_back(target);
    }
  };
  switch (terminator.kind) {
  case MirTerminatorKind::Goto:
    append(terminator.target);
    break;
  case MirTerminatorKind::Branch:
    // A literal boolean condition has only one executable successor. Keep
    // both structural CFG edges for dominance and loan checking, but do not
    // merge a semantically unreachable loop exit into ownership availability.
    // This is a typed MIR literal proof, not general constant evaluation.
    if (terminator.value && terminator.value->type == SemanticType::Bool) {
      const MirOperand *condition = &*terminator.value;
      std::unordered_set<MirValueId> visited;
      while (condition->kind == MirOperandKind::Value &&
             visited.insert(condition->value).second) {
        const MirValue *value = body.findValue(condition->value);
        const MirBlock *block =
            value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
        const MirInstruction *definition = nullptr;
        if (block != nullptr) {
          const auto found = std::find_if(
              block->instructions.begin(), block->instructions.end(),
              [&](const MirInstruction &instruction) {
                return instruction.id == value->definition &&
                       instruction.result == condition->value;
              });
          if (found != block->instructions.end())
            definition = &*found;
        }
        if (definition == nullptr ||
            definition->kind != MirInstructionKind::Compute ||
            definition->info.type != SemanticType::Bool)
          break;
        if (definition->operation == MirOperation::Identity &&
            definition->operands.size() == 1) {
          condition = &definition->operands.front();
          continue;
        }
        if (definition->operation == MirOperation::Literal &&
            definition->literal) {
          if (const bool *literal = std::get_if<bool>(&*definition->literal)) {
            append(*literal ? terminator.target : terminator.elseTarget);
            return result;
          }
        }
        break;
      }
      if (condition->kind == MirOperandKind::Constant && condition->literal) {
        if (const bool *literal = std::get_if<bool>(&*condition->literal)) {
          append(*literal ? terminator.target : terminator.elseTarget);
          return result;
        }
      }
    }
    append(terminator.target);
    append(terminator.elseTarget);
    break;
  case MirTerminatorKind::Switch:
    append(terminator.target);
    for (const MirSwitchTarget &target : terminator.switchTargets) {
      append(target.target);
    }
    break;
  case MirTerminatorKind::Invoke:
    append(terminator.target);
    append(terminator.elseTarget);
    break;
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    break;
  }
  return result;
}

[[nodiscard]] bool tracksScalarInitialization(const MirPlace &place) {
  if ((place.root != MirPlaceRootKind::Binding &&
       place.root != MirPlaceRootKind::Temporary) ||
      !place.projections.empty() || place.traits.drop != DropKind::Trivial ||
      place.traits.containsBorrowedState ||
      (place.key && (place.key->receiver || !place.key->projections.empty()))) {
    return false;
  }
  switch (place.type.kind) {
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
}

[[nodiscard]] bool sameScalarInitializationStorage(const MirPlace &left,
                                                   const MirPlace &right) {
  if (left.root != right.root) {
    return false;
  }
  if (left.root == MirPlaceRootKind::Temporary) {
    return left.temporary != 0 && left.temporary == right.temporary;
  }
  if (left.root != MirPlaceRootKind::Binding) {
    return false;
  }
  if (left.key && right.key) {
    return *left.key == *right.key;
  }
  return left.binding != 0 && left.binding == right.binding;
}

[[nodiscard]] MirVerificationResult
verifyMirScalarInitializationFlow(const MirBody &body, std::size_t owner) {
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return failure(body, owner,
                   "scalar initialization flow has an invalid entry block");
  }
  constexpr std::size_t noStorage = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> storageForPlace(body.places.size(), noStorage);
  std::vector<MirPlaceId> representatives;
  std::vector<bool> initial;
  for (const MirPlace &place : body.places) {
    if (place.id == 0 || place.id > body.places.size() ||
        !tracksScalarInitialization(place)) {
      continue;
    }
    const auto existing =
        std::find_if(representatives.begin(), representatives.end(),
                     [&](MirPlaceId representative) {
                       return sameScalarInitializationStorage(
                           place, body.places[representative - 1]);
                     });
    const std::size_t storage = existing == representatives.end()
                                    ? representatives.size()
                                    : static_cast<std::size_t>(std::distance(
                                          representatives.begin(), existing));
    if (existing == representatives.end()) {
      representatives.push_back(place.id);
      initial.push_back(false);
    }
    storageForPlace[place.id - 1] = storage;
    initial[storage] = initial[storage] || place.initiallyAvailable;
  }

  std::vector<std::optional<std::vector<bool>>> entryStates(body.blocks.size());
  entryStates[body.entry - 1] = std::move(initial);
  std::queue<MirBlockId> pending;
  pending.push(body.entry);

  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock *block = body.findBlock(blockId);
    if (block == nullptr || !entryStates[blockId - 1]) {
      continue;
    }
    std::vector<bool> available = *entryStates[blockId - 1];
    for (const MirInstruction &instruction : block->instructions) {
      const auto requireAvailable = [&](const MirOperand &operand)
          -> std::optional<MirVerificationResult> {
        if ((operand.kind != MirOperandKind::Copy &&
             operand.kind != MirOperandKind::Move) ||
            operand.place == 0 || operand.place > storageForPlace.size()) {
          return std::nullopt;
        }
        const std::size_t storage = storageForPlace[operand.place - 1];
        if (storage == noStorage || available[storage]) {
          return std::nullopt;
        }
        return failure(body, owner,
                       "place access requires a definitely initialized "
                       "scalar place",
                       block->id, instruction.id);
      };
      if (instruction.receiver) {
        if (std::optional<MirVerificationResult> invalid =
                requireAvailable(*instruction.receiver)) {
          return std::move(*invalid);
        }
      }
      for (const MirOperand &operand : instruction.operands) {
        if (std::optional<MirVerificationResult> invalid =
                requireAvailable(operand)) {
          return std::move(*invalid);
        }
      }
      if (instruction.destination &&
          (instruction.kind == MirInstructionKind::Initialize ||
           instruction.kind == MirInstructionKind::Assign) &&
          *instruction.destination != 0 &&
          *instruction.destination <= storageForPlace.size() &&
          storageForPlace[*instruction.destination - 1] != noStorage) {
        const std::size_t storage =
            storageForPlace[*instruction.destination - 1];
        if (instruction.kind == MirInstructionKind::Initialize &&
            instruction.operands.empty()) {
          const MirPlace *destination =
              body.findPlace(*instruction.destination);
          if (body.kind == MirBodyKind::Module &&
              block->programInitializationStep != 0 && destination != nullptr &&
              destination->root == MirPlaceRootKind::Binding &&
              destination->projections.empty() &&
              !destination->initiallyAvailable) {
            available[storage] = true;
          }
          continue;
        }
        const bool initialized = available[storage];
        if (instruction.kind == MirInstructionKind::Assign && !initialized) {
          return failure(body, owner,
                         "assignment requires a definitely initialized "
                         "scalar destination",
                         block->id, instruction.id);
        }
        available[storage] = true;
      }
    }

    if (block->terminator.value) {
      const MirOperand &operand = *block->terminator.value;
      if ((operand.kind == MirOperandKind::Copy ||
           operand.kind == MirOperandKind::Move) &&
          operand.place != 0 && operand.place <= storageForPlace.size() &&
          storageForPlace[operand.place - 1] != noStorage &&
          !available[storageForPlace[operand.place - 1]]) {
        return failure(body, owner,
                       "terminator place access requires a definitely "
                       "initialized scalar place",
                       block->id);
      }
    }

    for (const MirBlockId successor :
         ownershipSuccessors(body, block->terminator)) {
      if (successor == 0 || successor > body.blocks.size()) {
        continue;
      }
      std::optional<std::vector<bool>> &incoming = entryStates[successor - 1];
      if (!incoming) {
        incoming = available;
        pending.push(successor);
        continue;
      }
      std::vector<bool> merged = *incoming;
      for (std::size_t index = 0; index < merged.size(); ++index) {
        merged[index] = merged[index] && available[index];
      }
      if (merged != *incoming) {
        incoming = std::move(merged);
        pending.push(successor);
      }
    }
  }
  return {};
}

[[nodiscard]] MirVerificationResult verifyMirOwnershipFlow(const MirBody &body,
                                                           std::size_t owner) {
  std::vector<PlaceKey> trackedRoots;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!instruction.ownership ||
          (instruction.ownership->kind != OwnershipEventKind::Move &&
           instruction.ownership->kind !=
               OwnershipEventKind::ConsumeReceiver)) {
        continue;
      }
      PlaceKey root = ownershipRoot(instruction.ownership->place);
      if (root.valid() && std::find(trackedRoots.begin(), trackedRoots.end(),
                                    root) == trackedRoots.end()) {
        trackedRoots.push_back(std::move(root));
      }
    }
  }
  std::vector<std::optional<MirOwnershipFlowState>> entryStates(
      body.blocks.size());
  MirOwnershipFlowState initialState;
  for (const MirPlace &place : body.places) {
    if (place.key && place.root == MirPlaceRootKind::Binding &&
        place.projections.empty() && !place.initiallyAvailable &&
        tracksOwnershipRoot(trackedRoots, *place.key)) {
      setOwnershipState(initialState, *place.key,
                        OwnershipStateSet::Uninitialized);
    }
  }
  entryStates[body.entry - 1] = std::move(initialState);
  std::queue<MirBlockId> pending;
  pending.push(body.entry);

  bool validating = false;
  const auto requireAvailable =
      [&](const MirOwnershipFlowState &state, MirPlaceId placeId,
          const MirBlock &block, const MirInstruction &instruction,
          std::string_view operation,
          bool includeIndirectDescendants =
              false) -> std::optional<MirVerificationResult> {
    if (!validating)
      return std::nullopt;
    const std::optional<PlaceKey> place = ownershipPlaceKey(body, placeId);
    if (!place) {
      return std::nullopt;
    }
    if (const MirOwnershipPlaceState *unavailable = unavailableOwnershipState(
            state, *place, includeIndirectDescendants)) {
      return failure(body, owner,
                     std::string(operation) +
                         " requires a definitely available place (state " +
                         std::to_string(unavailable->state.bits) + ")",
                     block.id, instruction.id);
    }
    return std::nullopt;
  };

  // First solve availability for the complete executable CFG. Checking an
  // exact semantic before-state while only one predecessor has arrived would
  // reject a valid Available|Moved join (for example a conditional break).
  // The second pass validates operations against those stable entry states;
  // transfers are still derived from MIR operations, never from event.after.
  for (const bool validationPass : {false, true}) {
    validating = validationPass;
    if (validating) {
      for (const MirBlock &block : body.blocks)
        if (entryStates[block.id - 1])
          pending.push(block.id);
    }
    while (!pending.empty()) {
      const MirBlockId blockId = pending.front();
      pending.pop();
      const MirBlock *block = body.findBlock(blockId);
      if (block == nullptr || !entryStates[blockId - 1]) {
        continue;
      }
      MirOwnershipFlowState state = *entryStates[blockId - 1];

      for (const MirInstruction &instruction : block->instructions) {
        if (instruction.ownership &&
            (!instruction.ownership->place.valid() ||
             instruction.ownership->place.domain != body.placeDomain)) {
          return failure(body, owner,
                         "ownership event has an invalid place domain or key",
                         block->id, instruction.id);
        }
        if (instruction.ownership && !instruction.ownership->reachable) {
          continue;
        }
        const auto checkOperand = [&](const MirOperand &operand)
            -> std::optional<MirVerificationResult> {
          switch (operand.kind) {
          case MirOperandKind::Address:
          case MirOperandKind::Copy:
          case MirOperandKind::BorrowRead:
          case MirOperandKind::BorrowWrite:
            return requireAvailable(state, operand.place, *block, instruction,
                                    "place access");
          case MirOperandKind::Move:
          case MirOperandKind::Value:
          case MirOperandKind::Constant:
            return std::nullopt;
          case MirOperandKind::Loan:
            return operand.place == 0
                       ? std::nullopt
                       : requireAvailable(state, operand.place, *block,
                                          instruction,
                                          "reference address source");
          }
          return std::nullopt;
        };
        if (instruction.receiver) {
          if (std::optional<MirVerificationResult> invalid =
                  checkOperand(*instruction.receiver)) {
            return std::move(*invalid);
          }
        }
        for (const MirOperand &operand : instruction.operands) {
          if (std::optional<MirVerificationResult> invalid =
                  checkOperand(operand)) {
            return std::move(*invalid);
          }
        }

        const bool retainedReceiver =
            instruction.kind == MirInstructionKind::CallInput &&
            instruction.callInputKind == HirCallInputKind::RetainedReceiver;
        if ((instruction.kind == MirInstructionKind::Move ||
             retainedReceiver) &&
            !instruction.operands.empty()) {
          const MirPlaceId source =
              retainedReceiver && instruction.retainedOwnerPlace
                  ? *instruction.retainedOwnerPlace
                  : instruction.operands.front().place;
          const std::optional<PlaceKey> place = ownershipPlaceKey(body, source);
          if (!place) {
            continue;
          }
          if (hasDynamicIndex(*place)) {
            return failure(body, owner,
                           "move uses a dynamic-index place without an alias "
                           "proof",
                           block->id, instruction.id);
          }
          if (std::optional<MirVerificationResult> invalid = requireAvailable(
                  state, source, *block, instruction, "move", true)) {
            return std::move(*invalid);
          }
          if (!instruction.ownership ||
              instruction.ownership->kind !=
                  (retainedReceiver ? OwnershipEventKind::ConsumeReceiver
                                    : OwnershipEventKind::Move) ||
              instruction.ownership->place != *place ||
              instruction.ownership->before != OwnershipStateSet::Available ||
              instruction.ownership->after != OwnershipStateSet::Moved) {
            return failure(body, owner,
                           "move does not preserve its semantic ownership "
                           "event and place identity",
                           block->id, instruction.id);
          }
          setOwnershipState(state, *place, OwnershipStateSet::Moved);
          continue;
        }

        if (instruction.destination) {
          const std::optional<PlaceKey> destination =
              ownershipPlaceKey(body, *instruction.destination);
          if (!destination) {
            continue;
          }
          if (instruction.kind == MirInstructionKind::Initialize) {
            const MirPlace *destinationPlace =
                body.findPlace(*instruction.destination);
            // Lexical-drop bindings always spell their scope end as a Drop, so
            // an Initialize must only ever see storage the flow model already
            // proved uninitialized. Trivially dropped bindings deliberately
            // leave scope without any MIR instruction: a block-scoped local
            // that was moved (or conditionally moved) still carries Moved
            // state across a loop back edge even though its lifetime ended at
            // the closing brace. Their re-initialization at the declaration is
            // the implicit scope end, so it may begin from any prior state.
            if (validating && tracksOwnershipRoot(trackedRoots, *destination) &&
                destinationPlace != nullptr &&
                destinationPlace->root == MirPlaceRootKind::Binding &&
                destinationPlace->projections.empty() &&
                !destinationPlace->initiallyAvailable &&
                destinationPlace->traits.drop == DropKind::Lexical &&
                !ownershipStateAt(state, *destination)
                     .definitely(OwnershipState::Uninitialized)) {
              return failure(body, owner,
                             "initialization targets a place whose lifetime is "
                             "already active",
                             block->id, instruction.id);
            }
            restoreOwnershipPlace(state, *destination);
          } else if (instruction.kind == MirInstructionKind::Drop) {
            if (validating && tracksOwnershipRoot(trackedRoots, *destination) &&
                ownershipStateAt(state, *destination)
                    .contains(OwnershipState::Uninitialized)) {
              return failure(body, owner,
                             "drop requires a definitely active place lifetime",
                             block->id, instruction.id);
            }
            restoreOwnershipPlace(state, *destination);
            if (tracksOwnershipRoot(trackedRoots, *destination)) {
              setOwnershipState(state, *destination,
                                OwnershipStateSet::Uninitialized);
            }
          } else if (instruction.kind == MirInstructionKind::Assign &&
                     instruction.operation == MirOperation::Assign) {
            if (!tracksOwnershipRoot(trackedRoots, *destination)) {
              continue;
            }
            const bool indexedOwnershipPlace = std::any_of(
                destination->projections.begin(),
                destination->projections.end(),
                [](const PlaceProjection &projection) {
                  return projection.kind ==
                             PlaceProjectionKind::ConstantIndex ||
                         projection.kind == PlaceProjectionKind::DynamicIndex;
                });
            if (indexedOwnershipPlace && !instruction.ownership) {
              return failure(body, owner,
                             "assignment does not preserve its semantic "
                             "reinitialization event and place identity",
                             block->id, instruction.id);
            }
            if (!instruction.ownership) {
              continue;
            }
            if (validating && (instruction.ownership->kind !=
                                   OwnershipEventKind::Reinitialize ||
                               instruction.ownership->place != *destination ||
                               instruction.ownership->before !=
                                   ownershipStateAt(state, *destination) ||
                               instruction.ownership->after !=
                                   OwnershipStateSet::Available)) {
              return failure(body, owner,
                             "assignment ownership event disagrees with its "
                             "destination place",
                             block->id, instruction.id);
            }
            const MirOwnershipPlaceState *unavailableAncestor =
                unavailableOwnershipState(state, *destination);
            if (validating && unavailableAncestor != nullptr) {
              const PlaceRelationResult relation =
                  placeRelation(unavailableAncestor->place, *destination);
              if (!relation.compatibleDomain ||
                  relation.relation == PlaceRelation::LeftStrictPrefix ||
                  relation.relation == PlaceRelation::MayAlias) {
                return failure(body, owner,
                               "assignment cannot prove that its destination "
                               "is independent of unavailable storage",
                               block->id, instruction.id);
              }
            }
            restoreOwnershipPlace(state, *destination);
          } else if (instruction.kind == MirInstructionKind::Assign ||
                     instruction.kind == MirInstructionKind::Modify) {
            if (std::optional<MirVerificationResult> invalid =
                    requireAvailable(state, *instruction.destination, *block,
                                     instruction, "mutation")) {
              return std::move(*invalid);
            }
          }
        }
      }

      if (validating)
        continue;
      for (const MirBlockId successor :
           ownershipSuccessors(body, block->terminator)) {
        if (successor == 0 || successor > body.blocks.size()) {
          continue;
        }
        std::optional<MirOwnershipFlowState> &incoming =
            entryStates[successor - 1];
        const MirOwnershipFlowState merged =
            incoming ? joinOwnershipState(*incoming, state) : state;
        if (!incoming || !sameOwnershipState(*incoming, merged)) {
          incoming = merged;
          pending.push(successor);
        }
      }
    }
  }
  return {};
}

[[nodiscard]] bool isLoanAncestor(const MirBody &body, MirLoanId ancestor,
                                  MirLoanId descendant) {
  if (ancestor == 0 || descendant == 0 || ancestor == descendant) {
    return false;
  }
  std::unordered_set<MirLoanId> visited;
  MirLoanId current = descendant;
  while (current != 0 && visited.insert(current).second) {
    const MirLoan *loan = body.findLoan(current);
    if (loan == nullptr) {
      return false;
    }
    current = loan->parent;
    if (current == ancestor) {
      return true;
    }
  }
  return false;
}

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
  case MirTerminatorKind::Invoke:
    return {terminator.target, terminator.elseTarget};
  case MirTerminatorKind::None:
  case MirTerminatorKind::Return:
  case MirTerminatorKind::PropagateFailure:
  case MirTerminatorKind::Unreachable:
  case MirTerminatorKind::Exit:
  case MirTerminatorKind::ContainFailure:
  case MirTerminatorKind::TerminateCleanupFailure:
    return {};
  }
  return {};
}

// Reference formals and receivers have caller-owned dependencies, not an
// owning formal carrier. Local value copies may retain the same dependency:
// whole-program verification independently proves each exact publication,
// in addition to its concrete owner and reference-field paths. The reference
// formal itself must never become a by-value carrier of this entry.
[[nodiscard]] bool abstractStoredEntry(const MirBody &body,
                                       const MirLoan &loan) {
  if (loan.kind != MirLoanKind::Stored || !loan.entry || loan.escapes ||
      loan.designatedFields.empty() || loan.parent != 0 ||
      loan.storedDependency != 0 || loan.lifecycleBound ||
      loan.producedBy != 0 || loan.storedField != 0)
    return false;
  const MirPlace *source = body.findPlace(loan.source);
  if (source == nullptr || !source->projections.empty())
    return false;
  return (source->root == MirPlaceRootKind::This &&
          source->type.kind == SemanticType::Class) ||
         (source->root == MirPlaceRootKind::Binding &&
          source->type.kind == SemanticType::Reference &&
          source->type.arguments.size() == 1 &&
          source->type.arguments.front().kind == SemanticType::Class &&
          std::find(loan.carriers.begin(), loan.carriers.end(),
                    source->binding) == loan.carriers.end());
}

struct AbstractStoredAccess {
  bool overlaps = false;
  bool referent = false;
  bool referenceStorage = false;
  bool invalidReferentProjection = false;
  bool mutableReferent = false;
};

[[nodiscard]] AbstractStoredAccess
abstractStoredAccess(const MirLoan &loan, const MirCanonicalPlace &place) {
  AbstractStoredAccess result;
  const auto inspectPath = [&](const std::vector<SymbolId> &fields,
                               AccessMode access) {
    if (fields.empty())
      return;
    for (std::size_t index = 0;
         index < std::min(fields.size(), place.projections.size()); ++index)
      if (place.projections[index].kind != MirProjectionKind::Field ||
          place.projections[index].field != fields[index])
        return;
    result.overlaps = true;
    if (place.projections.size() < fields.size())
      return;
    result.referenceStorage =
        result.referenceStorage || place.projections.size() == fields.size();
    result.referent =
        result.referent || (place.projections.size() > fields.size() &&
                            place.projections[fields.size()].kind ==
                                MirProjectionKind::Dereference);
    result.mutableReferent =
        result.mutableReferent || (access == AccessMode::Mutable &&
                                   place.projections.size() > fields.size() &&
                                   place.projections[fields.size()].kind ==
                                       MirProjectionKind::Dereference);
    result.invalidReferentProjection =
        result.invalidReferentProjection ||
        (place.projections.size() > fields.size() &&
         place.projections[fields.size()].kind !=
             MirProjectionKind::Dereference);
  };
  inspectPath(loan.designatedFields, loan.access);
  for (const auto &dependency : loan.additionalDependencies)
    inspectPath(dependency.designatedFields, dependency.access);
  return result;
}

[[nodiscard]] bool
loanPermitsPlace(const MirBody &body, const MirLoan &loan,
                 const MirCanonicalPlace &place, AccessMode requested,
                 const std::unordered_map<HirBindingId, std::vector<MirLoanId>>
                     &bindingLoans,
                 const MirLoanState &state) {
  const auto permits = [&](MirPlaceId source, AccessMode access) {
    return source != 0 &&
           (requested == AccessMode::ReadOnly ||
            access == AccessMode::Mutable) &&
           canonicalPlaceContains(
               canonicalPlace(body, source, bindingLoans, state), place);
  };
  if (loan.entry && loan.kind == MirLoanKind::Stored &&
      !loan.designatedFields.empty()) {
    const auto origin = canonicalPlace(body, loan.source, bindingLoans, state);
    const auto access = abstractStoredAccess(loan, place);
    return sameCanonicalRoot(origin, place) && access.referent &&
           !access.invalidReferentProjection &&
           (requested == AccessMode::ReadOnly || access.mutableReferent);
  }
  return permits(loan.source, loan.access) ||
         std::any_of(loan.additionalDependencies.begin(),
                     loan.additionalDependencies.end(),
                     [&](const auto &dependency) {
                       return permits(dependency.source, dependency.access);
                     });
}

[[nodiscard]] MirVerificationResult verifyMirLoanFlow(const MirBody &body,
                                                      std::size_t owner) {
  std::vector<std::size_t> producerCounts(body.loans.size(), 0);
  std::vector<std::size_t> activationCounts(body.loans.size(), 0);
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  std::vector<std::vector<MirLoanId>> children(body.loans.size());
  std::vector<std::vector<MirLoanId>> dependentShells(body.loans.size());
  for (const MirLoan &loan : body.loans) {
    for (const HirBindingId carrier : loan.carriers) {
      bindingLoans[carrier].push_back(loan.id);
    }
    if (loan.parent != 0) {
      children[loan.parent - 1].push_back(loan.id);
    }
    if (loan.storedDependency != 0) {
      dependentShells[loan.storedDependency - 1].push_back(loan.id);
    }
  }
  const MirLoanState inactive(body.loans.size(), MirLoanFlowState::Inactive);
  for (const MirLoan &loan : body.loans) {
    if (loan.parent == 0) {
      continue;
    }
    const MirLoan &parent = body.loans[loan.parent - 1];
    auto parentSources = parent.additionalSources();
    parentSources.insert(parentSources.begin(), parent.source);
    auto childSources = loan.additionalSources();
    childSources.insert(childSources.begin(), loan.source);
    if (std::any_of(
            childSources.begin(), childSources.end(), [&](MirPlaceId child) {
              const auto childPlace =
                  canonicalPlace(body, child, bindingLoans, inactive);
              return std::none_of(
                  parentSources.begin(), parentSources.end(),
                  [&](MirPlaceId source) {
                    return canonicalPlaceContains(
                        canonicalPlace(body, source, bindingLoans, inactive),
                        childPlace);
                  });
            })) {
      return failure(body, owner,
                     "child loan " + std::to_string(loan.id) +
                         " is not contained within storage protected by "
                         "parent loan " +
                         std::to_string(loan.parent));
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!instruction.loan ||
          instruction.kind == MirInstructionKind::EndBorrow) {
        continue;
      }
      if (instruction.kind == MirInstructionKind::ActivateBorrow) {
        ++activationCounts[*instruction.loan - 1];
        continue;
      }
      if (instruction.kind != MirInstructionKind::Borrow &&
          instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct &&
          instruction.kind != MirInstructionKind::CallInput) {
        return failure(body, owner,
                       "only borrow, call, construct, and call-input "
                       "instructions may produce a loan",
                       block.id, instruction.id);
      }
      if (body.loans[*instruction.loan - 1].twoPhase &&
          (instruction.kind != MirInstructionKind::Call ||
           instruction.borrowOrigin == BorrowOriginKind::None ||
           instruction.borrowAccess != AccessMode::Mutable)) {
        return failure(body, owner,
                       "a two-phase loan must be produced by one mutable "
                       "borrow-returning call",
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
    const std::size_t expectedActivations = loan.twoPhase ? 1 : 0;
    if (activationCounts[index] != expectedActivations) {
      return failure(body, owner,
                     "loan " + std::to_string(index + 1) +
                         (loan.twoPhase
                              ? " must have exactly one activation instruction"
                              : " is not two-phase and must not be activated"));
    }
  }

  std::unordered_set<MirLoanId> conditionalDependencies;
  std::unordered_set<MirLoanId> fieldMappedLoans;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.loan && !instruction.borrowFields.empty())
        fieldMappedLoans.insert(*instruction.loan);
      if (instruction.kind != MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1 ||
          !instruction.lifecycle.front().conditional)
        continue;
      const auto *drop =
          body.findDropObligation(instruction.lifecycle.front().source);
      const auto *place =
          drop == nullptr ? nullptr : body.findPlace(drop->place);
      const auto carrier =
          place == nullptr ? MirTemporaryCarrierDependency{}
                           : findMirTemporaryCarrierDependency(body, *place);
      if (!carrier)
        continue;
      const bool receiver = std::any_of(
          body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
            return (loan.kind == MirLoanKind::CallArgument ||
                    loan.kind == MirLoanKind::CallResult) &&
                   loan.source == place->id &&
                   loan.storedDependency == carrier.loan->id;
          });
      if (!receiver)
        continue;
      for (const MirLoan &loan : body.loans) {
        if (mirLoanRetainsDependency(body, carrier.loan->id, loan.id))
          conditionalDependencies.insert(loan.id);
      }
    }
  }

  std::vector<std::optional<MirLoanState>> blockEntries(body.blocks.size());
  MirLoanState entryState(body.loans.size(), MirLoanFlowState::Inactive);
  for (const MirLoan &loan : body.loans) {
    if (loan.entry) {
      entryState[loan.id - 1] = MirLoanFlowState::Active;
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

    const auto abstractAuthorityForLoan = [&](const MirLoan &dependency,
                                              const MirLoan &derived) {
      if (!abstractStoredEntry(body, dependency))
        return false;
      const auto origin =
          canonicalPlace(body, dependency.source, bindingLoans, active);
      const auto place =
          canonicalPlace(body, derived.source, bindingLoans, active);
      if (origin.ambiguous || place.ambiguous ||
          !sameCanonicalRoot(origin, place))
        return false;
      if (derived.storedDependency == dependency.id)
        return true;
      // A chain borrowing the designated referent is not a carrier-shell
      // reference. Its ordinary parent already protects the physical target;
      // the abstract entry supplies the field's external-owner authority,
      // not a competing sibling borrow. Never extend this to shell storage,
      // unrelated fields, or a stronger access mode.
      const auto access = abstractStoredAccess(dependency, place);
      const auto *derivedSource = body.findPlace(derived.source);
      const auto carrierPrefix = [&](const std::vector<SymbolId> &fields) {
        if (derivedSource == nullptr ||
            derivedSource->type.kind != SemanticType::Class ||
            !derivedSource->traits.containsBorrowedState ||
            place.projections.empty() ||
            place.projections.size() >= fields.size() ||
            (derived.kind != MirLoanKind::CallResult &&
             derived.kind != MirLoanKind::Stored))
          return false;
        for (std::size_t index = 0; index < place.projections.size(); ++index)
          if (place.projections[index].kind != MirProjectionKind::Field ||
              place.projections[index].field != fields[index])
            return false;
        return true;
      };
      // A call on a nested borrowed carrier publishes a loan rooted at that
      // carrier, not at its private reference field. The independently checked
      // call-origin contract supplies the projection into the stored referent.
      // Only an exact nonempty prefix of a designated reference path qualifies;
      // unrelated fields and the enclosing receiver never gain this authority.
      const bool nestedCarrier =
          carrierPrefix(dependency.designatedFields) ||
          std::any_of(dependency.additionalDependencies.begin(),
                      dependency.additionalDependencies.end(),
                      [&](const auto &item) {
                        return carrierPrefix(item.designatedFields);
                      });
      if ((!access.referent && !nestedCarrier) ||
          access.invalidReferentProjection)
        return false;
      const auto *source = body.findPlace(dependency.source);
      const AccessMode shellAccess =
          place.throughLoan != 0 ? body.loans[place.throughLoan - 1].access
          : source->type.kind == SemanticType::Reference
              ? source->type.referenceAccess
              : source->access;
      return derived.access == AccessMode::ReadOnly ||
             (shellAccess == AccessMode::Mutable &&
              dependency.access == AccessMode::Mutable);
    };

    const auto requireStoredDependency =
        [&](MirLoanId loan, const char *context, MirInstructionId instruction)
        -> std::optional<MirVerificationResult> {
      MirLoanId dependency = body.loans[loan - 1].storedDependency;
      while (dependency != 0) {
        if (active[dependency - 1] != MirLoanFlowState::Active) {
          return failure(body, owner,
                         "carrier-shell loan " + std::to_string(loan) +
                             " requires active stored dependency loan " +
                             std::to_string(dependency) + " in " + context,
                         block.id, instruction);
        }
        dependency = body.loans[dependency - 1].storedDependency;
      }
      return std::nullopt;
    };
    const auto requireActive =
        [&](MirLoanId loan, const char *context,
            MirInstructionId instruction =
                0) -> std::optional<MirVerificationResult> {
      if (active[loan - 1] == MirLoanFlowState::Inactive) {
        return failure(body, owner,
                       "loan " + std::to_string(loan) +
                           " is used after its borrow has ended in " + context,
                       block.id, instruction);
      }
      if (active[loan - 1] == MirLoanFlowState::Reserved) {
        return failure(
            body, owner,
            "loan " + std::to_string(loan) +
                " is used before its two-phase borrow is activated in " +
                context,
            block.id, instruction);
      }
      if (active[loan - 1] == MirLoanFlowState::Suspended) {
        return failure(body, owner,
                       "loan " + std::to_string(loan) +
                           " is used while an exclusive child reborrow is "
                           "active in " +
                           context,
                       block.id, instruction);
      }
      if (active[loan - 1] != MirLoanFlowState::Active) {
        return failure(body, owner,
                       "conditionally live temporary dependency cannot "
                       "authorize an executable loan use in " +
                           std::string(context),
                       block.id, instruction);
      }
      return requireStoredDependency(loan, context, instruction);
    };
    const auto checkPlace =
        [&](MirPlaceId placeId, bool write, const char *context,
            MirInstructionId instruction = 0,
            MirLoanId exemptLoan = 0) -> std::optional<MirVerificationResult> {
      const MirCanonicalPlace place =
          canonicalPlace(body, placeId, bindingLoans, active);
      const auto opaqueBorrowCarrierAccess = [&](const MirLoan &loan) {
        const MirPlace *accessed = body.findPlace(placeId);
        const bool storedEntryCarrier =
            loan.kind == MirLoanKind::Stored && loan.entry &&
            accessed != nullptr &&
            accessed->root == MirPlaceRootKind::Binding &&
            std::find(loan.carriers.begin(), loan.carriers.end(),
                      accessed->binding) != loan.carriers.end();
        const bool canonicalStoredEntryCarrier =
            loan.kind == MirLoanKind::Stored && loan.entry &&
            !place.ambiguous && place.root == MirPlaceRootKind::Binding &&
            std::find(loan.carriers.begin(), loan.carriers.end(),
                      place.binding) != loan.carriers.end();
        const bool externalCarrierTransfer =
            loan.kind == MirLoanKind::Return ||
            (body.kind == MirBodyKind::Constructor &&
             loan.kind == MirLoanKind::Stored && loan.storedField != 0);
        const MirPlace *returnSource =
            write && externalCarrierTransfer && loan.escapes
                ? body.findPlace(loan.source)
                : nullptr;
        const auto isReturnedCarrier = [&](const MirPlace *source) {
          if (source == nullptr || accessed == nullptr ||
              source->root != MirPlaceRootKind::Binding)
            return false;
          if (source->traits.containsBorrowedState &&
              source->projections.empty() && accessed->id == source->id)
            return true;
          if (accessed->root != MirPlaceRootKind::Binding ||
              accessed->binding != source->binding ||
              !accessed->projections.empty())
            return false;
          // A field-mapped publication can name the exact external referent
          // instead of the compact parameter shell. Destroying that shell
          // still does not destroy the caller-owned referent. Require the
          // entry contract's checked reference path, even after its local
          // loan has ended; an ordinary owned field earns no exemption.
          const auto referent =
              canonicalPlace(body, source->id, bindingLoans, active);
          return std::any_of(
              body.loans.begin(), body.loans.end(), [&](const MirLoan &entry) {
                const auto *entrySource = body.findPlace(entry.source);
                const bool valueEntry =
                    entry.entry && entry.kind == MirLoanKind::Stored &&
                    entry.parent == 0 && entry.storedDependency == 0 &&
                    !entry.designatedFields.empty() && entrySource != nullptr &&
                    entrySource->root == MirPlaceRootKind::Binding &&
                    entrySource->projections.empty() &&
                    entrySource->traits.containsBorrowedState &&
                    std::find(entry.carriers.begin(), entry.carriers.end(),
                              entrySource->binding) != entry.carriers.end();
                if (!valueEntry && !abstractStoredEntry(body, entry))
                  return false;
                const auto origin =
                    canonicalPlace(body, entry.source, bindingLoans, active);
                if (!sameCanonicalRoot(origin, referent))
                  return false;
                const auto path = abstractStoredAccess(entry, referent);
                return path.referent && !path.invalidReferentProjection;
              });
        };
        const bool returnedCarrier =
            isReturnedCarrier(returnSource) ||
            (write && externalCarrierTransfer && loan.escapes &&
             std::any_of(loan.additionalDependencies.begin(),
                         loan.additionalDependencies.end(),
                         [&](const auto &dependency) {
                           return isReturnedCarrier(
                               body.findPlace(dependency.source));
                         }));
        // The entry loan denotes caller-owned storage that MIR cannot name,
        // not the by-value carrier object itself.  The same is true after an
        // exact stored-value return or constructor-field transfer: the loan
        // protects the caller-owned referent while the moved-from parameter
        // shell still needs scope-end destruction. Accessing the whole carrier
        // — reading it as a receiver, mutating it, or moving it — is
        // independent of that referent, so the stored-entry exception
        // covers both access modes; a read-receiver method on a by-value
        // mut cursor parameter (`first != last`) is the canonical read
        // case. A projection that still carries borrowed state receives no
        // exception, so this cannot reseat, write through, or duplicate
        // the stored reference.
        return returnedCarrier ||
               (canonicalStoredEntryCarrier && place.projections.empty()) ||
               (storedEntryCarrier &&
                (accessed->projections.empty() ||
                 !accessed->traits.containsBorrowedState));
      };
      if (place.ambiguous) {
        return failure(body, owner,
                       "cannot resolve the canonical storage for a place in " +
                           std::string(context),
                       block.id, instruction);
      }
      // An abstract Stored entry names external referents through a checked
      // receiver/formal, not a second borrow of that physical shell. Resolve
      // only its exact declared field paths; the shell's access and the
      // referent's independent access both bound a write through such a path.
      std::unordered_set<MirLoanId> abstractAccesses;
      for (const MirLoan &loan : body.loans) {
        if (active[loan.id - 1] == MirLoanFlowState::Inactive ||
            !abstractStoredEntry(body, loan))
          continue;
        const MirPlace *source = body.findPlace(loan.source);
        const MirCanonicalPlace origin =
            canonicalPlace(body, loan.source, bindingLoans, active);
        if (!sameCanonicalRoot(place, origin))
          continue;
        const AccessMode shellAccess =
            place.throughLoan != 0 ? body.loans[place.throughLoan - 1].access
            : source->type.kind == SemanticType::Reference
                ? source->type.referenceAccess
                : source->access;
        if (write && shellAccess != AccessMode::Mutable) {
          return failure(body, owner,
                         "read-only carrier shell is used for a write in " +
                             std::string(context),
                         block.id, instruction);
        }
        const auto access = abstractStoredAccess(loan, place);
        if (access.invalidReferentProjection) {
          return failure(body, owner,
                         "abstract stored referent access lacks its exact "
                         "designated reference dereference",
                         block.id, instruction);
        }
        if (write && access.referenceStorage) {
          return failure(body, owner,
                         "stored reference field cannot be reseated through "
                         "an abstract carrier dependency",
                         block.id, instruction);
        }
        if (write && access.referent && !access.mutableReferent) {
          return failure(body, owner,
                         "read-only stored dependency is used for a write in " +
                             std::string(context),
                         block.id, instruction);
        }
        abstractAccesses.insert(loan.id);
      }
      if (place.throughLoan != 0) {
        if (auto invalid = requireStoredDependency(place.throughLoan, context,
                                                   instruction)) {
          return invalid;
        }
        const MirLoanFlowState throughState = active[place.throughLoan - 1];
        if (throughState != MirLoanFlowState::Active &&
            throughState != MirLoanFlowState::Suspended) {
          if (auto invalid =
                  requireActive(place.throughLoan, context, instruction)) {
            return invalid;
          }
        } else if (throughState == MirLoanFlowState::Suspended) {
          const bool overlapsDescendant = std::any_of(
              body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
                if (active[loan.id - 1] == MirLoanFlowState::Inactive ||
                    !isLoanAncestor(body, place.throughLoan, loan.id)) {
                  return false;
                }
                const MirCanonicalPlace childPlace =
                    canonicalPlace(body, loan.source, bindingLoans, active);
                return canonicalPlacesOverlap(place, childPlace);
              });
          if (overlapsDescendant) {
            return failure(body, owner,
                           "loan " + std::to_string(place.throughLoan) +
                               " is used through storage covered by an active "
                               "child reborrow in " +
                               context,
                           block.id, instruction);
          }
        }
        if (write &&
            !loanPermitsPlace(body, body.loans[place.throughLoan - 1], place,
                              AccessMode::Mutable, bindingLoans, active) &&
            !opaqueBorrowCarrierAccess(body.loans[place.throughLoan - 1])) {
          return failure(body, owner,
                         "read-only loan " + std::to_string(place.throughLoan) +
                             " is used for a write in " + context,
                         block.id, instruction);
        }
      }

      for (const MirLoan &loan : body.loans) {
        const MirLoanFlowState loanState = active[loan.id - 1];
        if (loanState == MirLoanFlowState::Inactive || loan.id == exemptLoan ||
            loan.id == place.throughLoan ||
            (place.throughLoan != 0 &&
             isLoanAncestor(body, loan.id, place.throughLoan)) ||
            abstractAccesses.contains(loan.id) ||
            opaqueBorrowCarrierAccess(loan)) {
          continue;
        }
        const MirCanonicalPlace protectedPlace =
            canonicalPlace(body, loan.source, bindingLoans, active);
        const auto overlapsProtected = [&](const MirCanonicalPlace &referent) {
          // A copied stored reference points outside its caller-owned carrier.
          // Its exact dereference path must not turn a later mutable use of
          // that carrier's shell into a write of the external referent. The
          // abstract entry above still checks shell access and forbids
          // reference reseating; ordinary references into owned fields get no
          // exception.
          for (const auto entryId : abstractAccesses) {
            const auto &entry = body.loans[entryId - 1];
            const auto origin =
                canonicalPlace(body, entry.source, bindingLoans, active);
            if (!sameCanonicalRoot(origin, referent))
              continue;
            const auto borrowed = abstractStoredAccess(entry, referent);
            const auto accessed = abstractStoredAccess(entry, place);
            if (borrowed.referent && !borrowed.invalidReferentProjection &&
                !accessed.referent && !accessed.referenceStorage &&
                !accessed.invalidReferentProjection)
              return false;
          }
          return canonicalPlacesOverlap(place, referent);
        };
        const bool additionalConflict = std::any_of(
            loan.additionalDependencies.begin(),
            loan.additionalDependencies.end(), [&](const auto &dependency) {
              return dependency.source != 0 &&
                     (write || dependency.access == AccessMode::Mutable) &&
                     overlapsProtected(canonicalPlace(body, dependency.source,
                                                      bindingLoans, active));
            });
        const bool primaryConflict =
            overlapsProtected(protectedPlace) &&
            (write || loan.access == AccessMode::Mutable);
        if (!primaryConflict && !additionalConflict) {
          continue;
        }
        if (loanState == MirLoanFlowState::Reserved && !write) {
          continue;
        }
        return failure(body, owner,
                       std::string(write ? "write" : "read") +
                           " of a place conflicts with active " +
                           (loan.access == AccessMode::Mutable ? "mutable "
                                                               : "read-only ") +
                           "loan " + std::to_string(loan.id) + " in " + context,
                       block.id, instruction);
      }
      return std::nullopt;
    };
    const auto callInputDefinition =
        [&](const MirOperand &operand) -> const MirInstruction * {
      if (operand.kind != MirOperandKind::Value) {
        return nullptr;
      }
      const MirValue *value = body.findValue(operand.value);
      const MirBlock *definitionBlock =
          value == nullptr ? nullptr : body.findBlock(value->definitionBlock);
      if (definitionBlock == nullptr) {
        return nullptr;
      }
      const auto definition =
          std::find_if(definitionBlock->instructions.begin(),
                       definitionBlock->instructions.end(),
                       [&](const MirInstruction &candidate) {
                         return candidate.id == value->definition;
                       });
      return definition != definitionBlock->instructions.end() &&
                     definition->kind == MirInstructionKind::CallInput &&
                     definition->operands.size() == 1
                 ? &*definition
                 : nullptr;
    };
    const auto callInputSource = [&](const MirOperand &operand) {
      const MirInstruction *definition = callInputDefinition(operand);
      return definition == nullptr ? &operand : &definition->operands.front();
    };
    const auto checkOperand =
        [&](const MirOperand &operand, const char *context,
            MirInstructionId instruction =
                0) -> std::optional<MirVerificationResult> {
      const MirInstruction *input = callInputDefinition(operand);
      MirLoanId inputLoan = 0;
      if (input != nullptr && input->loan) {
        if (auto invalid = requireActive(*input->loan, context, instruction)) {
          return invalid;
        }
        inputLoan = *input->loan;
      }
      const MirOperand &resolved = *callInputSource(operand);
      switch (resolved.kind) {
      case MirOperandKind::Loan: {
        if (auto invalid = requireActive(resolved.loan, context, instruction)) {
          return invalid;
        }
        if (resolved.type.kind == SemanticType::Reference &&
            resolved.type.referenceAccess == AccessMode::Mutable &&
            body.loans[resolved.loan - 1].access != AccessMode::Mutable) {
          return failure(body, owner,
                         "read-only loan " + std::to_string(resolved.loan) +
                             " is used as a mutable reference in " + context,
                         block.id, instruction);
        }
        return std::nullopt;
      }
      case MirOperandKind::Copy:
      case MirOperandKind::Address:
      case MirOperandKind::BorrowRead:
        return checkPlace(resolved.place, false, context, instruction,
                          inputLoan);
      case MirOperandKind::Move: {
        const MirPlace *moved = body.findPlace(resolved.place);
        if (moved != nullptr && moved->root == MirPlaceRootKind::Binding &&
            moved->projections.empty()) {
          const auto found = bindingLoans.find(moved->binding);
          MirLoanId carrierLoan = 0;
          bool ambiguous = false;
          if (found != bindingLoans.end()) {
            for (const MirLoanId loan : found->second) {
              if (active[loan - 1] == MirLoanFlowState::Inactive) {
                continue;
              }
              if (carrierLoan != 0) {
                ambiguous = true;
                break;
              }
              carrierLoan = loan;
            }
          }
          if (!ambiguous && carrierLoan != 0) {
            if (auto invalid =
                    requireActive(carrierLoan, context, instruction)) {
              return invalid;
            }
            // A live external dependency does not make a borrowed physical
            // carrier shell movable.
            return checkPlace(resolved.place, true, context, instruction,
                              carrierLoan);
          }
        }
        return checkPlace(resolved.place, true, context, instruction);
      }
      case MirOperandKind::BorrowWrite:
        return checkPlace(resolved.place, true, context, instruction,
                          inputLoan);
      case MirOperandKind::Value:
      case MirOperandKind::Constant:
        return std::nullopt;
      }
      return std::nullopt;
    };
    const auto checkCallBorrowAliases = [&](const MirInstruction &instruction)
        -> std::optional<MirVerificationResult> {
      if (instruction.kind != MirInstructionKind::Call &&
          instruction.kind != MirInstructionKind::Construct) {
        return std::nullopt;
      }
      struct CallBorrow {
        MirCanonicalPlace place;
        bool write = false;
        std::size_t input = 0;
      };
      std::vector<CallBorrow> borrows;
      std::size_t input = 0;
      const auto appendBorrow = [&](const MirOperand &operand) {
        const std::size_t inputIndex = input++;
        const MirOperand &resolved = *callInputSource(operand);
        const auto appendLoanSources = [&](const MirLoan &loan,
                                           bool mutableInput) {
          const auto appendSources = [&](const MirLoan &record, bool write) {
            const auto append = [&](MirPlaceId source, AccessMode permission) {
              if (source == 0)
                return;
              borrows.push_back(
                  {.place = canonicalPlace(body, source, bindingLoans, active),
                   .write = write && permission == AccessMode::Mutable,
                   .input = inputIndex});
            };
            append(record.source, record.access);
            for (const auto &dependency : record.additionalDependencies)
              append(dependency.source, dependency.access);
          };
          bool writable = mutableInput;
          const MirLoan *record = &loan;
          while (record != nullptr) {
            appendSources(*record, writable);
            writable = writable && record->access == AccessMode::Mutable;
            record = record->storedDependency == 0
                         ? nullptr
                         : &body.loans[record->storedDependency - 1];
          }
        };
        if (resolved.kind == MirOperandKind::BorrowRead ||
            resolved.kind == MirOperandKind::BorrowWrite) {
          const MirCanonicalPlace place =
              canonicalPlace(body, resolved.place, bindingLoans, active);
          const bool write = resolved.kind == MirOperandKind::BorrowWrite;
          const MirInstruction *stage = callInputDefinition(operand);
          if (stage != nullptr && stage->loan) {
            appendLoanSources(body.loans[*stage->loan - 1], write);
          }
          borrows.push_back(
              {.place = place, .write = write, .input = inputIndex});
          if (place.throughLoan != 0 &&
              body.loans[place.throughLoan - 1].storedDependency != 0) {
            appendLoanSources(body.loans[place.throughLoan - 1], write);
          }
        } else if (resolved.kind == MirOperandKind::Loan) {
          const MirLoan *loan = body.findLoan(resolved.loan);
          if (loan != nullptr) {
            appendLoanSources(*loan,
                              resolved.type.kind == SemanticType::Reference &&
                                  resolved.type.referenceAccess ==
                                      AccessMode::Mutable);
          }
        }
      };
      if (instruction.receiver) {
        appendBorrow(*instruction.receiver);
      }
      for (const MirOperand &operand : instruction.operands) {
        appendBorrow(operand);
      }
      for (std::size_t left = 0; left < borrows.size(); ++left) {
        for (std::size_t right = left + 1; right < borrows.size(); ++right) {
          // One input's shell and inherited owner form one capability. Alias
          // conflicts compare distinct receiver/argument inputs, not that
          // input's own protected-place inventory against itself.
          if (borrows[left].input == borrows[right].input) {
            continue;
          }
          if (!borrows[left].write && !borrows[right].write) {
            continue;
          }
          if (canonicalPlacesOverlap(borrows[left].place,
                                     borrows[right].place)) {
            return failure(
                body, owner,
                "overlapping call-duration reference operands include a "
                "mutable borrow",
                block.id, instruction.id);
          }
        }
      }
      return std::nullopt;
    };

    std::optional<MirLoanId> activatedForCommit;
    for (const MirInstruction &instruction : block.instructions) {
      if (activatedForCommit) {
        const MirCanonicalPlace destination =
            instruction.destination
                ? canonicalPlace(body, *instruction.destination, bindingLoans,
                                 active)
                : MirCanonicalPlace{};
        if (((instruction.kind != MirInstructionKind::Assign ||
              instruction.operation != MirOperation::Assign) &&
             instruction.kind != MirInstructionKind::CleanupBeforeAssign) ||
            !instruction.destination || destination.ambiguous ||
            destination.throughLoan != *activatedForCommit) {
          return failure(body, owner,
                         "two-phase loan " +
                             std::to_string(*activatedForCommit) +
                             " is not committed by the immediately following "
                             "plain assignment",
                         block.id, instruction.id);
        }
        activatedForCommit.reset();
      }
      const bool hasReservation =
          std::any_of(active.begin(), active.end(), [](MirLoanFlowState state) {
            return state == MirLoanFlowState::Reserved;
          });
      if (hasReservation && (instruction.rawMemoryAccess ||
                             instruction.synchronization.kind !=
                                 SynchronizationOperationKind::None)) {
        return failure(body, owner,
                       "raw memory or synchronization may invalidate an "
                       "active two-phase borrow reservation",
                       block.id, instruction.id);
      }
      if (instruction.kind == MirInstructionKind::EndBorrow) {
        const MirLoanId loan = *instruction.loan;
        const auto &record = body.loans[loan - 1];
        if (body.kind == MirBodyKind::Constructor && record.escapes &&
            record.kind == MirLoanKind::Stored && record.storedField != 0 &&
            block.activeFailure == 0) {
          return failure(body, owner,
                         "constructor field dependency cannot retire on a "
                         "successful construction path",
                         block.id, instruction.id);
        }
        const MirLoanFlowState before = active[loan - 1];
        const bool conditional = conditionalDependencies.contains(loan) &&
                                 !body.loans[loan - 1].entry &&
                                 body.loans[loan - 1].carriers.empty();
        const bool reserved = active[loan - 1] == MirLoanFlowState::Reserved;
        if (active[loan - 1] == MirLoanFlowState::Inactive && !conditional) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is ended while it is inactive",
                         block.id, instruction.id);
        }
        if (loanFlowContains(active[loan - 1], MirLoanFlowState::Suspended)) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is ended before its active child reborrow",
                         block.id, instruction.id);
        }
        if (std::any_of(children[loan - 1].begin(), children[loan - 1].end(),
                        [&](MirLoanId child) {
                          return active[child - 1] !=
                                 MirLoanFlowState::Inactive;
                        })) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is ended before its child reborrow",
                         block.id, instruction.id);
        }
        if (std::any_of(dependentShells[loan - 1].begin(),
                        dependentShells[loan - 1].end(), [&](MirLoanId shell) {
                          return active[shell - 1] !=
                                 MirLoanFlowState::Inactive;
                        })) {
          return failure(body, owner,
                         "stored dependency loan " + std::to_string(loan) +
                             " is ended before its carrier-shell loan",
                         block.id, instruction.id);
        }
        active[loan - 1] = MirLoanFlowState::Inactive;
        const MirLoanId parent = body.loans[loan - 1].parent;
        if (parent != 0) {
          if (before == MirLoanFlowState::Inactive && conditional)
            continue;
          if (reserved) {
            if (active[parent - 1] != MirLoanFlowState::Active) {
              return failure(body, owner,
                             "ending two-phase reservation " +
                                 std::to_string(loan) + " finds parent loan " +
                                 std::to_string(parent) +
                                 " in an inconsistent flow state",
                             block.id, instruction.id);
            }
            continue;
          }
          const auto suspendsParentFor = [&](MirLoanId child) {
            const auto &parentLoan = body.loans[parent - 1];
            return (body.loans[child - 1].kind == MirLoanKind::Local ||
                    fieldMappedLoans.contains(child)) &&
                           parentLoan.kind == MirLoanKind::Stored &&
                           !parentLoan.designatedFields.empty()
                       ? loanPermitsPlace(
                             body, parentLoan,
                             canonicalPlace(body, body.loans[child - 1].source,
                                            bindingLoans, active),
                             AccessMode::Mutable, bindingLoans, active)
                       : parentLoan.access == AccessMode::Mutable;
          };
          const bool siblingSuspends = std::any_of(
              children[parent - 1].begin(), children[parent - 1].end(),
              [&](MirLoanId child) {
                return (loanFlowContains(active[child - 1],
                                         MirLoanFlowState::Active) ||
                        loanFlowContains(active[child - 1],
                                         MirLoanFlowState::Suspended)) &&
                       suspendsParentFor(child);
              });
          const bool suspendsParent =
              suspendsParentFor(loan) || siblingSuspends;
          const MirLoanFlowState expectedParent =
              suspendsParent ? MirLoanFlowState::Suspended
                             : MirLoanFlowState::Active;
          const bool conditionalParent =
              conditional && conditionalDependencies.contains(parent) &&
              !loanFlowContains(active[parent - 1],
                                MirLoanFlowState::Reserved) &&
              loanFlowContains(active[parent - 1], expectedParent);
          if (active[parent - 1] != expectedParent && !conditionalParent) {
            return failure(body, owner,
                           "ending child loan " + std::to_string(loan) +
                               " finds parent loan " + std::to_string(parent) +
                               " in an inconsistent flow state",
                           block.id, instruction.id);
          }
          if (suspendsParent) {
            if (!siblingSuspends) {
              active[parent - 1] = loanFlowContains(active[parent - 1],
                                                    MirLoanFlowState::Inactive)
                                       ? MirLoanFlowState::MaybeActive
                                       : MirLoanFlowState::Active;
            }
          }
        }
        continue;
      }

      if (instruction.loan) {
        if (auto invalid = requireStoredDependency(
                *instruction.loan, "loan production or activation",
                instruction.id)) {
          return *invalid;
        }
      }

      if (instruction.kind == MirInstructionKind::ActivateBorrow) {
        const MirLoanId loan = *instruction.loan;
        const MirLoan &reserved = body.loans[loan - 1];
        if (!reserved.twoPhase ||
            active[loan - 1] != MirLoanFlowState::Reserved) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is activated without one live two-phase "
                             "reservation",
                         block.id, instruction.id);
        }
        const MirLoanId parent = reserved.parent;
        if (parent != 0 && active[parent - 1] != MirLoanFlowState::Active) {
          return failure(body, owner,
                         "two-phase loan " + std::to_string(loan) +
                             " cannot activate while its parent loan is not "
                             "active",
                         block.id, instruction.id);
        }
        const MirCanonicalPlace reservedPlace =
            canonicalPlace(body, reserved.source, bindingLoans, active);
        if (reservedPlace.ambiguous) {
          return failure(body, owner,
                         "two-phase loan " + std::to_string(loan) +
                             " has an ambiguous reserved place",
                         block.id, instruction.id);
        }
        for (const MirLoan &other : body.loans) {
          if (other.id == loan || other.id == parent ||
              active[other.id - 1] == MirLoanFlowState::Inactive ||
              isLoanAncestor(body, other.id, loan) ||
              abstractAuthorityForLoan(other, reserved)) {
            continue;
          }
          const MirCanonicalPlace otherPlace =
              canonicalPlace(body, other.source, bindingLoans, active);
          if (abstractStoredEntry(body, other) &&
              sameCanonicalRoot(reservedPlace, otherPlace) &&
              !abstractStoredAccess(other, reservedPlace).overlaps)
            continue;
          if (canonicalPlacesOverlap(reservedPlace, otherPlace)) {
            return failure(body, owner,
                           "two-phase loan " + std::to_string(loan) +
                               " activates while overlapping loan " +
                               std::to_string(other.id) + " remains live",
                           block.id, instruction.id);
          }
        }
        if (parent != 0 &&
            body.loans[parent - 1].access == AccessMode::Mutable) {
          active[parent - 1] = MirLoanFlowState::Suspended;
        }
        active[loan - 1] = MirLoanFlowState::Active;
        activatedForCommit = loan;
        continue;
      }

      if (instruction.kind == MirInstructionKind::Borrow && instruction.loan &&
          body.loans[*instruction.loan - 1].parent != 0) {
        const MirLoanId loan = *instruction.loan;
        const MirLoanId parent = body.loans[loan - 1].parent;
        if (body.loans[loan - 1].kind == MirLoanKind::Stored) {
          return failure(body, owner,
                         "stored child loan " + std::to_string(loan) +
                             " must be produced by a call or construct",
                         block.id, instruction.id);
        }
        if (active[loan - 1] != MirLoanFlowState::Inactive) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is produced while it is already live",
                         block.id, instruction.id);
        }
        if (instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Loan ||
            instruction.operands.front().loan != parent) {
          return failure(body, owner,
                         "child loan " + std::to_string(loan) +
                             " must borrow from its declared parent loan",
                         block.id, instruction.id);
        }
        if (active[parent - 1] == MirLoanFlowState::Inactive) {
          return failure(body, owner,
                         "child loan " + std::to_string(loan) +
                             " is created after parent loan " +
                             std::to_string(parent) + " has ended",
                         block.id, instruction.id);
        }
        if (active[parent - 1] != MirLoanFlowState::Active &&
            active[parent - 1] != MirLoanFlowState::Suspended) {
          if (auto invalid = requireActive(parent, "direct child reborrow",
                                           instruction.id)) {
            return *invalid;
          }
        }
        const MirCanonicalPlace childPlace = canonicalPlace(
            body, body.loans[loan - 1].source, bindingLoans, active);
        if (!loanPermitsPlace(body, body.loans[parent - 1], childPlace,
                              body.loans[loan - 1].access, bindingLoans,
                              active)) {
          return failure(body, owner,
                         "child loan " + std::to_string(loan) +
                             " cannot exclusively reborrow a read-only parent",
                         block.id, instruction.id);
        }
        for (const MirLoan &other : body.loans) {
          if (other.id == parent ||
              active[other.id - 1] == MirLoanFlowState::Inactive ||
              isLoanAncestor(body, other.id, parent) ||
              abstractAuthorityForLoan(other, body.loans[loan - 1])) {
            continue;
          }
          const MirCanonicalPlace otherPlace =
              canonicalPlace(body, other.source, bindingLoans, active);
          if (abstractStoredEntry(body, other) &&
              sameCanonicalRoot(childPlace, otherPlace) &&
              !abstractStoredAccess(other, childPlace).overlaps)
            continue;
          const bool sharedEphemeralAccess =
              body.loans[loan - 1].kind == MirLoanKind::CallResult &&
              other.kind == MirLoanKind::CallResult &&
              body.loans[loan - 1].access == AccessMode::ReadOnly &&
              other.access == AccessMode::ReadOnly;
          const bool sharedStoredAccess =
              body.loans[parent - 1].kind == MirLoanKind::Stored &&
              body.loans[loan - 1].access == AccessMode::ReadOnly &&
              other.access == AccessMode::ReadOnly &&
              !loanPermitsPlace(body, body.loans[parent - 1], childPlace,
                                AccessMode::Mutable, bindingLoans, active);
          if (canonicalPlacesOverlap(childPlace, otherPlace) &&
              !sharedEphemeralAccess && !sharedStoredAccess) {
            return failure(body, owner,
                           "child loan " + std::to_string(loan) +
                               " conflicts with active sibling loan " +
                               std::to_string(other.id),
                           block.id, instruction.id);
          }
        }
        if (loanPermitsPlace(body, body.loans[parent - 1], childPlace,
                             AccessMode::Mutable, bindingLoans, active)) {
          active[parent - 1] = MirLoanFlowState::Suspended;
        }
        active[loan - 1] = MirLoanFlowState::Active;
        continue;
      }

      if (auto invalid = checkCallBorrowAliases(instruction)) {
        return *invalid;
      }

      const auto permitsSuspendedSharedOrigin = [&](const MirOperand &operand,
                                                    bool selectedOrigin) {
        const MirOperand &resolved = *callInputSource(operand);
        if (!selectedOrigin || instruction.kind != MirInstructionKind::Call ||
            !instruction.loan ||
            (resolved.kind != MirOperandKind::Loan &&
             resolved.kind != MirOperandKind::BorrowRead)) {
          return false;
        }
        const MirLoan &produced = body.loans[*instruction.loan - 1];
        const MirLoanId parent = produced.parent;
        if (parent == 0 || produced.kind != MirLoanKind::CallResult ||
            produced.access != AccessMode::ReadOnly ||
            active[parent - 1] != MirLoanFlowState::Suspended) {
          return false;
        }
        bool throughParent =
            resolved.kind == MirOperandKind::Loan && resolved.loan == parent;
        if (!throughParent && resolved.place != 0) {
          const MirPlace *place = body.findPlace(resolved.place);
          throughParent = place != nullptr &&
                          place->root == MirPlaceRootKind::Binding &&
                          std::find(body.loans[parent - 1].carriers.begin(),
                                    body.loans[parent - 1].carriers.end(),
                                    place->binding) !=
                              body.loans[parent - 1].carriers.end();
          if (!throughParent) {
            throughParent =
                canonicalPlace(body, resolved.place, bindingLoans, active)
                    .throughLoan == parent;
          }
        }
        return throughParent &&
               std::all_of(children[parent - 1].begin(),
                           children[parent - 1].end(), [&](MirLoanId child) {
                             const MirLoan &sibling = body.loans[child - 1];
                             return active[child - 1] ==
                                        MirLoanFlowState::Inactive ||
                                    (sibling.kind == MirLoanKind::CallResult &&
                                     sibling.access == AccessMode::ReadOnly);
                           });
      };

      if (instruction.receiver) {
        const bool selectedOrigin =
            instruction.borrowOrigin == BorrowOriginKind::Receiver;
        if (!permitsSuspendedSharedOrigin(*instruction.receiver,
                                          selectedOrigin)) {
          if (auto invalid = checkOperand(*instruction.receiver, "a receiver",
                                          instruction.id)) {
            return *invalid;
          }
        }
      }
      for (std::size_t operandIndex = 0;
           operandIndex < instruction.operands.size(); ++operandIndex) {
        const MirOperand &operand = instruction.operands[operandIndex];
        const bool selectedOrigin =
            instruction.borrowOrigin == BorrowOriginKind::Argument &&
            instruction.borrowArgument == operandIndex;
        if (!permitsSuspendedSharedOrigin(operand, selectedOrigin)) {
          if (auto invalid = checkOperand(operand, "an instruction operand",
                                          instruction.id)) {
            return *invalid;
          }
        }
      }
      if (instruction.retainedOwnerPlace) {
        const MirPlace *referent =
            instruction.operands.size() == 1
                ? body.findPlace(instruction.operands.front().place)
                : nullptr;
        // The stage's exact, temporary accessor loan supplies execution
        // access; it does not make other aliases to the owner consumable.
        // The local shape and program proof independently bind this loan to
        // this anchor and to a transparent UniqueOwner accessor.
        if (referent == nullptr || referent->root != MirPlaceRootKind::Loan)
          return failure(body, owner,
                         "retained owner has no exact accessor loan", block.id,
                         instruction.id);
        if (auto invalid = checkPlace(*instruction.retainedOwnerPlace, true,
                                      "retained owner consumption",
                                      instruction.id, referent->loan))
          return *invalid;
      }
      if (instruction.destination) {
        bool carrierDrop = false;
        if (instruction.kind == MirInstructionKind::Drop) {
          const MirPlace *dropped = body.findPlace(*instruction.destination);
          if (dropped != nullptr &&
              dropped->root == MirPlaceRootKind::Binding &&
              dropped->projections.empty() &&
              dropped->traits.containsBorrowedState) {
            const auto found = bindingLoans.find(dropped->binding);
            MirLoanId carrierLoan = 0;
            bool ambiguous = false;
            if (found != bindingLoans.end()) {
              for (const MirLoanId loan : found->second) {
                if (active[loan - 1] == MirLoanFlowState::Inactive) {
                  continue;
                }
                if (carrierLoan != 0) {
                  ambiguous = true;
                  break;
                }
                carrierLoan = loan;
              }
            }
            if (!ambiguous && carrierLoan != 0) {
              carrierDrop = true;
              if (auto invalid =
                      requireActive(carrierLoan, "an instruction destination",
                                    instruction.id)) {
                return *invalid;
              }
              if (auto invalid = checkPlace(*instruction.destination, true,
                                            "a carrier shell drop",
                                            instruction.id, carrierLoan)) {
                return *invalid;
              }
            }
          }
        }
        if (!carrierDrop) {
          if (auto invalid =
                  checkPlace(*instruction.destination, true,
                             "an instruction destination", instruction.id)) {
            return *invalid;
          }
        }
      }

      if (instruction.loan) {
        const MirLoanId loan = *instruction.loan;
        const MirLoanId parent = body.loans[loan - 1].parent;
        if (parent != 0) {
          MirLoanId selectedAlias = 0;
          const auto operandFromParent = [&](const MirOperand &operand) {
            const auto mappedAliasDependency = [&](MirLoanId aliasId) {
              const auto *alias = body.findLoan(aliasId);
              const auto &dependency = body.loans[parent - 1];
              const auto &result = body.loans[loan - 1];
              if (alias == nullptr || alias->storedDependency != parent ||
                  active[aliasId - 1] != MirLoanFlowState::Active ||
                  active[parent - 1] != MirLoanFlowState::Active ||
                  dependency.kind != MirLoanKind::Stored ||
                  instruction.kind != MirInstructionKind::Call ||
                  instruction.borrowFields.size() != 1)
                return false;
              const auto &mapping = instruction.borrowFields.front();
              const bool referenceResult =
                  result.kind == MirLoanKind::CallResult &&
                  mapping.resultFields.empty();
              const bool carrierResult =
                  result.kind == MirLoanKind::Stored &&
                  instruction.info.category == ValueCategory::Value &&
                  instruction.info.type.kind == SemanticType::Class &&
                  instruction.info.traits.containsBorrowedState &&
                  !mapping.resultFields.empty() &&
                  result.additionalDependencies.empty() &&
                  (result.designatedFields.empty() ||
                   result.designatedFields == mapping.resultFields);
              if ((!referenceResult && !carrierResult) ||
                  mapping.sourceFields.empty() ||
                  mapping.origin != instruction.borrowOrigin ||
                  mapping.access != result.access ||
                  (mapping.origin == BorrowOriginKind::Argument &&
                   mapping.parameter != instruction.borrowArgument))
                return false;
              // The selected alias explicitly retains this exact dependency;
              // unrelated overlapping owners are not interchangeable. The
              // field map names the referent path, and the independent program
              // check substitutes that path against the selected input type.
              // This body check also verifies active access to the published
              // referent rather than accepting the alias link by itself.
              const bool permitted = loanPermitsPlace(
                  body, dependency,
                  canonicalPlace(body, result.source, bindingLoans, active),
                  result.access, bindingLoans, active);
              if (permitted)
                // This exact input alias authorizes the reborrow; it is not
                // an independent overlapping sibling. No other alias is
                // exempted merely for retaining the same external owner.
                selectedAlias = aliasId;
              return permitted;
            };
            const MirOperand *resolved = callInputSource(operand);
            bool movedCarrier = resolved->kind == MirOperandKind::Move;
            if (resolved->kind == MirOperandKind::Value) {
              const MirValue *value = body.findValue(resolved->value);
              const MirInstruction *producer = nullptr;
              if (value != nullptr) {
                for (const MirBlock &candidateBlock : body.blocks) {
                  const auto found =
                      std::find_if(candidateBlock.instructions.begin(),
                                   candidateBlock.instructions.end(),
                                   [&](const MirInstruction &candidate) {
                                     return candidate.id == value->definition;
                                   });
                  if (found != candidateBlock.instructions.end()) {
                    producer = &*found;
                    break;
                  }
                }
              }
              if (producer != nullptr &&
                  producer->kind == MirInstructionKind::Move &&
                  producer->operands.size() == 1) {
                movedCarrier = true;
                resolved = callInputSource(producer->operands.front());
              }
              if (producer != nullptr && producer->loan &&
                  abstractStoredEntry(body, body.loans[parent - 1])) {
                const auto *retained = body.findLoan(*producer->loan);
                if (retained != nullptr &&
                    active[retained->id - 1] == MirLoanFlowState::Active &&
                    retained->access == AccessMode::ReadOnly &&
                    loanPermitsPlace(body, body.loans[parent - 1],
                                     canonicalPlace(body, retained->source,
                                                    bindingLoans, active),
                                     body.loans[loan - 1].access, bindingLoans,
                                     active))
                  return true;
              }
            }
            if (resolved->kind == MirOperandKind::Loan) {
              return resolved->loan == parent ||
                     mappedAliasDependency(resolved->loan);
            }
            if (resolved->place == 0) {
              return false;
            }
            const MirPlace *place = body.findPlace(resolved->place);
            if (place != nullptr && place->root == MirPlaceRootKind::Binding &&
                (std::find(body.loans[parent - 1].carriers.begin(),
                           body.loans[parent - 1].carriers.end(),
                           place->binding) !=
                     body.loans[parent - 1].carriers.end() ||
                 (place->projections.empty() &&
                  mir_detail::bindingReceivedStoredLoan(body, place->binding,
                                                        parent)))) {
              return true;
            }
            const MirCanonicalPlace operandPlace =
                canonicalPlace(body, resolved->place, bindingLoans, active);
            if (operandPlace.throughLoan == parent)
              return true;
            if (mappedAliasDependency(operandPlace.throughLoan))
              return true;
            if (place != nullptr && place->root == MirPlaceRootKind::Binding) {
              const auto aliases = bindingLoans.find(place->binding);
              if (aliases != bindingLoans.end() &&
                  std::count_if(aliases->second.begin(), aliases->second.end(),
                                [&](MirLoanId id) {
                                  return active[id - 1] !=
                                         MirLoanFlowState::Inactive;
                                }) == 1 &&
                  std::any_of(aliases->second.begin(), aliases->second.end(),
                              mappedAliasDependency))
                return true;
            }
            // An owned argument forwards the external ancestry of its
            // consumed carrier, rather than borrowing that carrier shell.
            // Call-input/lifecycle verification separately proves the exact
            // by-value transfer; this check binds its active stored loan to
            // the declared parent instead of accepting any overlapping owner.
            const auto inheritedParent = [&](MirLoanId id) {
              const auto *transferred = body.findLoan(id);
              return transferred && transferred->kind == MirLoanKind::Stored &&
                     transferred->parent == parent &&
                     active[transferred->id - 1] != MirLoanFlowState::Inactive;
            };
            const auto binding =
                place && place->root == MirPlaceRootKind::Binding &&
                        place->projections.empty()
                    ? bindingLoans.find(place->binding)
                    : bindingLoans.end();
            const bool exactAncestry =
                inheritedParent(operandPlace.throughLoan) ||
                (binding != bindingLoans.end() &&
                 std::count_if(binding->second.begin(), binding->second.end(),
                               [&](MirLoanId id) {
                                 return active[id - 1] !=
                                        MirLoanFlowState::Inactive;
                               }) == 1 &&
                 std::any_of(binding->second.begin(), binding->second.end(),
                             inheritedParent));
            return movedCarrier &&
                   instruction.borrowOrigin == BorrowOriginKind::Argument &&
                   instruction.borrowArgument <
                       instruction.parameterTypes.size() &&
                   instruction.parameterTypes[instruction.borrowArgument]
                           .kind != SemanticType::Reference &&
                   body.loans[loan - 1].kind == MirLoanKind::Stored &&
                   exactAncestry;
          };
          bool selectedOriginFromParent = false;
          if (instruction.borrowOrigin == BorrowOriginKind::Receiver &&
              instruction.receiver) {
            selectedOriginFromParent = operandFromParent(*instruction.receiver);
          } else if (instruction.borrowOrigin == BorrowOriginKind::Argument &&
                     instruction.borrowArgument < instruction.operands.size()) {
            selectedOriginFromParent = operandFromParent(
                instruction.operands[instruction.borrowArgument]);
          }
          const bool derivedCallResult =
              instruction.kind == MirInstructionKind::Call &&
              body.loans[loan - 1].kind == MirLoanKind::CallResult;
          const bool derivedStoredCarrier =
              (instruction.kind == MirInstructionKind::Call ||
               instruction.kind == MirInstructionKind::Construct) &&
              body.loans[loan - 1].kind == MirLoanKind::Stored;
          if ((!derivedCallResult && !derivedStoredCarrier) ||
              !selectedOriginFromParent) {
            return failure(body, owner,
                           "derived loan " + std::to_string(loan) +
                               " must be produced through its declared "
                               "parent loan",
                           block.id, instruction.id);
          }
          if (active[loan - 1] != MirLoanFlowState::Inactive) {
            return failure(body, owner,
                           "loan " + std::to_string(loan) +
                               " is produced while it is already live",
                           block.id, instruction.id);
          }
          if (active[parent - 1] == MirLoanFlowState::Inactive ||
              (body.loans[loan - 1].twoPhase &&
               active[parent - 1] != MirLoanFlowState::Active)) {
            return failure(body, owner,
                           "derived loan " + std::to_string(loan) +
                               " is created without one active parent loan " +
                               std::to_string(parent),
                           block.id, instruction.id);
          }
          const MirCanonicalPlace childPlace = canonicalPlace(
              body, body.loans[loan - 1].source, bindingLoans, active);
          const bool fieldMapped = fieldMappedLoans.contains(loan);
          if (fieldMapped
                  ? !loanPermitsPlace(body, body.loans[parent - 1], childPlace,
                                      body.loans[loan - 1].access, bindingLoans,
                                      active)
                  : (body.loans[parent - 1].access != AccessMode::Mutable &&
                     body.loans[loan - 1].access != AccessMode::ReadOnly)) {
            return failure(body, owner,
                           "derived loan " + std::to_string(loan) +
                               " cannot mutably reborrow a read-only parent",
                           block.id, instruction.id);
          }
          const MirCanonicalPlace parentPlace = canonicalPlace(
              body, body.loans[parent - 1].source, bindingLoans, active);
          if (!fieldMapped &&
              !canonicalPlaceContains(parentPlace, childPlace)) {
            return failure(body, owner,
                           "derived loan " + std::to_string(loan) +
                               " is not contained within storage protected "
                               "by parent loan " +
                               std::to_string(parent),
                           block.id, instruction.id);
          }
          for (const MirLoan &other : body.loans) {
            if (other.id == parent ||
                active[other.id - 1] == MirLoanFlowState::Inactive ||
                isLoanAncestor(body, other.id, parent) ||
                (selectedAlias != 0 &&
                 (other.id == selectedAlias ||
                  isLoanAncestor(body, other.id, selectedAlias))) ||
                abstractAuthorityForLoan(other, body.loans[loan - 1])) {
              continue;
            }
            const MirCanonicalPlace otherPlace =
                canonicalPlace(body, other.source, bindingLoans, active);
            if (abstractStoredEntry(body, other) &&
                sameCanonicalRoot(childPlace, otherPlace) &&
                !abstractStoredAccess(other, childPlace).overlaps)
              continue;
            const bool compatibleReadOnlyDependency =
                (body.loans[loan - 1].kind == MirLoanKind::CallResult ||
                 body.loans[loan - 1].kind == MirLoanKind::Stored) &&
                body.loans[loan - 1].access == AccessMode::ReadOnly &&
                other.access == AccessMode::ReadOnly;
            const bool compatibleReservationRead =
                body.loans[loan - 1].access == AccessMode::ReadOnly &&
                other.twoPhase &&
                active[other.id - 1] == MirLoanFlowState::Reserved;
            if (canonicalPlacesOverlap(childPlace, otherPlace) &&
                !compatibleReadOnlyDependency && !compatibleReservationRead) {
              return failure(body, owner,
                             "derived loan " + std::to_string(loan) +
                                 " conflicts with active sibling loan " +
                                 std::to_string(other.id),
                             block.id, instruction.id);
            }
          }
          if (!body.loans[loan - 1].twoPhase &&
              (fieldMapped
                   ? loanPermitsPlace(body, body.loans[parent - 1], childPlace,
                                      AccessMode::Mutable, bindingLoans, active)
                   : body.loans[parent - 1].access == AccessMode::Mutable)) {
            active[parent - 1] = MirLoanFlowState::Suspended;
          }
          active[loan - 1] = body.loans[loan - 1].twoPhase
                                 ? MirLoanFlowState::Reserved
                                 : MirLoanFlowState::Active;
          continue;
        }
        if (active[loan - 1] != MirLoanFlowState::Inactive) {
          return failure(body, owner,
                         "loan " + std::to_string(loan) +
                             " is produced while it is already live",
                         block.id, instruction.id);
        }
        active[loan - 1] = body.loans[loan - 1].twoPhase
                               ? MirLoanFlowState::Reserved
                               : MirLoanFlowState::Active;
      }
    }

    if (activatedForCommit) {
      return failure(body, owner,
                     "two-phase loan " + std::to_string(*activatedForCommit) +
                         " is activated at the end of a block without its "
                         "immediate committing assignment",
                     block.id);
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
        block.terminator.kind == MirTerminatorKind::PropagateFailure ||
        block.terminator.kind == MirTerminatorKind::ContainFailure ||
        block.terminator.kind == MirTerminatorKind::Exit) {
      for (const MirLoan &loan : body.loans) {
        // A successful constructor publishes every cleanup-bound field along
        // with its dependency. An inactive loan is not harmless here: it
        // would let the constructed guard outlive the resource its destructor
        // still accesses. Failure exits instead retire it after rollback.
        if (body.kind == MirBodyKind::Constructor &&
            block.terminator.kind == MirTerminatorKind::Return &&
            loan.lifecycleBound && loan.kind == MirLoanKind::Stored &&
            loan.escapes && loan.storedField != 0 &&
            active[loan.id - 1] != MirLoanFlowState::Active) {
          return failure(body, owner,
                         "cleanup-bound constructor field dependency is not "
                         "active at successful publication",
                         block.id);
        }
        if (active[loan.id - 1] == MirLoanFlowState::Inactive) {
          continue;
        }
        if (block.terminator.kind == MirTerminatorKind::PropagateFailure) {
          return failure(body, owner,
                         "loan " + std::to_string(loan.id) +
                             " remains active at a failure exit",
                         block.id);
        }
        // A returned shell carries this exact caller-owned dependency out of
        // the body. The entry itself is not globally escaping: every other
        // return and every failure/ordinary exit must still end its lifetime.
        const MirLoan *returned =
            block.terminator.kind == MirTerminatorKind::Return &&
                    block.terminator.returnLoan
                ? body.findLoan(*block.terminator.returnLoan)
                : nullptr;
        if (abstractStoredEntry(body, loan) && returned != nullptr &&
            returned->kind == MirLoanKind::Return && returned->escapes &&
            active[returned->id - 1] == MirLoanFlowState::Active &&
            returned->storedDependency == loan.id) {
          continue;
        }
        if (body.kind == MirBodyKind::Constructor &&
            block.terminator.kind == MirTerminatorKind::Return &&
            abstractStoredEntry(body, loan) &&
            std::any_of(body.loans.begin(), body.loans.end(),
                        [&](const MirLoan &published) {
                          return published.kind == MirLoanKind::Stored &&
                                 published.escapes &&
                                 published.storedField != 0 &&
                                 published.storedDependency == loan.id &&
                                 active[published.id - 1] ==
                                     MirLoanFlowState::Active;
                        }))
          continue;
        if (!loan.escapes) {
          return failure(body, owner,
                         "non-escaping loan " + std::to_string(loan.id) +
                             " remains active at a body exit",
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
      bool changed = false;
      for (std::size_t index = 0; index < active.size(); ++index) {
        if ((*entry)[index] == active[index])
          continue;
        const MirLoanId loan = index + 1;
        if (!conditionalDependencies.contains(loan) ||
            loanFlowContains((*entry)[index], MirLoanFlowState::Reserved) ||
            loanFlowContains(active[index], MirLoanFlowState::Reserved)) {
          return failure(
              body, owner,
              "loan " + std::to_string(loan) +
                  " has inconsistent active/suspended state at CFG join",
              successor);
        }
        const auto joined = static_cast<MirLoanFlowState>(
            static_cast<unsigned>((*entry)[index]) |
            static_cast<unsigned>(active[index]));
        changed = changed || joined != (*entry)[index];
        (*entry)[index] = joined;
      }
      if (changed)
        pending.push(successor);
    }
  }
  return {};
}

using MirLifecycleState = std::vector<OwnershipStateSet>;

[[nodiscard]] MirLifecycleState
joinLifecycleState(const MirLifecycleState &left,
                   const MirLifecycleState &right) {
  MirLifecycleState result = left;
  for (std::size_t index = 0; index < result.size(); ++index) {
    result[index] = result[index] | right[index];
  }
  return result;
}

[[nodiscard]] MirVerificationResult verifyMirLifecycleFlow(const MirBody &body,
                                                           std::size_t owner) {
  if (body.dropObligations.empty()) {
    return {};
  }

  std::unordered_map<MirLoanId, std::vector<MirDropObligationId>> carrierDrops;
  std::unordered_set<MirDropObligationId> receiverCarrierDrops;
  for (const MirDropObligation &drop : body.dropObligations) {
    const MirPlace *place = body.findPlace(drop.place);
    const auto carrier = place == nullptr
                             ? MirTemporaryCarrierDependency{}
                             : findMirTemporaryCarrierDependency(body, *place);
    if (!carrier || carrier.drop->id != drop.id)
      continue;
    const bool receiver = std::any_of(
        body.loans.begin(), body.loans.end(), [&](const MirLoan &loan) {
          return (loan.kind == MirLoanKind::CallArgument ||
                  loan.kind == MirLoanKind::CallResult) &&
                 loan.source == place->id &&
                 loan.storedDependency == carrier.loan->id;
        });
    // A returned/stored carrier may publish its dependency before the later
    // ownership transfer. An ephemeral receiver or a result borrowing its
    // physical storage keeps this exact shell until its value cleanup.
    if (!receiver)
      continue;
    receiverCarrierDrops.insert(drop.id);
    for (const MirLoan &loan : body.loans) {
      if (mirLoanRetainsDependency(body, carrier.loan->id, loan.id))
        carrierDrops[loan.id].push_back(drop.id);
    }
  }

  MirLifecycleState initial(body.dropObligations.size(),
                            OwnershipStateSet::Uninitialized);
  for (const MirDropObligation &obligation : body.dropObligations) {
    if (obligation.initiallyActive) {
      initial[obligation.id - 1] = OwnershipStateSet::Available;
    }
  }
  std::vector<std::optional<MirLifecycleState>> entries(body.blocks.size());
  entries[body.entry - 1] = initial;
  const auto applyInstructions =
      [&](const MirBlock &block, MirLifecycleState &state,
          bool validate) -> std::optional<MirVerificationResult> {
    const auto require = [&](MirDropObligationId obligation,
                             OwnershipState required, const char *operation,
                             MirInstructionId instruction)
        -> std::optional<MirVerificationResult> {
      if (validate && !state[obligation - 1].definitely(required)) {
        return failure(body, owner,
                       std::string(operation) + " requires obligation " +
                           std::to_string(obligation) +
                           " in a definite state (actual " +
                           std::to_string(state[obligation - 1].bits) + ")",
                       block.id, instruction);
      }
      return std::nullopt;
    };

    for (const MirInstruction &instruction : block.instructions) {
      if (validate && instruction.kind == MirInstructionKind::EndBorrow &&
          instruction.loan) {
        const auto *loan = body.findLoan(*instruction.loan);
        if (body.kind == MirBodyKind::Constructor && loan != nullptr &&
            loan->kind == MirLoanKind::Stored && loan->escapes &&
            loan->storedField != 0) {
          for (const auto &drop : body.dropObligations) {
            if (drop.kind == MirDropObligationKind::ConstructionRollback) {
              if (auto invalid = require(
                      drop.id, OwnershipState::Uninitialized,
                      "constructor dependency retirement after rollback",
                      instruction.id))
                return *invalid;
            }
          }
        }
        const auto found = carrierDrops.find(*instruction.loan);
        if (found != carrierDrops.end()) {
          for (const auto drop : found->second) {
            if (auto invalid = require(
                    drop, OwnershipState::Uninitialized,
                    "temporary carrier dependency retirement", instruction.id))
              return *invalid;
          }
        }
      }
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        if (validate && receiverCarrierDrops.contains(event.source) &&
            event.kind != MirLifecycleEventKind::Drop) {
          return failure(body, owner,
                         "ephemeral receiver carrier must retain its exact "
                         "value obligation until cleanup",
                         block.id, instruction.id);
        }
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          if (auto invalid =
                  require(event.target, OwnershipState::Uninitialized,
                          "initialization", instruction.id)) {
            return *invalid;
          }
          state[event.target - 1] = OwnershipStateSet::Available;
          break;
        case MirLifecycleEventKind::Move:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "move", instruction.id)) {
            return *invalid;
          }
          if (auto invalid =
                  require(event.target, OwnershipState::Uninitialized,
                          "move destination", instruction.id)) {
            return *invalid;
          }
          state[event.source - 1] = OwnershipStateSet::Moved;
          state[event.target - 1] = OwnershipStateSet::Available;
          break;
        case MirLifecycleEventKind::Reparent:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "reparent", instruction.id)) {
            return *invalid;
          }
          if (auto invalid =
                  require(event.target, OwnershipState::Uninitialized,
                          "reparent destination", instruction.id)) {
            return *invalid;
          }
          state[event.source - 1] = OwnershipStateSet::Uninitialized;
          state[event.target - 1] = OwnershipStateSet::Available;
          break;
        case MirLifecycleEventKind::Replace:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "replacement source", instruction.id)) {
            return *invalid;
          }
          if (validate && event.target != 0 &&
              state[event.target - 1].contains(OwnershipState::Uninitialized)) {
            return failure(body, owner,
                           "replacement targets an inactive obligation",
                           block.id, instruction.id);
          }
          state[event.source - 1] = OwnershipStateSet::Moved;
          if (event.target != 0) {
            state[event.target - 1] = OwnershipStateSet::Available;
          }
          break;
        case MirLifecycleEventKind::TransferOut:
          if (auto invalid = require(event.source, OwnershipState::Available,
                                     "transfer", instruction.id)) {
            return *invalid;
          }
          state[event.source - 1] = OwnershipStateSet::Uninitialized;
          break;
        case MirLifecycleEventKind::Drop: {
          const OwnershipStateSet before = state[event.source - 1];
          if (validate && before.definitely(OwnershipState::Uninitialized)) {
            return failure(body, owner, "drop targets an inactive obligation",
                           block.id, instruction.id);
          }
          const bool requiresConditional =
              before.contains(OwnershipState::Uninitialized);
          if (validate && event.conditional != requiresConditional) {
            return failure(body, owner,
                           "drop conditionality disagrees with path state",
                           block.id, instruction.id);
          }
          state[event.source - 1] = OwnershipStateSet::Uninitialized;
          break;
        }
        }
      }
      if (validate && instruction.fullExpressionEnd != 0) {
        for (const MirDropObligation &obligation : body.dropObligations) {
          if (obligation.fullExpression != instruction.fullExpressionEnd) {
            continue;
          }
          if (!state[obligation.id - 1].definitely(
                  OwnershipState::Uninitialized)) {
            return failure(
                body, owner,
                "full-expression boundary retains active drop obligation " +
                    std::to_string(obligation.id),
                block.id, instruction.id);
          }
        }
      }
      if (validate && instruction.cleanupBoundaryEnd != 0) {
        const MirCleanupBoundary &boundary =
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1];
        for (const MirDropObligationId obligation : boundary.obligations) {
          if (!state[obligation - 1].definitely(
                  OwnershipState::Uninitialized)) {
            return failure(
                body, owner,
                "lexical cleanup boundary retains active drop obligation " +
                    std::to_string(obligation),
                block.id, instruction.id);
          }
        }
      }
    }
    return std::nullopt;
  };

  const auto applyInvokeSuccess =
      [&](const MirBlock &block, MirLifecycleState &state,
          bool validate) -> std::optional<MirVerificationResult> {
    for (const MirLifecycleEvent &event : block.terminator.successLifecycle) {
      if (validate && (event.kind != MirLifecycleEventKind::Initialize ||
                       event.target == 0 || event.target > state.size() ||
                       !state[event.target - 1].definitely(
                           OwnershipState::Uninitialized))) {
        return failure(body, owner,
                       "invoke success initialization requires an exact "
                       "uninitialized obligation",
                       block.id, block.terminator.invokeInstruction);
      }
      if (event.target != 0 && event.target <= state.size()) {
        state[event.target - 1] = OwnershipStateSet::Available;
      }
    }
    return std::nullopt;
  };

  // First converge every block entry. Validating while the worklist is still
  // incomplete is order-dependent: a conditional drop may initially see only
  // the inactive predecessor and reject before the active predecessor joins.
  std::queue<MirBlockId> pending;
  pending.push(body.entry);
  while (!pending.empty()) {
    const MirBlockId blockId = pending.front();
    pending.pop();
    const MirBlock &block = body.blocks[blockId - 1];
    MirLifecycleState state = *entries[blockId - 1];
    (void)applyInstructions(block, state, false);

    for (const MirBlockId successor : successors(block.terminator)) {
      MirLifecycleState outgoing = state;
      if (block.terminator.kind == MirTerminatorKind::Invoke &&
          successor == block.terminator.target) {
        (void)applyInvokeSuccess(block, outgoing, false);
      }
      std::optional<MirLifecycleState> &entry = entries[successor - 1];
      const MirLifecycleState merged =
          entry ? joinLifecycleState(*entry, outgoing) : outgoing;
      if (!entry || *entry != merged) {
        entry = merged;
        pending.push(successor);
      }
    }
  }

  // Then validate each reachable block against its final joined entry state.
  for (const MirBlock &block : body.blocks) {
    if (!entries[block.id - 1]) {
      continue;
    }
    MirLifecycleState state = *entries[block.id - 1];
    if (auto invalid = applyInstructions(block, state, true)) {
      return *invalid;
    }

    if (block.terminator.kind == MirTerminatorKind::Return ||
        block.terminator.kind == MirTerminatorKind::PropagateFailure ||
        block.terminator.kind == MirTerminatorKind::ContainFailure ||
        block.terminator.kind == MirTerminatorKind::Exit) {
      const auto live = std::find_if(
          state.begin(), state.end(), [](OwnershipStateSet candidate) {
            return !candidate.definitely(OwnershipState::Uninitialized);
          });
      if (live != state.end()) {
        return failure(
            body, owner,
            "body exit retains active drop obligation " +
                std::to_string(std::distance(state.begin(), live) + 1),
            block.id);
      }
    }

    if (block.terminator.kind == MirTerminatorKind::Invoke) {
      MirLifecycleState success = state;
      if (auto invalid = applyInvokeSuccess(block, success, true)) {
        return *invalid;
      }
    }
  }
  return {};
}

} // namespace

bool mirCanonicalPlaceContains(const MirBody &body, MirPlaceId parent,
                               MirPlaceId child) {
  std::unordered_map<HirBindingId, std::vector<MirLoanId>> bindingLoans;
  for (const MirLoan &loan : body.loans)
    for (const HirBindingId carrier : loan.carriers)
      bindingLoans[carrier].push_back(loan.id);
  const MirLoanState inactive(body.loans.size(), MirLoanFlowState::Inactive);
  return canonicalPlaceContains(
      canonicalPlace(body, parent, bindingLoans, inactive),
      canonicalPlace(body, child, bindingLoans, inactive));
}

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
        if (projection.index == 0) {
          continue;
        }
        addUse({.value = projection.index,
                .kind = MirValueUseKind::PlaceIndex,
                .place = place.id});
      }
    }
  }

  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone) {
        continue;
      }
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
    const auto addEdgeUses = [&](MirBlockId target, std::size_t edgeIndex,
                                 const std::vector<MirOperand> &arguments) {
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index].kind != MirOperandKind::Value) {
          continue;
        }
        addUse({.value = arguments[index].value,
                .kind = MirValueUseKind::EdgeArgument,
                .block = block.id,
                .edgeTarget = target,
                .edgeIndex = edgeIndex,
                .operandIndex = index});
      }
    };
    addEdgeUses(block.terminator.target, 0, block.terminator.targetArguments);
    addEdgeUses(block.terminator.elseTarget, 1, block.terminator.elseArguments);
    for (std::size_t index = 0; index < block.terminator.switchTargets.size();
         ++index) {
      const MirSwitchTarget &target = block.terminator.switchTargets[index];
      addEdgeUses(target.target, index + 2, target.arguments);
    }
  }
  return valid;
}

std::optional<MirScalarPromotionRewrite>
rewriteMirScalarPlace(MirBody &body, MirPlaceId placeId) {
  if (!verifyMirBody(body).valid()) {
    return std::nullopt;
  }
  const MirPlace *sourcePlace = body.findPlace(placeId);
  if (sourcePlace == nullptr ||
      sourcePlace->root != MirPlaceRootKind::Binding ||
      sourcePlace->binding == 0 || !sourcePlace->projections.empty() ||
      sourcePlace->access != AccessMode::Mutable ||
      sourcePlace->initiallyAvailable ||
      !isMirBlockParameterType(sourcePlace->type) ||
      sourcePlace->traits.drop != DropKind::Trivial ||
      sourcePlace->traits.containsBorrowedState ||
      !body.failureRecords.empty() || !body.cleanupBoundaries.empty() ||
      std::any_of(
          body.loans.begin(), body.loans.end(),
          [&](const MirLoan &loan) { return loan.protectsSource(placeId); }) ||
      std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                  [&](const MirDropObligation &drop) {
                    return drop.place == placeId;
                  })) {
    return std::nullopt;
  }
  const MirPlace place = *sourcePlace;
  const auto sameStorage = [&](const MirPlace &other) {
    if (other.id == place.id) {
      return false;
    }
    if (place.key && other.key) {
      return *place.key == *other.key;
    }
    return other.root == MirPlaceRootKind::Binding &&
           other.binding == place.binding;
  };
  if (std::any_of(body.places.begin(), body.places.end(), sameStorage)) {
    return std::nullopt;
  }

  std::vector<MirInstructionId> writes;
  std::vector<MirInstructionId> loads;
  const MirInstruction *provenanceLoad = nullptr;
  for (const MirBlock &block : body.blocks) {
    if (!block.reachable || block.failureParameter != 0 ||
        block.activeFailure != 0 ||
        block.terminator.kind == MirTerminatorKind::Invoke ||
        (block.terminator.value && block.terminator.value->place == placeId)) {
      return std::nullopt;
    }
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone) {
        continue;
      }
      const bool destination =
          instruction.destination && *instruction.destination == placeId;
      const bool operand = std::any_of(
          instruction.operands.begin(), instruction.operands.end(),
          [&](const MirOperand &value) { return value.place == placeId; });
      const bool receiver =
          instruction.receiver && instruction.receiver->place == placeId;
      if (!destination && !operand && !receiver &&
          instruction.successResultDestination != placeId) {
        continue;
      }
      const bool neutral =
          instruction.unsafeOperation == UnsafeOperationKind::None &&
          !instruction.rawMemoryAccess &&
          instruction.synchronization.kind ==
              SynchronizationOperationKind::None &&
          instruction.definedFailure.empty() &&
          instruction.localFailureSites.empty() &&
          instruction.lifecycle.empty() && instruction.fullExpressionEnd == 0 &&
          instruction.cleanupBoundaryEnd == 0 && !receiver;
      if (destination) {
        const bool simpleWrite =
            neutral && instruction.operands.size() == 1 &&
            (instruction.operands.front().kind == MirOperandKind::Value ||
             instruction.operands.front().kind == MirOperandKind::Constant) &&
            (instruction.kind == MirInstructionKind::Initialize ||
             (instruction.kind == MirInstructionKind::Assign &&
              instruction.operation == MirOperation::Assign));
        if (!simpleWrite) {
          return std::nullopt;
        }
        writes.push_back(instruction.id);
        continue;
      }
      const bool simpleLoad =
          neutral && instruction.kind == MirInstructionKind::Load &&
          instruction.result && instruction.operands.size() == 1 &&
          instruction.operands.front().kind == MirOperandKind::Copy;
      if (!simpleLoad) {
        return std::nullopt;
      }
      loads.push_back(instruction.id);
      if (provenanceLoad == nullptr) {
        provenanceLoad = &instruction;
      }
    }
  }
  if (writes.empty() || loads.empty() || provenanceLoad == nullptr ||
      provenanceLoad->hirValue == 0) {
    return std::nullopt;
  }

  const auto successorIds = [](const MirTerminator &terminator) {
    std::vector<MirBlockId> result;
    switch (terminator.kind) {
    case MirTerminatorKind::Goto:
      result.push_back(terminator.target);
      break;
    case MirTerminatorKind::Branch:
      result.push_back(terminator.target);
      result.push_back(terminator.elseTarget);
      break;
    case MirTerminatorKind::Switch:
      result.push_back(terminator.target);
      for (const MirSwitchTarget &target : terminator.switchTargets) {
        result.push_back(target.target);
      }
      break;
    case MirTerminatorKind::None:
    case MirTerminatorKind::Invoke:
    case MirTerminatorKind::Return:
    case MirTerminatorKind::PropagateFailure:
    case MirTerminatorKind::Unreachable:
    case MirTerminatorKind::Exit:
    case MirTerminatorKind::ContainFailure:
    case MirTerminatorKind::TerminateCleanupFailure:
      break;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
  };

  std::vector<std::vector<MirBlockId>> predecessors(body.blocks.size());
  std::vector<std::size_t> incoming(body.blocks.size(), 0);
  for (const MirBlock &block : body.blocks) {
    for (const MirBlockId target : successorIds(block.terminator)) {
      if (target == 0 || target > body.blocks.size()) {
        return std::nullopt;
      }
      predecessors[target - 1].push_back(block.id);
      ++incoming[target - 1];
    }
  }
  std::queue<MirBlockId> ready;
  for (std::size_t index = 0; index < incoming.size(); ++index) {
    if (incoming[index] == 0) {
      ready.push(index + 1);
    }
  }
  std::vector<MirBlockId> order;
  while (!ready.empty()) {
    const MirBlockId block = ready.front();
    ready.pop();
    order.push_back(block);
    for (const MirBlockId target :
         successorIds(body.blocks[block - 1].terminator)) {
      if (--incoming[target - 1] == 0) {
        ready.push(target);
      }
    }
  }
  if (order.size() != body.blocks.size() || order.front() != body.entry) {
    return std::nullopt;
  }

  MirBody candidate = body;
  MirScalarPromotionRewrite rewrite{.place = placeId};
  std::vector<std::optional<MirOperand>> outgoing(candidate.blocks.size());
  const auto appendArgument = [](MirTerminator &terminator, MirBlockId target,
                                 const MirOperand &argument) {
    bool matched = false;
    if ((terminator.kind == MirTerminatorKind::Goto ||
         terminator.kind == MirTerminatorKind::Branch ||
         terminator.kind == MirTerminatorKind::Switch) &&
        terminator.target == target) {
      terminator.targetArguments.push_back(argument);
      matched = true;
    }
    if (terminator.kind == MirTerminatorKind::Branch &&
        terminator.elseTarget == target) {
      terminator.elseArguments.push_back(argument);
      matched = true;
    }
    if (terminator.kind == MirTerminatorKind::Switch) {
      for (MirSwitchTarget &edge : terminator.switchTargets) {
        if (edge.target == target) {
          edge.arguments.push_back(argument);
          matched = true;
        }
      }
    }
    return matched;
  };
  const auto replacementIdentity = [](const MirInstruction &source,
                                      MirOperand operand) {
    MirInstruction result;
    result.id = source.id;
    result.kind = MirInstructionKind::Compute;
    result.hirValue = source.hirValue;
    result.hirStatement = source.hirStatement;
    result.result = source.result;
    result.operands.push_back(std::move(operand));
    result.operation = MirOperation::Identity;
    result.info = source.info;
    return result;
  };

  for (const MirBlockId blockId : order) {
    MirBlock &block = candidate.blocks[blockId - 1];
    std::optional<MirOperand> current;
    const std::vector<MirBlockId> &incomingBlocks = predecessors[blockId - 1];
    if (blockId != candidate.entry) {
      if (incomingBlocks.empty()) {
        return std::nullopt;
      }
      for (const MirBlockId predecessor : incomingBlocks) {
        if (!outgoing[predecessor - 1]) {
          return std::nullopt;
        }
        if (!current) {
          current = outgoing[predecessor - 1];
        } else if (*current != *outgoing[predecessor - 1]) {
          current.reset();
          break;
        }
      }
      if (!current) {
        const MirValueId parameterValue = candidate.values.size() + 1;
        ExpressionInfo info = provenanceLoad->info;
        info.type = place.type;
        candidate.values.push_back({.id = parameterValue,
                                    .sourceValue = provenanceLoad->hirValue,
                                    .info = info,
                                    .definitionBlock = blockId});
        block.parameters.push_back(
            {.value = parameterValue,
             .type = place.type,
             .sourceValue = provenanceLoad->hirValue,
             .sourceStatement = provenanceLoad->hirStatement});
        rewrite.parameterBlocks.push_back(blockId);
        rewrite.createdValues.push_back(parameterValue);
        for (const MirBlockId predecessor : incomingBlocks) {
          if (!appendArgument(candidate.blocks[predecessor - 1].terminator,
                              blockId, *outgoing[predecessor - 1])) {
            return std::nullopt;
          }
        }
        current = MirOperand{.kind = MirOperandKind::Value,
                             .value = parameterValue,
                             .type = place.type};
      }
    }

    for (MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone) {
        continue;
      }
      const bool write = instruction.destination == placeId;
      const bool load = instruction.kind == MirInstructionKind::Load &&
                        instruction.operands.size() == 1 &&
                        instruction.operands.front().place == placeId;
      if (write) {
        current = instruction.operands.front();
        const bool liveResult =
            instruction.result &&
            !body.valueUses[*instruction.result - 1].empty();
        if (liveResult) {
          instruction = replacementIdentity(instruction, *current);
          rewrite.replacedInstructions.push_back(instruction.id);
        } else {
          const MirInstructionId id = instruction.id;
          instruction = makeMirInstructionTombstone(instruction);
          rewrite.removedInstructions.push_back(id);
        }
      } else if (load) {
        if (!current || !instruction.result) {
          return std::nullopt;
        }
        instruction = replacementIdentity(instruction, *current);
        rewrite.replacedInstructions.push_back(instruction.id);
      }
    }
    outgoing[blockId - 1] = current;
  }

  std::sort(rewrite.parameterBlocks.begin(), rewrite.parameterBlocks.end());
  std::sort(rewrite.replacedInstructions.begin(),
            rewrite.replacedInstructions.end());
  std::sort(rewrite.removedInstructions.begin(),
            rewrite.removedInstructions.end());
  if (!rebuildMirValueUses(candidate)) {
    return std::nullopt;
  }
  rebuildMirReachability(candidate);
  if (!verifyMirBody(candidate).valid()) {
    return std::nullopt;
  }
  body = std::move(candidate);
  return rewrite;
}

std::optional<MirDeadInstructionRewrite>
rewriteMirDeadInstruction(MirProgram &program, MirBodyAddress address,
                          MirInstructionId instructionId) {
  if (!verifyMirProgram(program).valid()) {
    return std::nullopt;
  }
  MirProgram candidate = program;
  MirBody *body = findMirBody(candidate, address);
  if (body == nullptr || instructionId == 0) {
    return std::nullopt;
  }
  MirBlock *ownerBlock = nullptr;
  MirInstruction *target = nullptr;
  for (MirBlock &block : body->blocks) {
    const auto found =
        std::find_if(block.instructions.begin(), block.instructions.end(),
                     [&](const MirInstruction &instruction) {
                       return instruction.id == instructionId;
                     });
    if (found != block.instructions.end()) {
      ownerBlock = &block;
      target = &*found;
      break;
    }
  }
  if (ownerBlock == nullptr || target == nullptr || target->tombstone ||
      target->kind != MirInstructionKind::Compute || !target->result ||
      !body->usesOf(*target->result).empty()) {
    return std::nullopt;
  }
  const MirEffectTraits effect = effects(candidate, *target);
  const bool neutral =
      effect.removableWhenUnused && !effect.mayTrap && !effect.writesPlace &&
      !effect.readsUnknownMemory && !effect.writesUnknownMemory &&
      !effect.allocates && !effect.invokesRuntime && !effect.startsLoan &&
      !effect.endsLoan && !effect.dependsOnLoan && !effect.movesValue &&
      !effect.initializesValue && !effect.dropsValue &&
      !effect.invokesUserCode && !effect.targetDependent &&
      !effect.maySynchronize && !target->ownership &&
      target->lifecycle.empty() && target->fullExpressionEnd == 0 &&
      target->cleanupBoundaryEnd == 0 && target->definedFailure.empty() &&
      target->localFailureSites.empty() && !target->rawMemoryAccess &&
      target->unsafeOperation == UnsafeOperationKind::None &&
      target->hostedStartupOperation == 0 && !target->loan &&
      !target->programConstantSubstitution;
  if (!neutral) {
    return std::nullopt;
  }

  const MirDeadInstructionRewrite rewrite{.body = address,
                                          .block = ownerBlock->id,
                                          .instruction = target->id,
                                          .sourceValue = target->hirValue,
                                          .sourceStatement =
                                              target->hirStatement,
                                          .result = target->result};
  *target = makeMirInstructionTombstone(*target);
  if (!rebuildMirValueUses(*body)) {
    return std::nullopt;
  }
  rebuildMirReachability(*body);
  if (!verifyMirProgram(candidate).valid()) {
    return std::nullopt;
  }
  program = std::move(candidate);
  return rewrite;
}

std::optional<MirUnreachableBlockRewrite>
rewriteMirUnreachableBlock(MirBody &body, MirBlockId blockId) {
  if (!verifyMirBody(body).valid()) {
    return std::nullopt;
  }
  const MirBlock *source = body.findBlock(blockId);
  if (source == nullptr || source->id == body.entry || source->reachable ||
      source->programInitializationStep != 0 || source->failureParameter != 0 ||
      source->activeFailure != 0 || !source->parameters.empty() ||
      !body.loans.empty() || !body.cleanupBoundaries.empty() ||
      !body.dropObligations.empty() || !body.failureRecords.empty() ||
      !body.rangeIterations.empty() ||
      std::any_of(source->instructions.begin(), source->instructions.end(),
                  [](const MirInstruction &instruction) {
                    return instruction.hostedStartupOperation != 0 ||
                           instruction.constructorInitializer != 0 ||
                           instruction.preparedParameterDrop ||
                           instruction.successResultDrop ||
                           instruction.successResultDestination ||
                           instruction.loan || instruction.ownership ||
                           !instruction.lifecycle.empty() ||
                           instruction.fullExpressionEnd != 0 ||
                           instruction.cleanupBoundaryEnd != 0 ||
                           !instruction.definedFailure.empty() ||
                           !instruction.localFailureSites.empty() ||
                           instruction.rawMemoryAccess ||
                           instruction.synchronization.kind !=
                               SynchronizationOperationKind::None;
                  }) ||
      !source->terminator.successLifecycle.empty() ||
      source->terminator.hostedStartupOperation != 0 ||
      source->terminator.failureRecord != 0 ||
      source->terminator.invokeInstruction != 0 ||
      source->terminator.returnLoan) {
    return std::nullopt;
  }

  MirBody candidate = body;
  MirBlock &block = *candidate.findBlock(blockId);
  MirUnreachableBlockRewrite rewrite{.block = blockId,
                                     .sourceValue = block.terminator.hirValue,
                                     .sourceStatement =
                                         block.terminator.hirStatement};
  for (MirInstruction &instruction : block.instructions) {
    rewrite.removedInstructions.push_back(instruction.id);
    instruction = makeMirInstructionTombstone(instruction);
  }
  block.terminator = {.kind = MirTerminatorKind::Unreachable,
                      .hirValue = block.terminator.hirValue,
                      .hirStatement = block.terminator.hirStatement};
  if (!rebuildMirValueUses(candidate)) {
    return std::nullopt;
  }
  rebuildMirReachability(candidate);
  if (!verifyMirBody(candidate).valid()) {
    return std::nullopt;
  }
  body = std::move(candidate);
  return rewrite;
}

namespace {

[[nodiscard]] const MirInstruction *linearValueDefinition(const MirBody &body,
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
                     return instruction.id == value->definition &&
                            instruction.result == valueId;
                   });
  return found == block->instructions.end() ? nullptr : &*found;
}

[[nodiscard]] bool hasOneExactValueUse(const MirBody &body, MirValueId valueId,
                                       MirValueUseKind expectedKind,
                                       MirInstructionId expectedInstruction,
                                       std::size_t expectedOperandIndex) {
  std::size_t count = 0;
  bool matched = false;
  const auto record = [&](MirValueUseKind kind,
                          MirInstructionId instruction = 0,
                          std::size_t operandIndex = 0) {
    ++count;
    matched = matched ||
              (kind == expectedKind && instruction == expectedInstruction &&
               (kind != MirValueUseKind::InstructionOperand ||
                operandIndex == expectedOperandIndex));
  };

  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value && place.value == valueId) {
      record(MirValueUseKind::PlaceRoot);
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Index ||
           projection.kind == MirProjectionKind::RawIndex) &&
          projection.index == valueId) {
        record(MirValueUseKind::PlaceIndex);
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value &&
          instruction.receiver->value == valueId) {
        record(MirValueUseKind::InstructionReceiver, instruction.id);
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind == MirOperandKind::Value &&
            instruction.operands[index].value == valueId) {
          record(MirValueUseKind::InstructionOperand, instruction.id, index);
        }
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value &&
        block.terminator.value->value == valueId) {
      record(MirValueUseKind::Terminator);
    }
  }
  return count == 1 && matched;
}

[[nodiscard]] bool consumedCallableReceiver(
    const MirBody &body, const MirOperand &operand,
    MirValueUseKind expectedUseKind, MirInstructionId expectedUseInstruction,
    std::size_t expectedOperandIndex = 0, std::size_t depth = 0) {
  if (depth > body.values.size() + body.instructionCount() ||
      operand.kind != MirOperandKind::Value) {
    return false;
  }
  if (!hasOneExactValueUse(body, operand.value, expectedUseKind,
                           expectedUseInstruction, expectedOperandIndex)) {
    // Owning moves retain one non-executable root for their full-expression
    // cleanup. It is not another invocation or an alias through which the
    // consumed receiver can be observed.
    std::size_t executableUses = 0;
    std::size_t lifetimeRoots = 0;
    const MirValue *value = body.findValue(operand.value);
    for (const MirValueUse &use : body.usesOf(operand.value)) {
      if (use.kind == expectedUseKind &&
          use.instruction == expectedUseInstruction &&
          use.operandIndex == expectedOperandIndex) {
        ++executableUses;
        continue;
      }
      const MirPlace *place = use.kind == MirValueUseKind::PlaceRoot
                                  ? body.findPlace(use.place)
                                  : nullptr;
      if (value == nullptr || place == nullptr ||
          place->root != MirPlaceRootKind::Value ||
          place->value != operand.value || !place->projections.empty() ||
          place->type != value->info.type ||
          std::count_if(body.dropObligations.begin(),
                        body.dropObligations.end(), [&](const auto &drop) {
                          return drop.kind == MirDropObligationKind::Value &&
                                 drop.place == place->id &&
                                 drop.value == value->sourceValue &&
                                 drop.fullExpression != 0 &&
                                 drop.dropType.type == value->info.type;
                        }) != 1) {
        return false;
      }
      ++lifetimeRoots;
    }
    if (executableUses != 1 || lifetimeRoots != 1)
      return false;
  }

  const MirInstruction *definition = linearValueDefinition(body, operand.value);
  if (definition == nullptr) {
    return false;
  }
  if (definition->operands.size() != 1)
    return false;
  // Fresh owned receivers already have a full-expression materialization.
  // Invocation aliases that exact slot; it does not fabricate another Move.
  // CallInput verification owns its producer, loan, and cleanup schedule.
  if (definition->kind == MirInstructionKind::CallInput &&
      definition->callInputRole == MirCallInputRole::Receiver &&
      definition->callInputKind == HirCallInputKind::MutableTemporaryBorrow) {
    const MirPlace *place = body.findPlace(definition->operands.front().place);
    return expectedUseKind == MirValueUseKind::InstructionReceiver &&
           place != nullptr && !place->initiallyAvailable &&
           place->type.kind == SemanticType::Class && place->traits.movable &&
           !place->traits.containsBorrowedState &&
           definition->operands.front().kind == MirOperandKind::BorrowWrite &&
           findMirTemporaryMaterialization(body, *place, definition->hirValue)
                   .producer != nullptr;
  }
  if (definition->kind == MirInstructionKind::Move) {
    return definition->operands.front().kind == MirOperandKind::Move &&
           definition->ownership &&
           definition->ownership->kind == OwnershipEventKind::Move &&
           definition->ownership->before == OwnershipStateSet::Available &&
           definition->ownership->after == OwnershipStateSet::Moved;
  }
  if (definition->kind == MirInstructionKind::CallInput &&
      definition->callInputKind == HirCallInputKind::RetainedReceiver) {
    return expectedUseKind == MirValueUseKind::InstructionReceiver &&
           definition->callInputRole == MirCallInputRole::Receiver &&
           definition->ownership &&
           definition->ownership->kind == OwnershipEventKind::ConsumeReceiver;
  }
  if (definition->kind == MirInstructionKind::CallInput &&
      definition->callInputKind == HirCallInputKind::MoveValue) {
    return consumedCallableReceiver(body, definition->operands.front(),
                                    MirValueUseKind::InstructionOperand,
                                    definition->id, 0, depth + 1);
  }
  return definition->kind == MirInstructionKind::Compute &&
         definition->operation == MirOperation::Identity &&
         consumedCallableReceiver(body, definition->operands.front(),
                                  MirValueUseKind::InstructionOperand,
                                  definition->id, 0, depth + 1);
}

[[nodiscard]] bool
movedValueIntoInstruction(const MirBody &body, const MirOperand &operand,
                          MirInstructionId expectedInstruction,
                          std::size_t expectedOperandIndex,
                          std::size_t depth = 0) {
  if (operand.kind != MirOperandKind::Value || depth > body.values.size()) {
    return false;
  }
  const MirInstruction *definition = linearValueDefinition(body, operand.value);
  if (definition != nullptr &&
      definition->kind == MirInstructionKind::CallInput &&
      definition->callInputKind == HirCallInputKind::MoveValue &&
      definition->operands.size() == 1 &&
      definition->info.type == operand.type &&
      definition->operands.front().type == operand.type &&
      hasOneExactValueUse(body, operand.value,
                          MirValueUseKind::InstructionOperand,
                          expectedInstruction, expectedOperandIndex)) {
    // Ordinary argument preparation can stage an owned carrier after its
    // explicit Move. Its source may retain a value-rooted cleanup identity,
    // which is metadata rather than a second executable use. The CallInput
    // verifier separately owns preparation, destination and transfer lifetime.
    return movedValueIntoInstruction(body, definition->operands.front(),
                                     definition->id, 0, depth + 1);
  }
  if (definition == nullptr || definition->kind != MirInstructionKind::Move ||
      definition->operands.size() != 1 ||
      definition->operands.front().kind != MirOperandKind::Move ||
      !definition->ownership ||
      definition->ownership->kind != OwnershipEventKind::Move ||
      definition->ownership->before != OwnershipStateSet::Available ||
      definition->ownership->after != OwnershipStateSet::Moved) {
    return false;
  }

  std::size_t executableUses = 0;
  bool matched = false;
  std::optional<MirPlaceId> bookkeepingPlace;
  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value && place.value == operand.value) {
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
          projection.index == operand.value) {
        return false;
      }
    }
  }
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
          instruction.receiver->value == operand.value) {
        ++executableUses;
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind != MirOperandKind::Value ||
            instruction.operands[index].value != operand.value) {
          continue;
        }
        ++executableUses;
        matched = matched || (instruction.id == expectedInstruction &&
                              index == expectedOperandIndex);
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value &&
        block.terminator.value->value == operand.value) {
      ++executableUses;
    }
    if (bookkeepingPlace && block.terminator.value &&
        block.terminator.value->place == *bookkeepingPlace) {
      return false;
    }
  }
  return executableUses == 1 && matched;
}

[[nodiscard]] bool
freshOwnedValueIntoInstruction(const MirBody &body, const MirOperand &operand,
                               MirInstructionId expectedInstruction,
                               std::size_t expectedOperandIndex) {
  if (operand.kind != MirOperandKind::Value) {
    return false;
  }
  const MirInstruction *definition = linearValueDefinition(body, operand.value);
  const MirInstruction *producer = definition;
  const MirInstruction *preparation = nullptr;
  MirOperand producerResult = operand;
  const auto exactPreparedSourceUse = [&](MirValueId valueId) {
    const MirInstruction *source = linearValueDefinition(body, valueId);
    if (source == nullptr || valueId == 0 || valueId > body.valueUses.size())
      return false;
    std::size_t executableUses = 0;
    MirPlaceId root = 0;
    for (const MirValueUse &use : body.valueUses[valueId - 1]) {
      if (use.kind == MirValueUseKind::PlaceRoot) {
        const auto *place = body.findPlace(use.place);
        if (root != 0 || place == nullptr || !place->projections.empty() ||
            place->sourceValue != source->hirValue ||
            place->type != source->info.type)
          return false;
        root = place->id;
        const auto count = std::count_if(
            body.dropObligations.begin(), body.dropObligations.end(),
            [&](const MirDropObligation &drop) {
              return drop.kind == MirDropObligationKind::Value &&
                     drop.place == root && drop.value == source->hirValue &&
                     drop.dropType.type == source->info.type;
            });
        if (count != 1)
          return false;
      } else if (use.kind == MirValueUseKind::InstructionOperand &&
                 use.instruction == definition->id && use.operandIndex == 0) {
        ++executableUses;
      } else {
        return false;
      }
    }
    // A value-rooted lifetime record is bookkeeping, not permission to read
    // or alias the fresh source through a second executable place.
    if (root != 0) {
      for (const auto &block : body.blocks) {
        for (const auto &instruction : block.instructions) {
          if (instruction.destination == root ||
              (instruction.receiver && instruction.receiver->place == root) ||
              std::any_of(
                  instruction.operands.begin(), instruction.operands.end(),
                  [&](const MirOperand &input) { return input.place == root; }))
            return false;
        }
        if (block.terminator.value && block.terminator.value->place == root)
          return false;
      }
    }
    return executableUses == 1;
  };
  if (definition != nullptr &&
      definition->kind == MirInstructionKind::CallInput &&
      (definition->callInputKind == HirCallInputKind::MoveValue ||
       (definition->callInputKind == HirCallInputKind::Value &&
        definition->info.type.hasFunctionItemShape() &&
        definition->info.type == operand.type &&
        definition->info.traits.drop == DropKind::Trivial &&
        !definition->destination && !definition->preparedParameterDrop &&
        definition->lifecycle.empty())) &&
      definition->operands.size() == 1 &&
      definition->operands.front().kind == MirOperandKind::Value &&
      exactPreparedSourceUse(definition->operands.front().value)) {
    producerResult = definition->operands.front();
    producer = linearValueDefinition(body, producerResult.value);
    preparation = definition->callInputKind == HirCallInputKind::MoveValue
                      ? definition
                      : nullptr;
  }
  const bool exactClosure = producer != nullptr &&
                            producer->kind == MirInstructionKind::Compute &&
                            producer->operation == MirOperation::Closure &&
                            producer->lambdaTarget.has_value();
  const bool exactFunctionItem =
      producer != nullptr && producer->kind == MirInstructionKind::Compute &&
      producer->operation == MirOperation::FunctionItem &&
      producer->functionTarget.has_value();
  const bool exactConstruction =
      producer != nullptr && producer->kind == MirInstructionKind::Construct &&
      producer->constructorTarget.has_value();
  const bool exactDefaultConstruction =
      producer != nullptr && producer->kind == MirInstructionKind::Call &&
      producer->result.has_value() &&
      producer->info.type.kind == SemanticType::Class &&
      producer->info.category == ValueCategory::Value && !producer->receiver &&
      producer->operands.empty() && !producer->functionTarget &&
      !producer->constructorTarget && !producer->lambdaTarget &&
      !producer->nativeCallbackAdapter && !producer->threadTaskAdapter &&
      producer->intrinsic == IntrinsicKind::None &&
      producer->operation == MirOperation::None &&
      producer->synchronization.kind == SynchronizationOperationKind::None &&
      producer->definedFailure.empty() && producer->callableArguments.empty();
  if ((!exactClosure && !exactFunctionItem && !exactConstruction &&
       !exactDefaultConstruction) ||
      producer->result != std::optional{producerResult.value} ||
      producerResult.type != operand.type || definition == nullptr ||
      definition->result != operand.value ||
      definition->info.type != operand.type) {
    return false;
  }

  std::size_t executableUses = 0;
  bool matched = false;
  const MirInstruction *consumer = nullptr;
  std::optional<MirPlaceId> bookkeepingPlace;
  std::optional<MirDropObligationId> bookkeepingDrop;
  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Value && place.value == operand.value) {
      const auto found = std::find_if(
          body.dropObligations.begin(), body.dropObligations.end(),
          [&](const MirDropObligation &obligation) {
            return obligation.kind == MirDropObligationKind::Value &&
                   obligation.place == place.id &&
                   obligation.value == definition->hirValue &&
                   obligation.dropType.type == definition->info.type;
          });
      if (bookkeepingPlace || !place.projections.empty() ||
          place.sourceValue != definition->hirValue ||
          found == body.dropObligations.end()) {
        return false;
      }
      bookkeepingPlace = place.id;
      bookkeepingDrop = found->id;
    }
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Index ||
           projection.kind == MirProjectionKind::RawIndex) &&
          projection.index == operand.value) {
        return false;
      }
    }
  }
  if (preparation != nullptr) {
    const MirPlace *place = preparation->destination
                                ? body.findPlace(*preparation->destination)
                                : nullptr;
    const MirDropObligation *drop =
        preparation->preparedParameterDrop
            ? body.findDropObligation(*preparation->preparedParameterDrop)
            : nullptr;
    if (bookkeepingPlace || bookkeepingDrop || place == nullptr ||
        drop == nullptr || place->root != MirPlaceRootKind::Temporary ||
        !place->projections.empty() ||
        place->sourceValue != preparation->hirValue ||
        place->type != operand.type ||
        drop->kind != MirDropObligationKind::PreparedParameter ||
        drop->place != place->id || drop->dropType.type != operand.type ||
        drop->initiallyActive) {
      return false;
    }
    bookkeepingPlace = place->id;
    bookkeepingDrop = drop->id;
  }

  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (bookkeepingPlace) {
        const bool usesBookkeeping =
            (instruction.destination &&
             *instruction.destination == *bookkeepingPlace) ||
            (instruction.receiver &&
             instruction.receiver->place == *bookkeepingPlace) ||
            std::any_of(instruction.operands.begin(),
                        instruction.operands.end(),
                        [&](const MirOperand &candidate) {
                          return candidate.place == *bookkeepingPlace;
                        });
        const bool exactPreparationDestination =
            instruction.id == definition->id && instruction.destination &&
            *instruction.destination == *bookkeepingPlace &&
            (!instruction.receiver ||
             instruction.receiver->place != *bookkeepingPlace) &&
            std::none_of(instruction.operands.begin(),
                         instruction.operands.end(),
                         [&](const MirOperand &candidate) {
                           return candidate.place == *bookkeepingPlace;
                         });
        if (usesBookkeeping && !exactPreparationDestination) {
          return false;
        }
      }
      if (instruction.receiver &&
          instruction.receiver->kind == MirOperandKind::Value &&
          instruction.receiver->value == operand.value) {
        ++executableUses;
      }
      for (std::size_t index = 0; index < instruction.operands.size();
           ++index) {
        if (instruction.operands[index].kind != MirOperandKind::Value ||
            instruction.operands[index].value != operand.value) {
          continue;
        }
        ++executableUses;
        if (instruction.id == expectedInstruction &&
            index == expectedOperandIndex) {
          matched = true;
          consumer = &instruction;
        }
      }
    }
    if (block.terminator.value &&
        block.terminator.value->kind == MirOperandKind::Value &&
        block.terminator.value->value == operand.value) {
      ++executableUses;
    }
    if (bookkeepingPlace && block.terminator.value &&
        block.terminator.value->place == *bookkeepingPlace) {
      return false;
    }
  }
  if (executableUses != 1 || !matched || consumer == nullptr) {
    return false;
  }

  if (definition->info.traits.drop == DropKind::Trivial &&
      preparation == nullptr) {
    return !bookkeepingPlace && !bookkeepingDrop;
  }
  if (!bookkeepingPlace || !bookkeepingDrop) {
    return false;
  }
  const bool initialized =
      std::any_of(definition->lifecycle.begin(), definition->lifecycle.end(),
                  [&](const MirLifecycleEvent &event) {
                    if (event.target != *bookkeepingDrop || event.conditional ||
                        event.failureCleanup)
                      return false;
                    if (event.kind == MirLifecycleEventKind::Initialize)
                      return event.source == 0;
                    if (preparation == nullptr ||
                        event.kind != MirLifecycleEventKind::Reparent)
                      return false;
                    // A fresh owning closure/construction already has a live
                    // value obligation. Preparing it transfers that exact
                    // lifetime rather than initializing a second owner.
                    // The lifecycle verifier independently proves activation
                    // and the source-to-target state transition.
                    const auto *source = body.findDropObligation(event.source);
                    const auto *place = source == nullptr
                                            ? nullptr
                                            : body.findPlace(source->place);
                    return source != nullptr && place != nullptr &&
                           source->kind == MirDropObligationKind::Value &&
                           source->value == producer->hirValue &&
                           source->dropType.type == operand.type &&
                           place->root == MirPlaceRootKind::Value &&
                           place->value == producerResult.value &&
                           place->sourceValue == producer->hirValue &&
                           place->projections.empty();
                  });
  const bool transferred =
      std::any_of(consumer->lifecycle.begin(), consumer->lifecycle.end(),
                  [&](const MirLifecycleEvent &event) {
                    return event.kind == MirLifecycleEventKind::TransferOut &&
                           event.source == *bookkeepingDrop &&
                           event.target == 0 && !event.conditional &&
                           !event.failureCleanup;
                  });
  return initialized && transferred;
}

[[nodiscard]] bool
validDefinedFailureOperation(const DefinedFailureOperation &operation) {
  if (operation.propagation >= FailurePropagationKind::Count) {
    return false;
  }
  for (std::size_t originIndex = 0; originIndex < operation.localOrigins.size();
       ++originIndex) {
    const DefinedFailureOrigin &origin = operation.localOrigins[originIndex];
    if (origin.outcomes.empty() || origin.sourceUnit == 0 ||
        origin.end <= origin.start || origin.line < 1) {
      return false;
    }
    for (std::size_t previousIndex = 0; previousIndex < originIndex;
         ++previousIndex) {
      const DefinedFailureOrigin &previous =
          operation.localOrigins[previousIndex];
      if (previous.sourceUnit == origin.sourceUnit &&
          previous.start == origin.start && previous.end == origin.end &&
          previous.line == origin.line &&
          previous.outcomes == origin.outcomes) {
        return false;
      }
    }
    for (std::size_t index = 0; index < origin.outcomes.size(); ++index) {
      const DefinedFailureOutcome outcome = origin.outcomes[index];
      if (!validDefinedFailureOutcome(outcome)) {
        return false;
      }
      if (index != 0) {
        const DefinedFailureOutcome previous = origin.outcomes[index - 1];
        if (previous.code > outcome.code ||
            (previous.code == outcome.code &&
             previous.detail >= outcome.detail)) {
          return false;
        }
      }
    }
  }
  return true;
}

[[nodiscard]] bool
validFailureInstructionShape(const MirInstruction &instruction) {
  if (!validDefinedFailureOperation(instruction.definedFailure)) {
    return false;
  }
  if (instruction.localFailureSites.size() !=
          instruction.definedFailure.localOrigins.size() ||
      std::any_of(instruction.localFailureSites.begin(),
                  instruction.localFailureSites.end(),
                  [](FailureSiteId site) { return site == 0; })) {
    return false;
  }
  if (!instruction.definedFailure.localOrigins.empty() &&
      instruction.kind != MirInstructionKind::Compute &&
      instruction.kind != MirInstructionKind::Load &&
      instruction.kind != MirInstructionKind::Assign &&
      instruction.kind != MirInstructionKind::Modify &&
      instruction.kind != MirInstructionKind::Move &&
      instruction.kind != MirInstructionKind::Borrow &&
      instruction.kind != MirInstructionKind::CallInput &&
      instruction.kind != MirInstructionKind::Call &&
      instruction.kind != MirInstructionKind::AllocateObject &&
      instruction.kind != MirInstructionKind::StorageConstructPreflight &&
      instruction.kind != MirInstructionKind::CleanupBeforeAssign &&
      instruction.kind != MirInstructionKind::Construct) {
    return false;
  }
  if (intrinsicBooleanFailureOutcome(instruction.intrinsic) ||
      instruction.intrinsic == IntrinsicKind::Panic) {
    const bool panic = instruction.intrinsic == IntrinsicKind::Panic;
    const SemanticType parameter =
        panic ? SemanticType::StringView : SemanticType::Bool;
    const DefinedFailureOutcome expected =
        panic ? DefinedFailureOutcome{DefinedFailureCode::Panic,
                                      DefinedFailureDetail::ExplicitPanic}
              : *intrinsicBooleanFailureOutcome(instruction.intrinsic);
    return instruction.kind == MirInstructionKind::Call &&
           !instruction.functionTarget && !instruction.constructorTarget &&
           !instruction.bodyTarget && !instruction.lambdaTarget &&
           !instruction.callableInvocation && !instruction.result &&
           !instruction.receiver && !instruction.destination &&
           instruction.info.type == SemanticType::Void &&
           instruction.neverReturns == panic &&
           instruction.tracksCaller == panic &&
           instruction.operands.size() == 1 &&
           instruction.operands.front().type == parameter &&
           instruction.parameterTypes == std::vector<SemanticType>{parameter} &&
           instruction.definedFailure.propagation ==
               FailurePropagationKind::None &&
           instruction.definedFailure.localOrigins.size() == 1 &&
           instruction.definedFailure.localOrigins.front().outcomes ==
               std::vector<DefinedFailureOutcome>{expected} &&
           instruction.lifecycle.empty() &&
           instruction.safetyDischarges.empty();
  }
  switch (instruction.definedFailure.propagation) {
  case FailurePropagationKind::None:
    return true;
  case FailurePropagationKind::DirectCall:
    return instruction.kind == MirInstructionKind::Call &&
           instruction.dispatch == CallDispatch::Static &&
           instruction.functionTarget.has_value();
  case FailurePropagationKind::VirtualCall:
    return instruction.kind == MirInstructionKind::Call &&
           instruction.dispatch == CallDispatch::Virtual &&
           instruction.functionTarget.has_value();
  case FailurePropagationKind::Constructor:
    return (instruction.kind == MirInstructionKind::Construct ||
            instruction.kind == MirInstructionKind::InitializeBase ||
            (instruction.kind == MirInstructionKind::Call &&
             ((instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
               instruction.objectAllocation != 0) ||
              (instruction.intrinsic == IntrinsicKind::ObjectConstruct &&
               instruction.constructorKind == ConstructorKind::Ordinary &&
               !instruction.operands.empty() &&
               instruction.operands.size() ==
                   instruction.parameterTypes.size() &&
               instruction.parameterTypes.front().kind ==
                   SemanticType::RawPointer &&
               instruction.unsafeOperation ==
                   UnsafeOperationKind::ObjectLifetime &&
               instruction.storagePreflight == 0) ||
              (instruction.intrinsic == IntrinsicKind::PrefixStorageAppend &&
               instruction.constructorKind == ConstructorKind::Ordinary &&
               !instruction.operands.empty() &&
               instruction.operands.size() ==
                   instruction.parameterTypes.size() &&
               instruction.parameterTypes.front().kind ==
                   SemanticType::PrefixStorage &&
               instruction.storagePreflight != 0)))) &&
           instruction.constructorTarget.has_value();
  case FailurePropagationKind::Callable:
    return instruction.kind == MirInstructionKind::Call &&
           (instruction.lambdaTarget.has_value() ||
            instruction.callableInvocation.has_value());
  case FailurePropagationKind::TaskJoin:
    return instruction.kind == MirInstructionKind::Call &&
           instruction.synchronization.kind ==
               SynchronizationOperationKind::ThreadJoin;
  case FailurePropagationKind::BodyCall:
    return instruction.kind == MirInstructionKind::CallBody &&
           instruction.bodyTarget.has_value();
  case FailurePropagationKind::Destructor:
    return instruction.kind == MirInstructionKind::Drop ||
           (instruction.kind == MirInstructionKind::CleanupBeforeAssign &&
            instruction.replacementCleanup) ||
           (instruction.kind == MirInstructionKind::Call &&
            instruction.elementCleanup &&
            (instruction.intrinsic == IntrinsicKind::ObjectDestroy ||
             instruction.intrinsic == IntrinsicKind::PrefixStoragePop ||
             instruction.intrinsic == IntrinsicKind::PrefixStorageErase ||
             instruction.intrinsic == IntrinsicKind::OptionalStorageReset));
  case FailurePropagationKind::Count:
    return false;
  }
  return false;
}

[[nodiscard]] bool validSafetyDischarges(const MirInstruction &instruction) {
  const auto less = [](const MirSafetyDischarge &left,
                       const MirSafetyDischarge &right) {
    return std::tuple{static_cast<std::size_t>(left.family), left.origin,
                      static_cast<std::size_t>(left.outcome.code),
                      static_cast<std::size_t>(left.outcome.detail)} <
           std::tuple{static_cast<std::size_t>(right.family), right.origin,
                      static_cast<std::size_t>(right.outcome.code),
                      static_cast<std::size_t>(right.outcome.detail)};
  };
  if (!std::is_sorted(instruction.safetyDischarges.begin(),
                      instruction.safetyDischarges.end(), less) ||
      std::adjacent_find(instruction.safetyDischarges.begin(),
                         instruction.safetyDischarges.end()) !=
          instruction.safetyDischarges.end()) {
    return false;
  }
  for (const MirSafetyDischarge &discharge : instruction.safetyDischarges) {
    if (discharge.family >= MirSafetyCheckFamily::Count ||
        discharge.origin >= instruction.definedFailure.localOrigins.size()) {
      return false;
    }
    const auto &outcomes =
        instruction.definedFailure.localOrigins[discharge.origin].outcomes;
    if (std::find(outcomes.begin(), outcomes.end(), discharge.outcome) ==
        outcomes.end()) {
      return false;
    }
    const bool familyMatches =
        (discharge.family == MirSafetyCheckFamily::Bounds &&
         discharge.outcome.code == DefinedFailureCode::IndexOutOfBounds) ||
        (discharge.family == MirSafetyCheckFamily::ArithmeticOverflow &&
         discharge.outcome.code == DefinedFailureCode::IntegerOverflow) ||
        (discharge.family == MirSafetyCheckFamily::ArithmeticDomain &&
         discharge.outcome.code == DefinedFailureCode::ModuloByZero);
    if (!familyMatches) {
      return false;
    }
    const bool admittedShape = [&]() {
      switch (discharge.family) {
      case MirSafetyCheckFamily::Bounds:
        if (discharge.outcome.detail == DefinedFailureDetail::FixedArray) {
          return instruction.kind == MirInstructionKind::Load ||
                 instruction.kind == MirInstructionKind::Assign;
        }
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::StorageBoundsCheck) {
          return discharge.outcome.detail == DefinedFailureDetail::Vector ||
                 discharge.outcome.detail == DefinedFailureDetail::String ||
                 discharge.outcome.detail ==
                     DefinedFailureDetail::PrivateStorage;
        }
        return discharge.outcome.detail ==
                   DefinedFailureDetail::PrivateStorage &&
               instruction.kind == MirInstructionKind::Call &&
               (instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
                instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut);
      case MirSafetyCheckFamily::ArithmeticOverflow:
        if (instruction.kind == MirInstructionKind::Compute) {
          return (instruction.operation == MirOperation::Add &&
                  discharge.outcome.detail == DefinedFailureDetail::Addition) ||
                 (instruction.operation == MirOperation::Subtract &&
                  discharge.outcome.detail ==
                      DefinedFailureDetail::Subtraction) ||
                 (instruction.operation == MirOperation::Multiply &&
                  discharge.outcome.detail ==
                      DefinedFailureDetail::Multiplication) ||
                 (instruction.operation == MirOperation::Negate &&
                  discharge.outcome.detail == DefinedFailureDetail::Negation);
        }
        return instruction.kind == MirInstructionKind::Assign &&
               ((instruction.operation == MirOperation::AddAssign &&
                 discharge.outcome.detail == DefinedFailureDetail::Addition) ||
                (instruction.operation == MirOperation::SubtractAssign &&
                 discharge.outcome.detail ==
                     DefinedFailureDetail::Subtraction) ||
                (instruction.operation == MirOperation::MultiplyAssign &&
                 discharge.outcome.detail ==
                     DefinedFailureDetail::Multiplication));
      case MirSafetyCheckFamily::ArithmeticDomain:
        return instruction.kind == MirInstructionKind::Compute &&
               instruction.operation == MirOperation::Remainder &&
               discharge.outcome.detail == DefinedFailureDetail::IntegerModulo;
      case MirSafetyCheckFamily::Count:
        return false;
      }
      return false;
    }();
    if (!admittedShape) {
      return false;
    }
  }
  return true;
}

} // namespace

bool supportsMirFailureControlFlow(MirBodyKind kind) {
  // Constructor bodies are admitted now that partial-construction rollback
  // is represented: each completed subobject transferred into `this` arms a
  // ConstructionRollback obligation, every defined-failure edge drains the
  // armed set in reverse stage order, and normal completion retires it by
  // transfer to the caller. Field/static initializer bodies remain excluded
  // until their construction schedules are staged the same way.
  return kind == MirBodyKind::Module || kind == MirBodyKind::Function ||
         kind == MirBodyKind::Constructor ||
         kind == MirBodyKind::FieldInitializers ||
         kind == MirBodyKind::Destructor || kind == MirBodyKind::Lambda ||
         kind == MirBodyKind::HostedStartup;
}

bool mirBodyRoutesFailureEdges(const MirBody &body) {
  if (!supportsMirFailureControlFlow(body.kind)) {
    return false;
  }
  if (body.kind != MirBodyKind::Constructor &&
      body.kind != MirBodyKind::FieldInitializers) {
    return true;
  }
  // A constructor that silently transfers any subobject into `this` without
  // arming rollback routes no defined-failure edges at all: an edge anywhere
  // in the body could not drain that subobject, so the whole body stays on
  // the compatibility failure authority instead of leaking through verified
  // MIR. Rollback retirement transfers are exempt; they end obligations the
  // failure edges already drained or the caller now owns.
  // A TransferOut is unarmed when its source is not a rollback obligation
  // and its instruction carries no ownership continuation: the rollback
  // retirement (rollback-kind source) and the typed-transfer form (a paired
  // event that targets a live local obligation) are exempt.
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      bool continuesOwnership = false;
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        continuesOwnership = continuesOwnership || event.target != 0;
      }
      if (continuesOwnership) {
        continue;
      }
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        if (event.kind != MirLifecycleEventKind::TransferOut) {
          continue;
        }
        // An ordered owned call argument retires its local staging obligation
        // because the callee owns that parameter before the invocation can
        // fail. It is not a transfer into `this`; the surrounding Construct
        // still has to arm the completed field subobject below. The ordinary
        // call-input/value-use verifiers prove the corresponding unique call.
        if (instruction.kind == MirInstructionKind::CallInput &&
            instruction.callInputRole == MirCallInputRole::Argument &&
            instruction.callInputKind == HirCallInputKind::MoveValue) {
          continue;
        }
        // The exact optional-storage construct consumes a prepared payload
        // into an already-live private storage subobject. The surrounding
        // field obligation was armed by its declaration initializer; this
        // transfer retires only the payload's call staging obligation and is
        // not an untracked transfer of a subobject into `this`.
        if (instruction.kind == MirInstructionKind::Call &&
            instruction.intrinsic == IntrinsicKind::OptionalStorageConstruct) {
          continue;
        }
        const MirDropObligation *source = body.findDropObligation(event.source);
        // Calls and selected constructors stage owning inputs until invocation,
        // then hand their prepared obligations to the callee. This is not
        // publication of a constructor subobject. Ordered-call verification
        // independently requires each transfer to belong to this exact
        // invocation and occur once, with caller cleanup retained until the
        // callee begins.
        if (source != nullptr &&
            source->kind == MirDropObligationKind::PreparedParameter &&
            (instruction.kind == MirInstructionKind::Call ||
             (instruction.kind == MirInstructionKind::Construct &&
              instruction.constructorTarget &&
              instruction.constructorKind == ConstructorKind::Ordinary)) &&
            instruction.callSite != 0) {
          continue;
        }
        // This retirement publishes the allocation into its ordinary owner,
        // not into `this`. The independent allocation protocol and lifecycle
        // dataflow prove its unique construction-success continuation.
        if (source != nullptr &&
            source->kind == MirDropObligationKind::ObjectAllocation &&
            instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.objectAllocation == source->id) {
          continue;
        }
        if (source != nullptr &&
            source->kind != MirDropObligationKind::ConstructionRollback) {
          return false;
        }
      }
    }
  }
  return true;
}

const MirInstruction *
mirStorageConstructForPreflight(const MirBody &body,
                                MirInstructionId preflight) {
  if (preflight == 0)
    return nullptr;
  const MirBlock *source = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &instruction : block.instructions)
      if (instruction.id == preflight) {
        if (source != nullptr ||
            instruction.kind != MirInstructionKind::StorageConstructPreflight ||
            block.instructions.back().id != instruction.id ||
            block.terminator.kind != MirTerminatorKind::Invoke ||
            block.terminator.invokeInstruction != instruction.id ||
            !block.terminator.successLifecycle.empty())
          return nullptr;
        source = &block;
      }
  const auto *success =
      source == nullptr ? nullptr : body.findBlock(source->terminator.target);
  if (success == nullptr || success->instructions.empty())
    return nullptr;
  const auto &check = source->instructions.back();
  const auto &append = success->instructions.front();
  const std::size_t operandCount = 1;
  if (append.kind != MirInstructionKind::Call ||
      append.intrinsic != IntrinsicKind::PrefixStorageAppend ||
      append.storagePreflight != preflight || !append.constructorTarget ||
      append.constructorKind != ConstructorKind::Ordinary ||
      check.callSite != append.callSite || check.hirValue != append.hirValue ||
      check.operands.size() != operandCount ||
      append.operands.size() < operandCount ||
      !std::equal(check.operands.begin(), check.operands.end(),
                  append.operands.begin()))
    return nullptr;
  std::size_t incoming = 0;
  for (const auto &block : body.blocks) {
    for (const auto successor : successors(block.terminator))
      if (successor == success->id)
        ++incoming;
    for (const auto &instruction : block.instructions)
      if (instruction.storagePreflight == preflight &&
          instruction.id != append.id)
        return nullptr;
  }
  return incoming == 1 ? &append : nullptr;
}

bool requiresMirFailureControlFlow(const MirInstruction &instruction,
                                   MirFailureControlFlowPosition position) {
  if (instruction.kind == MirInstructionKind::StorageConstructPreflight)
    return true;
  if (instruction.kind == MirInstructionKind::CleanupBeforeAssign)
    return !instruction.definedFailure.empty();
  if (instruction.kind == MirInstructionKind::InitializeBase) {
    return instruction.definedFailure.propagation ==
           FailurePropagationKind::Constructor;
  }
  if (instruction.kind == MirInstructionKind::AllocateObject) {
    return instruction.objectAllocation != 0 &&
           instruction.successResultDrop == instruction.objectAllocation &&
           !instruction.definedFailure.localOrigins.empty();
  }
  if (instruction.kind == MirInstructionKind::Drop &&
      instruction.definedFailure.localOrigins.empty() &&
      instruction.definedFailure.propagation ==
          FailurePropagationKind::Destructor &&
      instruction.destination && instruction.lifecycle.size() == 1 &&
      instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
      instruction.lifecycle.front().source != 0 &&
      instruction.lifecycle.front().target == 0) {
    return true;
  }
  // Most eligible instruction shapes are position-independent: a checked
  // scalar computation, load, or eligible ordinary call carries the same
  // Invoke/record/cleanup contract whether it is a full-expression root, a
  // nested call argument after prepared owners, or any other nested value
  // position. Failure routing reads the live temporary/scope state at the
  // instruction. Position also identifies the prepared-constructor and typed
  // thread-join contracts below; other construction destinations retain their
  // own publication rules.
  // A state-preserving read leaves nothing for a failure edge to unwind, so it
  // does not disqualify an otherwise eligible operation. Any event that moves,
  // reinitializes, or otherwise transitions ownership still does, because the
  // failure edge would have to restore the prior state.
  const bool statePreservingOwnership =
      !instruction.ownership ||
      (instruction.ownership->kind == OwnershipEventKind::Read &&
       instruction.ownership->before == OwnershipStateSet::Available &&
       instruction.ownership->after == OwnershipStateSet::Available &&
       instruction.ownership->reachable);
  // A checked assignment to a trivially destroyed place writes nothing when it
  // fails and runs no lifecycle event, so its failure edge needs only the
  // ordinary temporary and scope cleanup. This covers the narrowing compound
  // forms that keep a closed instruction because semantics folds their
  // arithmetic and conversion into one origin. An assignment that replaces an
  // owning value still needs the destination's own unwinding rule.
  const bool trivialCommitDestination =
      instruction.kind == MirInstructionKind::Assign &&
      instruction.lifecycle.empty() &&
      instruction.info.traits.drop == DropKind::Trivial &&
      !instruction.info.traits.containsBorrowedState;
  const bool locallyFailingBorrowIntrinsic =
      (instruction.intrinsic == IntrinsicKind::PrefixStorageRead ||
       instruction.intrinsic == IntrinsicKind::PrefixStorageReadMut ||
       instruction.intrinsic == IntrinsicKind::OptionalStorageRead ||
       instruction.intrinsic == IntrinsicKind::OptionalStorageReadMut ||
       instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrow ||
       instruction.intrinsic == IntrinsicKind::UniqueOwnerBorrowMut ||
       instruction.intrinsic == IntrinsicKind::SharedOwnerBorrow ||
       instruction.intrinsic == IntrinsicKind::ExpectedValue) &&
      !instruction.localFailureSites.empty() &&
      !instruction.definedFailure.localOrigins.empty();
  // A checked element write reinitializing an always-available trivially
  // destroyed place is equally state-preserving: the write helper leaves the
  // destination untouched when it fails, and the ownership state does not
  // transition (Available before and after), so the failure edge has nothing
  // to restore. Without this admission a dynamic fixed-array element write
  // keeps its failure sites but never receives the Invoke edge that consumes
  // the checked helper's status, silently dropping the defined
  // index_out_of_bounds failure that the matching read raises.
  const bool statePreservingReinitialize =
      trivialCommitDestination && instruction.ownership &&
      instruction.ownership->kind == OwnershipEventKind::Reinitialize &&
      instruction.ownership->before == OwnershipStateSet::Available &&
      instruction.ownership->after == OwnershipStateSet::Available &&
      instruction.ownership->reachable;
  const bool borrowedInvocation =
      instruction.kind == MirInstructionKind::Call ||
      (instruction.kind == MirInstructionKind::Construct &&
       instruction.constructorTarget &&
       instruction.constructorKind == ConstructorKind::Ordinary);
  const bool borrowedCallResult =
      borrowedInvocation && instruction.loan &&
      instruction.borrowOrigin != BorrowOriginKind::None &&
      !instruction.successResultDrop &&
      (instruction.definedFailure.propagation != FailurePropagationKind::None ||
       locallyFailingBorrowIntrinsic);
  const bool fallibleBorrowCarrier =
      instruction.info.type.kind == SemanticType::Expected &&
      instruction.info.type.arguments.size() == 2 &&
      instruction.info.type.arguments.front().kind == SemanticType::Reference &&
      instruction.info.type.arguments.front().referenceAccess ==
          AccessMode::ReadOnly &&
      instruction.info.type.arguments.front().arguments.size() == 1;
  const bool transferredBorrowedCallResult =
      borrowedInvocation && instruction.loan &&
      instruction.borrowOrigin != BorrowOriginKind::None &&
      instruction.successResultDrop && instruction.result &&
      (instruction.info.type.kind == SemanticType::Class ||
       fallibleBorrowCarrier) &&
      instruction.info.category == ValueCategory::Value &&
      instruction.info.traits.movable &&
      instruction.info.traits.containsBorrowedState &&
      instruction.definedFailure.propagation != FailurePropagationKind::None;
  const bool directInitializationResult =
      (instruction.kind == MirInstructionKind::Call ||
       instruction.kind == MirInstructionKind::Construct) &&
      instruction.successResultDestination && instruction.result &&
      !instruction.destination && !instruction.loan &&
      !instruction.successResultDrop &&
      instruction.info.traits.drop == DropKind::Lexical;
  if (instruction.definedFailure.empty() ||
      (instruction.destination && !trivialCommitDestination) ||
      (instruction.loan && !borrowedCallResult &&
       !transferredBorrowedCallResult) ||
      (!statePreservingOwnership && !statePreservingReinitialize) ||
      instruction.info.type.kind == SemanticType::Reference) {
    return false;
  }
  if (trivialCommitDestination) {
    return true;
  }
  if (instruction.kind == MirInstructionKind::Compute ||
      instruction.kind == MirInstructionKind::Load) {
    return instruction.info.traits.drop == DropKind::Trivial &&
           instruction.lifecycle.empty() && !instruction.successResultDrop;
  }
  // A construction is an ordered invocation with the same failure shape as an
  // ordinary call: it may raise before producing anything, and its
  // cleanup-owning result must be initialized only on the success edge.
  if ((instruction.kind != MirInstructionKind::Call &&
       instruction.kind != MirInstructionKind::Construct) ||
      std::any_of(instruction.lifecycle.begin(), instruction.lifecycle.end(),
                  [](const MirLifecycleEvent &event) {
                    return event.kind != MirLifecycleEventKind::TransferOut;
                  })) {
    return false;
  }
  if (borrowedCallResult) {
    return true;
  }
  if (transferredBorrowedCallResult) {
    return instruction.info.traits.drop == DropKind::Lexical;
  }
  if (directInitializationResult) {
    return true;
  }
  if (instruction.successResultDrop) {
    return instruction.info.traits.drop == DropKind::Lexical;
  }
  // Expected<scalar, E> crosses the transformed failure boundary through a
  // value out-parameter. It therefore has no semantic result drop to arm:
  // the callee publishes only on its successful Invoke edge, after which the
  // value can feed either a direct return or another cleanup-free expression.
  // Keep this aligned with the backend boundary, which deliberately admits
  // only scalar/void Expected payloads here; Expected<Class, E> uses the
  // explicit placement-result contract below instead.
  const auto passiveExpectedPayload = [](const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Void:
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
      return true;
    default:
      return false;
    }
  };
  if (instruction.kind == MirInstructionKind::Call && instruction.result &&
      instruction.info.type.kind == SemanticType::Expected &&
      instruction.info.type.arguments.size() == 2 &&
      passiveExpectedPayload(instruction.info.type.arguments.front()) &&
      instruction.info.traits.drop == DropKind::Lexical) {
    return true;
  }
  // A typed thread join returns Expected<Result, thread_errc> while also
  // carrying the joined worker's defined-failure channel. When Result is a
  // class and the join is returned directly from the private wrapper, HIR has
  // no temporary drop identity to arm: the backend's placement result owns
  // the successful value. The Invoke is still mandatory so a worker failure
  // reaches the wrapper's failure record instead of being mistaken for an
  // ordinary Expected error.
  if (position == MirFailureControlFlowPosition::FullExpressionRoot &&
      instruction.kind == MirInstructionKind::Call && instruction.result &&
      instruction.intrinsic == IntrinsicKind::ThreadJoin &&
      instruction.definedFailure.propagation ==
          FailurePropagationKind::TaskJoin &&
      instruction.info.type.kind == SemanticType::Expected &&
      instruction.info.type.arguments.size() == 2 &&
      instruction.info.type.arguments.front().kind == SemanticType::Class &&
      instruction.info.traits.drop == DropKind::Lexical) {
    return true;
  }
  // A class-valued call (including an Expected with a class alternative) can
  // lack a result-drop identity when it returns directly or produces a
  // cleanup-free value for a later consumer. Failure
  // still needs an Invoke in either position: no result exists until success,
  // and existing parameters/scopes must unwind before propagation. Storage
  // publication is independently owned by the result's consumer/obligation;
  // the failure edge must not depend on whether that consumer is a Return,
  // argument checkpoint, assignment, or discard.
  // Construction needs the same failure-before-publication rule even when
  // its class has no active cleanup. In particular, a constructor initializer
  // can publish such a result into a field without a drop obligation; that
  // does not make a failing constructor safe to invoke without an edge.
  // The consumer still owns publication, exactly as for a class-valued call.
  if ((instruction.kind == MirInstructionKind::Call ||
       instruction.kind == MirInstructionKind::Construct) &&
      instruction.result &&
      (instruction.info.type.kind == SemanticType::Class ||
       (instruction.kind == MirInstructionKind::Call &&
        instruction.info.type.kind == SemanticType::Expected &&
        instruction.info.type.arguments.size() == 2 &&
        (instruction.info.type.arguments.front().kind == SemanticType::Class ||
         instruction.info.type.arguments.back().kind ==
             SemanticType::Class))) &&
      instruction.info.traits.drop == DropKind::Lexical) {
    return true;
  }
  return instruction.info.traits.drop == DropKind::Trivial;
}

namespace {

[[nodiscard]] MirVerificationResult
verifyMirHostedStartupBodyStructure(const MirBody &body, std::size_t owner) {
  if (owner == 0 || body.entry == 0 || body.entry > body.blocks.size() ||
      body.returnType != SemanticType::Int32 ||
      body.placeDomain.snapshot == 0 || body.placeDomain.body == 0) {
    return failure(
        body, owner,
        "hosted-startup body identity or return contract is invalid");
  }
  if (!body.loans.empty() || !body.fullExpressions.empty() ||
      !body.programConstantSubstitutions.empty() ||
      !body.rangeIterations.empty()) {
    return failure(body, owner,
                   "hosted-startup body retains source-only tables");
  }

  const auto validValue = [&](MirValueId id) {
    return id != 0 && id <= body.values.size();
  };
  const auto validPlace = [&](MirPlaceId id) {
    return id != 0 && id <= body.places.size();
  };
  const auto validDrop = [&](MirDropObligationId id) {
    return id != 0 && id <= body.dropObligations.size();
  };
  const auto validFailureRecord = [&](MirFailureRecordId id) {
    return id != 0 && id <= body.failureRecords.size();
  };
  const auto validOperand = [&](const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value:
      return validValue(operand.value) && operand.place == 0 &&
             operand.loan == 0 && !operand.literal &&
             body.values[operand.value - 1].info.type == operand.type;
    case MirOperandKind::Constant:
      return operand.value == 0 && operand.place == 0 && operand.loan == 0 &&
             operand.literal &&
             literalMatchesType(*operand.literal, operand.type);
    case MirOperandKind::Address:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      return operand.value == 0 && validPlace(operand.place) &&
             operand.loan == 0 && !operand.literal &&
             body.places[operand.place - 1].type == operand.type;
    case MirOperandKind::Loan:
      return false;
    }
    return false;
  };

  for (std::size_t index = 0; index < body.values.size(); ++index) {
    const MirValue &value = body.values[index];
    if (value.id != index + 1 || value.hostedStartupOperation == 0 ||
        value.sourceValue != 0 || value.info.type == SemanticType::Unknown ||
        value.info.traits != semanticTraits(value.info.type) ||
        value.definitionBlock == 0 ||
        value.definitionBlock > body.blocks.size() || value.definition == 0) {
      return failure(body, owner,
                     "hosted-startup value has invalid generated provenance");
    }
  }

  std::unordered_set<MirTemporaryId> temporaries;
  for (std::size_t index = 0; index < body.places.size(); ++index) {
    const MirPlace &place = body.places[index];
    const bool temporary = place.root == MirPlaceRootKind::Temporary;
    const bool value = place.root == MirPlaceRootKind::Value;
    if (place.id != index + 1 || place.hostedStartupOperation == 0 ||
        (!temporary && !value) || place.binding != 0 || place.symbol != 0 ||
        place.capture != 0 || place.loan != 0 ||
        (temporary != (place.temporary != 0)) ||
        (value != (place.value != 0)) ||
        (temporary && !temporaries.insert(place.temporary).second) ||
        (value && !validValue(place.value)) || !place.projections.empty() ||
        place.type == SemanticType::Unknown ||
        place.traits != semanticTraits(place.type) || place.sourceValue != 0 ||
        place.key || place.initiallyAvailable) {
      return failure(body, owner,
                     "hosted-startup place has invalid generated provenance");
    }
    if (value && body.values[place.value - 1].info.type != place.type) {
      return failure(body, owner,
                     "hosted-startup value place has a mismatched type");
    }
  }

  for (std::size_t index = 0; index < body.dropObligations.size(); ++index) {
    const MirDropObligation &drop = body.dropObligations[index];
    const MirPlace *place =
        validPlace(drop.place) ? &body.places[drop.place - 1] : nullptr;
    const MirValue *value = validValue(drop.generatedValue)
                                ? &body.values[drop.generatedValue - 1]
                                : nullptr;
    const bool valueKind = drop.kind == MirDropObligationKind::Value;
    const bool preparedKind =
        drop.kind == MirDropObligationKind::PreparedParameter;
    if (drop.id != index + 1 || drop.hostedStartupOperation == 0 ||
        drop.hirObligation != 0 || drop.constructionOrder != index + 1 ||
        (!valueKind && !preparedKind) || place == nullptr || value == nullptr ||
        drop.binding != 0 || drop.value != 0 || drop.hirFullExpression != 0 ||
        drop.fullExpression != 0 || drop.initiallyActive ||
        drop.dropType.type.kind != SemanticType::Class ||
        !drop.dropType.classInstance || drop.dropType.lambdaInstance ||
        !drop.dropType.requiresActiveCleanup ||
        place->type != drop.dropType.type || value->info.type != place->type ||
        (valueKind && (place->root != MirPlaceRootKind::Value ||
                       place->value != drop.generatedValue)) ||
        (preparedKind && place->root != MirPlaceRootKind::Temporary)) {
      return failure(
          body, owner,
          "hosted-startup drop has invalid generated ownership provenance");
    }
  }

  for (std::size_t index = 0; index < body.cleanupBoundaries.size(); ++index) {
    const MirCleanupBoundary &boundary = body.cleanupBoundaries[index];
    if (boundary.id != index + 1 || boundary.hostedStartupOperation == 0 ||
        boundary.kind != MirCleanupBoundaryKind::Failure ||
        boundary.obligations.empty() ||
        std::any_of(boundary.obligations.begin(), boundary.obligations.end(),
                    [&](MirDropObligationId obligation) {
                      return !validDrop(obligation);
                    })) {
      return failure(body, owner,
                     "hosted-startup failure boundary has invalid generated "
                     "provenance");
    }
  }
  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    const MirFailureRecord &record = body.failureRecords[index];
    if (record.id != index + 1 || record.hostedStartupOperation == 0 ||
        record.producerBlock == 0 ||
        record.producerBlock > body.blocks.size() ||
        record.producerInstruction == 0 || record.parameterBlock == 0 ||
        record.parameterBlock > body.blocks.size() ||
        record.producerBlock == record.parameterBlock) {
      return failure(body, owner,
                     "hosted-startup failure record has invalid generated "
                     "provenance");
    }
  }

  std::unordered_set<MirInstructionId> instructionIds;
  std::vector<std::size_t> definitions(body.values.size(), 0);
  std::vector<std::size_t> cleanupBoundaryMarkers(body.cleanupBoundaries.size(),
                                                  0);
  std::vector<std::size_t> failureParameters(body.failureRecords.size(), 0);
  std::vector<std::size_t> failureInvokes(body.failureRecords.size(), 0);
  std::vector<std::size_t> failureEndpoints(body.failureRecords.size(), 0);
  std::unordered_map<MirInstructionId, const MirInstruction *> instructions;
  std::unordered_map<MirInstructionId, MirBlockId> instructionBlocks;
  for (std::size_t blockIndex = 0; blockIndex < body.blocks.size();
       ++blockIndex) {
    const MirBlock &block = body.blocks[blockIndex];
    if (block.id != blockIndex + 1 || block.programInitializationStep != 0 ||
        !block.parameters.empty() ||
        (block.failureParameter != 0 &&
         !validFailureRecord(block.failureParameter)) ||
        (block.activeFailure != 0 &&
         !validFailureRecord(block.activeFailure)) ||
        (block.failureParameter != 0 && block.activeFailure == 0)) {
      return failure(body, owner,
                     "hosted-startup block has source or failure provenance",
                     block.id);
    }
    if (block.failureParameter != 0) {
      const MirFailureRecord &parameter =
          body.failureRecords[block.failureParameter - 1];
      if (parameter.parameterBlock != block.id) {
        return failure(body, owner,
                       "hosted-startup failure parameter names another block",
                       block.id);
      }
      ++failureParameters[block.failureParameter - 1];
    }
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.id == 0 ||
          !instructionIds.insert(instruction.id).second ||
          instruction.hostedStartupOperation == 0 ||
          instruction.hirValue != 0 || instruction.hirStatement != 0 ||
          instruction.callSite != 0 ||
          instruction.constructorInitializer != 0 ||
          instruction.unsafeOperation != UnsafeOperationKind::None ||
          instruction.rawMemoryAccess || instruction.loan ||
          instruction.borrowOrigin != BorrowOriginKind::None ||
          instruction.borrowArgument != 0 ||
          instruction.borrowAccess != AccessMode::ReadOnly ||
          !instruction.borrowArguments.empty() ||
          !instruction.borrowFields.empty() || instruction.borrowPlace ||
          instruction.literal ||
          instruction.literalProvenance != MirLiteralProvenance{} ||
          instruction.programConstantSubstitution || instruction.enumOwner ||
          instruction.enumValue || instruction.enumVariant ||
          instruction.payloadIndex ||
          instruction.intrinsic != IntrinsicKind::None ||
          instruction.synchronization != SynchronizationOperation{} ||
          !instruction.safetyDischarges.empty() ||
          instruction.dispatch != CallDispatch::Static ||
          instruction.dispatchOwner != SemanticType::Unknown ||
          instruction.lambdaTarget || !instruction.callableArguments.empty() ||
          instruction.callableBoundary || instruction.callableInvocation ||
          instruction.ownership || instruction.fullExpressionEnd != 0 ||
          (instruction.cleanupBoundaryEnd != 0 &&
           instruction.cleanupBoundaryEnd > body.cleanupBoundaries.size()) ||
          (instruction.kind != MirInstructionKind::CallBody &&
           instruction.bodyTarget) ||
          (instruction.kind == MirInstructionKind::CallBody &&
           !instruction.bodyTarget) ||
          (instruction.destination && !validPlace(*instruction.destination)) ||
          (instruction.result && !validValue(*instruction.result)) ||
          (instruction.receiver && !validOperand(*instruction.receiver)) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return !validOperand(operand);
                      }) ||
          (instruction.preparedParameterDrop &&
           !validDrop(*instruction.preparedParameterDrop)) ||
          (instruction.successResultDrop &&
           !validDrop(*instruction.successResultDrop)) ||
          instruction.successResultDestination ||
          instruction.objectAllocation != 0 ||
          instruction.localFailureSites.size() !=
              instruction.definedFailure.localOrigins.size() ||
          !validFailureInstructionShape(instruction)) {
        return failure(body, owner,
                       "hosted-startup instruction has invalid generated "
                       "shape or provenance",
                       block.id, instruction.id);
      }
      instructions.emplace(instruction.id, &instruction);
      instructionBlocks.emplace(instruction.id, block.id);
      if (instruction.cleanupBoundaryEnd != 0) {
        const MirCleanupBoundary &boundary =
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1];
        if (instruction.kind != MirInstructionKind::Lifecycle ||
            block.activeFailure == 0 ||
            boundary.kind != MirCleanupBoundaryKind::Failure) {
          return failure(body, owner,
                         "hosted-startup cleanup marker is attached to the "
                         "wrong generated control flow",
                         block.id, instruction.id);
        }
        ++cleanupBoundaryMarkers[instruction.cleanupBoundaryEnd - 1];
      }
      if (block.activeFailure != 0) {
        const bool failureDrop =
            instruction.kind == MirInstructionKind::Drop &&
            instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
            instruction.lifecycle.front().failureCleanup;
        const bool failureBoundary =
            instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.cleanupBoundaryEnd != 0;
        if (!failureDrop && !failureBoundary) {
          return failure(body, owner,
                         "hosted-startup active-failure block contains a "
                         "non-cleanup instruction",
                         block.id, instruction.id);
        }
      }
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        const bool source = event.source != 0;
        const bool target = event.target != 0;
        bool valid = !event.conditional;
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          valid = valid && !event.failureCleanup && !source && target;
          break;
        case MirLifecycleEventKind::Reparent:
          valid = valid && !event.failureCleanup && source && target &&
                  event.source != event.target;
          break;
        case MirLifecycleEventKind::TransferOut:
          valid = valid && !event.failureCleanup && source && !target;
          break;
        case MirLifecycleEventKind::Drop:
          valid = valid && source && !target &&
                  event.failureCleanup == (block.activeFailure != 0) &&
                  instruction.kind == MirInstructionKind::Drop &&
                  instruction.destination && validDrop(event.source) &&
                  body.dropObligations[event.source - 1].place ==
                      *instruction.destination;
          break;
        case MirLifecycleEventKind::Move:
        case MirLifecycleEventKind::Replace:
          valid = false;
          break;
        }
        if (!valid || (source && !validDrop(event.source)) ||
            (target && !validDrop(event.target))) {
          return failure(body, owner,
                         "hosted-startup lifecycle event is invalid", block.id,
                         instruction.id);
        }
      }
      if (instruction.result) {
        const MirValue &value = body.values[*instruction.result - 1];
        if (value.definitionBlock != block.id ||
            value.definition != instruction.id ||
            value.info != instruction.info) {
          return failure(body, owner,
                         "hosted-startup result does not match its value",
                         block.id, instruction.id);
        }
        ++definitions[*instruction.result - 1];
      }
    }

    const MirTerminator &terminator = block.terminator;
    const auto validTarget = [&](MirBlockId target) {
      return target != 0 && target <= body.blocks.size();
    };
    const bool validKind =
        (terminator.kind == MirTerminatorKind::Goto &&
         validTarget(terminator.target)) ||
        (terminator.kind == MirTerminatorKind::Branch &&
         validTarget(terminator.target) && validTarget(terminator.elseTarget) &&
         terminator.value && validOperand(*terminator.value) &&
         terminator.value->type == SemanticType::Bool) ||
        (terminator.kind == MirTerminatorKind::Return && terminator.value &&
         validOperand(*terminator.value) &&
         terminator.value->type == SemanticType::Int32) ||
        (terminator.kind == MirTerminatorKind::Invoke &&
         validTarget(terminator.target) && validTarget(terminator.elseTarget) &&
         terminator.target != terminator.elseTarget &&
         terminator.invokeInstruction != 0 &&
         validFailureRecord(terminator.failureRecord)) ||
        (terminator.kind == MirTerminatorKind::ContainFailure &&
         validFailureRecord(terminator.failureRecord)) ||
        (terminator.kind == MirTerminatorKind::TerminateCleanupFailure &&
         validFailureRecord(terminator.failureRecord));
    if (terminator.hostedStartupOperation == 0 || terminator.hirValue != 0 ||
        terminator.hirStatement != 0 || !validKind || terminator.returnLoan ||
        !terminator.targetArguments.empty() ||
        !terminator.elseArguments.empty() ||
        !terminator.switchTargets.empty() ||
        ((terminator.kind != MirTerminatorKind::Invoke) &&
         (!terminator.successLifecycle.empty() ||
          terminator.invokeInstruction != 0)) ||
        ((terminator.kind != MirTerminatorKind::Invoke &&
          terminator.kind != MirTerminatorKind::ContainFailure &&
          terminator.kind != MirTerminatorKind::TerminateCleanupFailure) &&
         terminator.failureRecord != 0)) {
      return failure(body, owner,
                     "hosted-startup terminator has invalid generated shape "
                     "or provenance",
                     block.id);
    }
    if (terminator.kind == MirTerminatorKind::Invoke) {
      const auto invocation = instructions.find(terminator.invokeInstruction);
      const MirFailureRecord &record =
          body.failureRecords[terminator.failureRecord - 1];
      const MirBlock &normal = body.blocks[terminator.target - 1];
      const MirBlock &failed = body.blocks[terminator.elseTarget - 1];
      const MirFailureRecordId expectedFailureActive =
          block.activeFailure == 0 ? terminator.failureRecord
                                   : block.activeFailure;
      const MirInstruction *producer =
          invocation == instructions.end() ? nullptr : invocation->second;
      const bool exactHostedFailureProducer =
          producer != nullptr && !producer->definedFailure.empty() &&
          ((producer->kind == MirInstructionKind::Load &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) ||
           (producer->kind == MirInstructionKind::Compute &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::None) ||
           (producer->kind == MirInstructionKind::Call &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::DirectCall) ||
           (producer->kind == MirInstructionKind::Construct &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Constructor) ||
           (producer->kind == MirInstructionKind::CallBody &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::BodyCall) ||
           (block.activeFailure != 0 &&
            producer->kind == MirInstructionKind::Drop &&
            producer->definedFailure.propagation ==
                FailurePropagationKind::Destructor));
      const bool exactSuccessLifecycle =
          producer != nullptr && producer->successResultDrop
              ? terminator.successLifecycle.size() == 1 &&
                    terminator.successLifecycle.front().kind ==
                        MirLifecycleEventKind::Initialize &&
                    terminator.successLifecycle.front().source == 0 &&
                    terminator.successLifecycle.front().target ==
                        *producer->successResultDrop &&
                    !terminator.successLifecycle.front().conditional &&
                    !terminator.successLifecycle.front().failureCleanup
              : terminator.successLifecycle.empty();
      if (producer == nullptr ||
          instructionBlocks[terminator.invokeInstruction] != block.id ||
          block.instructions.empty() ||
          block.instructions.back().id != terminator.invokeInstruction ||
          !exactHostedFailureProducer || record.producerBlock != block.id ||
          record.producerInstruction != terminator.invokeInstruction ||
          record.parameterBlock != terminator.elseTarget ||
          normal.failureParameter != 0 ||
          normal.activeFailure != block.activeFailure ||
          failed.failureParameter != terminator.failureRecord ||
          failed.activeFailure != expectedFailureActive ||
          terminator.failureRecord == block.activeFailure || terminator.value ||
          !exactSuccessLifecycle) {
        return failure(body, owner,
                       "hosted-startup invoke does not match its exact "
                       "generated record or successors",
                       block.id, terminator.invokeInstruction);
      }
      ++failureInvokes[terminator.failureRecord - 1];
    }
    const bool emptyFailureEndpoint =
        terminator.invokeInstruction == 0 && !terminator.value &&
        terminator.target == 0 && terminator.elseTarget == 0 &&
        terminator.switchTargets.empty() && terminator.successLifecycle.empty();
    if (terminator.kind == MirTerminatorKind::ContainFailure) {
      if (block.activeFailure == 0 ||
          block.activeFailure != terminator.failureRecord ||
          !emptyFailureEndpoint) {
        return failure(body, owner,
                       "hosted-startup containment does not consume its "
                       "exact primary record",
                       block.id);
      }
      ++failureEndpoints[terminator.failureRecord - 1];
    }
    if (terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      if (block.activeFailure == 0 || block.failureParameter == 0 ||
          block.failureParameter != terminator.failureRecord ||
          block.activeFailure == terminator.failureRecord ||
          !block.instructions.empty() || !emptyFailureEndpoint) {
        return failure(body, owner,
                       "hosted-startup cleanup secondary does not terminate "
                       "immediately",
                       block.id);
      }
      ++failureEndpoints[terminator.failureRecord - 1];
    }
  }

  if (instructionIds.size() != body.instructionCount()) {
    return failure(body, owner,
                   "hosted-startup instruction identities are not unique");
  }
  for (MirInstructionId id = 1; id <= body.instructionCount(); ++id) {
    if (!instructionIds.contains(id)) {
      return failure(body, owner,
                     "hosted-startup instruction identities are not dense");
    }
  }

  std::vector<std::size_t> failurePredecessors(body.failureRecords.size(), 0);
  for (const MirBlock &block : body.blocks) {
    if (block.activeFailure != 0 &&
        block.terminator.kind != MirTerminatorKind::Goto &&
        block.terminator.kind != MirTerminatorKind::Invoke &&
        block.terminator.kind != MirTerminatorKind::ContainFailure &&
        block.terminator.kind != MirTerminatorKind::TerminateCleanupFailure) {
      return failure(body, owner,
                     "hosted-startup active-failure path bypasses its exact "
                     "terminal endpoint",
                     block.id);
    }
    for (const MirBlockId successor : successors(block.terminator)) {
      const MirBlock &target = body.blocks[successor - 1];
      const bool invokeFailure =
          block.terminator.kind == MirTerminatorKind::Invoke &&
          block.terminator.elseTarget == successor;
      const MirFailureRecordId expectedActive =
          invokeFailure
              ? (block.activeFailure == 0 ? block.terminator.failureRecord
                                          : block.activeFailure)
              : block.activeFailure;
      if (target.activeFailure != expectedActive ||
          (!invokeFailure && target.failureParameter != 0)) {
        return failure(body, owner,
                       "hosted-startup edge does not preserve exact active "
                       "failure state",
                       successor);
      }
      if (target.failureParameter != 0) {
        if (!invokeFailure ||
            block.terminator.failureRecord != target.failureParameter) {
          return failure(body, owner,
                         "hosted-startup failure parameter has a non-failure "
                         "predecessor",
                         successor);
        }
        ++failurePredecessors[target.failureParameter - 1];
      }
    }
  }

  for (const MirFailureRecord &record : body.failureRecords) {
    const MirBlock *parameter = body.findBlock(record.parameterBlock);
    if (parameter == nullptr) {
      continue;
    }
    const bool primary = parameter->activeFailure == record.id;
    if (!primary) {
      const MirBlock *producer = body.findBlock(record.producerBlock);
      if (parameter->failureParameter != record.id ||
          parameter->activeFailure == 0 ||
          parameter->activeFailure == record.id || producer == nullptr ||
          producer->activeFailure != parameter->activeFailure ||
          !parameter->instructions.empty() ||
          parameter->terminator.kind !=
              MirTerminatorKind::TerminateCleanupFailure ||
          parameter->terminator.failureRecord != record.id) {
        return failure(body, owner,
                       "hosted-startup secondary failure is not the first "
                       "cleanup failure or does not terminate immediately",
                       parameter->id);
      }
      continue;
    }

    const MirBlock *cursor = parameter;
    std::unordered_set<MirBlockId> chain;
    std::size_t previousOrder = std::numeric_limits<std::size_t>::max();
    while (true) {
      if (cursor == nullptr || cursor->activeFailure != record.id ||
          !chain.insert(cursor->id).second ||
          (cursor->failureParameter != 0 &&
           cursor->failureParameter != record.id)) {
        return failure(body, owner,
                       "hosted-startup primary cleanup is cyclic or changes "
                       "its active record",
                       cursor == nullptr ? 0 : cursor->id);
      }
      for (const MirInstruction &instruction : cursor->instructions) {
        if (instruction.kind != MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1 ||
            !instruction.lifecycle.front().failureCleanup) {
          continue;
        }
        const MirDropObligation *drop =
            body.findDropObligation(instruction.lifecycle.front().source);
        if (drop == nullptr || drop->constructionOrder >= previousOrder) {
          return failure(body, owner,
                         "hosted-startup failure cleanup is not globally in "
                         "reverse construction order",
                         cursor->id, instruction.id);
        }
        previousOrder = drop->constructionOrder;
      }
      if (cursor->terminator.kind == MirTerminatorKind::ContainFailure) {
        if (cursor->terminator.failureRecord != record.id) {
          return failure(body, owner,
                         "hosted-startup primary cleanup contains another "
                         "record",
                         cursor->id);
        }
        break;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Goto) {
        cursor = body.findBlock(cursor->terminator.target);
        continue;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Invoke) {
        const MirBlock *secondary =
            body.findBlock(cursor->terminator.elseTarget);
        if (secondary == nullptr || secondary->activeFailure != record.id ||
            secondary->failureParameter != cursor->terminator.failureRecord ||
            secondary->failureParameter == record.id ||
            !secondary->instructions.empty() ||
            secondary->terminator.kind !=
                MirTerminatorKind::TerminateCleanupFailure ||
            secondary->terminator.failureRecord !=
                secondary->failureParameter) {
          return failure(body, owner,
                         "hosted-startup fallible cleanup does not route its "
                         "secondary directly to emergency termination",
                         cursor->id, cursor->terminator.invokeInstruction);
        }
        cursor = body.findBlock(cursor->terminator.target);
        continue;
      }
      return failure(body, owner,
                     "hosted-startup primary cleanup bypasses containment",
                     cursor->id);
    }
    for (const MirBlock &candidate : body.blocks) {
      const bool primaryState = candidate.activeFailure == record.id &&
                                (candidate.failureParameter == 0 ||
                                 candidate.failureParameter == record.id);
      if (primaryState && !chain.contains(candidate.id)) {
        return failure(body, owner,
                       "hosted-startup primary cleanup block is disconnected",
                       candidate.id);
      }
    }
  }

  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    if (failureParameters[index] != 1 || failureInvokes[index] != 1 ||
        failurePredecessors[index] != 1 || failureEndpoints[index] != 1) {
      return failure(body, owner,
                     "hosted-startup failure record lacks one exact invoke, "
                     "parameter, predecessor, or endpoint");
    }
  }

  std::unordered_map<MirInstructionId, std::size_t> failureDropCoverage;
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.cleanupBoundaryEnd == 0) {
        continue;
      }
      const MirCleanupBoundary &boundary =
          body.cleanupBoundaries[marker.cleanupBoundaryEnd - 1];
      std::vector<const MirInstruction *> reversed;
      const MirBlock *cursor = &block;
      std::size_t cursorIndex = markerIndex;
      std::unordered_set<MirBlockId> visited;
      while (reversed.size() < boundary.obligations.size()) {
        if (!visited.insert(cursor->id).second) {
          break;
        }
        while (cursorIndex != 0 &&
               reversed.size() < boundary.obligations.size()) {
          const MirInstruction &candidate =
              cursor->instructions[cursorIndex - 1];
          if (candidate.kind != MirInstructionKind::Drop) {
            break;
          }
          reversed.push_back(&candidate);
          --cursorIndex;
        }
        if (reversed.size() == boundary.obligations.size()) {
          break;
        }
        if (cursorIndex != 0) {
          break;
        }
        const MirBlock *predecessor = nullptr;
        std::size_t predecessorCount = 0;
        for (const MirBlock &candidate : body.blocks) {
          const std::vector<MirBlockId> outgoing =
              successors(candidate.terminator);
          if (std::find(outgoing.begin(), outgoing.end(), cursor->id) ==
              outgoing.end()) {
            continue;
          }
          ++predecessorCount;
          if (candidate.terminator.kind == MirTerminatorKind::Invoke &&
              candidate.terminator.target == cursor->id &&
              candidate.activeFailure == cursor->activeFailure) {
            predecessor = &candidate;
          }
        }
        if (predecessorCount != 1 || predecessor == nullptr) {
          break;
        }
        cursor = predecessor;
        cursorIndex = cursor->instructions.size();
      }
      std::reverse(reversed.begin(), reversed.end());
      if (reversed.size() != boundary.obligations.size()) {
        return failure(body, owner,
                       "hosted-startup failure cleanup is not connected to "
                       "its boundary marker",
                       block.id, marker.id);
      }
      for (std::size_t index = 0; index < reversed.size(); ++index) {
        const MirInstruction &drop = *reversed[index];
        if (drop.lifecycle.size() != 1 ||
            drop.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            !drop.lifecycle.front().failureCleanup ||
            drop.lifecycle.front().source != boundary.obligations[index]) {
          return failure(body, owner,
                         "hosted-startup failure boundary does not name its "
                         "exact drop sequence",
                         block.id, drop.id);
        }
        ++failureDropCoverage[drop.id];
      }
    }
  }
  if (std::any_of(cleanupBoundaryMarkers.begin(), cleanupBoundaryMarkers.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "hosted-startup cleanup boundary lacks one exact marker");
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Drop &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().failureCleanup &&
          failureDropCoverage[instruction.id] != 1) {
        return failure(body, owner,
                       "hosted-startup failure drop is not covered exactly "
                       "once",
                       block.id, instruction.id);
      }
    }
  }

  if (std::any_of(definitions.begin(), definitions.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "hosted-startup value lacks one exact definition");
  }
  const std::vector<bool> reachable = reachableBlocks(body);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (!reachable[index] || body.blocks[index].reachable != reachable[index]) {
      return failure(body, owner,
                     "hosted-startup reachability inventory is invalid",
                     body.blocks[index].id);
    }
  }
  MirBody indexed = body;
  if (!rebuildMirValueUses(indexed) || indexed.valueUses != body.valueUses) {
    return failure(body, owner,
                   "hosted-startup value-use inventory is invalid");
  }
  if (MirVerificationResult lifecycle = verifyMirLifecycleFlow(body, owner);
      !lifecycle.valid()) {
    return lifecycle;
  }
  return {};
}

[[nodiscard]] bool bodyHasHostedStartupProvenance(const MirBody &body) {
  return std::any_of(body.cleanupBoundaries.begin(),
                     body.cleanupBoundaries.end(),
                     [](const MirCleanupBoundary &boundary) {
                       return boundary.hostedStartupOperation != 0;
                     }) ||
         std::any_of(body.failureRecords.begin(), body.failureRecords.end(),
                     [](const MirFailureRecord &record) {
                       return record.hostedStartupOperation != 0;
                     }) ||
         std::any_of(body.places.begin(), body.places.end(),
                     [](const MirPlace &place) {
                       return place.hostedStartupOperation != 0;
                     }) ||
         std::any_of(body.dropObligations.begin(), body.dropObligations.end(),
                     [](const MirDropObligation &drop) {
                       return drop.hostedStartupOperation != 0 ||
                              drop.generatedValue != 0;
                     }) ||
         std::any_of(body.values.begin(), body.values.end(),
                     [](const MirValue &value) {
                       return value.hostedStartupOperation != 0;
                     }) ||
         std::any_of(
             body.blocks.begin(), body.blocks.end(), [](const MirBlock &block) {
               return block.terminator.hostedStartupOperation != 0 ||
                      std::any_of(
                          block.instructions.begin(), block.instructions.end(),
                          [](const MirInstruction &instruction) {
                            return instruction.hostedStartupOperation != 0 ||
                                   instruction.kind ==
                                       MirInstructionKind::CallBody ||
                                   instruction.bodyTarget.has_value();
                          });
             });
}

[[nodiscard]] MirVerificationResult
verifyMirRangeIterations(const MirBody &body, std::size_t owner) {
  const std::optional<MirDominanceInfo> dominance = computeMirDominance(body);
  if (!dominance) {
    return failure(body, owner,
                   "range iteration inventory has an invalid body CFG");
  }
  for (std::size_t index = 0; index < body.rangeIterations.size(); ++index) {
    const MirRangeIteration &range = body.rangeIterations[index];
    const MirPlace *ownerPlace = body.findPlace(range.owner);
    const MirPlace *rangePlace = body.findPlace(range.rangeCarrier);
    const MirPlace *iteratorPlace = body.findPlace(range.iterator);
    const MirPlace *sentinelPlace = body.findPlace(range.sentinel);
    const MirPlace *elementPlace = body.findPlace(range.element);
    const MirLoan *rangeLoan = body.findLoan(range.rangeLoan);
    const MirLoan *elementLoan = body.findLoan(range.elementLoan);
    const MirBlock *conditionEntry = body.findBlock(range.conditionEntryBlock);
    const MirBlock *condition = body.findBlock(range.conditionBlock);
    const MirBlock *bodyBlock = body.findBlock(range.bodyBlock);
    const MirBlock *increment = body.findBlock(range.incrementBlock);
    const MirBlock *cleanup = body.findBlock(range.cleanupBlock);
    const MirBlock *exit = body.findBlock(range.exitBlock);
    const MirValue *payloadExtraction =
        body.findValue(range.expectedPayloadExtraction);
    const MirInstruction *payloadProducer = nullptr;
    const MirBlock *payloadDefinitionBlock = nullptr;
    if (payloadExtraction != nullptr && payloadExtraction->definition != 0) {
      payloadDefinitionBlock =
          body.findBlock(payloadExtraction->definitionBlock);
      for (const MirBlock &candidateBlock : body.blocks) {
        const auto found = std::find_if(candidateBlock.instructions.begin(),
                                        candidateBlock.instructions.end(),
                                        [&](const MirInstruction &instruction) {
                                          return instruction.id ==
                                                 payloadExtraction->definition;
                                        });
        if (found != candidateBlock.instructions.end()) {
          payloadProducer = &*found;
          break;
        }
      }
    }
    const std::size_t payloadRoots =
        payloadExtraction == nullptr
            ? 0
            : std::count_if(body.places.begin(), body.places.end(),
                            [&](const MirPlace &place) {
                              return place.root == MirPlaceRootKind::Loan &&
                                     place.loan == range.rangeLoan &&
                                     place.projections.empty() &&
                                     place.type == range.rangeType &&
                                     place.sourceValue ==
                                         payloadExtraction->sourceValue;
                            });
    if (range.id != index + 1 || range.hirStatement == 0 ||
        range.coreLoop == 0 || ownerPlace == nullptr || rangePlace == nullptr ||
        iteratorPlace == nullptr || sentinelPlace == nullptr ||
        elementPlace == nullptr || rangeLoan == nullptr ||
        conditionEntry == nullptr || condition == nullptr ||
        bodyBlock == nullptr || increment == nullptr || cleanup == nullptr ||
        exit == nullptr) {
      return failure(body, owner,
                     "range iteration inventory contains an invalid identity");
    }
    const auto carries = [](const MirLoan &loan, HirBindingId binding) {
      return binding != 0 &&
             std::find(loan.carriers.begin(), loan.carriers.end(), binding) !=
                 loan.carriers.end();
    };
    if (rangeLoan->access != range.elementAccess || rangeLoan->escapes ||
        (range.elementLoan != 0 &&
         (elementLoan == nullptr ||
          elementLoan->access != range.elementAccess || elementLoan->escapes ||
          (range.elementAccess == AccessMode::Mutable &&
           (range.elementLoan == range.rangeLoan ||
            elementLoan->parent != range.rangeLoan))))) {
      return failure(body, owner,
                     "range iteration contains an incoherent or escaping "
                     "range/element loan");
    }
    if (!carries(*rangeLoan, rangePlace->binding) ||
        (range.elementLoan != 0 &&
         !carries(*elementLoan, elementPlace->binding)) ||
        (range.elementAccess == AccessMode::Mutable &&
         range.strategy == RangeIterationStrategy::Structural &&
         !carries(*rangeLoan, iteratorPlace->binding))) {
      return failure(body, owner,
                     "range iteration loan does not retain its exact carrier");
    }
    if (condition->terminator.kind != MirTerminatorKind::Branch ||
        condition->terminator.target != range.bodyBlock ||
        increment->terminator.kind != MirTerminatorKind::Goto ||
        increment->terminator.target != range.conditionEntryBlock ||
        cleanup->terminator.kind != MirTerminatorKind::Goto ||
        cleanup->terminator.target != range.exitBlock) {
      return failure(
          body, owner,
          "range iteration has no exact compare/body/increment "
          "control-flow cycle (condition-kind=" +
              std::to_string(static_cast<int>(condition->terminator.kind)) +
              ", condition-target=" +
              std::to_string(condition->terminator.target) + ", body=" +
              std::to_string(range.bodyBlock) + ", increment-kind=" +
              std::to_string(static_cast<int>(increment->terminator.kind)) +
              ", increment-target=" +
              std::to_string(increment->terminator.target) +
              ", condition=" + std::to_string(range.conditionEntryBlock) + ")",
          range.conditionBlock);
    }
    if (range.strategy == RangeIterationStrategy::FixedArray &&
        (range.rangeType.kind != SemanticType::Array ||
         range.rangeType.arrayLengthParameterId != 0 ||
         range.rangeType.arrayLength != range.arrayLength ||
         range.arrayLength == 0 || range.iteratorType != SemanticType::UInt64 ||
         range.sentinelType != SemanticType::UInt64)) {
      return failure(body, owner,
                     "fixed-array range iteration lost its indexed strategy");
    }
    if (range.ownsRangeTemporary !=
        (ownerPlace->type.kind != SemanticType::Reference)) {
      return failure(body, owner,
                     "range owner temporary classification is inconsistent");
    }
    if (range.ownsRangeTemporary &&
        ownerPlace->traits.drop == DropKind::Lexical) {
      const MirDropObligation *drop = body.findDropObligation(range.ownerDrop);
      if (drop == nullptr || drop->place != range.owner ||
          !drop->dropType.requiresActiveCleanup) {
        return failure(body, owner,
                       "owned range temporary lacks its exact cleanup "
                       "obligation");
      }
    } else if (range.ownerDrop != 0) {
      const MirDropObligation *drop = body.findDropObligation(range.ownerDrop);
      if (drop == nullptr || drop->place != range.owner) {
        return failure(body, owner,
                       "range owner cleanup targets a different place");
      }
    }
    const MirPlace *rangeLoanSource = body.findPlace(rangeLoan->source);
    const bool exactExpectedPayloadProjection =
        !range.expectedPayloadProjection
            ? range.expectedPayloadExtraction == 0
            : payloadExtraction != nullptr && payloadProducer != nullptr &&
                  payloadProducer->kind == MirInstructionKind::Call &&
                  payloadProducer->intrinsic == IntrinsicKind::ExpectedValue &&
                  payloadProducer->borrowOrigin == BorrowOriginKind::Receiver &&
                  payloadProducer->borrowAccess == AccessMode::ReadOnly &&
                  payloadProducer->receiver &&
                  payloadProducer->receiver->kind ==
                      MirOperandKind::BorrowRead &&
                  payloadProducer->receiver->place == rangeLoan->source &&
                  payloadProducer->operands.empty() &&
                  payloadProducer->result == range.expectedPayloadExtraction &&
                  payloadProducer->loan == range.rangeLoan &&
                  payloadProducer->localFailureSites.size() == 1 &&
                  payloadProducer->definedFailure.localOrigins.size() == 1 &&
                  payloadProducer->definedFailure.propagation ==
                      FailurePropagationKind::None &&
                  payloadDefinitionBlock != nullptr &&
                  payloadDefinitionBlock->terminator.kind ==
                      MirTerminatorKind::Invoke &&
                  payloadDefinitionBlock->terminator.invokeInstruction ==
                      payloadProducer->id &&
                  rangeLoan->kind == MirLoanKind::CallResult &&
                  rangeLoan->producedBy == payloadExtraction->sourceValue &&
                  rangeLoanSource != nullptr &&
                  rangeLoanSource->type.kind == SemanticType::Expected &&
                  rangeLoanSource->type.arguments.size() == 2 &&
                  rangeLoanSource->type.arguments.front() == range.rangeType &&
                  payloadExtraction->info.type == range.rangeType &&
                  payloadExtraction->info.category == ValueCategory::Place &&
                  payloadRoots == 1 &&
                  dominance->dominates(payloadExtraction->definitionBlock,
                                       range.conditionEntryBlock);
    if (!exactExpectedPayloadProjection) {
      return failure(body, owner,
                     "expected-payload range lost its checked extraction, "
                     "rooted loan, or dominance proof");
    }
  }
  return {};
}

} // namespace

namespace mir_detail {

MirVerificationResult verificationFailure(const MirBody &body,
                                          std::size_t owner,
                                          std::string message, MirBlockId block,
                                          MirInstructionId instruction) {
  return failure(body, owner, std::move(message), block, instruction);
}

void appendVerification(MirVerificationResult &destination,
                        MirVerificationResult source) {
  append(destination, std::move(source));
}

std::vector<MirBlockId> terminatorSuccessors(const MirTerminator &terminator) {
  return successors(terminator);
}

bool constantMatchesType(const ConstantValue &constant,
                         const SemanticType &type, const MirProgram *program) {
  return programConstantMatchesType(constant, type, program);
}

bool validCallInputType(const SemanticType &source,
                        const SemanticType &parameter, HirCallInputKind kind) {
  if (source == parameter) {
    return true;
  }
  if (kind != HirCallInputKind::Value) {
    return false;
  }
  if (parameter == SemanticType::CString) {
    return source == SemanticType::StringView ||
           source == SemanticType::NullPtr;
  }
  if (parameter.kind == SemanticType::NativeFunction) {
    return parameter.hasNativeFunctionShape() &&
           source == SemanticType::NullPtr;
  }
  if (parameter.kind != SemanticType::RawPointer) {
    return false;
  }
  if (source == SemanticType::NullPtr) {
    return true;
  }
  return source.kind == SemanticType::RawPointer &&
         parameter.arguments.size() == 1 && source.arguments.size() == 1 &&
         parameter.arguments.front() == source.arguments.front() &&
         parameter.pointerAccess == AccessMode::ReadOnly &&
         source.pointerAccess == AccessMode::Mutable;
}

const MirInstruction *linearDefinition(const MirBody &body, MirValueId value) {
  return linearValueDefinition(body, value);
}

bool receiverIsConsumed(const MirBody &body, const MirOperand &operand,
                        MirValueUseKind expectedUseKind,
                        MirInstructionId expectedUseInstruction,
                        std::size_t expectedOperandIndex) {
  return consumedCallableReceiver(body, operand, expectedUseKind,
                                  expectedUseInstruction, expectedOperandIndex);
}

bool movedValueInto(const MirBody &body, const MirOperand &operand,
                    MirInstructionId expectedInstruction,
                    std::size_t expectedOperandIndex) {
  return movedValueIntoInstruction(body, operand, expectedInstruction,
                                   expectedOperandIndex);
}

bool freshOwnedValueInto(const MirBody &body, const MirOperand &operand,
                         MirInstructionId expectedInstruction,
                         std::size_t expectedOperandIndex) {
  return freshOwnedValueIntoInstruction(body, operand, expectedInstruction,
                                        expectedOperandIndex);
}

CanonicalPlace canonicalBorrowOrigin(
    const MirBody &body, MirPlaceId place,
    const std::unordered_map<HirBindingId, std::vector<MirLoanId>>
        &bindingLoans,
    bool physicalCarrier) {
  MirCanonicalPlace canonical =
      canonicalBorrowOriginPlace(body, place, bindingLoans, physicalCarrier);
  return {.root = canonical.root,
          .binding = canonical.binding,
          .symbol = canonical.symbol,
          .temporary = canonical.temporary,
          .value = canonical.value,
          .projections = std::move(canonical.projections),
          .throughLoan = canonical.throughLoan,
          .ambiguous = canonical.ambiguous};
}

bool sameCanonicalPlace(const CanonicalPlace &left,
                        const CanonicalPlace &right) {
  const MirCanonicalPlace lhs{.root = left.root,
                              .binding = left.binding,
                              .symbol = left.symbol,
                              .temporary = left.temporary,
                              .value = left.value,
                              .projections = left.projections,
                              .throughLoan = left.throughLoan,
                              .ambiguous = left.ambiguous};
  const MirCanonicalPlace rhs{.root = right.root,
                              .binding = right.binding,
                              .symbol = right.symbol,
                              .temporary = right.temporary,
                              .value = right.value,
                              .projections = right.projections,
                              .throughLoan = right.throughLoan,
                              .ambiguous = right.ambiguous};
  return ::lang::sameCanonicalPlace(lhs, rhs);
}

bool sameGlobalBorrowPlace(const BorrowOriginPlace &expected,
                           const CanonicalPlace &actual) {
  const MirCanonicalPlace canonical{.root = actual.root,
                                    .binding = actual.binding,
                                    .symbol = actual.symbol,
                                    .temporary = actual.temporary,
                                    .value = actual.value,
                                    .projections = actual.projections,
                                    .throughLoan = actual.throughLoan,
                                    .ambiguous = actual.ambiguous};
  return ::lang::sameGlobalBorrowPlace(expected, canonical);
}

} // namespace mir_detail

// Base projections are constructor-lifetime identities, not general lvalues.
// Keep the first base protocol closed: only its InitializeBase, failure Drop,
// and completion TransferOut may mention the subobject or its obligation.
static MirVerificationResult verifyBaseInitializations(const MirBody &body,
                                                       std::size_t owner) {
  const MirPlace *base = nullptr;
  for (const MirPlace &place : body.places) {
    if (std::none_of(place.projections.begin(), place.projections.end(),
                     [](const MirPlaceProjection &projection) {
                       return projection.kind == MirProjectionKind::Base;
                     }))
      continue;
    if (base != nullptr || place.binding || place.symbol || place.capture ||
        place.temporary || place.value || place.loan || place.sourceValue ||
        place.key || place.initiallyAvailable)
      return failure(body, owner,
                     "base initialization place is aliased or invalid");
    base = &place;
  }
  if (base == nullptr)
    return {};
  const MirDropObligation *rollback = nullptr;
  for (const auto &drop : body.dropObligations) {
    if (drop.place != base->id)
      continue;
    if (rollback || drop.kind != MirDropObligationKind::ConstructionRollback ||
        drop.initiallyActive || drop.hirObligation || drop.binding ||
        drop.value || drop.generatedValue || drop.hirFullExpression ||
        drop.fullExpression)
      return failure(body, owner,
                     "base initialization needs one rollback obligation");
    rollback = &drop;
  }
  if (rollback == nullptr)
    return failure(body, owner, "base initialization rollback is missing");
  const auto baseOperand = [&](const MirOperand &operand) {
    return operand.place == base->id;
  };
  for (const auto &loan : body.loans) {
    if (loan.protectsSource(base->id))
      return failure(body, owner,
                     "base initialization place cannot be borrowed");
  }
  std::size_t stages = 0;
  for (const auto &block : body.blocks) {
    for (const auto &instruction : block.instructions) {
      const bool stage =
          instruction.kind == MirInstructionKind::InitializeBase &&
          instruction.destination == base->id &&
          instruction.successResultDrop == rollback->id;
      const bool drop =
          instruction.kind == MirInstructionKind::Drop &&
          instruction.destination == base->id &&
          instruction.lifecycle.size() == 1 &&
          instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
          instruction.lifecycle.front().source == rollback->id &&
          instruction.lifecycle.front().failureCleanup;
      if (stage)
        ++stages;
      if ((instruction.receiver && baseOperand(*instruction.receiver)) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      baseOperand) ||
          instruction.successResultDestination == base->id ||
          (instruction.destination == base->id && !stage && !drop) ||
          (instruction.successResultDrop == rollback->id && !stage))
        return failure(body, owner,
                       "base initialization has a foreign place use", block.id,
                       instruction.id);
      for (const auto &event : instruction.lifecycle) {
        if (event.source != rollback->id && event.target != rollback->id)
          continue;
        const bool retirement =
            instruction.kind == MirInstructionKind::Lifecycle &&
            event.kind == MirLifecycleEventKind::TransferOut &&
            event.source == rollback->id && event.target == 0 &&
            !event.conditional && !event.failureCleanup;
        const bool activation =
            stage && instruction.definedFailure.empty() &&
            instruction.lifecycle.size() == 1 &&
            event.kind == MirLifecycleEventKind::Initialize &&
            event.source == 0 && event.target == rollback->id &&
            !event.conditional && !event.failureCleanup;
        if (!drop && !retirement && !activation)
          return failure(body, owner,
                         "base initialization has a foreign lifecycle use",
                         block.id, instruction.id);
      }
    }
    const auto &term = block.terminator;
    if ((term.value && baseOperand(*term.value)) ||
        std::any_of(term.targetArguments.begin(), term.targetArguments.end(),
                    baseOperand) ||
        std::any_of(term.elseArguments.begin(), term.elseArguments.end(),
                    baseOperand))
      return failure(body, owner, "base initialization place cannot escape",
                     block.id);
    for (const auto &target : term.switchTargets) {
      if (std::any_of(target.arguments.begin(), target.arguments.end(),
                      baseOperand))
        return failure(body, owner, "base initialization place cannot escape",
                       block.id);
    }
  }
  if (stages != 1)
    return failure(body, owner, "base initialization needs exactly one stage");
  return {};
}

// Raw object storage is not an owning T value. Freeze its complete, closed
// protocol independently of the general lifecycle dataflow verifier: allocate,
// construct on the allocation-success edge, then retire storage only on the
// construction-success edge. The dataflow verifier additionally proves that
// every failure path drains the still-armed resource exactly once.
static MirVerificationResult verifyObjectAllocations(const MirBody &body,
                                                     std::size_t owner) {
  struct Protocol {
    const MirInstruction *allocate = nullptr;
    const MirBlock *allocationBlock = nullptr;
    const MirInstruction *construct = nullptr;
    const MirBlock *constructionBlock = nullptr;
    const MirInstruction *retire = nullptr;
  };
  std::vector<Protocol> protocols(body.dropObligations.size());
  std::unordered_map<MirPlaceId, MirDropObligationId> rawPlaces;
  for (const auto &drop : body.dropObligations) {
    if (drop.kind != MirDropObligationKind::ObjectAllocation) {
      continue;
    }
    const MirPlace *place = body.findPlace(drop.place);
    if (place == nullptr || !place->projections.empty() || place->binding ||
        place->symbol || place->value || place->loan || place->capture ||
        place->key || place->initiallyAvailable || drop.generatedValue ||
        drop.initiallyActive ||
        !rawPlaces.emplace(drop.place, drop.id).second) {
      return failure(body, owner, "object allocation has an invalid raw place");
    }
    for (const auto &candidate : body.places) {
      if (candidate.id != place->id &&
          candidate.root == MirPlaceRootKind::Temporary &&
          candidate.temporary == place->temporary) {
        return failure(body, owner, "object allocation raw place is aliased");
      }
    }
  }
  const auto rawOperand = [&](const MirOperand &operand) {
    return rawPlaces.contains(operand.place);
  };
  const auto rawEvent = [&](const MirLifecycleEvent &event) {
    const auto *source = body.findDropObligation(event.source);
    const auto *target = body.findDropObligation(event.target);
    return (source &&
            source->kind == MirDropObligationKind::ObjectAllocation) ||
           (target && target->kind == MirDropObligationKind::ObjectAllocation);
  };
  for (const auto &loan : body.loans) {
    if (rawPlaces.contains(loan.source) ||
        std::any_of(loan.additionalDependencies.begin(),
                    loan.additionalDependencies.end(),
                    [&](const auto &dependency) {
                      return rawPlaces.contains(dependency.source);
                    })) {
      return failure(body, owner, "object allocation cannot be borrowed");
    }
  }
  for (const auto &block : body.blocks) {
    for (const auto &instruction : block.instructions) {
      const bool construction =
          instruction.kind == MirInstructionKind::Call &&
          instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
          instruction.constructorKind == ConstructorKind::Ordinary &&
          instruction.constructorTarget &&
          instruction.info.type.kind == SemanticType::UniqueOwner &&
          instruction.info.type.arguments.size() == 1 &&
          instruction.info.type.arguments.front().kind == SemanticType::Class;
      if ((construction ||
           instruction.kind == MirInstructionKind::AllocateObject) &&
          instruction.objectAllocation == 0) {
        return failure(body, owner, "object allocation protocol is missing",
                       block.id, instruction.id);
      }
      if (instruction.objectAllocation != 0) {
        const auto *drop =
            body.findDropObligation(instruction.objectAllocation);
        if (!drop || drop->kind != MirDropObligationKind::ObjectAllocation ||
            instruction.hirValue != body.findPlace(drop->place)->sourceValue) {
          return failure(body, owner, "object allocation identity is invalid",
                         block.id, instruction.id);
        }
        auto &protocol = protocols[drop->id - 1];
        bool valid = false;
        if (instruction.kind == MirInstructionKind::AllocateObject) {
          const std::vector<DefinedFailureOutcome> outcomes{
              {DefinedFailureCode::AllocationFailure,
               DefinedFailureDetail::UniqueOwner}};
          valid = !protocol.allocate &&
                  instruction.definedFailure.propagation ==
                      FailurePropagationKind::None &&
                  instruction.definedFailure.localOrigins.size() == 1 &&
                  instruction.definedFailure.localOrigins.front().outcomes ==
                      outcomes &&
                  instruction.safetyDischarges.empty();
          protocol.allocate = &instruction;
          protocol.allocationBlock = &block;
        } else if (construction) {
          valid = !protocol.construct && !instruction.receiver &&
                  !instruction.destination && !instruction.loan &&
                  !instruction.functionTarget && !instruction.lambdaTarget &&
                  instruction.info.type == drop->dropType.type &&
                  instruction.definedFailure.localOrigins.empty() &&
                  instruction.safetyDischarges.empty();
          protocol.construct = &instruction;
          protocol.constructionBlock = &block;
        } else if (instruction.kind == MirInstructionKind::Lifecycle) {
          valid = !protocol.retire &&
                  instruction.lifecycle ==
                      std::vector<MirLifecycleEvent>{
                          {.kind = MirLifecycleEventKind::TransferOut,
                           .source = drop->id}};
          protocol.retire = &instruction;
        }
        if (!valid) {
          return failure(body, owner,
                         "object allocation protocol instruction is invalid",
                         block.id, instruction.id);
        }
      }
      if ((instruction.receiver && rawOperand(*instruction.receiver)) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      rawOperand)) {
        return failure(body, owner,
                       "object allocation cannot be used as a live object",
                       block.id, instruction.id);
      }
      if (instruction.destination &&
          rawPlaces.contains(*instruction.destination)) {
        const auto id = rawPlaces.at(*instruction.destination);
        const bool allocation =
            instruction.kind == MirInstructionKind::AllocateObject &&
            instruction.objectAllocation == id;
        const bool cleanup =
            instruction.kind == MirInstructionKind::Drop &&
            instruction.definedFailure.empty() &&
            !instruction.constructorTarget && !instruction.functionTarget &&
            instruction.lifecycle == std::vector<MirLifecycleEvent>{
                                         {.kind = MirLifecycleEventKind::Drop,
                                          .source = id,
                                          .failureCleanup = true}};
        if (!allocation && !cleanup) {
          return failure(body, owner,
                         "object allocation has an invalid storage use",
                         block.id, instruction.id);
        }
      }
      for (const auto &event : instruction.lifecycle) {
        if (rawEvent(event) &&
            !((instruction.kind == MirInstructionKind::Lifecycle &&
               instruction.objectAllocation == event.source &&
               event.kind == MirLifecycleEventKind::TransferOut) ||
              (instruction.kind == MirInstructionKind::Drop &&
               instruction.destination &&
               rawPlaces.contains(*instruction.destination) &&
               rawPlaces.at(*instruction.destination) == event.source))) {
          return failure(body, owner,
                         "object allocation has a foreign lifecycle event",
                         block.id, instruction.id);
        }
      }
    }
    const auto &edge = block.terminator;
    if ((edge.value && rawOperand(*edge.value)) ||
        std::any_of(edge.targetArguments.begin(), edge.targetArguments.end(),
                    rawOperand) ||
        std::any_of(edge.elseArguments.begin(), edge.elseArguments.end(),
                    rawOperand)) {
      return failure(body, owner,
                     "object allocation cannot escape its protocol", block.id);
    }
    for (const auto &target : edge.switchTargets) {
      if (std::any_of(target.arguments.begin(), target.arguments.end(),
                      rawOperand)) {
        return failure(body, owner,
                       "object allocation cannot escape through a switch",
                       block.id);
      }
    }
    for (const auto &event : edge.successLifecycle) {
      if (rawEvent(event)) {
        const auto *drop = body.findDropObligation(event.target);
        const auto &protocol = drop ? protocols[drop->id - 1] : Protocol{};
        if (!drop || protocol.allocationBlock != &block ||
            event !=
                MirLifecycleEvent{.kind = MirLifecycleEventKind::Initialize,
                                  .target = drop->id}) {
          return failure(body, owner,
                         "object allocation has a foreign success event",
                         block.id);
        }
      }
    }
  }
  const auto firstLive = [](const MirBlock *block) -> const MirInstruction * {
    if (block) {
      for (const auto &instruction : block->instructions) {
        if (!instruction.tombstone)
          return &instruction;
      }
    }
    return nullptr;
  };
  for (const auto &drop : body.dropObligations) {
    if (drop.kind != MirDropObligationKind::ObjectAllocation)
      continue;
    const auto &protocol = protocols[drop.id - 1];
    if (!protocol.allocate || !protocol.construct || !protocol.retire ||
        protocol.allocationBlock->terminator.kind !=
            MirTerminatorKind::Invoke ||
        protocol.allocationBlock->terminator.invokeInstruction !=
            protocol.allocate->id ||
        firstLive(
            body.findBlock(protocol.allocationBlock->terminator.target)) !=
            protocol.construct) {
      return failure(
          body, owner,
          "object allocation must construct only on allocation success");
    }
    const auto ownerResult =
        std::find_if(body.dropObligations.begin(), body.dropObligations.end(),
                     [&](const MirDropObligation &candidate) {
                       return candidate.kind == MirDropObligationKind::Value &&
                              candidate.value == protocol.construct->hirValue &&
                              candidate.dropType.type == drop.dropType.type;
                     });
    if (ownerResult == body.dropObligations.end() ||
        ownerResult->fullExpression != drop.fullExpression) {
      return failure(
          body, owner,
          "object allocation and owner must share one full expression");
    }
    const MirInstruction *success = nullptr;
    if (protocol.construct->definedFailure.empty()) {
      bool found = false;
      for (const auto &instruction : protocol.constructionBlock->instructions) {
        if (instruction.tombstone)
          continue;
        if (found) {
          success = &instruction;
          break;
        }
        found = &instruction == protocol.construct;
      }
    } else if (protocol.constructionBlock->terminator.kind ==
                   MirTerminatorKind::Invoke &&
               protocol.constructionBlock->terminator.invokeInstruction ==
                   protocol.construct->id) {
      success = firstLive(
          body.findBlock(protocol.constructionBlock->terminator.target));
    }
    if (success != protocol.retire) {
      return failure(
          body, owner,
          "object allocation must transfer only on construction success");
    }
  }
  return {};
}

MirVerificationResult verifyMirBody(const MirBody &body, std::size_t owner) {
  if (body.hasCallerLocation && body.kind != MirBodyKind::Function) {
    return failure(body, owner,
                   "only a source function body can receive caller location");
  }
  if (auto vocabulary = verifyMirOptimizationVocabulary(body, owner);
      !vocabulary.valid()) {
    return vocabulary;
  }
  if (body.entry == 0 || body.entry > body.blocks.size()) {
    return failure(body, owner, "entry block is outside the body");
  }
  if (std::any_of(body.values.begin(), body.values.end(),
                  [](const MirValue &value) {
                    return value.info.type.kind ==
                           SemanticType::FunctionSignature;
                  }) ||
      std::any_of(body.places.begin(), body.places.end(),
                  [](const MirPlace &place) {
                    return place.type.kind == SemanticType::FunctionSignature;
                  })) {
    return failure(
        body, owner,
        "function signature metadata cannot be a runtime value or place");
  }
  if (body.kind == MirBodyKind::HostedStartup) {
    return verifyMirHostedStartupBodyStructure(body, owner);
  }
  if (bodyHasHostedStartupProvenance(body)) {
    return failure(body, owner,
                   "source MIR body retains generated hosted-startup "
                   "provenance");
  }

  std::unordered_map<HirValueId, const ConstantValue *>
      expectedProgramConstants;
  for (const MirProgramConstantSubstitution &substitution :
       body.programConstantSubstitutions) {
    if (substitution.hirValue == 0 ||
        std::holds_alternative<ConstantCheckedIntegerResult>(
            substitution.constant) ||
        !expectedProgramConstants
             .emplace(substitution.hirValue, &substitution.constant)
             .second) {
      return failure(body, owner,
                     "program-constant substitution inventory is invalid");
    }
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
  std::unordered_set<HirFullExpressionId> hirFullExpressions;
  for (std::size_t index = 0; index < body.fullExpressions.size(); ++index) {
    const MirFullExpression &expression = body.fullExpressions[index];
    if (expression.id != index + 1 || expression.hirExpression == 0 ||
        (index != 0 && expression.hirExpression <=
                           body.fullExpressions[index - 1].hirExpression) ||
        !hirFullExpressions.insert(expression.hirExpression).second ||
        expression.roots.empty() ||
        ((expression.statement == 0) ==
         (expression.constructorInitializer == 0)) ||
        std::any_of(expression.roots.begin(), expression.roots.end(),
                    [&](HirValueId root) { return root == 0; })) {
      return failure(body, owner,
                     "full-expression table has an invalid identity or root");
    }
  }
  for (std::size_t index = 0; index < body.cleanupBoundaries.size(); ++index) {
    const MirCleanupBoundary &boundary = body.cleanupBoundaries[index];
    std::size_t previous = std::numeric_limits<std::size_t>::max();
    if (boundary.id != index + 1 || boundary.obligations.empty() ||
        boundary.kind >= MirCleanupBoundaryKind::Count) {
      return failure(body, owner,
                     "cleanup-boundary table has an invalid identity");
    }
    for (const MirDropObligationId obligationId : boundary.obligations) {
      const MirDropObligation *obligation =
          body.findDropObligation(obligationId);
      if (obligation == nullptr ||
          (boundary.kind == MirCleanupBoundaryKind::Normal &&
           obligation->kind != MirDropObligationKind::Binding) ||
          obligation->constructionOrder >= previous) {
        return failure(body, owner,
                       "cleanup-boundary obligations are not an exact "
                       "reverse construction sequence");
      }
      previous = obligation->constructionOrder;
    }
  }
  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    const MirFailureRecord &record = body.failureRecords[index];
    if (record.id != index + 1 || record.producerBlock == 0 ||
        record.producerBlock > body.blocks.size() ||
        record.producerInstruction == 0 || record.parameterBlock == 0 ||
        record.parameterBlock > body.blocks.size() ||
        record.producerBlock == record.parameterBlock) {
      return failure(body, owner,
                     "failure-record table has an invalid identity or edge");
    }
  }
  if ((body.kind == MirBodyKind::Constructor ||
       body.kind == MirBodyKind::FieldInitializers) &&
      !mirBodyRoutesFailureEdges(body) && !body.failureRecords.empty()) {
    return failure(body, owner,
                   "construction body with an unarmed subobject transfer "
                   "must not carry defined-failure edges");
  }
  const auto validOperand = [&](const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value: {
      const MirValue *value = body.findValue(operand.value);
      return value != nullptr && operand.place == 0 && operand.loan == 0 &&
             !operand.literal && operand.type == value->info.type;
    }
    case MirOperandKind::Constant:
      return operand.value == 0 && operand.place == 0 && operand.loan == 0 &&
             operand.literal &&
             literalMatchesType(*operand.literal, operand.type);
    case MirOperandKind::Address:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      return operand.value == 0 && operand.loan == 0 && !operand.literal &&
             validPlace(operand.place);
    case MirOperandKind::Loan:
      if (operand.literal || !validLoan(operand.loan)) {
        return false;
      }
      if (operand.value != 0) {
        const MirValue *value = body.findValue(operand.value);
        return operand.place == 0 && value != nullptr &&
               value->info.type == operand.type;
      }
      return operand.place == 0 ||
             (validPlace(operand.place) &&
              ((operand.type.kind == SemanticType::Reference &&
                operand.type.arguments.size() == 1 &&
                body.places[operand.place - 1].type ==
                    operand.type.arguments.front()) ||
               body.places[operand.place - 1].type == operand.type));
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
    case MirOperation::NativeCallback:
    case MirOperation::FunctionItem:
    case MirOperation::PayloadConstruct:
    case MirOperation::PayloadExtract:
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
    if (instruction.operation == MirOperation::Convert &&
        (resultType.kind == SemanticType::RawPointer ||
         (!instruction.operands.empty() &&
          instruction.operands.front().type.kind == SemanticType::RawPointer)))
      return resultType == instruction.info.type &&
             mir_detail::rawPointerConversion(instruction);
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
    if (instruction.operation == MirOperation::Identity) {
      return instruction.operands.size() == 1 &&
             instruction.operands.front().type == resultType;
    }
    if (instruction.operation == MirOperation::ExpectedHasValue) {
      if (instruction.operands.size() != 1 ||
          resultType != SemanticType::Bool) {
        return false;
      }
      const MirOperand &operand = instruction.operands.front();
      if (operand.type.kind != SemanticType::Expected ||
          operand.type.arguments.size() != 2) {
        return false;
      }
      if (operand.kind == MirOperandKind::Value) {
        return true;
      }
      if (operand.kind != MirOperandKind::BorrowRead) {
        return false;
      }
      const MirPlace *place = body.findPlace(operand.place);
      return place != nullptr && place->type == operand.type;
    }
    if (isUnaryOperation(instruction.operation)) {
      return instruction.operands.size() == 1;
    }
    switch (instruction.operation) {
    case MirOperation::FunctionItem:
      return instruction.functionTarget && *instruction.functionTarget != 0 &&
             instruction.operands.empty() && resultType.hasFunctionItemShape();
    case MirOperation::NativeCallback:
      return instruction.nativeCallbackAdapter &&
             *instruction.nativeCallbackAdapter != 0 &&
             instruction.operands.empty() &&
             resultType.kind == SemanticType::NativeFunction;
    case MirOperation::Literal: {
      const MirLiteralProvenance &provenance = instruction.literalProvenance;
      const MirValue *source = provenance.sourceValue == 0
                                   ? nullptr
                                   : body.findValue(provenance.sourceValue);
      const bool computeFoldShape =
          provenance.kind == MirLiteralProvenanceKind::ComputeFold &&
          provenance.sourceValue == 0 && !provenance.sourceValues.empty() &&
          provenance.sourceValues.size() <= 2 &&
          std::all_of(provenance.sourceValues.begin(),
                      provenance.sourceValues.end(),
                      [&](MirValueId sourceValue) {
                        return sourceValue != 0 &&
                               sourceValue != *instruction.result &&
                               body.findValue(sourceValue) != nullptr;
                      });
      const bool sparseConstantShape =
          provenance.kind == MirLiteralProvenanceKind::SparseConstant &&
          provenance.sourceValue == 0 &&
          provenance.sourceOperation != MirOperation::None &&
          provenance.sourceOperation != MirOperation::Literal &&
          !provenance.sourceValues.empty() &&
          std::all_of(provenance.sourceValues.begin(),
                      provenance.sourceValues.end(),
                      [&](MirValueId sourceValue) {
                        return sourceValue != 0 &&
                               body.findValue(sourceValue) != nullptr;
                      });
      return instruction.operands.empty() && instruction.literal &&
             literalMatchesType(*instruction.literal, resultType) &&
             ((provenance.kind == MirLiteralProvenanceKind::Source &&
               provenance.sourceValue == 0) ||
              (provenance.kind == MirLiteralProvenanceKind::IdentityFold &&
               provenance.sourceValue != 0 &&
               provenance.sourceValue != *instruction.result &&
               source != nullptr && source->info.type == resultType) ||
              computeFoldShape || sparseConstantShape);
    }
    case MirOperation::EnumConstant:
      return instruction.operands.empty() && instruction.enumOwner &&
             *instruction.enumOwner != 0 && instruction.enumValue;
    case MirOperation::Aggregate:
      return true;
    case MirOperation::Closure:
      return instruction.lambdaTarget && *instruction.lambdaTarget != 0 &&
             instruction.closureCaptureTypes.size() ==
                 instruction.operands.size() &&
             instruction.closureCaptureModes.size() ==
                 instruction.operands.size();
    case MirOperation::PayloadConstruct:
      return instruction.enumOwner && instruction.enumVariant &&
             !instruction.payloadIndex &&
             resultType.kind == SemanticType::Enum &&
             resultType.enumId == *instruction.enumOwner;
    case MirOperation::PayloadExtract:
      return instruction.enumOwner && instruction.enumVariant &&
             instruction.payloadIndex && instruction.operands.size() == 1 &&
             instruction.operands.front().type.kind == SemanticType::Enum &&
             instruction.operands.front().type.enumId == *instruction.enumOwner;
    default:
      return false;
    }
  };
  const auto validInstructionShape = [&](const MirInstruction &instruction) {
    if (instruction.tombstone) {
      return instruction == makeMirInstructionTombstone(instruction);
    }
    const bool hasResult = instruction.result.has_value();
    const bool noOperation = instruction.operation == MirOperation::None;
    if (instruction.nativeCalleeType) {
      const auto &signature = *instruction.nativeCalleeType;
      const auto parameters = signature.nativeFunctionParameterTypes();
      if (!signature.hasNativeFunctionShape() ||
          instruction.kind != MirInstructionKind::Call ||
          instruction.unsafeOperation !=
              UnsafeOperationKind::ForeignPointerCall ||
          !instruction.receiver || instruction.receiver->type != signature ||
          instruction.functionTarget || instruction.constructorTarget ||
          instruction.lambdaTarget || instruction.bodyTarget ||
          instruction.intrinsic != IntrinsicKind::None ||
          instruction.callableInvocation !=
              CallableInvocationCapability::Read ||
          instruction.info.type != *signature.nativeFunctionReturnType() ||
          instruction.parameterTypes.size() != parameters.size() ||
          !std::equal(parameters.begin(), parameters.end(),
                      instruction.parameterTypes.begin()))
        return false;
    }
    const bool callable = instruction.kind == MirInstructionKind::Call ||
                          instruction.kind == MirInstructionKind::Construct;
    if (!instruction.borrowFields.empty() &&
        (!callable || instruction.borrowOrigin == BorrowOriginKind::None ||
         instruction.intrinsic != IntrinsicKind::None ||
         (!instruction.functionTarget && !instruction.constructorTarget)))
      return false;
    const bool callInput = instruction.kind == MirInstructionKind::CallInput;
    const bool targetlessBorrowProbe =
        instruction.kind == MirInstructionKind::Call &&
        instruction.parameterTypes.empty() && instruction.lifecycle.empty() &&
        !instruction.functionTarget && !instruction.constructorTarget &&
        !instruction.lambdaTarget;
    const bool sourceLocationIntrinsic =
        instruction.intrinsic == IntrinsicKind::SourceFileName ||
        instruction.intrinsic == IntrinsicKind::SourceFunctionName ||
        instruction.intrinsic == IntrinsicKind::SourceLine ||
        instruction.intrinsic == IntrinsicKind::SourceColumn;
    const bool sourceLocationText =
        instruction.intrinsic == IntrinsicKind::SourceFileName ||
        instruction.intrinsic == IntrinsicKind::SourceFunctionName;
    const SemanticType sourceLocationResult =
        sourceLocationText ? SemanticType::StringView : SemanticType::UInt32;
    const MirPlace *successResultDestination =
        instruction.successResultDestination
            ? body.findPlace(*instruction.successResultDestination)
            : nullptr;
    if ((instruction.kind != MirInstructionKind::Call &&
         (instruction.neverReturns || instruction.tracksCaller)) ||
        (instruction.tracksCaller != instruction.callerLocation.has_value()) ||
        (instruction.tracksCaller != (instruction.callerLocationSite != 0)) ||
        (instruction.forwardsCallerLocation !=
         (instruction.tracksCaller && body.hasCallerLocation)) ||
        (instruction.callerLocation &&
         (instruction.callerLocation->sourceUnit == 0 ||
          instruction.callerLocation->end <=
              instruction.callerLocation->start ||
          instruction.callerLocation->line < 1)) ||
        ((instruction.neverReturns || instruction.tracksCaller) &&
         !instruction.functionTarget &&
         instruction.intrinsic != IntrinsicKind::Panic) ||
        (instruction.ownership &&
         instruction.ownership->kind == OwnershipEventKind::ConsumeReceiver &&
         (!callInput ||
          instruction.callInputKind != HirCallInputKind::RetainedReceiver)) ||
        (instruction.retainedOwnerPlace &&
         (!callInput ||
          instruction.callInputKind != HirCallInputKind::RetainedReceiver)) ||
        !validSynchronizationOperation(instruction.synchronization) ||
        (instruction.replacementCleanup &&
         instruction.kind != MirInstructionKind::CleanupBeforeAssign) ||
        (instruction.replacementPair != 0 &&
         instruction.kind != MirInstructionKind::CleanupBeforeAssign &&
         instruction.kind != MirInstructionKind::Assign) ||
        (instruction.storagePreflight != 0 &&
         (instruction.kind != MirInstructionKind::Call ||
          instruction.intrinsic != IntrinsicKind::PrefixStorageAppend ||
          !instruction.constructorTarget ||
          instruction.constructorKind != ConstructorKind::Ordinary)) ||
        (instruction.constructorInitializer != 0 &&
         (body.kind != MirBodyKind::Constructor ||
          (instruction.kind != MirInstructionKind::Initialize &&
           instruction.kind != MirInstructionKind::InitializeBase))) ||
        (instruction.synchronization.kind !=
             SynchronizationOperationKind::None &&
         instruction.kind != MirInstructionKind::Call) ||
        (callable && !targetlessBorrowProbe &&
         instruction.parameterTypes.size() != instruction.operands.size()) ||
        (!callable && instruction.kind != MirInstructionKind::InitializeBase &&
         !instruction.parameterTypes.empty()) ||
        (callInput != instruction.callInputRole.has_value()) ||
        (callInput && instruction.callSite == 0) ||
        (!callInput && instruction.defaultArgument) ||
        (instruction.defaultArgument &&
         instruction.callInputRole != MirCallInputRole::Argument) ||
        (!callInput && instruction.preparedParameterDrop) ||
        (instruction.kind != MirInstructionKind::Call &&
         instruction.kind != MirInstructionKind::Construct &&
         instruction.kind != MirInstructionKind::AllocateObject &&
         instruction.kind != MirInstructionKind::InitializeBase &&
         instruction.successResultDrop) ||
        (instruction.kind != MirInstructionKind::Call &&
         instruction.kind != MirInstructionKind::Construct &&
         instruction.successResultDestination) ||
        (instruction.successResultDrop &&
         instruction.successResultDestination) ||
        (instruction.successResultDestination &&
         (successResultDestination == nullptr ||
          successResultDestination->root != MirPlaceRootKind::Binding ||
          !successResultDestination->projections.empty() ||
          successResultDestination->type != instruction.info.type)) ||
        (!callInput && instruction.callInputIndex != 0) ||
        (!callInput && instruction.callInputRole) ||
        (instruction.kind != MirInstructionKind::CallInput &&
         instruction.kind != MirInstructionKind::Call &&
         instruction.kind != MirInstructionKind::Construct &&
         instruction.kind != MirInstructionKind::StorageConstructPreflight &&
         instruction.callSite != 0) ||
        ((instruction.kind == MirInstructionKind::Call ||
          instruction.kind == MirInstructionKind::Construct) &&
         instruction.callSite != 0 &&
         instruction.callSite != instruction.hirValue) ||
        (instruction.kind != MirInstructionKind::Call &&
         (instruction.callableInvocation || instruction.callableBoundary ||
          !instruction.callableArguments.empty())) ||
        (instruction.operation != MirOperation::Closure &&
         (!instruction.closureCaptureTypes.empty() ||
          !instruction.closureCaptureModes.empty())) ||
        ((instruction.operation == MirOperation::NativeCallback) !=
         instruction.nativeCallbackAdapter.has_value()) ||
        (instruction.earlyExitFullExpressionEnd &&
         (instruction.kind != MirInstructionKind::Lifecycle ||
          instruction.fullExpressionEnd == 0 ||
          instruction.cleanupBoundaryEnd != 0)) ||
        (instruction.operation != MirOperation::Literal &&
         instruction.literal.has_value()) ||
        (instruction.operation != MirOperation::Literal &&
         (instruction.literalProvenance.kind !=
              MirLiteralProvenanceKind::None ||
          instruction.literalProvenance.sourceValue != 0)) ||
        (instruction.programConstantSubstitution &&
         (instruction.kind != MirInstructionKind::Compute ||
          (instruction.operation != MirOperation::Literal &&
           instruction.operation != MirOperation::EnumConstant &&
           instruction.operation != MirOperation::Negate))) ||
        (instruction.kind != MirInstructionKind::CallBody &&
         instruction.bodyTarget.has_value()) ||
        (instruction.sourceLocation.has_value() != sourceLocationIntrinsic) ||
        (sourceLocationIntrinsic &&
         (instruction.kind != MirInstructionKind::Call || !instruction.result ||
          instruction.info.type != sourceLocationResult ||
          instruction.info.category != ValueCategory::Value ||
          instruction.receiver || !instruction.operands.empty() ||
          !instruction.parameterTypes.empty() || instruction.functionTarget ||
          instruction.constructorTarget || instruction.lambdaTarget ||
          instruction.bodyTarget || instruction.callableInvocation ||
          instruction.callableBoundary ||
          !instruction.callableArguments.empty() ||
          instruction.synchronization.kind !=
              SynchronizationOperationKind::None ||
          !instruction.definedFailure.empty() ||
          !instruction.localFailureSites.empty() ||
          !instruction.lifecycle.empty()))) {
      return false;
    }
    if (simdIntrinsic(instruction.intrinsic)) {
      if (instruction.kind != MirInstructionKind::Call || instruction.result ||
          instruction.info.type != SemanticType::Void || instruction.receiver ||
          instruction.functionTarget || instruction.constructorTarget ||
          instruction.lambdaTarget || instruction.bodyTarget ||
          instruction.callableInvocation || instruction.callableBoundary ||
          instruction.loan || instruction.borrowPlace ||
          instruction.borrowOrigin != BorrowOriginKind::None ||
          !instruction.borrowArguments.empty() ||
          instruction.synchronization.kind !=
              SynchronizationOperationKind::None ||
          !instruction.callableArguments.empty() ||
          !instruction.definedFailure.empty() ||
          !instruction.localFailureSites.empty() ||
          !instruction.lifecycle.empty() || instruction.operands.size() != 3 ||
          instruction.parameterTypes.size() != 3)
        return false;
      std::array<SemanticType, 3> arrays;
      for (std::size_t index = 0; index < 3; ++index) {
        const SemanticType &parameter = instruction.parameterTypes[index];
        const MirOperand &operand = instruction.operands[index];
        if (parameter.kind != SemanticType::Reference ||
            parameter.arguments.size() != 1 ||
            parameter.referenceAccess !=
                (index == 2 ? AccessMode::Mutable : AccessMode::ReadOnly) ||
            operand.type != parameter ||
            operand.kind != (index == 2 ? MirOperandKind::BorrowWrite
                                        : MirOperandKind::BorrowRead))
          return false;
        const MirPlace *place = body.findPlace(operand.place);
        arrays[index] = parameter.arguments[0];
        if (place == nullptr || place->type != arrays[index] ||
            (index == 2 && place->access != AccessMode::Mutable) ||
            arrays[index].kind != SemanticType::Array ||
            arrays[index].arguments.size() != 1 ||
            arrays[index].arrayLengthParameterId != 0 ||
            arrays[index].arrayLength < 1 || arrays[index].arrayLength > 64)
          return false;
      }
      if (arrays[0] != arrays[1] ||
          (arrays[0].arguments[0] != SemanticType::Float &&
           arrays[0].arguments[0] != SemanticType::Double) ||
          arrays[0].arrayLength != arrays[2].arrayLength ||
          arrays[2].arguments[0] !=
              (simdComparisonIntrinsic(instruction.intrinsic)
                   ? SemanticType::Bool
                   : arrays[0].arguments[0]))
        return false;
    }
    const auto validCallableInvocation = [&]() {
      if (!instruction.callableInvocation) {
        return !instruction.callableBoundary;
      }
      if (*instruction.callableInvocation ==
          CallableInvocationCapability::Once) {
        return instruction.receiver &&
               consumedCallableReceiver(body, *instruction.receiver,
                                        MirValueUseKind::InstructionReceiver,
                                        instruction.id);
      }
      if (!instruction.receiver) {
        return false;
      }
      if (instruction.receiver->kind == MirOperandKind::Value ||
          instruction.receiver->kind == MirOperandKind::Move ||
          (*instruction.callableInvocation ==
               CallableInvocationCapability::Read &&
           (instruction.receiver->kind == MirOperandKind::BorrowRead ||
            instruction.receiver->kind == MirOperandKind::BorrowWrite))) {
        return true;
      }
      if (*instruction.callableInvocation ==
              CallableInvocationCapability::Mutable &&
          instruction.receiver->kind == MirOperandKind::BorrowWrite) {
        return true;
      }
      if (instruction.receiver->kind != MirOperandKind::Loan) {
        return false;
      }
      const MirLoan *loan = body.findLoan(instruction.receiver->loan);
      return loan != nullptr && (*instruction.callableInvocation ==
                                     CallableInvocationCapability::Read ||
                                 loan->access == AccessMode::Mutable);
    };
    const auto sameCallInputTraits = [](const SemanticTypeTraits &left,
                                        const SemanticTypeTraits &right) {
      return left.ownership == right.ownership && left.drop == right.drop &&
             left.copyable == right.copyable && left.movable == right.movable &&
             left.copyAssignable == right.copyAssignable &&
             left.moveAssignable == right.moveAssignable &&
             left.containsBorrowedState == right.containsBorrowedState &&
             left.transferCapable == right.transferCapable &&
             left.shareCapable == right.shareCapable &&
             left.relocation == right.relocation;
    };
    const auto validOwnedValueInputType = [](const ExpressionInfo &info) {
      return info.type.kind == SemanticType::Class ||
             info.type.kind == SemanticType::Lambda ||
             (info.type.kind == SemanticType::Enum &&
              info.traits.drop == DropKind::Lexical);
    };
    const auto validClassCopyInput = [&](const MirInstruction &input,
                                         const MirOperand &operand) {
      const MirPlace *place = body.findPlace(operand.place);
      // Entry-owned pack elements have one canonical lifetime place shared
      // by every expansion. Unlike expression-local projections, that place
      // has no single HIR expression origin; its exact entry obligation is
      // the source identity.
      const bool entryPackElement =
          place != nullptr && place->sourceValue == 0 &&
          place->root == MirPlaceRootKind::Binding &&
          place->initiallyAvailable && place->projections.size() == 1 &&
          place->projections.front().kind == MirProjectionKind::PackElement &&
          std::count_if(body.dropObligations.begin(),
                        body.dropObligations.end(),
                        [&](const MirDropObligation &drop) {
                          return drop.kind == MirDropObligationKind::Binding &&
                                 drop.binding == place->binding &&
                                 drop.place == place->id &&
                                 drop.initiallyActive &&
                                 drop.dropType.type == input.info.type;
                        }) == 1;
      const bool exactSource =
          validOwnedValueInputType(input.info) && input.info.traits.copyable &&
          (!input.info.traits.containsBorrowedState ||
           input.info.type.kind == SemanticType::Class) &&
          operand.kind == MirOperandKind::Copy && place != nullptr &&
          place->type == input.info.type &&
          (place->sourceValue == input.hirValue || entryPackElement) &&
          place->traits.copyable &&
          sameCallInputTraits(place->traits, input.info.traits);
      if (!exactSource) {
        return false;
      }
      if (!input.preparedParameterDrop) {
        return !input.destination && input.lifecycle.empty();
      }
      const MirDropObligation *prepared =
          body.findDropObligation(*input.preparedParameterDrop);
      return prepared != nullptr &&
             prepared->kind == MirDropObligationKind::PreparedParameter &&
             input.destination == prepared->place &&
             prepared->dropType.type == input.info.type &&
             input.lifecycle.size() == 1 &&
             input.lifecycle.front().kind ==
                 MirLifecycleEventKind::Initialize &&
             input.lifecycle.front().source == 0 &&
             input.lifecycle.front().target == prepared->id;
    };
    const auto validClassMoveInput = [&](const MirInstruction &input,
                                         const MirOperand &operand) {
      if (!validOwnedValueInputType(input.info) || !input.info.traits.movable ||
          (input.info.traits.containsBorrowedState &&
           input.info.type.kind != SemanticType::Class) ||
          operand.kind != MirOperandKind::Value) {
        return false;
      }
      const MirValue *source = body.findValue(operand.value);
      const MirInstruction *definition =
          source == nullptr ? nullptr
                            : linearValueDefinition(body, operand.value);
      if (source == nullptr || definition == nullptr ||
          source->sourceValue != input.hirValue ||
          source->info.type != input.info.type ||
          source->info.category != ValueCategory::Value ||
          definition->hirValue != input.hirValue ||
          definition->kind == MirInstructionKind::Load ||
          definition->kind == MirInstructionKind::CallInput ||
          !sameCallInputTraits(source->info.traits, input.info.traits) ||
          !sameCallInputTraits(definition->info.traits, input.info.traits)) {
        return false;
      }
      std::vector<MirDropObligationId> obligations;
      for (const MirDropObligation &obligation : body.dropObligations) {
        if (obligation.kind == MirDropObligationKind::Value &&
            obligation.value == source->sourceValue &&
            obligation.dropType.type == input.info.type) {
          obligations.push_back(obligation.id);
        }
      }
      if (input.preparedParameterDrop) {
        const MirDropObligation *prepared =
            body.findDropObligation(*input.preparedParameterDrop);
        // A concrete pack expansion stages a selected element directly from
        // its owning parameter. The adjacent Move/CallInput pair has no
        // separately materialized HIR temporary; retain the exact binding's
        // lifecycle transfer instead of inventing an unowned value.
        const MirPlace *movedPack =
            definition->kind == MirInstructionKind::Move &&
                    definition->operands.size() == 1 &&
                    definition->operands.front().kind == MirOperandKind::Move
                ? body.findPlace(definition->operands.front().place)
                : nullptr;
        const bool packProjection =
            movedPack != nullptr &&
            movedPack->root == MirPlaceRootKind::Binding &&
            movedPack->projections.size() == 1 &&
            movedPack->projections.front().kind ==
                MirProjectionKind::PackElement;
        bool adjacentPackMove = false;
        if (packProjection) {
          for (const MirBlock &block : body.blocks)
            for (std::size_t index = 1; index < block.instructions.size();
                 ++index)
              if (block.instructions[index].id == input.id &&
                  block.instructions[index - 1].id == definition->id)
                adjacentPackMove = true;
        }
        const MirDropObligation *packDrop = nullptr;
        if (adjacentPackMove)
          for (const MirDropObligation &drop : body.dropObligations)
            if (drop.kind == MirDropObligationKind::Binding &&
                drop.place == movedPack->id)
              packDrop = &drop;
        const bool exactLifecycle =
            packDrop != nullptr
                ? obligations.empty() && input.lifecycle.size() == 1 &&
                      input.lifecycle.front().kind ==
                          MirLifecycleEventKind::Move &&
                      input.lifecycle.front().source == packDrop->id
            : obligations.empty()
                ? input.lifecycle.size() == 1 &&
                      input.lifecycle.front().kind ==
                          MirLifecycleEventKind::Initialize &&
                      input.lifecycle.front().source == 0
                : obligations.size() == 1 && input.lifecycle.size() == 1 &&
                      input.lifecycle.front().kind ==
                          MirLifecycleEventKind::Reparent &&
                      input.lifecycle.front().source == obligations.front();
        return prepared != nullptr && exactLifecycle &&
               prepared->kind == MirDropObligationKind::PreparedParameter &&
               input.destination == prepared->place &&
               prepared->dropType.type == input.info.type &&
               input.lifecycle.front().target == prepared->id &&
               !input.lifecycle.front().conditional;
      }
      if (obligations.empty()) {
        return !input.destination && input.lifecycle.empty();
      }
      // An explicitly moved receiver remains a caller-owned temporary until
      // full-expression cleanup. Invocation uses that exact value; unlike a
      // by-value argument, it does not transfer the obligation to the callee.
      if (input.callInputRole == MirCallInputRole::Receiver &&
          definition->kind == MirInstructionKind::Move) {
        return !input.destination && obligations.size() == 1 &&
               input.lifecycle.empty();
      }
      return !input.destination && obligations.size() == 1 &&
             input.lifecycle.size() == 1 &&
             input.lifecycle.front().kind ==
                 MirLifecycleEventKind::TransferOut &&
             input.lifecycle.front().source == obligations.front() &&
             input.lifecycle.front().target == 0 &&
             !input.lifecycle.front().conditional;
    };
    const auto validBorrowedTemporaryMoveInput =
        [&](const MirInstruction &input, const MirOperand &operand) {
          if (input.callInputRole != MirCallInputRole::Argument || input.loan ||
              input.info.type.kind != SemanticType::Class ||
              !input.info.traits.movable ||
              !input.info.traits.containsBorrowedState ||
              operand.kind != MirOperandKind::Value) {
            return false;
          }
          const MirValue *source = body.findValue(operand.value);
          const MirInstruction *producer =
              source == nullptr ? nullptr
                                : linearValueDefinition(body, operand.value);
          if (source == nullptr || producer == nullptr ||
              source->sourceValue != input.hirValue ||
              source->info.type != input.info.type ||
              source->info.category != ValueCategory::Value ||
              !source->info.traits.containsBorrowedState ||
              !sameCallInputTraits(source->info.traits, input.info.traits) ||
              producer->kind != MirInstructionKind::Call ||
              !producer->functionTarget || producer->constructorTarget ||
              producer->lambdaTarget || producer->bodyTarget ||
              producer->callableInvocation ||
              producer->intrinsic != IntrinsicKind::None || !producer->result ||
              *producer->result != source->id ||
              producer->hirValue != input.hirValue || !producer->loan ||
              producer->borrowOrigin == BorrowOriginKind::None ||
              producer->successResultDrop == std::nullopt ||
              !sameCallInputTraits(producer->info.traits, input.info.traits)) {
            return false;
          }
          const MirLoan *retained = body.findLoan(*producer->loan);
          const MirPlace *loanSource =
              retained == nullptr ? nullptr : body.findPlace(retained->source);
          if (retained == nullptr || loanSource == nullptr ||
              retained->kind != MirLoanKind::Stored || retained->entry ||
              retained->escapes || retained->storedField != 0 ||
              !retained->carriers.empty() ||
              retained->producedBy != input.hirValue) {
            return false;
          }

          const MirDropObligation *drop = nullptr;
          const MirPlace *slot = nullptr;
          for (const MirDropObligation &candidate : body.dropObligations) {
            if (candidate.kind != MirDropObligationKind::Value ||
                candidate.value != source->sourceValue ||
                candidate.dropType.type != input.info.type) {
              continue;
            }
            if (drop != nullptr) {
              return false;
            }
            drop = &candidate;
            slot = body.findPlace(candidate.place);
          }
          const MirDropObligation *prepared =
              input.preparedParameterDrop
                  ? body.findDropObligation(*input.preparedParameterDrop)
                  : nullptr;
          const bool preparedTransfer =
              prepared != nullptr && drop != nullptr &&
              prepared->kind == MirDropObligationKind::PreparedParameter &&
              !prepared->initiallyActive &&
              input.destination == prepared->place &&
              prepared->dropType == drop->dropType &&
              input.lifecycle.size() == 1 &&
              input.lifecycle.front().kind == MirLifecycleEventKind::Reparent &&
              input.lifecycle.front().target == prepared->id;
          const bool directTransfer = !input.preparedParameterDrop &&
                                      !input.destination &&
                                      input.lifecycle.size() == 1 &&
                                      input.lifecycle.front().kind ==
                                          MirLifecycleEventKind::TransferOut &&
                                      input.lifecycle.front().target == 0;
          return drop != nullptr && slot != nullptr &&
                 producer->successResultDrop == drop->id &&
                 slot->root == MirPlaceRootKind::Value &&
                 slot->value == source->id && slot->projections.empty() &&
                 slot->type == input.info.type &&
                 slot->traits.containsBorrowedState &&
                 (preparedTransfer || directTransfer) &&
                 input.lifecycle.front().source == drop->id &&
                 !input.lifecycle.front().conditional &&
                 !input.lifecycle.front().failureCleanup;
        };
    const auto validTemporaryReceiverInput = [&](const MirInstruction &input,
                                                 const MirOperand &operand,
                                                 MirOperandKind
                                                     expectedOperand) {
      if (!input.callInputRole ||
          *input.callInputRole != MirCallInputRole::Receiver ||
          input.preparedParameterDrop || input.destination ||
          !input.lifecycle.empty() ||
          input.info.type.kind != SemanticType::Class ||
          input.info.category != ValueCategory::Value ||
          operand.kind != expectedOperand) {
        return false;
      }
      const MirPlace *place = body.findPlace(operand.place);
      const MirTemporaryMaterialization materialization =
          place == nullptr
              ? MirTemporaryMaterialization{}
              : findMirTemporaryMaterialization(body, *place, input.hirValue);
      const MirValue *source = materialization.value;
      const MirInstruction *producer = materialization.producer;
      if (input.info.traits.containsBorrowedState) {
        const auto carrier =
            place == nullptr ? MirTemporaryCarrierDependency{}
                             : findMirTemporaryCarrierDependency(body, *place);
        const auto *shell = body.findLoan(input.loan.value_or(0));
        if (!carrier || shell == nullptr ||
            shell->kind != MirLoanKind::CallArgument ||
            shell->source != place->id || shell->parent != 0 ||
            shell->storedDependency != carrier.loan->id ||
            shell->producedBy != input.callSite ||
            shell->access != (expectedOperand == MirOperandKind::BorrowWrite
                                  ? AccessMode::Mutable
                                  : AccessMode::ReadOnly) ||
            !producer || producer->dispatch != CallDispatch::Static ||
            producer->lambdaTarget ||
            producer->intrinsic != IntrinsicKind::None ||
            (producer->kind == MirInstructionKind::Call
                 ? !producer->functionTarget
                 : producer->kind != MirInstructionKind::Construct ||
                       !producer->constructorTarget ||
                       producer->constructorKind !=
                           ConstructorKind::Ordinary)) {
          return false;
        }
      }
      const bool ordinaryConstruction =
          producer != nullptr &&
          producer->kind == MirInstructionKind::Construct &&
          producer->constructorKind == ConstructorKind::Ordinary &&
          producer->constructorTarget && !producer->functionTarget &&
          !producer->lambdaTarget && producer->intrinsic == IntrinsicKind::None;
      const bool ordinaryCall =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          producer->functionTarget && !producer->constructorTarget &&
          !producer->lambdaTarget && producer->intrinsic == IntrinsicKind::None;
      const bool lambdaCall =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          producer->lambdaTarget && !producer->functionTarget &&
          !producer->constructorTarget &&
          producer->intrinsic == IntrinsicKind::None;
      const bool specialConstruction =
          producer != nullptr &&
          producer->kind == MirInstructionKind::Construct &&
          producer->constructorKind != ConstructorKind::Ordinary &&
          !producer->constructorTarget && !producer->functionTarget &&
          !producer->lambdaTarget && producer->intrinsic == IntrinsicKind::None;
      const bool expectedExtraction =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          (producer->intrinsic == IntrinsicKind::ExpectedValue ||
           producer->intrinsic == IntrinsicKind::ExpectedError) &&
          !producer->functionTarget && !producer->lambdaTarget &&
          !producer->constructorTarget;
      const bool joinedTemporary =
          producer != nullptr && place != nullptr &&
          place->root == MirPlaceRootKind::Temporary &&
          (producer->kind == MirInstructionKind::Load ||
           producer->kind == MirInstructionKind::Move);
      // A generated zero-argument class construction has no source
      // constructor instance. The program verifier additionally requires
      // the exact concrete owner's generated default-constructor status.
      const bool generatedDefault =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          !producer->functionTarget && !producer->constructorTarget &&
          !producer->lambdaTarget && !producer->receiver && !producer->loan &&
          !producer->ownership && producer->operands.empty() &&
          producer->parameterTypes.empty() &&
          producer->constructorKind == ConstructorKind::Ordinary &&
          producer->intrinsic == IntrinsicKind::None;
      if (place == nullptr || source == nullptr || producer == nullptr ||
          place->access != AccessMode::Mutable ||
          place->sourceValue != input.hirValue ||
          place->type != input.info.type ||
          !sameCallInputTraits(place->traits, input.info.traits) ||
          source->sourceValue != input.hirValue ||
          source->info.type != input.info.type ||
          source->info.category != ValueCategory::Value ||
          !sameCallInputTraits(source->info.traits, input.info.traits) ||
          !producer->result || *producer->result != source->id ||
          producer->hirValue != input.hirValue ||
          producer->info.type != input.info.type ||
          producer->info.category != ValueCategory::Value ||
          !sameCallInputTraits(producer->info.traits, input.info.traits) ||
          (!ordinaryConstruction && !ordinaryCall && !lambdaCall &&
           !expectedExtraction && !specialConstruction && !generatedDefault &&
           !joinedTemporary)) {
        return false;
      }

      std::vector<const MirDropObligation *> obligations;
      for (const MirDropObligation &obligation : body.dropObligations) {
        if (obligation.kind == MirDropObligationKind::Value &&
            obligation.value == input.hirValue) {
          obligations.push_back(&obligation);
        }
      }
      if (obligations.size() != 1 ||
          obligations.front()->place != operand.place ||
          obligations.front()->dropType.type != input.info.type ||
          obligations.front()->hirFullExpression == 0 ||
          obligations.front()->fullExpression == 0) {
        return false;
      }
      const MirDropObligationId obligation = obligations.front()->id;
      // The join temporary is initialized on its incoming edges. Ordinary
      // initialization and lifecycle dataflow prove it live at this stage.
      return joinedTemporary || producer->successResultDrop == obligation ||
             std::any_of(
                 producer->lifecycle.begin(), producer->lifecycle.end(),
                 [&](const MirLifecycleEvent &event) {
                   return event.target == obligation &&
                          (event.kind == MirLifecycleEventKind::Initialize ||
                           event.kind == MirLifecycleEventKind::Reparent ||
                           event.kind == MirLifecycleEventKind::Move);
                 });
    };
    const auto validTemporaryArgumentInput = [&](const MirInstruction &input,
                                                 const MirOperand &operand) {
      if (!input.callInputRole ||
          *input.callInputRole != MirCallInputRole::Argument ||
          input.defaultArgument || input.preparedParameterDrop ||
          input.destination || !input.lifecycle.empty() || !input.loan ||
          input.info.type.kind != SemanticType::Reference ||
          input.info.type.referenceAccess != AccessMode::ReadOnly ||
          input.info.type.arguments.size() != 1 ||
          input.info.type.arguments.front().kind != SemanticType::Class ||
          input.info.category != ValueCategory::Value ||
          operand.kind != MirOperandKind::BorrowRead ||
          operand.type != input.info.type) {
        return false;
      }
      const SemanticType &referent = input.info.type.arguments.front();
      const MirPlace *place = body.findPlace(operand.place);
      const MirTemporaryMaterialization materialization =
          place == nullptr
              ? MirTemporaryMaterialization{}
              : findMirTemporaryMaterialization(body, *place, input.hirValue);
      const MirValue *source = materialization.value;
      const MirInstruction *producer = materialization.producer;
      const MirLoan *loan = body.findLoan(*input.loan);
      const bool ordinaryConstruction =
          producer != nullptr &&
          producer->kind == MirInstructionKind::Construct &&
          producer->constructorKind == ConstructorKind::Ordinary &&
          producer->constructorTarget && !producer->functionTarget &&
          !producer->lambdaTarget && producer->intrinsic == IntrinsicKind::None;
      const bool ordinaryCall =
          producer != nullptr && producer->kind == MirInstructionKind::Call &&
          producer->functionTarget && !producer->constructorTarget &&
          !producer->lambdaTarget && !producer->bodyTarget &&
          !producer->callableInvocation &&
          producer->intrinsic == IntrinsicKind::None;
      if (place == nullptr || source == nullptr || producer == nullptr ||
          loan == nullptr || loan->kind != MirLoanKind::CallArgument ||
          loan->source != place->id || loan->access != AccessMode::ReadOnly ||
          loan->entry || loan->escapes || loan->parent != 0 ||
          loan->producedBy != input.callSite ||
          place->root != MirPlaceRootKind::Value ||
          !place->projections.empty() || place->access != AccessMode::Mutable ||
          place->sourceValue != input.hirValue || place->type != referent ||
          source->sourceValue != input.hirValue ||
          source->info.type != referent ||
          source->info.category != ValueCategory::Value ||
          source->info.traits.containsBorrowedState ||
          !sameCallInputTraits(place->traits, source->info.traits) ||
          !producer->result || *producer->result != source->id ||
          producer->hirValue != input.hirValue ||
          producer->info.type != referent ||
          producer->info.category != ValueCategory::Value ||
          producer->info.traits.containsBorrowedState ||
          !sameCallInputTraits(producer->info.traits, source->info.traits) ||
          (!ordinaryConstruction && !ordinaryCall)) {
        return false;
      }

      std::vector<const MirDropObligation *> obligations;
      for (const MirDropObligation &obligation : body.dropObligations) {
        if (obligation.kind == MirDropObligationKind::Value &&
            obligation.value == input.hirValue) {
          obligations.push_back(&obligation);
        }
      }
      if (obligations.size() != 1 ||
          obligations.front()->place != operand.place ||
          obligations.front()->dropType.type != referent ||
          obligations.front()->hirFullExpression == 0 ||
          obligations.front()->fullExpression == 0) {
        return false;
      }
      const MirDropObligationId obligation = obligations.front()->id;
      return producer->successResultDrop == obligation ||
             std::any_of(
                 producer->lifecycle.begin(), producer->lifecycle.end(),
                 [&](const MirLifecycleEvent &event) {
                   return event.target == obligation && !event.conditional &&
                          !event.failureCleanup &&
                          (event.kind == MirLifecycleEventKind::Initialize ||
                           event.kind == MirLifecycleEventKind::Reparent ||
                           event.kind == MirLifecycleEventKind::Move);
                 });
    };
    switch (instruction.kind) {
    case MirInstructionKind::Compute:
      return !instruction.receiver && !instruction.loan &&
             (instruction.operation == MirOperation::FunctionItem
                  ? instruction.functionTarget.has_value()
                  : !instruction.functionTarget) &&
             !instruction.constructorTarget &&
             (instruction.operation == MirOperation::Closure ||
              !instruction.lambdaTarget) &&
             validCompute(instruction);
    case MirInstructionKind::Load:
      return noOperation && hasResult && !instruction.destination &&
             instruction.operands.size() == 1 &&
             instruction.operands.front().kind == MirOperandKind::Copy;
    case MirInstructionKind::Initialize: {
      if (!noOperation || hasResult || !instruction.destination ||
          instruction.operands.size() > 1) {
        return false;
      }
      const MirPlace *destination = body.findPlace(*instruction.destination);
      if (destination == nullptr ||
          destination->root != MirPlaceRootKind::Binding ||
          destination->type.kind != SemanticType::Reference) {
        return true;
      }
      // A reference field without an in-class initializer is represented by
      // an empty placeholder in the class field-initializer body; a
      // constructor initializer supplies the actual address later.
      if (instruction.operands.empty()) {
        return body.kind == MirBodyKind::FieldInitializers;
      }
      if (instruction.operands.size() != 1 ||
          instruction.operands.front().kind != MirOperandKind::Loan ||
          instruction.operands.front().place == 0 ||
          destination->type.arguments.size() != 1) {
        return false;
      }
      const MirOperand &source = instruction.operands.front();
      const MirPlace *address = body.findPlace(source.place);
      const MirLoan *loan = body.findLoan(source.loan);
      const MirLoan *addressLoan =
          address != nullptr && address->root == MirPlaceRootKind::Loan
              ? body.findLoan(address->loan)
              : nullptr;
      return address != nullptr && loan != nullptr &&
             // A Stored loan describes external provenance, not a runtime
             // address. A reference may name the actual carrier's place or
             // a separately produced child address, but not that metadata.
             (address->root != MirPlaceRootKind::Loan ||
              (addressLoan != nullptr &&
               addressLoan->kind != MirLoanKind::Stored)) &&
             std::find(loan->carriers.begin(), loan->carriers.end(),
                       destination->binding) != loan->carriers.end() &&
             (destination->type.referenceAccess != AccessMode::Mutable ||
              loan->access == AccessMode::Mutable);
    }
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
    case MirInstructionKind::Move: {
      const MirOperand *operand = instruction.operands.size() == 1
                                      ? &instruction.operands.front()
                                      : nullptr;
      const MirPlace *source =
          operand == nullptr ? nullptr : body.findPlace(operand->place);
      return noOperation && hasResult && operand != nullptr &&
             operand->kind == MirOperandKind::Move && source != nullptr &&
             operand->type == source->type &&
             instruction.info.type == source->type;
    }
    case MirInstructionKind::Borrow:
      return noOperation && !hasResult && instruction.loan &&
             instruction.operands.size() == 1 &&
             (instruction.operands.front().kind == MirOperandKind::BorrowRead ||
              instruction.operands.front().kind ==
                  MirOperandKind::BorrowWrite ||
              instruction.operands.front().kind == MirOperandKind::Loan);
    case MirInstructionKind::ActivateBorrow:
      return noOperation && !hasResult && !instruction.destination &&
             !instruction.receiver && instruction.loan &&
             instruction.operands.empty() &&
             instruction.info.type == SemanticType::Unknown;
    case MirInstructionKind::CallInput: {
      const bool temporaryArgument =
          instruction.callInputRole == MirCallInputRole::Argument &&
          instruction.callInputKind == HirCallInputKind::ReadTemporaryBorrow;
      const bool temporaryCarrierReceiver =
          instruction.callInputRole == MirCallInputRole::Receiver &&
          instruction.info.traits.containsBorrowedState &&
          (instruction.callInputKind == HirCallInputKind::ReadTemporaryBorrow ||
           instruction.callInputKind ==
               HirCallInputKind::MutableTemporaryBorrow);
      if (!noOperation || !hasResult ||
          (instruction.destination.has_value() !=
           instruction.preparedParameterDrop.has_value()) ||
          instruction.receiver ||
          instruction.loan.has_value() !=
              (temporaryArgument || temporaryCarrierReceiver) ||
          instruction.functionTarget || instruction.constructorTarget ||
          instruction.lambdaTarget || instruction.fullExpressionEnd != 0 ||
          instruction.cleanupBoundaryEnd != 0 ||
          instruction.operands.size() != 1 ||
          !mir_detail::validCallInputType(instruction.operands.front().type,
                                          instruction.info.type,
                                          instruction.callInputKind) ||
          (*instruction.callInputRole == MirCallInputRole::Receiver &&
           (instruction.callInputIndex != 0 ||
            instruction.callInputKind == HirCallInputKind::CopyValue))) {
        return false;
      }
      const MirOperand &operand = instruction.operands.front();
      switch (instruction.callInputKind) {
      case HirCallInputKind::RetainedReceiver: {
        const MirPlace *place = body.findPlace(operand.place);
        const MirPlace *anchor =
            instruction.retainedOwnerPlace
                ? body.findPlace(*instruction.retainedOwnerPlace)
                : place;
        const MirLoan *loan =
            place != nullptr && place->root == MirPlaceRootKind::Loan
                ? body.findLoan(place->loan)
                : nullptr;
        const MirPlace *loanSource =
            loan == nullptr ? nullptr : body.findPlace(loan->source);
        const bool exactPointee =
            instruction.retainedOwnerPlace && place != nullptr &&
            place->root == MirPlaceRootKind::Loan &&
            place->projections.empty() && loan != nullptr &&
            loan->kind == MirLoanKind::CallResult &&
            loan->access == AccessMode::Mutable && !loan->entry &&
            !loan->escapes && !loan->lifecycleBound && !loan->twoPhase &&
            loan->parent == 0 && loan->carriers.empty() &&
            loanSource != nullptr && anchor != nullptr &&
            loanSource->root == anchor->root &&
            loanSource->key == anchor->key &&
            loanSource->projections == anchor->projections &&
            loanSource->type == anchor->type;
        return instruction.callInputRole == MirCallInputRole::Receiver &&
               !instruction.preparedParameterDrop && !instruction.destination &&
               instruction.lifecycle.empty() &&
               instruction.definedFailure.empty() &&
               (instruction.info.type.kind == SemanticType::Class ||
                instruction.info.type.kind == SemanticType::Lambda) &&
               instruction.info.category == ValueCategory::Value &&
               instruction.info.traits.movable &&
               !instruction.info.traits.containsBorrowedState &&
               operand.kind == MirOperandKind::BorrowWrite &&
               place != nullptr && anchor != nullptr &&
               (!instruction.retainedOwnerPlace || exactPointee) &&
               anchor->root == MirPlaceRootKind::This && anchor->key &&
               anchor->key->receiver && place->type == instruction.info.type &&
               place->access == AccessMode::Mutable &&
               anchor->access == AccessMode::Mutable &&
               !anchor->traits.containsBorrowedState &&
               (!instruction.retainedOwnerPlace ||
                anchor->type.kind == SemanticType::Class) &&
               !anchor->projections.empty() &&
               std::all_of(anchor->projections.begin(),
                           anchor->projections.end(),
                           [](const auto &projection) {
                             return projection.kind == MirProjectionKind::Field;
                           }) &&
               instruction.ownership &&
               instruction.ownership->kind ==
                   OwnershipEventKind::ConsumeReceiver &&
               instruction.ownership->place == *anchor->key &&
               instruction.ownership->before == OwnershipStateSet::Available &&
               instruction.ownership->after == OwnershipStateSet::Moved;
      }
      case HirCallInputKind::Value:
        return !instruction.preparedParameterDrop &&
               instruction.lifecycle.empty() &&
               operand.kind == MirOperandKind::Value;
      case HirCallInputKind::CopyValue:
        return validClassCopyInput(instruction, operand);
      case HirCallInputKind::MoveValue:
        return validClassMoveInput(instruction, operand);
      case HirCallInputKind::MoveBorrowedTemporary:
        return validBorrowedTemporaryMoveInput(instruction, operand);
      case HirCallInputKind::ReadBorrow:
        return !instruction.preparedParameterDrop &&
               instruction.lifecycle.empty() &&
               (operand.kind == MirOperandKind::BorrowRead ||
                operand.kind == MirOperandKind::Loan);
      case HirCallInputKind::MutableBorrow: {
        if (instruction.preparedParameterDrop ||
            !instruction.lifecycle.empty()) {
          return false;
        }
        if (operand.kind == MirOperandKind::BorrowWrite) {
          return true;
        }
        if (operand.kind != MirOperandKind::Loan) {
          return false;
        }
        const MirLoan *loan = body.findLoan(operand.loan);
        return loan != nullptr && loan->access == AccessMode::Mutable;
      }
      case HirCallInputKind::ReadTemporaryBorrow:
        return temporaryArgument
                   ? validTemporaryArgumentInput(instruction, operand)
                   : validTemporaryReceiverInput(instruction, operand,
                                                 MirOperandKind::BorrowRead);
      case HirCallInputKind::MutableTemporaryBorrow:
        return validTemporaryReceiverInput(instruction, operand,
                                           MirOperandKind::BorrowWrite);
      }
      return false;
    }
    case MirInstructionKind::Call:
      return noOperation &&
             hasResult == (instruction.info.type.kind != SemanticType::Void) &&
             (!instruction.successResultDestination ||
              (instruction.result && !instruction.destination &&
               !instruction.loan && !instruction.definedFailure.empty() &&
               instruction.info.traits.drop == DropKind::Lexical)) &&
             (instruction.callSite == 0 ||
              (instruction.nativeCalleeType && instruction.receiver &&
               !instruction.functionTarget && !instruction.constructorTarget &&
               !instruction.lambdaTarget) ||
              (instruction.lambdaTarget && !instruction.functionTarget &&
               !instruction.constructorTarget && instruction.receiver &&
               instruction.callableInvocation &&
               instruction.intrinsic == IntrinsicKind::None) ||
              (!instruction.lambdaTarget &&
               ((instruction.functionTarget &&
                 instruction.intrinsic == IntrinsicKind::None &&
                 !instruction.constructorTarget) ||
                (!instruction.functionTarget &&
                 (instruction.intrinsic == IntrinsicKind::Panic ||
                  instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
                  instruction.intrinsic == IntrinsicKind::ObjectConstruct ||
                  instruction.intrinsic == IntrinsicKind::PrefixStorageAppend ||
                  instruction.intrinsic == IntrinsicKind::PrefixStorageInsert ||
                  instruction.intrinsic ==
                      IntrinsicKind::OptionalStorageConstruct))))) &&
             validCallableInvocation() &&
             (!instruction.callableBoundary ||
              *instruction.callableBoundary == CallableBoundary::Confined) &&
             (!instruction.constructorTarget ||
              ((instruction.intrinsic == IntrinsicKind::AllocateUniqueOwner ||
                instruction.intrinsic == IntrinsicKind::ObjectConstruct ||
                instruction.intrinsic == IntrinsicKind::PrefixStorageAppend ||
                instruction.intrinsic == IntrinsicKind::PrefixStorageInsert ||
                instruction.intrinsic ==
                    IntrinsicKind::OptionalStorageConstruct) &&
               !instruction.functionTarget && !instruction.lambdaTarget)) &&
             std::all_of(
                 instruction.callableArguments.begin(),
                 instruction.callableArguments.end(),
                 [&](const CallableArgumentBoundary &argument) {
                   return argument.parameterIndex <
                              instruction.operands.size() &&
                          (argument.boundary == CallableBoundary::Confined ||
                           argument.boundary == CallableBoundary::Owned);
                 }) &&
             std::adjacent_find(instruction.callableArguments.begin(),
                                instruction.callableArguments.end(),
                                [](const CallableArgumentBoundary &left,
                                   const CallableArgumentBoundary &right) {
                                  return left.parameterIndex >=
                                         right.parameterIndex;
                                }) == instruction.callableArguments.end() &&
             (instruction.dispatch != CallDispatch::Virtual ||
              (instruction.functionTarget && instruction.receiver &&
               instruction.dispatchOwner.kind == SemanticType::Class)) &&
             validRawMemberCallReceiver(instruction);
    case MirInstructionKind::Construct:
      return noOperation && hasResult && !instruction.receiver &&
             (!instruction.successResultDestination ||
              (!instruction.destination && !instruction.loan &&
               !instruction.definedFailure.empty() &&
               instruction.info.traits.drop == DropKind::Lexical)) &&
             instruction.info.type.kind == SemanticType::Class &&
             instruction.intrinsic == IntrinsicKind::None &&
             !instruction.functionTarget && !instruction.lambdaTarget &&
             (instruction.callSite == 0 ||
              (instruction.constructorTarget &&
               instruction.constructorKind == ConstructorKind::Ordinary &&
               !instruction.operands.empty())) &&
             (instruction.constructorKind == ConstructorKind::Ordinary ||
              (!instruction.constructorTarget &&
               instruction.operands.size() == 1));
    case MirInstructionKind::Drop:
      return noOperation && !hasResult && instruction.destination &&
             instruction.operands.empty();
    case MirInstructionKind::CleanupBeforeAssign: {
      const MirPlace *destination =
          instruction.destination ? body.findPlace(*instruction.destination)
                                  : nullptr;
      return noOperation && !hasResult && destination != nullptr &&
             instruction.replacementCleanup &&
             instruction.replacementPair != 0 &&
             instruction.replacementCleanup->type == destination->type &&
             !instruction.replacementCleanup->recursiveCleanup &&
             !instruction.receiver && instruction.operands.empty() &&
             !instruction.loan && !instruction.functionTarget &&
             !instruction.constructorTarget && !instruction.bodyTarget &&
             !instruction.lambdaTarget && instruction.lifecycle.empty() &&
             !instruction.ownership &&
             instruction.intrinsic == IntrinsicKind::None;
    }
    case MirInstructionKind::EndBorrow:
      return noOperation && !hasResult && instruction.loan &&
             instruction.operands.empty();
    case MirInstructionKind::Lifecycle:
      return noOperation && !hasResult && !instruction.destination &&
             !instruction.receiver && instruction.operands.empty() &&
             !instruction.loan && !instruction.functionTarget &&
             !instruction.constructorTarget && !instruction.lambdaTarget &&
             ((!instruction.lifecycle.empty() &&
               instruction.fullExpressionEnd == 0 &&
               instruction.cleanupBoundaryEnd == 0) ||
              (instruction.lifecycle.empty() &&
               ((instruction.fullExpressionEnd != 0) !=
                (instruction.cleanupBoundaryEnd != 0))));
    case MirInstructionKind::CallBody:
      return noOperation && !hasResult && !instruction.destination &&
             !instruction.receiver && instruction.operands.empty() &&
             instruction.parameterTypes.empty() && !instruction.loan &&
             !instruction.functionTarget && !instruction.constructorTarget &&
             instruction.bodyTarget.has_value() && !instruction.lambdaTarget &&
             instruction.info.type == SemanticType::Void;
    case MirInstructionKind::StorageConstructPreflight: {
      const std::vector<DefinedFailureOutcome> outcomes =
          std::vector<DefinedFailureOutcome>{
              {DefinedFailureCode::InvalidStorageState,
               DefinedFailureDetail::RelocationCapacity}};
      return noOperation && !hasResult && !instruction.destination &&
             !instruction.receiver && !instruction.loan &&
             !instruction.functionTarget && !instruction.constructorTarget &&
             !instruction.bodyTarget && !instruction.lambdaTarget &&
             instruction.intrinsic == IntrinsicKind::None &&
             instruction.info.type == SemanticType::Void &&
             instruction.lifecycle.empty() && !instruction.ownership &&
             !instruction.rawMemoryAccess &&
             instruction.unsafeOperation == UnsafeOperationKind::None &&
             instruction.callSite != 0 &&
             instruction.callSite == instruction.hirValue &&
             instruction.operands.size() == 1 &&
             instruction.operands.front().type.kind ==
                 SemanticType::PrefixStorage &&
             instruction.operands.front().kind == MirOperandKind::Value &&
             instruction.definedFailure.propagation ==
                 FailurePropagationKind::None &&
             instruction.definedFailure.localOrigins.size() == 1 &&
             instruction.definedFailure.localOrigins.front().outcomes ==
                 outcomes;
    }
    case MirInstructionKind::AllocateObject: {
      const MirDropObligation *allocation =
          body.findDropObligation(instruction.objectAllocation);
      return noOperation && !hasResult && instruction.destination &&
             !instruction.receiver && instruction.operands.empty() &&
             instruction.parameterTypes.empty() && !instruction.loan &&
             !instruction.functionTarget && !instruction.constructorTarget &&
             !instruction.bodyTarget && !instruction.lambdaTarget &&
             instruction.intrinsic == IntrinsicKind::None &&
             instruction.lifecycle.empty() && allocation != nullptr &&
             allocation->kind == MirDropObligationKind::ObjectAllocation &&
             allocation->place == *instruction.destination &&
             instruction.successResultDrop == allocation->id &&
             allocation->dropType.type == instruction.info.type;
    }
    case MirInstructionKind::InitializeBase: {
      const MirPlace *base = instruction.destination
                                 ? body.findPlace(*instruction.destination)
                                 : nullptr;
      const MirDropObligation *rollback =
          instruction.successResultDrop
              ? body.findDropObligation(*instruction.successResultDrop)
              : nullptr;
      return body.kind == MirBodyKind::Constructor && noOperation &&
             !hasResult && base != nullptr && rollback != nullptr &&
             !instruction.receiver && instruction.constructorInitializer != 0 &&
             instruction.operands.size() == instruction.parameterTypes.size() &&
             std::equal(
                 instruction.operands.begin(), instruction.operands.end(),
                 instruction.parameterTypes.begin(),
                 [](const MirOperand &operand, const SemanticType &type) {
                   return operand.type == type &&
                          (operand.kind == MirOperandKind::Value ||
                           operand.kind == MirOperandKind::Constant);
                 }) &&
             !instruction.loan && !instruction.functionTarget &&
             !instruction.bodyTarget && !instruction.lambdaTarget &&
             instruction.callSite == 0 && instruction.hirValue == 0 &&
             instruction.hirStatement == 0 &&
             instruction.intrinsic == IntrinsicKind::None &&
             instruction.constructorKind == ConstructorKind::Ordinary &&
             !instruction.ownership &&
             instruction.definedFailure.localOrigins.empty() &&
             ((instruction.constructorTarget &&
               instruction.definedFailure.propagation ==
                   FailurePropagationKind::Constructor &&
               instruction.lifecycle.empty()) ||
              (instruction.definedFailure.empty() &&
               instruction.lifecycle.size() == 1 &&
               instruction.lifecycle.front().kind ==
                   MirLifecycleEventKind::Initialize &&
               instruction.lifecycle.front().source == 0 &&
               instruction.lifecycle.front().target == rollback->id &&
               !instruction.lifecycle.front().conditional &&
               !instruction.lifecycle.front().failureCleanup)) &&
             base->root == MirPlaceRootKind::This &&
             base->projections.size() == 1 &&
             base->projections.front().kind == MirProjectionKind::Base &&
             base->access == AccessMode::Mutable &&
             instruction.info.access == AccessMode::Mutable &&
             instruction.info.category == ValueCategory::Place &&
             instruction.info.traits == base->traits &&
             base->type == instruction.info.type && rollback != nullptr &&
             rollback->kind == MirDropObligationKind::ConstructionRollback &&
             rollback->place == base->id && !rollback->initiallyActive &&
             rollback->dropType.type == base->type;
    }
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
        (place.capture != 0 && (place.root != MirPlaceRootKind::Symbol ||
                                body.kind != MirBodyKind::Lambda)) ||
        (place.root == MirPlaceRootKind::Temporary && place.temporary == 0) ||
        (place.root == MirPlaceRootKind::Value && !validValue(place.value)) ||
        (place.root == MirPlaceRootKind::Loan && !validLoan(place.loan))) {
      return failure(body, owner,
                     "place " + std::to_string(place.id) +
                         " has an invalid identity or root");
    }
    if (place.key) {
      if (!place.key->valid() || place.key->domain != body.placeDomain) {
        return failure(body, owner,
                       "place " + std::to_string(place.id) +
                           " has an invalid ownership domain or key");
      }
    }
    expectedUseCount += place.root == MirPlaceRootKind::Value ? 1 : 0;
    for (const MirPlaceProjection &projection : place.projections) {
      if ((projection.kind == MirProjectionKind::Base &&
           (body.kind != MirBodyKind::Constructor ||
            place.root != MirPlaceRootKind::This ||
            place.projections.size() != 1 ||
            place.type.kind != SemanticType::Class ||
            projection.selection == 0 || projection.field != 0 ||
            projection.index != 0 || projection.constantIndex)) ||
          (projection.kind == MirProjectionKind::Field &&
           projection.field == 0) ||
          (projection.kind == MirProjectionKind::RawIndex &&
           !validValue(projection.index)) ||
          (projection.kind == MirProjectionKind::Index &&
           ((projection.index == 0 && !projection.constantIndex &&
             projection.selection == 0) ||
            (projection.index != 0 && !validValue(projection.index)) ||
            (projection.constantIndex && projection.selection != 0))) ||
          (projection.kind == MirProjectionKind::PackElement &&
           (!projection.constantIndex || projection.field != 0 ||
            projection.index != 0 || projection.selection != 0))) {
        return failure(body, owner,
                       "place " + std::to_string(place.id) +
                           " has an invalid projection");
      }
      if (projection.kind == MirProjectionKind::PackElement) {
        const bool firstProjection = &projection == &place.projections.front();
        const auto root =
            std::find_if(body.places.begin(), body.places.end(),
                         [&](const MirPlace &candidate) {
                           return candidate.id != place.id &&
                                  candidate.root == MirPlaceRootKind::Binding &&
                                  candidate.binding == place.binding &&
                                  candidate.projections.empty();
                         });
        const std::size_t element =
            static_cast<std::size_t>(*projection.constantIndex);
        if (!firstProjection || place.projections.size() != 1 ||
            place.root != MirPlaceRootKind::Binding ||
            root == body.places.end() ||
            root->type.kind != SemanticType::TypePack ||
            !root->type.concretePack ||
            element >= root->type.arguments.size() ||
            root->type.arguments[element] != place.type ||
            place.access != AccessMode::ReadOnly ||
            (place.type.kind != SemanticType::Class &&
             place.type.kind != SemanticType::Lambda &&
             place.traits != semanticTraits(place.type)) ||
            !place.initiallyAvailable) {
          return failure(body, owner,
                         "place " + std::to_string(place.id) +
                             " has an invalid concrete pack element");
        }
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
      expectedUseCount += (projection.kind == MirProjectionKind::Index ||
                           projection.kind == MirProjectionKind::RawIndex) &&
                                  projection.index != 0
                              ? 1
                              : 0;
    }
  }

  std::unordered_set<SemanticLoanId> semanticLoans;
  for (std::size_t index = 0; index < body.loans.size(); ++index) {
    const MirLoan &loan = body.loans[index];
    if (loan.id != index + 1 || !validPlace(loan.source) ||
        std::any_of(loan.additionalDependencies.begin(),
                    loan.additionalDependencies.end(),
                    [&](const auto &dependency) {
                      return (dependency.source == 0
                                  ? dependency.designatedFields.empty()
                                  : !validPlace(dependency.source)) ||
                             (dependency.access != AccessMode::ReadOnly &&
                              dependency.access != AccessMode::Mutable);
                    }) ||
        (loan.parent != 0 &&
         (!validLoan(loan.parent) || loan.parent == loan.id)) ||
        (loan.storedDependency != 0 && (!validLoan(loan.storedDependency) ||
                                        loan.storedDependency == loan.id))) {
      return failure(body, owner,
                     "loan " + std::to_string(loan.id) +
                         " has an invalid identity, parent, stored dependency, "
                         "or source place");
    }
    if (loan.storedDependency != 0) {
      const MirLoan &dependency = body.loans[loan.storedDependency - 1];
      const bool derivedStoredDependency =
          loan.kind == MirLoanKind::Stored && loan.parent != 0 &&
          body.loans[loan.parent - 1].storedDependency == loan.storedDependency;
      const bool publishedShellDependency =
          body.kind == MirBodyKind::Constructor &&
          loan.kind == MirLoanKind::Stored && loan.storedField != 0 &&
          loan.escapes && loan.parent == 0 &&
          abstractStoredEntry(body, dependency);
      const auto *source = body.findPlace(loan.source);
      const bool constructedShellDependency =
          loan.kind == MirLoanKind::Stored && !loan.entry && !loan.escapes &&
          loan.storedField == 0 && loan.parent == 0 && loan.producedBy != 0 &&
          source != nullptr && source->root != MirPlaceRootKind::Loan &&
          source->traits.containsBorrowedState;
      if ((loan.kind == MirLoanKind::Stored && !derivedStoredDependency &&
           !publishedShellDependency && !constructedShellDependency) ||
          dependency.kind != MirLoanKind::Stored ||
          loan.storedDependency == loan.parent ||
          (loan.entry && !dependency.entry)) {
        return failure(body, owner,
                       "carrier-shell loan " + std::to_string(loan.id) +
                           " must retain an independent Stored dependency "
                           "with a compatible entry lifetime");
      }
    }
    if (loan.parent != 0) {
      const MirLoan &parent = body.loans[loan.parent - 1];
      if (parent.storedDependency != 0 &&
          loan.storedDependency != parent.storedDependency) {
        return failure(body, owner,
                       "child loan " + std::to_string(loan.id) +
                           " does not preserve its parent's stored dependency");
      }
      if (loan.escapes || (loan.kind != MirLoanKind::Local &&
                           loan.kind != MirLoanKind::CallResult &&
                           loan.kind != MirLoanKind::Stored)) {
        return failure(body, owner,
                       "child loan " + std::to_string(loan.id) +
                           " must be a non-escaping Local, CallResult, or "
                           "Stored loan");
      }
      const bool sharedDerivedCall =
          (loan.kind == MirLoanKind::Local ||
           loan.kind == MirLoanKind::CallResult ||
           (loan.kind == MirLoanKind::Stored &&
            (parent.kind == MirLoanKind::CallResult ||
             (parent.kind == MirLoanKind::Stored &&
              parent.storedDependency != 0)))) &&
          loan.access == AccessMode::ReadOnly &&
          parent.access == AccessMode::ReadOnly;
      const bool projectedStoredChild = parent.kind == MirLoanKind::Stored &&
                                        !parent.designatedFields.empty();
      const bool sharedCleanupDependency =
          loan.kind == MirLoanKind::Stored && loan.lifecycleBound &&
          loan.access == AccessMode::ReadOnly &&
          (parent.kind == MirLoanKind::Parameter ||
           parent.kind == MirLoanKind::Stored) &&
          parent.access == AccessMode::ReadOnly;
      if (loan.entry ||
          (parent.access != AccessMode::Mutable && !sharedDerivedCall &&
           !projectedStoredChild && !sharedCleanupDependency)) {
        return failure(body, owner,
                       "child loan " + std::to_string(loan.id) +
                           " must be non-entry and either have a mutable "
                           "parent or be a read-only derived local/call result "
                           "or stored accessor result");
      }
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
    if (loan.storedField != 0 && !loan.designatedFields.empty()) {
      return failure(body, owner,
                     "loan " + std::to_string(loan.id) +
                         " cannot both initialize and designate a stored "
                         "reference field");
    }
    if (!loan.designatedFields.empty() &&
        !((loan.kind == MirLoanKind::Stored && !loan.escapes) ||
          (loan.kind == MirLoanKind::Return && loan.escapes))) {
      return failure(body, owner,
                     "loan " + std::to_string(loan.id) +
                         " designates a stored referent without being one "
                         "non-escaping Stored loan or an escaping Return loan");
    }
    if (loan.twoPhase) {
      const MirPlace *source = body.findPlace(loan.source);
      if (loan.kind != MirLoanKind::CallResult || loan.entry || loan.escapes ||
          loan.access != AccessMode::Mutable || loan.producedBy == 0 ||
          loan.semanticLoan != 0 || loan.storedField != 0 ||
          !loan.designatedFields.empty() || !loan.carriers.empty() ||
          source == nullptr || source->root == MirPlaceRootKind::Symbol ||
          source->root == MirPlaceRootKind::Temporary ||
          source->root == MirPlaceRootKind::Value) {
        return failure(body, owner,
                       "two-phase loan " + std::to_string(loan.id) +
                           " must be one non-escaping mutable call-result "
                           "reservation over stable local storage");
      }
    }
    if (loan.kind == MirLoanKind::CallArgument) {
      const MirPlace *source = body.findPlace(loan.source);
      const auto carrier =
          source == nullptr ? MirTemporaryCarrierDependency{}
                            : findMirTemporaryCarrierDependency(body, *source);
      const bool receiver =
          carrier && loan.storedDependency == carrier.loan->id;
      if (loan.semanticLoan != 0 || loan.parent != 0 || loan.entry ||
          loan.escapes || (!receiver && loan.access != AccessMode::ReadOnly) ||
          loan.producedBy == 0 || !loan.carriers.empty() ||
          loan.storedField != 0 || loan.lifecycleBound || loan.twoPhase ||
          !loan.additionalSources().empty() || source == nullptr ||
          (source->root != MirPlaceRootKind::Value &&
           (!receiver || source->root != MirPlaceRootKind::Temporary)) ||
          !source->projections.empty() ||
          source->type.kind != SemanticType::Class ||
          source->access != AccessMode::Mutable ||
          source->traits.containsBorrowedState != receiver ||
          (!receiver && loan.storedDependency != 0)) {
        return failure(body, owner,
                       "call-argument loan " + std::to_string(loan.id) +
                           " must be one exact non-escaping temporary input "
                           "loan with its required stored dependency");
      }
    }
    if (loan.entry) {
      const MirPlace *source = body.findPlace(loan.source);
      const bool packElement =
          source != nullptr && source->projections.size() == 1 &&
          source->projections.front().kind == MirProjectionKind::PackElement &&
          source->type.kind == SemanticType::Reference &&
          source->type.referenceAccess == loan.access &&
          loan.kind == MirLoanKind::Parameter && loan.semanticLoan != 0;
      if (!abstractStoredEntry(body, loan) &&
          (loan.producedBy != 0 || source == nullptr ||
           source->root != MirPlaceRootKind::Binding ||
           (!source->projections.empty() && !packElement) ||
           std::find(loan.carriers.begin(), loan.carriers.end(),
                     source->binding) == loan.carriers.end())) {
        return failure(body, owner,
                       "entry loan " + std::to_string(loan.id) +
                           " must originate at a formal parameter binding "
                           "that remains one of its carriers, or be an exact "
                           "abstract stored receiver/reference entry");
      }
    }
    if (loan.kind == MirLoanKind::Parameter && !loan.entry) {
      return failure(body, owner,
                     "parameter loan " + std::to_string(loan.id) +
                         " is not active at body entry");
    }
  }

  // Validate the union of physical reborrow and retained-dependency edges.
  // Their meanings remain distinct, but either edge extends the referenced
  // lifetime, so a cycle across the two families cannot be executable.
  std::vector<unsigned char> loanColors(body.loans.size(), 0);
  for (const MirLoan &loan : body.loans) {
    std::vector<std::pair<MirLoanId, bool>> pending{{loan.id, false}};
    while (!pending.empty()) {
      const auto [id, exit] = pending.back();
      pending.pop_back();
      if (exit) {
        loanColors[id - 1] = 2;
        continue;
      }
      if (loanColors[id - 1] == 2)
        continue;
      if (loanColors[id - 1] == 1) {
        return failure(body, owner,
                       "loan " + std::to_string(id) +
                           " participates in a parent/stored-dependency cycle");
      }
      loanColors[id - 1] = 1;
      pending.emplace_back(id, true);
      const MirLoan &record = body.loans[id - 1];
      if (record.parent != 0)
        pending.emplace_back(record.parent, false);
      if (record.storedDependency != 0)
        pending.emplace_back(record.storedDependency, false);
    }
  }

  // A return carrier remains callee-owned until cleanup succeeds. Expected
  // publication can consume either its success payload or its exact
  // Unexpected<Error> wrapper; neither is the completed Expected yet.
  const auto isReturnCarrierType = [&](const SemanticType &type) {
    const bool fallibleBorrowPayload =
        body.returnType.kind == SemanticType::Expected &&
        body.returnType.arguments.size() == 2 &&
        body.returnType.arguments.front().kind == SemanticType::Reference &&
        body.returnType.arguments.front().referenceAccess ==
            AccessMode::ReadOnly &&
        body.returnType.arguments.front().arguments.size() == 1 &&
        type == body.returnType.arguments.front().arguments.front();
    return type == body.returnType ||
           (body.returnType.kind == SemanticType::Expected &&
            body.returnType.arguments.size() == 2 &&
            (type == body.returnType.arguments.front() ||
             fallibleBorrowPayload ||
             (type.kind == SemanticType::Unexpected &&
              type.arguments.size() == 1 &&
              type.arguments.front() == body.returnType.arguments.back())));
  };
  // Conditional and propagated owning values are materialized in a join
  // temporary, then transported by one exact Move SSA result. Their pending
  // return lifetime still owns that same physical place during callee cleanup.
  // Bind publication to the transport result without replacing its place or
  // accepting an arbitrary Temporary-rooted return obligation.
  const auto returnCarrierValue =
      [&](const MirDropObligation &obligation) -> MirValueId {
    const MirPlace *place = body.findPlace(obligation.place);
    if (place == nullptr || !place->projections.empty() ||
        place->sourceValue != obligation.value ||
        place->type != obligation.dropType.type) {
      return 0;
    }
    if (place->root == MirPlaceRootKind::Value) {
      const MirValue *value = body.findValue(place->value);
      return value != nullptr && value->sourceValue == obligation.value
                 ? value->id
                 : 0;
    }
    if (place->root != MirPlaceRootKind::Temporary || place->temporary == 0 ||
        place->value != 0) {
      return 0;
    }
    MirValueId result = 0;
    for (const MirValue &value : body.values) {
      if (value.sourceValue != obligation.value ||
          value.info.type != obligation.dropType.type ||
          value.info.category != ValueCategory::Value) {
        continue;
      }
      const MirBlock *block = body.findBlock(value.definitionBlock);
      if (block == nullptr)
        continue;
      const auto transport = std::find_if(
          block->instructions.begin(), block->instructions.end(),
          [&](const MirInstruction &instruction) {
            return instruction.id == value.definition &&
                   instruction.result == value.id &&
                   instruction.hirValue == obligation.value &&
                   instruction.kind == MirInstructionKind::Move &&
                   instruction.operation == MirOperation::None &&
                   instruction.intrinsic == IntrinsicKind::None &&
                   instruction.operands.size() == 1 &&
                   instruction.operands.front().kind == MirOperandKind::Move &&
                   instruction.operands.front().place == place->id &&
                   instruction.operands.front().type == place->type &&
                   instruction.lifecycle.empty();
          });
      if (transport == block->instructions.end())
        continue;
      if (result != 0)
        return 0;
      result = value.id;
    }
    return result;
  };
  std::unordered_set<HirDropObligationId> hirDropObligations;
  for (std::size_t index = 0; index < body.dropObligations.size(); ++index) {
    const MirDropObligation &obligation = body.dropObligations[index];
    const MirPlace *place = body.findPlace(obligation.place);
    const bool bindingKind = obligation.kind == MirDropObligationKind::Binding;
    const bool valueKind = obligation.kind == MirDropObligationKind::Value;
    const bool preparedKind =
        obligation.kind == MirDropObligationKind::PreparedParameter;
    const bool allocationKind =
        obligation.kind == MirDropObligationKind::ObjectAllocation;
    const bool returnKind =
        obligation.kind == MirDropObligationKind::ReturnResult;
    // Compiler-generated for one completed constructor subobject: no HIR
    // obligation or full-expression identity, and the place is the exact
    // This-rooted field place of its stage.
    const bool rollbackKind =
        obligation.kind == MirDropObligationKind::ConstructionRollback;
    const bool validHirIdentity =
        preparedKind || rollbackKind || allocationKind || returnKind
            ? obligation.hirObligation == 0
            : obligation.hirObligation != 0 &&
                  hirDropObligations.insert(obligation.hirObligation).second;
    const bool exactFullExpression =
        bindingKind || rollbackKind || returnKind
            ? obligation.fullExpression == 0 &&
                  obligation.hirFullExpression == 0
            : obligation.fullExpression != 0 &&
                  obligation.fullExpression <= body.fullExpressions.size() &&
                  body.fullExpressions[obligation.fullExpression - 1]
                          .hirExpression == obligation.hirFullExpression;
    const auto &dropType = obligation.dropType;
    const bool arrayRecipe =
        dropType.type.kind == SemanticType::Array && dropType.fixedArray &&
        dropType.type.arguments.size() == 1 && dropType.type.arrayLength != 0 &&
        dropType.type.arrayLengthParameterId == 0 &&
        dropType.aggregate.size() == 1 &&
        dropType.aggregate.front().type == dropType.type.arguments.front() &&
        dropType.aggregate.front().requiresActiveCleanup &&
        dropType.aggregate.front().uniqueOwnerField == 0 &&
        !dropType.aggregate.front().uniqueOwnerBase &&
        !dropType.aggregate.front().captureIndex && !dropType.classInstance &&
        !dropType.lambdaInstance && !dropType.uniquePointeeClass &&
        !dropType.polymorphicCleanup;
    const bool alternativeRecipe =
        ((dropType.type.kind == SemanticType::Expected &&
          dropType.type.arguments.size() == 2) ||
         (dropType.type.kind == SemanticType::Unexpected &&
          dropType.type.arguments.size() == 1)) &&
        dropType.aggregate.size() == dropType.type.arguments.size() &&
        !dropType.classInstance && !dropType.lambdaInstance &&
        !dropType.destructor && !dropType.fixedArray &&
        !dropType.uniquePointeeClass && !dropType.polymorphicCleanup &&
        std::equal(dropType.aggregate.begin(), dropType.aggregate.end(),
                   dropType.type.arguments.begin(),
                   [](const auto &child, const auto &type) {
                     return child.type == type && child.uniqueOwnerField == 0 &&
                            !child.uniqueOwnerBase && !child.captureIndex;
                   });
    if (obligation.id != index + 1 ||
        obligation.constructionOrder != index + 1 || !validHirIdentity ||
        place == nullptr || place->type != obligation.dropType.type ||
        obligation.dropType.type == SemanticType::Unknown ||
        obligation.dropType.type.kind == SemanticType::Reference ||
        (bindingKind != (obligation.binding != 0)) ||
        (((valueKind || returnKind) != (obligation.value != 0)) &&
         !rollbackKind) ||
        (rollbackKind && (obligation.value != 0 || obligation.binding != 0)) ||
        !exactFullExpression ||
        (obligation.initiallyActive && !bindingKind &&
         !(rollbackKind && body.kind == MirBodyKind::Constructor)) ||
        (rollbackKind && body.kind == MirBodyKind::Constructor &&
         (place->root != MirPlaceRootKind::This ||
          place->projections.size() != 1 ||
          (place->projections.front().kind != MirProjectionKind::Field &&
           place->projections.front().kind != MirProjectionKind::Base))) ||
        (rollbackKind && body.kind == MirBodyKind::FieldInitializers &&
         (place->root != MirPlaceRootKind::Binding || place->binding == 0 ||
          !place->projections.empty())) ||
        (rollbackKind && body.kind != MirBodyKind::Constructor &&
         body.kind != MirBodyKind::FieldInitializers) ||
        (returnKind && (returnCarrierValue(obligation) == 0 ||
                        !isReturnCarrierType(obligation.dropType.type))) ||
        ((preparedKind || allocationKind) &&
         (place->root != MirPlaceRootKind::Temporary || place->temporary == 0 ||
          place->sourceValue == 0)) ||
        (allocationKind &&
         (obligation.dropType.type.kind != SemanticType::UniqueOwner ||
          obligation.dropType.type.arguments.size() != 1 ||
          obligation.dropType.type.arguments.front().kind !=
              SemanticType::Class ||
          obligation.dropType.destructor || obligation.dropType.fixedArray ||
          obligation.dropType.uniquePointeeClass ||
          !obligation.dropType.aggregate.empty() ||
          !obligation.dropType.requiresActiveCleanup)) ||
        (obligation.dropType.type.kind == SemanticType::Class &&
         !obligation.dropType.classInstance) ||
        (obligation.dropType.classInstance &&
         obligation.dropType.type.kind != SemanticType::Class) ||
        (obligation.dropType.destructor && !obligation.dropType.classInstance &&
         !obligation.dropType.uniquePointeeClass &&
         (!obligation.dropType.fixedArray ||
          !obligation.dropType.fixedArray->leafClassInstance)) ||
        obligation.dropType.uniqueOwnerField != 0 ||
        obligation.dropType.uniqueOwnerBase ||
        obligation.dropType.captureIndex ||
        obligation.dropType.payloadVariant ||
        obligation.dropType.payloadField ||
        obligation.dropType.recursiveCleanup ||
        (obligation.dropType.uniquePointeeClass &&
         ((obligation.dropType.type.kind != SemanticType::UniqueOwner ||
           obligation.dropType.type.arguments.size() != 1 ||
           obligation.dropType.type.arguments.front().kind !=
               SemanticType::Class) ||
          obligation.dropType.fixedArray ||
          obligation.dropType.lambdaInstance ||
          (obligation.dropType.polymorphicCleanup
               ? obligation.dropType.destructor ||
                     !obligation.dropType.aggregate.empty()
           : obligation.dropType.destructor
               ? !obligation.dropType.aggregate.empty()
               // A passive class has allocation cleanup but no pointee drop.
               // Program verification proves the exact class/recipe pairing.
               : obligation.dropType.aggregate.size() > 1))) ||
        (obligation.dropType.polymorphicCleanup &&
         !obligation.dropType.uniquePointeeClass) ||
        (!obligation.dropType.aggregate.empty() &&
         !obligation.dropType.uniquePointeeClass && !alternativeRecipe &&
         !arrayRecipe && obligation.dropType.type.kind != SemanticType::Enum &&
         obligation.dropType.type.kind != SemanticType::PrefixStorage &&
         !(obligation.dropType.type.kind == SemanticType::Lambda &&
           obligation.dropType.lambdaInstance &&
           !obligation.dropType.classInstance &&
           !obligation.dropType.destructor &&
           !obligation.dropType.fixedArray) &&
         (obligation.dropType.type.kind != SemanticType::Class ||
          !obligation.dropType.classInstance ||
          obligation.dropType.fixedArray ||
          obligation.dropType.lambdaInstance)) ||
        (!obligation.dropType.aggregate.empty() &&
         obligation.dropType.type.kind == SemanticType::PrefixStorage &&
         (obligation.dropType.aggregate.size() != 1 ||
          obligation.dropType.type.arguments.size() != 1 ||
          obligation.dropType.aggregate.front().type !=
              obligation.dropType.type.arguments.front() ||
          obligation.dropType.aggregate.front().uniqueOwnerField != 0 ||
          obligation.dropType.aggregate.front().uniqueOwnerBase ||
          obligation.dropType.classInstance || obligation.dropType.destructor ||
          obligation.dropType.uniquePointeeClass ||
          obligation.dropType.fixedArray ||
          obligation.dropType.lambdaInstance))) {
      return failure(body, owner,
                     "drop obligation " + std::to_string(obligation.id) +
                         " has an invalid identity, place, or type");
    }
    if (bindingKind && (place->root != MirPlaceRootKind::Binding ||
                        place->binding != obligation.binding)) {
      return failure(body, owner,
                     "binding drop obligation does not name its exact place");
    }
    if (valueKind && place->sourceValue != obligation.value) {
      return failure(body, owner,
                     "value drop obligation does not retain source identity");
    }
  }

  const auto operandCarriesObligation =
      [&](const MirOperand &operand, const MirDropObligation &obligation) {
        if (operand.type != obligation.dropType.type) {
          return false;
        }
        if (operand.kind == MirOperandKind::Value) {
          const MirValue *value = body.findValue(operand.value);
          if (obligation.kind == MirDropObligationKind::Value) {
            return value != nullptr && value->sourceValue == obligation.value;
          }
          const MirInstruction *definition =
              value == nullptr ? nullptr
                               : linearValueDefinition(body, operand.value);
          if (obligation.kind == MirDropObligationKind::Binding &&
              definition != nullptr &&
              definition->kind == MirInstructionKind::Move &&
              definition->operands.size() == 1 &&
              definition->operands.front().kind == MirOperandKind::Move &&
              definition->operands.front().place == obligation.place) {
            const MirPlace *place = body.findPlace(obligation.place);
            return place != nullptr && place->projections.size() == 1 &&
                   place->projections.front().kind ==
                       MirProjectionKind::PackElement;
          }
          return obligation.kind == MirDropObligationKind::PreparedParameter &&
                 definition != nullptr &&
                 definition->kind == MirInstructionKind::CallInput &&
                 definition->preparedParameterDrop == obligation.id;
        }
        if (operand.kind == MirOperandKind::Copy ||
            operand.kind == MirOperandKind::Move ||
            operand.kind == MirOperandKind::Address ||
            operand.kind == MirOperandKind::BorrowRead ||
            operand.kind == MirOperandKind::BorrowWrite) {
          const MirPlace *place = body.findPlace(operand.place);
          const MirPlace *required = body.findPlace(obligation.place);
          if (place == nullptr || required == nullptr) {
            return false;
          }
          if (place->id == required->id) {
            return true;
          }
          if (obligation.kind == MirDropObligationKind::Binding) {
            return place->root == MirPlaceRootKind::Binding &&
                   place->binding == obligation.binding &&
                   place->projections.empty();
          }
          return obligation.kind == MirDropObligationKind::Value &&
                 place->sourceValue == obligation.value &&
                 place->projections.empty();
        }
        return false;
      };
  const auto prematureReturnTransfer =
      [&](const MirBlock &block, const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        if (instruction.kind != MirInstructionKind::Lifecycle ||
            obligation.kind != MirDropObligationKind::Value ||
            !isReturnCarrierType(obligation.dropType.type) ||
            std::none_of(body.blocks.begin(), body.blocks.end(),
                         [&](const auto &candidate) {
                           const auto &term = candidate.terminator;
                           const auto *value =
                               term.value &&
                                       term.value->kind == MirOperandKind::Value
                                   ? body.findValue(term.value->value)
                                   : nullptr;
                           return term.kind == MirTerminatorKind::Return &&
                                  value != nullptr &&
                                  value->sourceValue == obligation.value;
                         }))
          return false;
        std::vector<MirBlockId> pending{block.id};
        std::unordered_set<MirBlockId> visited;
        while (!pending.empty()) {
          const auto id = pending.back();
          pending.pop_back();
          if (!visited.insert(id).second)
            continue;
          const auto *candidate = body.findBlock(id);
          if (candidate == nullptr)
            continue;
          if (candidate->terminator.kind == MirTerminatorKind::Invoke)
            return true;
          const auto edges = successors(candidate->terminator);
          pending.insert(pending.end(), edges.begin(), edges.end());
        }
        return false;
      };
  const auto instructionConsumesObligation =
      [&](const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        if (obligation.kind == MirDropObligationKind::ObjectAllocation) {
          return instruction.kind == MirInstructionKind::Lifecycle &&
                 instruction.objectAllocation == obligation.id;
        }
        if (obligation.kind == MirDropObligationKind::PreparedParameter &&
            instruction.kind == MirInstructionKind::Call &&
            instruction.receiver &&
            operandCarriesObligation(*instruction.receiver, obligation)) {
          return true;
        }
        for (std::size_t index = 0; index < instruction.operands.size();
             ++index) {
          const MirOperand &operand = instruction.operands[index];
          bool consuming = operand.kind == MirOperandKind::Move;
          if (operand.kind == MirOperandKind::Value) {
            switch (instruction.kind) {
            case MirInstructionKind::Initialize:
            case MirInstructionKind::Assign:
              consuming = index == 0;
              break;
            case MirInstructionKind::Call:
            case MirInstructionKind::Construct:
              consuming = index < instruction.parameterTypes.size() &&
                          instruction.parameterTypes[index].kind !=
                              SemanticType::Reference;
              // The reference-shaped special-member formal selects a
              // generated move; its input is an owned SSA value, not a borrow.
              // Ordinary reference parameters retain the non-consuming rule.
              consuming =
                  consuming ||
                  (instruction.kind == MirInstructionKind::Construct &&
                   instruction.constructorKind == ConstructorKind::Move &&
                   !instruction.constructorTarget &&
                   !instruction.functionTarget && !instruction.receiver &&
                   instruction.callSite == 0 &&
                   instruction.intrinsic == IntrinsicKind::None &&
                   instruction.operands.size() == 1 &&
                   instruction.parameterTypes.size() == 1 &&
                   instruction.parameterTypes.front().kind ==
                       SemanticType::Reference &&
                   instruction.parameterTypes.front().arguments ==
                       std::vector<SemanticType>{instruction.info.type} &&
                   operand.type == instruction.info.type &&
                   instruction.info.type.kind == SemanticType::Class &&
                   instruction.result && instruction.definedFailure.empty());
              break;
            case MirInstructionKind::CallInput:
              consuming =
                  index == 0 &&
                  (instruction.callInputKind == HirCallInputKind::MoveValue ||
                   instruction.callInputKind ==
                       HirCallInputKind::MoveBorrowedTemporary);
              break;
            case MirInstructionKind::Compute:
              consuming =
                  instruction.operation == MirOperation::Aggregate ||
                  instruction.operation == MirOperation::PayloadConstruct ||
                  instruction.operation == MirOperation::Unexpected ||
                  (instruction.operation == MirOperation::Closure &&
                   index < instruction.closureCaptureModes.size() &&
                   instruction.closureCaptureModes[index] ==
                       LambdaCaptureMode::Move) ||
                  ((instruction.operation == MirOperation::Identity ||
                    instruction.operation == MirOperation::Convert ||
                    instruction.operation == MirOperation::Comma) &&
                   index + 1 == instruction.operands.size());
              break;
            default:
              consuming = false;
              break;
            }
          }
          if (consuming && operandCarriesObligation(operand, obligation)) {
            return true;
          }
        }
        return false;
      };
  const auto instructionProducesObligation =
      [&](const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        if (instruction.kind == MirInstructionKind::InitializeBase) {
          const MirPlace *place = body.findPlace(obligation.place);
          return obligation.kind ==
                     MirDropObligationKind::ConstructionRollback &&
                 place != nullptr && place->root == MirPlaceRootKind::This &&
                 place->projections.size() == 1 &&
                 place->projections.front().kind == MirProjectionKind::Base &&
                 instruction.destination == place->id &&
                 instruction.successResultDrop == obligation.id &&
                 instruction.info.type == obligation.dropType.type;
        }
        if (obligation.kind == MirDropObligationKind::ObjectAllocation) {
          return instruction.kind == MirInstructionKind::AllocateObject &&
                 instruction.objectAllocation == obligation.id &&
                 instruction.destination == obligation.place &&
                 instruction.info.type == obligation.dropType.type;
        }
        const MirPlace *place = body.findPlace(obligation.place);
        const MirValue *result =
            instruction.result ? body.findValue(*instruction.result) : nullptr;
        const bool valueResult =
            obligation.kind == MirDropObligationKind::Value &&
            result != nullptr && result->sourceValue == obligation.value &&
            instruction.hirValue == obligation.value;
        const bool preparedResult =
            obligation.kind == MirDropObligationKind::PreparedParameter &&
            instruction.kind == MirInstructionKind::CallInput &&
            instruction.preparedParameterDrop == obligation.id &&
            result != nullptr && result->sourceValue == instruction.hirValue;
        if ((!valueResult && !preparedResult) || place == nullptr ||
            instruction.info.type != obligation.dropType.type) {
          return false;
        }
        if (place->root == MirPlaceRootKind::Value) {
          // A producer creates its result only here (or on its Invoke
          // success edge); an already-live value slot contradicts that
          // publication contract independently of drop-state metadata.
          return place->value == result->id && !instruction.destination &&
                 !place->initiallyAvailable;
        }
        if (place->root != MirPlaceRootKind::Temporary) {
          return false;
        }
        if (instruction.destination == place->id) {
          return true;
        }
        return (instruction.kind == MirInstructionKind::Call ||
                instruction.kind == MirInstructionKind::Construct) &&
               !instruction.destination &&
               instruction.successResultDrop == obligation.id;
      };
  const auto initializationTargetsObligation =
      [&](const MirInstruction &instruction,
          const MirDropObligation &obligation) {
        const MirPlace *place = body.findPlace(obligation.place);
        return instruction.kind == MirInstructionKind::Initialize &&
               place != nullptr && instruction.destination == place->id &&
               instruction.info.type == obligation.dropType.type;
      };
  const auto materializingInstructionKind = [](MirInstructionKind kind) {
    return kind == MirInstructionKind::Compute ||
           kind == MirInstructionKind::Move ||
           kind == MirInstructionKind::CallInput ||
           kind == MirInstructionKind::Call ||
           kind == MirInstructionKind::Construct;
  };

  if (body.valueUses.size() != body.values.size()) {
    return failure(body, owner,
                   "value-use index size does not match the value table");
  }
  std::vector<std::size_t> definitionCounts(body.values.size(), 0);
  for (const MirBlock &block : body.blocks) {
    for (const MirBlockParameter &parameter : block.parameters) {
      if (parameter.value != 0 && parameter.value <= definitionCounts.size()) {
        ++definitionCounts[parameter.value - 1];
      }
    }
  }
  for (std::size_t index = 0; index < body.values.size(); ++index) {
    const MirValue &value = body.values[index];
    const bool blockParameter =
        definitionCounts[index] == 1 && value.definition == 0;
    if (value.id != index + 1 || value.sourceValue == 0 ||
        value.definitionBlock == 0 ||
        (!blockParameter && value.definition == 0)) {
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
  std::unordered_map<MirInstructionId, std::size_t> instructionOrders;
  std::unordered_map<MirInstructionId, MirBlockId> instructionBlocks;
  std::unordered_map<MirInstructionId, const MirInstruction *> instructionsById;
  std::vector<std::size_t> fullExpressionMarkers(body.fullExpressions.size(),
                                                 0);
  std::vector<std::size_t> primaryFullExpressionMarkers(
      body.fullExpressions.size(), 0);
  std::vector<std::vector<const MirInstruction *>> fullExpressionMarkerRows(
      body.fullExpressions.size());
  std::vector<std::size_t> cleanupBoundaryMarkers(body.cleanupBoundaries.size(),
                                                  0);
  std::vector<std::size_t> failureParameterCounts(body.failureRecords.size(),
                                                  0);
  std::vector<std::size_t> failureInvokeCounts(body.failureRecords.size(), 0);
  std::vector<std::size_t> failureEndpointCounts(body.failureRecords.size(), 0);
  std::unordered_map<MirInstructionId, std::size_t> invokedInstructions;
  const auto terminalContinuationIsUnreachable =
      [&](const MirBlock &start, std::size_t instructionIndex) {
        const MirBlock *block = &start;
        std::size_t next = instructionIndex + 1;
        std::unordered_set<MirBlockId> visited;
        while (block != nullptr && visited.insert(block->id).second) {
          // Only lifetime bookkeeping may occupy the impossible successful
          // continuation. In particular, no later argument, expression or
          // ordinary return may be made reachable by losing terminality.
          for (; next < block->instructions.size(); ++next) {
            const MirInstruction &tail = block->instructions[next];
            if (!tail.tombstone && tail.kind != MirInstructionKind::Lifecycle &&
                tail.kind != MirInstructionKind::EndBorrow &&
                tail.kind != MirInstructionKind::Drop) {
              return false;
            }
          }
          const MirTerminator &terminator = block->terminator;
          if (terminator.kind == MirTerminatorKind::Unreachable) {
            return true;
          }
          if (terminator.kind != MirTerminatorKind::Goto &&
              terminator.kind != MirTerminatorKind::Invoke) {
            return false;
          }
          block = body.findBlock(terminator.target);
          next = 0;
        }
        return false;
      };
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    const MirBlock &block = body.blocks[index];
    if (block.id != index + 1) {
      return failure(body, owner,
                     "block identity does not match stored block order");
    }
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions.size(); ++instructionIndex) {
      if (block.instructions[instructionIndex].neverReturns &&
          !terminalContinuationIsUnreachable(block, instructionIndex)) {
        return failure(body, owner,
                       "terminal call has an ordinary normal continuation",
                       block.id, block.instructions[instructionIndex].id);
      }
    }
    if (body.kind != MirBodyKind::Module &&
        block.programInitializationStep != 0) {
      return failure(body, owner,
                     "non-module block carries a program-initialization step",
                     block.id);
    }
    if (block.id == body.entry && !block.parameters.empty()) {
      return failure(body, owner,
                     "entry block cannot require incoming value arguments",
                     block.id);
    }
    if (block.failureParameter != 0 && !block.parameters.empty()) {
      return failure(body, owner,
                     "failure-record block cannot also bind value parameters",
                     block.id);
    }
    std::unordered_set<MirValueId> blockParameters;
    for (const MirBlockParameter &parameter : block.parameters) {
      const MirValue *value = body.findValue(parameter.value);
      if (value == nullptr || !blockParameters.insert(parameter.value).second ||
          !isMirBlockParameterType(parameter.type) ||
          parameter.sourceValue == 0 || value->definitionBlock != block.id ||
          value->definition != 0 ||
          value->sourceValue != parameter.sourceValue ||
          value->info.type != parameter.type) {
        return failure(body, owner,
                       "block parameter has an invalid value, type, or "
                       "source provenance",
                       block.id);
      }
    }
    if (block.failureParameter != 0) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.failureParameter);
      if (record == nullptr || record->parameterBlock != block.id ||
          block.id == body.entry) {
        return failure(body, owner,
                       "failure block has an invalid fixed-record parameter",
                       block.id);
      }
      ++failureParameterCounts[block.failureParameter - 1];
    }
    if (block.activeFailure != 0) {
      const MirFailureRecord *active =
          body.findFailureRecord(block.activeFailure);
      if (active == nullptr || block.id == body.entry) {
        return failure(body, owner,
                       "failure-cleanup block has an invalid active record",
                       block.id);
      }
      if (block.failureParameter != 0) {
        const MirFailureRecord *parameter =
            body.findFailureRecord(block.failureParameter);
        const MirBlock *producer =
            parameter == nullptr ? nullptr
                                 : body.findBlock(parameter->producerBlock);
        const bool primary = block.failureParameter == block.activeFailure &&
                             producer != nullptr &&
                             producer->activeFailure == 0;
        const bool secondary = block.failureParameter != block.activeFailure &&
                               producer != nullptr &&
                               producer->activeFailure == block.activeFailure;
        if (!primary && !secondary) {
          return failure(body, owner,
                         "failure parameter does not bind a primary or the "
                         "first cleanup secondary",
                         block.id);
        }
      }
    } else if (block.failureParameter != 0) {
      return failure(body, owner,
                     "failure parameter is not carried as active cleanup",
                     block.id);
    }
    std::size_t previousFailureDrop = std::numeric_limits<std::size_t>::max();
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions.size(); ++instructionIndex) {
      const MirInstruction &instruction = block.instructions[instructionIndex];
      if (instruction.tombstone) {
        if (block.activeFailure != 0 || instruction.id == 0 ||
            !instructionIds.insert(instruction.id).second ||
            !validInstructionShape(instruction) ||
            (instruction.result && !validValue(*instruction.result))) {
          return failure(body, owner,
                         "instruction tombstone has an invalid canonical "
                         "shape or identity",
                         block.id, instruction.id);
        }
        instructionOrders.emplace(instruction.id, instructionIndex);
        instructionBlocks.emplace(instruction.id, block.id);
        instructionsById.emplace(instruction.id, &instruction);
        if (instruction.result) {
          const MirValue &value = body.values[*instruction.result - 1];
          if (value.definitionBlock != block.id ||
              value.definition != instruction.id ||
              value.info.type != instruction.info.type ||
              !body.valueUses[*instruction.result - 1].empty()) {
            return failure(body, owner,
                           "instruction tombstone retains a live or mismatched "
                           "result",
                           block.id, instruction.id);
          }
          ++definitionCounts[*instruction.result - 1];
        }
        continue;
      }
      if (block.activeFailure != 0) {
        const bool failureDrop =
            instruction.kind == MirInstructionKind::Drop &&
            instruction.lifecycle.size() == 1 &&
            instruction.lifecycle.front().kind == MirLifecycleEventKind::Drop &&
            instruction.lifecycle.front().failureCleanup;
        const bool failureBoundary =
            instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.cleanupBoundaryEnd != 0 &&
            instruction.cleanupBoundaryEnd <= body.cleanupBoundaries.size() &&
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1].kind ==
                MirCleanupBoundaryKind::Failure;
        if (instruction.kind != MirInstructionKind::EndBorrow && !failureDrop &&
            !failureBoundary) {
          return failure(body, owner,
                         "failure block contains a non-cleanup instruction",
                         block.id, instruction.id);
        }
        if (failureDrop) {
          const MirDropObligation *obligation =
              body.findDropObligation(instruction.lifecycle.front().source);
          if (obligation == nullptr ||
              obligation->constructionOrder >= previousFailureDrop) {
            return failure(body, owner,
                           "failure cleanup sequence is not in reverse "
                           "construction order",
                           block.id, instruction.id);
          }
          previousFailureDrop = obligation->constructionOrder;
        }
      }
      if (!validFailureInstructionShape(instruction) ||
          !validSafetyDischarges(instruction)) {
        const DefinedFailureOrigin *firstOrigin =
            instruction.definedFailure.localOrigins.empty()
                ? nullptr
                : &instruction.definedFailure.localOrigins.front();
        return failure(
            body, owner,
            "defined-failure metadata or safety-discharge metadata does not "
            "match instruction kind " +
                std::to_string(static_cast<unsigned>(instruction.kind)) +
                " and propagation " +
                std::to_string(static_cast<unsigned>(
                    instruction.definedFailure.propagation)) +
                "; local origins=" +
                std::to_string(instruction.definedFailure.localOrigins.size()) +
                (firstOrigin == nullptr
                     ? std::string{}
                     : ", first anchor unit=" +
                           std::to_string(firstOrigin->sourceUnit) +
                           " outcomes=" +
                           std::to_string(firstOrigin->outcomes.size())),
            block.id, instruction.id);
      }
      const bool invalidLoanValueEvidence = std::any_of(
          instruction.operands.begin(), instruction.operands.end(),
          [&](const MirOperand &operand) {
            if (operand.kind != MirOperandKind::Loan || operand.value == 0) {
              return false;
            }
            const MirLoan *published =
                instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
            const MirLoan *retained = body.findLoan(operand.loan);
            const bool storedCarrier =
                retained != nullptr && retained->kind == MirLoanKind::Stored &&
                retained->entry && !retained->designatedFields.empty();
            const bool dynamicTextCarrier =
                retained != nullptr && retained->kind == MirLoanKind::Stored &&
                retained->entry && retained->designatedFields.empty() &&
                (operand.type.kind == SemanticType::StringView ||
                 operand.type.kind == SemanticType::CString) &&
                published != nullptr &&
                published->access == AccessMode::ReadOnly;
            const bool callResult = retained != nullptr &&
                                    retained->kind == MirLoanKind::CallResult &&
                                    !retained->entry && !retained->escapes &&
                                    retained->producedBy != 0;
            const auto *value = body.findValue(operand.value);
            const auto *definition =
                value ? body.findBlock(value->definitionBlock) : nullptr;
            const bool constructedField =
                body.kind == MirBodyKind::Constructor && published &&
                published->kind == MirLoanKind::Stored &&
                published->storedField != 0 && retained && !retained->entry &&
                retained->kind == MirLoanKind::Stored && !retained->escapes &&
                value && definition && operand.place == 0 &&
                value->info.type == operand.type &&
                std::any_of(
                    definition->instructions.begin(),
                    definition->instructions.end(), [&](const auto &producer) {
                      return producer.id == value->definition &&
                             producer.kind == MirInstructionKind::Construct &&
                             producer.constructorTarget &&
                             producer.result == operand.value &&
                             producer.loan == retained->id &&
                             producer.hirValue == retained->producedBy;
                    });
            return instruction.kind != MirInstructionKind::Borrow ||
                   instruction.operands.size() != 1 || published == nullptr ||
                   (published->kind != MirLoanKind::Return &&
                    !(body.kind == MirBodyKind::Constructor &&
                      published->kind == MirLoanKind::Stored &&
                      published->storedField != 0 &&
                      (storedCarrier || constructedField))) ||
                   (!storedCarrier && !dynamicTextCarrier &&
                    !constructedField &&
                    (!callResult || operand.place != 0 || operand.value != 0));
          });
      if (instruction.id == 0 ||
          !instructionIds.insert(instruction.id).second ||
          instruction.intrinsic == IntrinsicKind::Count ||
          (instruction.destination && !validPlace(*instruction.destination)) ||
          (instruction.receiver && !validOperand(*instruction.receiver)) ||
          (instruction.loan && !validLoan(*instruction.loan)) ||
          (instruction.result && !validValue(*instruction.result)) ||
          (instruction.preparedParameterDrop &&
           body.findDropObligation(*instruction.preparedParameterDrop) ==
               nullptr) ||
          (instruction.successResultDrop &&
           body.findDropObligation(*instruction.successResultDrop) ==
               nullptr) ||
          (instruction.successResultDestination &&
           body.findPlace(*instruction.successResultDestination) == nullptr) ||
          std::any_of(instruction.operands.begin(), instruction.operands.end(),
                      [&](const MirOperand &operand) {
                        return !validOperand(operand);
                      }) ||
          invalidLoanValueEvidence || !validInstructionShape(instruction)) {
        return failure(body, owner,
                       "instruction has an invalid shape or reference",
                       block.id, instruction.id);
      }
      if (instruction.intrinsic == IntrinsicKind::ExpectedValue &&
          instruction.receiver &&
          instruction.receiver->type.kind == SemanticType::Expected &&
          instruction.receiver->type.arguments.size() == 2 &&
          instruction.receiver->type.arguments.front().kind ==
              SemanticType::Reference) {
        // A fallible reference stores an external address. Its extraction
        // must preserve the carrier's actual owner, never its shell storage.
        // Follow only explicit loan publication and copy/move transport.
        const MirLoan *extracted =
            instruction.loan ? body.findLoan(*instruction.loan) : nullptr;
        std::unordered_set<MirPlaceId> visiting;
        const auto preservesOwner = [&](auto &&self, MirPlaceId id) -> bool {
          const MirPlace *carrier = body.findPlace(id);
          if (carrier == nullptr || extracted == nullptr ||
              !visiting.insert(id).second)
            return false;
          const auto sameOwner = [&](const MirLoan &dependency) {
            return dependency.id != extracted->id &&
                   dependency.access == AccessMode::ReadOnly &&
                   sameCanonicalPlace(canonicalBorrowOriginPlace(
                                          body, dependency.source, {}, false),
                                      canonicalBorrowOriginPlace(
                                          body, extracted->source, {}, false));
          };
          if (carrier->root == MirPlaceRootKind::Binding) {
            return std::any_of(body.loans.begin(), body.loans.end(),
                               [&](const MirLoan &dependency) {
                                 return std::find(dependency.carriers.begin(),
                                                  dependency.carriers.end(),
                                                  carrier->binding) !=
                                            dependency.carriers.end() &&
                                        sameOwner(dependency);
                               });
          }
          if (carrier->root != MirPlaceRootKind::Value)
            return false;
          const MirValue *value = body.findValue(carrier->value);
          const MirBlock *definition =
              value ? body.findBlock(value->definitionBlock) : nullptr;
          if (definition == nullptr)
            return false;
          const auto producer = std::find_if(
              definition->instructions.begin(), definition->instructions.end(),
              [&](const MirInstruction &candidate) {
                return candidate.id == value->definition;
              });
          if (producer == definition->instructions.end())
            return false;
          if (producer->loan) {
            const MirLoan *dependency = body.findLoan(*producer->loan);
            return dependency != nullptr && sameOwner(*dependency);
          }
          return (producer->kind == MirInstructionKind::Move ||
                  producer->kind == MirInstructionKind::Load) &&
                 producer->operands.size() == 1 &&
                 self(self, producer->operands.front().place);
        };
        const MirPlace *origin =
            extracted ? body.findPlace(extracted->source) : nullptr;
        const SemanticType &reference =
            instruction.receiver->type.arguments.front();
        if (reference.arguments.size() != 1 ||
            reference.referenceAccess != AccessMode::ReadOnly ||
            instruction.info.type != reference.arguments.front() ||
            instruction.borrowAccess != AccessMode::ReadOnly ||
            origin == nullptr || origin->root == MirPlaceRootKind::Value ||
            origin->root == MirPlaceRootKind::Temporary ||
            extracted->source == instruction.receiver->place ||
            extracted->access != AccessMode::ReadOnly ||
            instruction.borrowOrigin != BorrowOriginKind::Receiver ||
            !preservesOwner(preservesOwner, instruction.receiver->place)) {
          return failure(
              body, owner,
              "fallible reference extraction lost its carrier's external owner",
              block.id, instruction.id);
        }
      }
      if (instruction.fullExpressionEnd != 0) {
        if (instruction.fullExpressionEnd > body.fullExpressions.size()) {
          return failure(body, owner,
                         "full-expression marker names an invalid boundary",
                         block.id, instruction.id);
        }
        ++fullExpressionMarkers[instruction.fullExpressionEnd - 1];
        if (!instruction.earlyExitFullExpressionEnd) {
          ++primaryFullExpressionMarkers[instruction.fullExpressionEnd - 1];
        }
        fullExpressionMarkerRows[instruction.fullExpressionEnd - 1].push_back(
            &instruction);
        const MirFullExpression &expression =
            body.fullExpressions[instruction.fullExpressionEnd - 1];
        const bool statementBoundary = expression.statement != 0;
        if (instruction.hirStatement != expression.statement ||
            (statementBoundary &&
             std::find(expression.roots.begin(), expression.roots.end(),
                       instruction.hirValue) == expression.roots.end()) ||
            (!statementBoundary && instruction.hirValue != 0)) {
          return failure(body, owner,
                         "full-expression marker does not retain its exact "
                         "HIR boundary identity",
                         block.id, instruction.id);
        }
      }
      if (instruction.cleanupBoundaryEnd != 0) {
        if (instruction.cleanupBoundaryEnd > body.cleanupBoundaries.size()) {
          return failure(body, owner,
                         "cleanup marker names an invalid boundary", block.id,
                         instruction.id);
        }
        const MirCleanupBoundary &boundary =
            body.cleanupBoundaries[instruction.cleanupBoundaryEnd - 1];
        if ((boundary.kind == MirCleanupBoundaryKind::Failure) !=
            (block.activeFailure != 0)) {
          return failure(body, owner,
                         "cleanup boundary is attached to the wrong control "
                         "flow kind",
                         block.id, instruction.id);
        }
        ++cleanupBoundaryMarkers[instruction.cleanupBoundaryEnd - 1];
      }
      const auto validLifecycleObligation = [&](MirDropObligationId id) {
        return id != 0 && body.findDropObligation(id) != nullptr;
      };
      for (const MirLifecycleEvent &event : instruction.lifecycle) {
        const bool hasSource = event.source != 0;
        const bool hasTarget = event.target != 0;
        bool validEvent = !event.conditional && !event.failureCleanup;
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          validEvent = !hasSource && hasTarget && !event.conditional &&
                       !event.failureCleanup;
          break;
        case MirLifecycleEventKind::Move:
        case MirLifecycleEventKind::Reparent:
          validEvent = hasSource && hasTarget && event.source != event.target &&
                       !event.conditional && !event.failureCleanup;
          break;
        case MirLifecycleEventKind::Replace:
          validEvent = hasSource && event.source != event.target &&
                       !event.conditional && !event.failureCleanup;
          break;
        case MirLifecycleEventKind::TransferOut:
          validEvent = hasSource && !hasTarget && !event.conditional &&
                       !event.failureCleanup;
          break;
        case MirLifecycleEventKind::Drop:
          validEvent = hasSource && !hasTarget;
          break;
        }
        validEvent = validEvent &&
                     (!hasSource || validLifecycleObligation(event.source)) &&
                     (!hasTarget || validLifecycleObligation(event.target));
        if (!validEvent) {
          return failure(body, owner,
                         "instruction has an invalid lifecycle event", block.id,
                         instruction.id);
        }
        const MirDropObligation *source =
            hasSource ? body.findDropObligation(event.source) : nullptr;
        const MirDropObligation *target =
            hasTarget ? body.findDropObligation(event.target) : nullptr;
        if (source != nullptr && target != nullptr &&
            source->dropType.type != target->dropType.type) {
          return failure(body, owner,
                         "lifecycle source and target types do not match",
                         block.id, instruction.id);
        }
        bool boundToInstruction = true;
        switch (event.kind) {
        case MirLifecycleEventKind::Initialize:
          boundToInstruction =
              target != nullptr &&
              (initializationTargetsObligation(instruction, *target) ||
               ((instruction.kind == MirInstructionKind::Compute ||
                 instruction.kind == MirInstructionKind::Move ||
                 instruction.kind == MirInstructionKind::CallInput ||
                 instruction.kind == MirInstructionKind::Call ||
                 instruction.kind == MirInstructionKind::Construct ||
                 instruction.kind == MirInstructionKind::InitializeBase) &&
                instructionProducesObligation(instruction, *target)));
          break;
        case MirLifecycleEventKind::Move:
          boundToInstruction =
              source != nullptr && target != nullptr &&
              (instruction.kind == MirInstructionKind::Move ||
               (instruction.kind == MirInstructionKind::CallInput &&
                instruction.callInputKind == HirCallInputKind::MoveValue &&
                instruction.preparedParameterDrop == target->id &&
                source->kind == MirDropObligationKind::Binding)) &&
              instructionConsumesObligation(instruction, *source) &&
              instructionProducesObligation(instruction, *target);
          break;
        case MirLifecycleEventKind::Reparent:
          boundToInstruction =
              source != nullptr && target != nullptr &&
              ((instruction.kind == MirInstructionKind::Lifecycle &&
                source->kind == MirDropObligationKind::Value &&
                target->kind == MirDropObligationKind::ReturnResult &&
                instruction.hirValue == source->value &&
                source->value == target->value &&
                source->place == target->place &&
                source->dropType == target->dropType && !event.conditional &&
                !event.failureCleanup) ||
               (instructionConsumesObligation(instruction, *source) &&
                (initializationTargetsObligation(instruction, *target) ||
                 ((instruction.kind == MirInstructionKind::Compute ||
                   instruction.kind == MirInstructionKind::CallInput) &&
                  instructionProducesObligation(instruction, *target)))));
          break;
        case MirLifecycleEventKind::Replace: {
          const MirPlace *destination =
              instruction.destination ? body.findPlace(*instruction.destination)
                                      : nullptr;
          boundToInstruction =
              source != nullptr &&
              instruction.kind == MirInstructionKind::Assign &&
              destination != nullptr &&
              destination->type == source->dropType.type &&
              instructionConsumesObligation(instruction, *source) &&
              (target == nullptr || target->place == destination->id);
          break;
        }
        case MirLifecycleEventKind::TransferOut:
          boundToInstruction =
              source != nullptr &&
              ((instruction.kind == MirInstructionKind::Lifecycle &&
                source->kind == MirDropObligationKind::Value &&
                instruction.hirValue == source->value) ||
               (instruction.kind == MirInstructionKind::Lifecycle &&
                source->kind == MirDropObligationKind::ReturnResult &&
                instruction.hirValue == source->value &&
                &instruction == &block.instructions.back() &&
                block.terminator.kind == MirTerminatorKind::Return &&
                block.terminator.value &&
                block.terminator.value->kind == MirOperandKind::Value &&
                block.terminator.value->value == returnCarrierValue(*source) &&
                !event.conditional && !event.failureCleanup) ||
               // Normal constructor completion retires each armed rollback
               // subobject by transferring it to the caller through one
               // standalone retirement instruction.
               (instruction.kind == MirInstructionKind::Lifecycle &&
                source->kind == MirDropObligationKind::ConstructionRollback &&
                (body.kind == MirBodyKind::Constructor ||
                 body.kind == MirBodyKind::FieldInitializers)) ||
               (instruction.kind == MirInstructionKind::Lifecycle &&
                source->kind == MirDropObligationKind::ObjectAllocation &&
                instruction.objectAllocation == source->id) ||
               ((instruction.kind == MirInstructionKind::Compute ||
                 instruction.kind == MirInstructionKind::Initialize ||
                 instruction.kind == MirInstructionKind::CallInput ||
                 instruction.kind == MirInstructionKind::Call ||
                 instruction.kind == MirInstructionKind::Construct) &&
                instructionConsumesObligation(instruction, *source)));
          if (boundToInstruction &&
              prematureReturnTransfer(block, instruction, *source)) {
            return failure(body, owner,
                           "return ownership transfers before fallible cleanup "
                           "without an unpublished-result obligation",
                           block.id, instruction.id);
          }
          break;
        case MirLifecycleEventKind::Drop:
          break;
        }
        if (!boundToInstruction) {
          return failure(body, owner,
                         "lifecycle event does not match its instruction "
                         "input, output, or destination",
                         block.id, instruction.id);
        }
        if ((event.kind == MirLifecycleEventKind::Drop) !=
            (instruction.kind == MirInstructionKind::Drop)) {
          return failure(body, owner,
                         "drop lifecycle event does not match its instruction",
                         block.id, instruction.id);
        }
        if (event.kind == MirLifecycleEventKind::Drop) {
          const MirDropObligation &obligation =
              *body.findDropObligation(event.source);
          if (event.failureCleanup != (block.activeFailure != 0) ||
              !instruction.destination ||
              *instruction.destination != obligation.place) {
            return failure(body, owner,
                           "drop lifecycle event names a different place",
                           block.id, instruction.id);
          }
        }
      }
      if (instruction.destination &&
          materializingInstructionKind(instruction.kind)) {
        const MirPlace *destination = body.findPlace(*instruction.destination);
        const bool boundResultSlot =
            destination != nullptr &&
            destination->root == MirPlaceRootKind::Temporary &&
            std::any_of(
                instruction.lifecycle.begin(), instruction.lifecycle.end(),
                [&](const MirLifecycleEvent &event) {
                  if (event.target == 0 ||
                      (event.kind != MirLifecycleEventKind::Initialize &&
                       event.kind != MirLifecycleEventKind::Move &&
                       event.kind != MirLifecycleEventKind::Reparent)) {
                    return false;
                  }
                  const MirDropObligation *obligation =
                      body.findDropObligation(event.target);
                  return obligation != nullptr &&
                         obligation->place == destination->id &&
                         instructionProducesObligation(instruction,
                                                       *obligation);
                });
        if (!boundResultSlot) {
          return failure(body, owner,
                         "materializing instruction has an unbound result slot",
                         block.id, instruction.id);
        }
      }
      instructionOrders.emplace(instruction.id, instructionIndex);
      instructionBlocks.emplace(instruction.id, block.id);
      instructionsById.emplace(instruction.id, &instruction);
      if (instruction.kind == MirInstructionKind::Borrow) {
        const MirLoan &produced = body.loans[*instruction.loan - 1];
        const MirOperand &source = instruction.operands.front();
        if (produced.kind == MirLoanKind::Return &&
            source.kind == MirOperandKind::Loan &&
            body.loans[source.loan - 1].storedDependency !=
                produced.storedDependency) {
          return failure(body, owner,
                         "return publication does not preserve the source "
                         "shell's stored dependency",
                         block.id, instruction.id);
        }
        const bool storedCarrierReturnTransfer = [&] {
          const bool fieldPublication = body.kind == MirBodyKind::Constructor &&
                                        produced.kind == MirLoanKind::Stored &&
                                        produced.storedField != 0;
          if ((produced.kind != MirLoanKind::Return && !fieldPublication) ||
              produced.entry || !produced.escapes || produced.parent != 0 ||
              source.kind != MirOperandKind::Loan ||
              instruction.hirValue == 0 ||
              instruction.hirValue != produced.producedBy) {
            return false;
          }
          const MirLoan *retained = body.findLoan(source.loan);
          if (retained == nullptr || retained->kind != MirLoanKind::Stored ||
              retained->source != produced.source ||
              retained->access != produced.access ||
              source.type != instruction.info.type) {
            return false;
          }
          const bool exactStoredCarrierEntry =
              retained->entry && !retained->designatedFields.empty();
          const bool exactDynamicTextEntry =
              retained->entry && retained->designatedFields.empty() &&
              (source.type.kind == SemanticType::StringView ||
               source.type.kind == SemanticType::CString) &&
              instruction.info.type == source.type;
          const auto *constructedValue = body.findValue(source.value);
          const auto *constructedBlock =
              constructedValue
                  ? body.findBlock(constructedValue->definitionBlock)
                  : nullptr;
          const bool exactConstructedField =
              fieldPublication && !retained->entry && !retained->escapes &&
              constructedValue && constructedValue->info.type == source.type &&
              constructedBlock &&
              std::any_of(
                  constructedBlock->instructions.begin(),
                  constructedBlock->instructions.end(),
                  [&](const auto &producer) {
                    return producer.id == constructedValue->definition &&
                           producer.kind == MirInstructionKind::Construct &&
                           producer.constructorTarget &&
                           producer.result == source.value &&
                           producer.loan == retained->id &&
                           producer.hirValue == retained->producedBy;
                  });
          const MirPlace *runtime = body.findPlace(source.place);
          const bool valueTransfer =
              source.place == 0 &&
              (((exactStoredCarrierEntry || exactDynamicTextEntry ||
                 exactConstructedField) &&
                source.value != 0) ||
               (!exactStoredCarrierEntry && !exactDynamicTextEntry &&
                !exactConstructedField && source.value == 0)) &&
              instruction.info.traits.containsBorrowedState;
          const bool referenceTransfer =
              source.place != 0 && source.value == 0 && runtime != nullptr &&
              runtime->type == source.type;
          if (!valueTransfer && !referenceTransfer) {
            return false;
          }
          // Copying a shared carrier produces a distinct result shell. Its
          // external owner is unchanged, while physical reference formals can
          // remain live for scope cleanup. Preserve the original entry until
          // those shells end; flow verification still requires child-first
          // retirement before every value Return and failure exit.
          if (!fieldPublication && valueTransfer &&
              source.type.kind == SemanticType::Class &&
              retained->access == AccessMode::ReadOnly &&
              abstractStoredEntry(body, *retained)) {
            const MirValue *copied = body.findValue(source.value);
            const auto copy = std::find_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const MirInstruction &candidate) {
                  return copied != nullptr &&
                         candidate.id == copied->definition;
                });
            const auto publication = std::find_if(
                block.instructions.begin(), block.instructions.end(),
                [&](const MirInstruction &candidate) {
                  return candidate.id == instruction.id;
                });
            if (copied != nullptr && copied->definitionBlock == block.id &&
                copied->info.type == source.type &&
                copy != block.instructions.end() && copy < publication &&
                copy->kind == MirInstructionKind::Load &&
                copy->result == source.value && copy->info.traits.copyable &&
                copy->operands.size() == 1 &&
                copy->operands.front().kind == MirOperandKind::Copy &&
                copy->operands.front().type == source.type &&
                std::any_of(
                    body.loans.begin(), body.loans.end(),
                    [&](const MirLoan &shell) {
                      return shell.storedDependency == retained->id;
                    }))
              return true;
          }
          const auto current =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const MirInstruction &candidate) {
                             return candidate.id == instruction.id;
                           });
          if (current == block.instructions.end() ||
              std::next(current) == block.instructions.end()) {
            return false;
          }
          const MirInstruction &retire = *std::next(current);
          return retire.kind == MirInstructionKind::EndBorrow && retire.loan &&
                 *retire.loan == source.loan &&
                 retire.hirValue == instruction.hirValue;
        }();
        // A summarized reference return forwards the runtime address held by
        // its transient call-result chain into the published Return loan (the
        // loan renames only provenance) and retires that chain at this exact
        // instruction boundary.
        const bool callResultReturnTransfer = [&] {
          if (produced.kind != MirLoanKind::Return || produced.entry ||
              !produced.escapes || produced.parent != 0 ||
              source.kind != MirOperandKind::Loan || source.place != 0 ||
              source.value != 0 || instruction.hirValue == 0 ||
              instruction.hirValue != produced.producedBy ||
              source.type != instruction.info.type) {
            return false;
          }
          const MirLoan *retained = body.findLoan(source.loan);
          if (retained == nullptr ||
              retained->kind != MirLoanKind::CallResult || retained->entry ||
              retained->escapes || retained->producedBy == 0 ||
              retained->producedBy != instruction.hirValue ||
              !retained->carriers.empty() ||
              retained->source != produced.source ||
              (retained->access != produced.access &&
               retained->access != AccessMode::Mutable)) {
            return false;
          }
          const auto current =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const MirInstruction &candidate) {
                             return candidate.id == instruction.id;
                           });
          if (current == block.instructions.end() ||
              std::next(current) == block.instructions.end()) {
            return false;
          }
          const MirInstruction &retire = *std::next(current);
          return retire.kind == MirInstructionKind::EndBorrow && retire.loan &&
                 *retire.loan == source.loan &&
                 retire.hirValue == instruction.hirValue;
        }();
        // A reference parameter's entry loan is seeded at the body entry and
        // has no producing instruction, so it cannot become the escaping
        // Return loan by reclassification: nothing would ever name the address
        // it publishes. A body that returns its read-only reference parameter
        // forwards that entry loan's caller-owned dependency into a fresh
        // parentless Return loan and retires the entry loan at this exact
        // instruction boundary, exactly as the by-value carrier and
        // nested-call transfers above do.
        const bool parameterEntryReturnTransfer = [&] {
          if (produced.kind != MirLoanKind::Return || produced.entry ||
              !produced.escapes || produced.parent != 0 ||
              source.kind != MirOperandKind::Loan || source.value != 0 ||
              instruction.hirValue == 0 ||
              instruction.hirValue != produced.producedBy ||
              source.type != instruction.info.type) {
            return false;
          }
          const MirLoan *retained = body.findLoan(source.loan);
          if (retained == nullptr || retained->kind != MirLoanKind::Parameter ||
              !retained->entry || retained->escapes || retained->parent != 0 ||
              retained->producedBy != 0 || retained->carriers.empty() ||
              retained->source != produced.source ||
              (retained->access != produced.access &&
               retained->access != AccessMode::Mutable)) {
            return false;
          }
          const MirPlace *carrier = body.findPlace(retained->source);
          if (carrier == nullptr ||
              carrier->root != MirPlaceRootKind::Binding ||
              !carrier->projections.empty() ||
              carrier->type.kind != SemanticType::Reference ||
              carrier->type.arguments.size() != 1 ||
              std::find(retained->carriers.begin(), retained->carriers.end(),
                        carrier->binding) == retained->carriers.end()) {
            return false;
          }
          if (source.place == 0) {
            // With no explicit address, only the parameter pointer itself
            // can be published, with its exact referent type.
            if (carrier->type.arguments.front() != instruction.info.type)
              return false;
          } else {
            const auto exactRuntimeAlias =
                [&](const auto &self, const MirPlace *runtime,
                    const MirBlock &consumerBlock,
                    const MirInstruction &consumer, std::size_t depth) -> bool {
              if (depth > body.places.size() || runtime == nullptr ||
                  runtime->root != MirPlaceRootKind::Binding ||
                  runtime->binding == carrier->binding ||
                  runtime->type != source.type ||
                  runtime->projections.size() != 1 ||
                  runtime->projections.front().kind !=
                      MirProjectionKind::Dereference ||
                  (produced.access == AccessMode::Mutable &&
                   runtime->access != AccessMode::Mutable) ||
                  std::find(retained->carriers.begin(),
                            retained->carriers.end(),
                            runtime->binding) == retained->carriers.end())
                return false;
              const MirInstruction *initialize = nullptr;
              const MirBlock *initializeBlock = nullptr;
              for (const auto &candidateBlock : body.blocks)
                for (const auto &candidate : candidateBlock.instructions) {
                  const auto *destination =
                      candidate.destination
                          ? body.findPlace(*candidate.destination)
                          : nullptr;
                  if (destination == nullptr ||
                      destination->root != MirPlaceRootKind::Binding ||
                      destination->binding != runtime->binding ||
                      !destination->projections.empty())
                    continue;
                  if (candidate.kind != MirInstructionKind::Initialize ||
                      initialize != nullptr ||
                      destination->type.kind != SemanticType::Reference ||
                      destination->type.arguments.size() != 1 ||
                      destination->type.arguments.front() != runtime->type ||
                      destination->type.referenceAccess != runtime->access ||
                      candidate.operands.size() != 1)
                    return false;
                  initialize = &candidate;
                  initializeBlock = &candidateBlock;
                }
              if (initialize == nullptr)
                return false;
              const auto &input = initialize->operands.front();
              const auto *address = body.findPlace(input.place);
              if (input.kind != MirOperandKind::Loan ||
                  input.loan != retained->id || input.type != runtime->type ||
                  input.value != 0 || address == nullptr ||
                  address->type != runtime->type)
                return false;
              // Ordinary verification proves the call result's producer and
              // Initialize address. Its sole publication must dominate this
              // return; carrier metadata or matching types alone grant nothing.
              if (initializeBlock == &consumerBlock) {
                const auto before =
                    std::find_if(consumerBlock.instructions.begin(),
                                 consumerBlock.instructions.end(),
                                 [&](const auto &candidate) {
                                   return &candidate == initialize;
                                 });
                const auto after =
                    std::find_if(consumerBlock.instructions.begin(),
                                 consumerBlock.instructions.end(),
                                 [&](const auto &candidate) {
                                   return &candidate == &consumer;
                                 });
                if (before >= after)
                  return false;
              } else {
                const auto dominance = computeMirDominance(body);
                if (!dominance || !dominance->dominates(initializeBlock->id,
                                                        consumerBlock.id))
                  return false;
              }
              if (address->root == MirPlaceRootKind::Binding) {
                if (address->binding == carrier->binding)
                  return address->projections.size() == 1 &&
                         address->projections.front().kind ==
                             MirProjectionKind::Dereference &&
                         address->type == carrier->type.arguments.front() &&
                         (runtime->access == AccessMode::ReadOnly ||
                          address->access == AccessMode::Mutable);
                return self(self, address, *initializeBlock, *initialize,
                            depth + 1);
              }
              const auto *resultLoan = body.findLoan(address->loan);
              return address->root == MirPlaceRootKind::Loan &&
                     address->projections.empty() && resultLoan != nullptr &&
                     resultLoan->kind == MirLoanKind::CallResult &&
                     resultLoan->parent == retained->id &&
                     resultLoan->source == retained->source &&
                     resultLoan->storedDependency ==
                         retained->storedDependency &&
                     resultLoan->producedBy != 0 &&
                     (runtime->access == AccessMode::ReadOnly ||
                      resultLoan->access == AccessMode::Mutable);
            };
            if (!exactRuntimeAlias(exactRuntimeAlias,
                                   body.findPlace(source.place), block,
                                   instruction, 0))
              return false;
          }
          const auto current =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const MirInstruction &candidate) {
                             return candidate.id == instruction.id;
                           });
          if (current == block.instructions.end() ||
              std::next(current) == block.instructions.end()) {
            return false;
          }
          const MirInstruction &retire = *std::next(current);
          return retire.kind == MirInstructionKind::EndBorrow && retire.loan &&
                 *retire.loan == source.loan &&
                 retire.hirValue == instruction.hirValue;
        }();
        // expected<T&, E> publishes the same pointer-and-provenance handoff
        // as a plain reference return, but its public result type is the
        // fallible carrier rather than Reference itself. The source loan is
        // retired at this exact boundary; the fresh parentless Return loan is
        // the only capability that escapes the body.
        const bool fallibleBorrowReturnTransfer = [&] {
          if (body.returnType.kind != SemanticType::Expected ||
              body.returnType.arguments.size() != 2 ||
              body.returnType.arguments.front().kind !=
                  SemanticType::Reference ||
              body.returnType.arguments.front().referenceAccess !=
                  AccessMode::ReadOnly ||
              body.returnType.arguments.front().arguments.size() != 1 ||
              produced.kind != MirLoanKind::Return || produced.entry ||
              !produced.escapes || produced.parent != 0 ||
              source.kind != MirOperandKind::Loan || source.place != 0 ||
              source.value != 0 || instruction.hirValue == 0 ||
              instruction.hirValue != produced.producedBy ||
              instruction.info.type != source.type ||
              (source.type !=
                   body.returnType.arguments.front().arguments.front() &&
               source.type != body.returnType)) {
            return false;
          }
          const MirLoan *retained = body.findLoan(source.loan);
          if (retained == nullptr || retained->id == produced.id ||
              retained->escapes || retained->source != produced.source ||
              (retained->access != produced.access &&
               retained->access != AccessMode::Mutable)) {
            return false;
          }
          const auto current =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const MirInstruction &candidate) {
                             return candidate.id == instruction.id;
                           });
          if (current == block.instructions.end() ||
              std::next(current) == block.instructions.end()) {
            return false;
          }
          const MirInstruction &retire = *std::next(current);
          return retire.kind == MirInstructionKind::EndBorrow && retire.loan &&
                 *retire.loan == source.loan &&
                 retire.hirValue == instruction.hirValue;
        }();
        if ((produced.parent == 0 && source.kind == MirOperandKind::Loan &&
             !storedCarrierReturnTransfer && !callResultReturnTransfer &&
             !parameterEntryReturnTransfer && !fallibleBorrowReturnTransfer) ||
            (produced.parent != 0 && (source.kind != MirOperandKind::Loan ||
                                      source.loan != produced.parent))) {
          return failure(body, owner,
                         "borrow instruction does not match the loan's parent",
                         block.id, instruction.id);
        }
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
    const auto validEdge = [&](MirBlockId target,
                               const std::vector<MirOperand> &arguments) {
      if (!validTarget(target)) {
        return false;
      }
      const std::vector<MirBlockParameter> &parameters =
          body.blocks[target - 1].parameters;
      if (arguments.size() != parameters.size()) {
        return false;
      }
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        if ((arguments[index].kind != MirOperandKind::Value &&
             arguments[index].kind != MirOperandKind::Constant) ||
            !validOperand(arguments[index]) ||
            arguments[index].type != parameters[index].type) {
          return false;
        }
      }
      return true;
    };
    if (block.terminator.kind == MirTerminatorKind::Goto &&
        (!validEdge(block.terminator.target,
                    block.terminator.targetArguments) ||
         !block.terminator.elseArguments.empty())) {
      return failure(body, owner, "goto target or value arguments are invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Branch &&
        (!validEdge(block.terminator.target,
                    block.terminator.targetArguments) ||
         !validEdge(block.terminator.elseTarget,
                    block.terminator.elseArguments) ||
         !block.terminator.value || !validOperand(*block.terminator.value) ||
         block.terminator.value->type != SemanticType::Bool)) {
      return failure(body, owner,
                     "branch condition, target, or value arguments are invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Switch &&
        (!block.terminator.value || !validOperand(*block.terminator.value) ||
         !validEdge(block.terminator.target,
                    block.terminator.targetArguments) ||
         !block.terminator.elseArguments.empty() ||
         std::any_of(block.terminator.switchTargets.begin(),
                     block.terminator.switchTargets.end(),
                     [&](const MirSwitchTarget &target) {
                       return !validEdge(target.target, target.arguments);
                     }))) {
      return failure(body, owner,
                     "switch value, target, or value arguments are invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Switch) {
      const SemanticType &switchType = block.terminator.value->type;
      if (!isIntegerType(switchType) && switchType != SemanticType::Char &&
          switchType.kind != SemanticType::Enum) {
        return failure(body, owner,
                       "switch selector is not an integer, char, or enum",
                       block.id);
      }
      for (std::size_t index = 0; index < block.terminator.switchTargets.size();
           ++index) {
        const MirSwitchTarget &target = block.terminator.switchTargets[index];
        const SwitchCaseValue *value = target.value ? &*target.value : nullptr;
        bool validCase =
            value != nullptr && value->type == switchType &&
            !(value->value.negative && value->value.magnitude == 0);
        if (validCase && value->kind == SwitchCaseKind::Integer) {
          const std::optional<CheckedIntegerDomain> domain =
              constantIntegerDomain(value->type);
          validCase = value->enumOwner == 0 && domain &&
                      checkedIntegerFits({.negative = value->value.negative,
                                          .magnitude = value->value.magnitude},
                                         *domain);
        } else if (validCase && value->kind == SwitchCaseKind::Character) {
          validCase = value->type == SemanticType::Char &&
                      value->enumOwner == 0 && !value->value.negative &&
                      value->value.magnitude <=
                          std::numeric_limits<std::uint8_t>::max();
        } else if (validCase && value->kind == SwitchCaseKind::Enumerator) {
          validCase = switchType.kind == SemanticType::Enum &&
                      switchType.enumId != 0 &&
                      value->enumOwner == switchType.enumId;
        } else {
          validCase = false;
        }
        const bool duplicate =
            value != nullptr &&
            std::any_of(block.terminator.switchTargets.begin(),
                        block.terminator.switchTargets.begin() +
                            static_cast<std::ptrdiff_t>(index),
                        [&](const MirSwitchTarget &earlier) {
                          return earlier.value == target.value;
                        });
        if (!validCase || duplicate) {
          return failure(body, owner,
                         "switch case is invalid, noncanonical, or duplicate",
                         block.id);
        }
      }
    }
    const bool edgeTerminator =
        block.terminator.kind == MirTerminatorKind::Goto ||
        block.terminator.kind == MirTerminatorKind::Branch ||
        block.terminator.kind == MirTerminatorKind::Switch ||
        block.terminator.kind == MirTerminatorKind::Invoke;
    if (!edgeTerminator && (!block.terminator.targetArguments.empty() ||
                            !block.terminator.elseArguments.empty())) {
      return failure(body, owner,
                     "terminal control flow retains value arguments", block.id);
    }
    const bool hasFailureTerminatorState =
        block.terminator.invokeInstruction != 0 ||
        block.terminator.failureRecord != 0 ||
        !block.terminator.successLifecycle.empty();
    if (block.terminator.kind != MirTerminatorKind::Invoke &&
        block.terminator.kind != MirTerminatorKind::PropagateFailure &&
        block.terminator.kind != MirTerminatorKind::ContainFailure &&
        block.terminator.kind != MirTerminatorKind::TerminateCleanupFailure &&
        hasFailureTerminatorState) {
      return failure(body, owner,
                     "non-failure terminator retains failure-edge state",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Invoke) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      const auto invocation =
          instructionsById.find(block.terminator.invokeInstruction);
      const MirFailureRecordId expectedFailureActive =
          block.activeFailure == 0 ? block.terminator.failureRecord
                                   : block.activeFailure;
      if (!validEdge(block.terminator.target,
                     block.terminator.targetArguments) ||
          !validEdge(block.terminator.elseTarget,
                     block.terminator.elseArguments) ||
          block.terminator.target == block.terminator.elseTarget ||
          block.terminator.value || block.terminator.returnLoan ||
          !block.terminator.switchTargets.empty() || record == nullptr ||
          record->producerBlock != block.id ||
          record->producerInstruction != block.terminator.invokeInstruction ||
          record->parameterBlock != block.terminator.elseTarget ||
          body.blocks[block.terminator.target - 1].failureParameter != 0 ||
          body.blocks[block.terminator.target - 1].activeFailure !=
              block.activeFailure ||
          body.blocks[block.terminator.elseTarget - 1].failureParameter !=
              block.terminator.failureRecord ||
          body.blocks[block.terminator.elseTarget - 1].activeFailure !=
              expectedFailureActive ||
          block.terminator.failureRecord == block.activeFailure ||
          invocation == instructionsById.end() ||
          instructionBlocks.at(block.terminator.invokeInstruction) !=
              block.id ||
          block.instructions.empty() ||
          block.instructions.back().id != block.terminator.invokeInstruction ||
          !supportsMirFailureControlFlow(body.kind) ||
          (!requiresMirFailureControlFlow(
               *invocation->second,
               MirFailureControlFlowPosition::FullExpressionRoot) &&
           !requiresMirFailureControlFlow(
               *invocation->second,
               MirFailureControlFlowPosition::PreparedCallArgumentRoot))) {
        return failure(body, owner,
                       "invoke terminator does not match its exact operation, "
                       "record, or successors",
                       block.id);
      }
      const std::optional<MirDropObligationId> successResultDrop =
          invocation->second->successResultDrop;
      const bool exactSuccessLifecycle =
          successResultDrop
              ? block.terminator.successLifecycle.size() == 1 &&
                    block.terminator.successLifecycle.front().kind ==
                        MirLifecycleEventKind::Initialize &&
                    block.terminator.successLifecycle.front().source == 0 &&
                    block.terminator.successLifecycle.front().target ==
                        *successResultDrop &&
                    !block.terminator.successLifecycle.front().conditional &&
                    !block.terminator.successLifecycle.front().failureCleanup
              : block.terminator.successLifecycle.empty();
      const MirDropObligation *successResult =
          successResultDrop ? body.findDropObligation(*successResultDrop)
                            : nullptr;
      const std::optional<MirPlaceId> successResultDestination =
          invocation->second->successResultDestination;
      bool exactSuccessDestination = !successResultDestination;
      if (successResultDestination && invocation->second->result) {
        const MirPlace *destination = body.findPlace(*successResultDestination);
        const MirInstruction *initialize = nullptr;
        std::size_t executableUses = 0;
        for (const MirValueUse &use :
             body.usesOf(*invocation->second->result)) {
          if (use.kind != MirValueUseKind::InstructionOperand) {
            ++executableUses;
            continue;
          }
          ++executableUses;
          const MirBlock *consumerBlock = body.findBlock(use.block);
          const auto consumer =
              consumerBlock == nullptr
                  ? std::vector<MirInstruction>::const_iterator{}
                  : std::find_if(consumerBlock->instructions.begin(),
                                 consumerBlock->instructions.end(),
                                 [&](const MirInstruction &candidate) {
                                   return candidate.id == use.instruction;
                                 });
          const MirInstruction *consumerInstruction =
              consumerBlock == nullptr ||
                      consumer == consumerBlock->instructions.end()
                  ? nullptr
                  : &*consumer;
          if (consumerInstruction != nullptr && use.operandIndex == 0 &&
              use.block == block.terminator.target &&
              consumerInstruction->kind == MirInstructionKind::Initialize &&
              consumerInstruction->destination == successResultDestination &&
              consumerInstruction->operands.size() == 1 &&
              consumerInstruction->operands.front().kind ==
                  MirOperandKind::Value &&
              consumerInstruction->operands.front().value ==
                  *invocation->second->result) {
            initialize = consumerInstruction;
          }
        }
        const MirDropObligation *destinationDrop = nullptr;
        bool duplicateDestinationDrop = false;
        for (const MirDropObligation &candidate : body.dropObligations) {
          if (candidate.kind == MirDropObligationKind::Binding &&
              candidate.place == *successResultDestination) {
            if (destinationDrop != nullptr) {
              duplicateDestinationDrop = true;
              break;
            }
            destinationDrop = &candidate;
          }
        }
        const bool exactDestinationLifecycle =
            !duplicateDestinationDrop &&
            (destinationDrop == nullptr
                 ? initialize != nullptr && initialize->lifecycle.empty()
                 : initialize != nullptr && initialize->lifecycle.size() == 1 &&
                       initialize->lifecycle.front().kind ==
                           MirLifecycleEventKind::Initialize &&
                       initialize->lifecycle.front().source == 0 &&
                       initialize->lifecycle.front().target ==
                           destinationDrop->id &&
                       !initialize->lifecycle.front().conditional &&
                       !initialize->lifecycle.front().failureCleanup);
        exactSuccessDestination =
            destination != nullptr &&
            destination->root == MirPlaceRootKind::Binding &&
            destination->projections.empty() &&
            destination->type == invocation->second->info.type &&
            executableUses == 1 && initialize != nullptr &&
            exactDestinationLifecycle;
      }
      if (!exactSuccessLifecycle || !exactSuccessDestination ||
          (successResultDrop &&
           (successResult == nullptr ||
            (successResult->kind != MirDropObligationKind::Value &&
             successResult->kind != MirDropObligationKind::ObjectAllocation &&
             !(successResult->kind ==
                   MirDropObligationKind::ConstructionRollback &&
               invocation->second->kind ==
                   MirInstructionKind::InitializeBase)) ||
            !instructionProducesObligation(*invocation->second,
                                           *successResult)))) {
        return failure(body, owner,
                       "invoke success edge does not initialize its exact "
                       "owning result or direct destination",
                       block.id, block.terminator.invokeInstruction);
      }
      if (invocation->second->result &&
          std::any_of(body.usesOf(*invocation->second->result).begin(),
                      body.usesOf(*invocation->second->result).end(),
                      [&](const MirValueUse &use) {
                        return use.block == block.terminator.elseTarget;
                      })) {
        return failure(body, owner,
                       "invoke result is used on its failure successor",
                       block.id, block.terminator.invokeInstruction);
      }
      ++failureInvokeCounts[block.terminator.failureRecord - 1];
      ++invokedInstructions[block.terminator.invokeInstruction];
    }
    const bool emptyFailureEndpointPayload =
        block.terminator.invokeInstruction == 0 && !block.terminator.value &&
        !block.terminator.returnLoan && block.terminator.target == 0 &&
        block.terminator.elseTarget == 0 &&
        block.terminator.switchTargets.empty() &&
        block.terminator.successLifecycle.empty();
    if (block.terminator.kind == MirTerminatorKind::PropagateFailure) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      if (record == nullptr || block.activeFailure == 0 ||
          block.activeFailure != block.terminator.failureRecord ||
          body.kind == MirBodyKind::HostedStartup ||
          !emptyFailureEndpointPayload) {
        return failure(body, owner,
                       "failure propagation does not preserve its exact "
                       "fixed record",
                       block.id);
      }
      ++failureEndpointCounts[block.terminator.failureRecord - 1];
    }
    if (block.terminator.kind == MirTerminatorKind::ContainFailure) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      if (record == nullptr || body.kind != MirBodyKind::HostedStartup ||
          block.activeFailure == 0 ||
          block.activeFailure != block.terminator.failureRecord ||
          !emptyFailureEndpointPayload) {
        return failure(body, owner,
                       "hosted containment does not consume its exact active "
                       "failure",
                       block.id);
      }
      ++failureEndpointCounts[block.terminator.failureRecord - 1];
    }
    if (block.terminator.kind == MirTerminatorKind::TerminateCleanupFailure) {
      const MirFailureRecord *secondary =
          body.findFailureRecord(block.terminator.failureRecord);
      const MirBlock *producer = secondary == nullptr
                                     ? nullptr
                                     : body.findBlock(secondary->producerBlock);
      if (secondary == nullptr || block.activeFailure == 0 ||
          block.failureParameter == 0 ||
          block.failureParameter != block.terminator.failureRecord ||
          block.activeFailure == block.terminator.failureRecord ||
          producer == nullptr ||
          producer->activeFailure != block.activeFailure ||
          !emptyFailureEndpointPayload) {
        return failure(body, owner,
                       "cleanup-failure termination does not preserve its "
                       "primary and first secondary records",
                       block.id);
      }
      ++failureEndpointCounts[block.terminator.failureRecord - 1];
    }
    if ((block.terminator.returnLoan &&
         block.terminator.kind != MirTerminatorKind::Return) ||
        (block.terminator.returnLoan &&
         !validLoan(*block.terminator.returnLoan))) {
      return failure(body, owner, "return dependency loan is invalid",
                     block.id);
    }
    if (block.terminator.kind == MirTerminatorKind::Return) {
      if (body.returnType == SemanticType::Void &&
          (block.terminator.value || block.terminator.returnLoan)) {
        return failure(body, owner, "void return carries a runtime result",
                       block.id);
      }
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
    const auto countEdgeUses = [](const std::vector<MirOperand> &arguments) {
      return static_cast<std::size_t>(std::count_if(
          arguments.begin(), arguments.end(), [](const MirOperand &operand) {
            return operand.kind == MirOperandKind::Value;
          }));
    };
    expectedUseCount += countEdgeUses(block.terminator.targetArguments);
    expectedUseCount += countEdgeUses(block.terminator.elseArguments);
    for (const MirSwitchTarget &target : block.terminator.switchTargets) {
      expectedUseCount += countEdgeUses(target.arguments);
    }
  }
  std::vector<std::size_t> failurePredecessorCounts(body.failureRecords.size(),
                                                    0);
  for (const MirBlock &block : body.blocks) {
    for (const MirBlockId successor : successors(block.terminator)) {
      const MirBlock &target = body.blocks[successor - 1];
      const MirFailureRecordId parameter = target.failureParameter;
      const bool invokeFailure =
          block.terminator.kind == MirTerminatorKind::Invoke &&
          block.terminator.elseTarget == successor;
      const MirFailureRecordId expectedActive =
          invokeFailure
              ? (block.activeFailure == 0 ? block.terminator.failureRecord
                                          : block.activeFailure)
              : block.activeFailure;
      if (target.activeFailure != expectedActive ||
          (!invokeFailure && parameter != 0)) {
        return failure(body, owner,
                       "control-flow edge does not preserve exact active "
                       "failure state",
                       successor);
      }
      if (parameter == 0) {
        continue;
      }
      const bool exactFailureEdge =
          invokeFailure && block.terminator.failureRecord == parameter;
      if (!exactFailureEdge) {
        return failure(body, owner,
                       "failure-record parameter has a non-failure "
                       "predecessor",
                       successor);
      }
      ++failurePredecessorCounts[parameter - 1];
    }
  }
  for (const MirFailureRecord &record : body.failureRecords) {
    const MirBlock *parameter = body.findBlock(record.parameterBlock);
    if (parameter == nullptr) {
      continue;
    }
    if (parameter->activeFailure != record.id) {
      if (parameter->failureParameter != record.id ||
          !parameter->instructions.empty() ||
          parameter->terminator.kind !=
              MirTerminatorKind::TerminateCleanupFailure ||
          parameter->terminator.failureRecord != record.id) {
        return failure(body, owner,
                       "cleanup secondary does not terminate immediately",
                       parameter->id);
      }
      continue;
    }

    const MirTerminatorKind expectedEndpoint =
        body.kind == MirBodyKind::HostedStartup
            ? MirTerminatorKind::ContainFailure
            : MirTerminatorKind::PropagateFailure;
    const MirBlock *cursor = parameter;
    std::unordered_set<MirBlockId> primaryChain;
    std::size_t previousConstructionOrder =
        std::numeric_limits<std::size_t>::max();
    while (true) {
      if (cursor == nullptr || cursor->activeFailure != record.id ||
          !primaryChain.insert(cursor->id).second ||
          (cursor->failureParameter != 0 &&
           cursor->failureParameter != record.id)) {
        return failure(body, owner,
                       "primary failure cleanup is cyclic or changes its "
                       "active record",
                       cursor == nullptr ? 0 : cursor->id);
      }
      for (const MirInstruction &instruction : cursor->instructions) {
        if (instruction.kind != MirInstructionKind::Drop ||
            instruction.lifecycle.size() != 1 ||
            !instruction.lifecycle.front().failureCleanup) {
          continue;
        }
        const MirDropObligation *obligation =
            body.findDropObligation(instruction.lifecycle.front().source);
        if (obligation == nullptr ||
            obligation->constructionOrder >= previousConstructionOrder) {
          return failure(body, owner,
                         "primary failure cleanup is not globally in reverse "
                         "construction order",
                         cursor->id, instruction.id);
        }
        previousConstructionOrder = obligation->constructionOrder;
      }

      if (cursor->terminator.kind == expectedEndpoint) {
        if (cursor->terminator.failureRecord != record.id) {
          return failure(body, owner,
                         "primary failure cleanup reaches the wrong terminal "
                         "record",
                         cursor->id);
        }
        break;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Goto) {
        cursor = body.findBlock(cursor->terminator.target);
        continue;
      }
      if (cursor->terminator.kind == MirTerminatorKind::Invoke) {
        const MirBlock *normal = body.findBlock(cursor->terminator.target);
        const MirBlock *secondary =
            body.findBlock(cursor->terminator.elseTarget);
        if (secondary == nullptr || secondary->activeFailure != record.id ||
            secondary->failureParameter != cursor->terminator.failureRecord ||
            secondary->failureParameter == record.id ||
            !secondary->instructions.empty() ||
            secondary->terminator.kind !=
                MirTerminatorKind::TerminateCleanupFailure ||
            secondary->terminator.failureRecord !=
                secondary->failureParameter) {
          return failure(body, owner,
                         "fallible failure cleanup does not route its first "
                         "secondary directly to emergency termination",
                         cursor->id, cursor->terminator.invokeInstruction);
        }
        cursor = normal;
        continue;
      }
      return failure(body, owner,
                     "primary failure cleanup bypasses its exact terminal "
                     "endpoint",
                     cursor->id);
    }

    for (const MirBlock &candidate : body.blocks) {
      const bool primaryState = candidate.activeFailure == record.id &&
                                (candidate.failureParameter == 0 ||
                                 candidate.failureParameter == record.id);
      if (primaryState && !primaryChain.contains(candidate.id)) {
        return failure(body, owner,
                       "active primary failure block is disconnected from "
                       "its parameter-to-endpoint chain",
                       candidate.id);
      }
    }
  }
  for (std::size_t index = 0; index < body.failureRecords.size(); ++index) {
    if (failureParameterCounts[index] != 1 || failureInvokeCounts[index] != 1 ||
        failurePredecessorCounts[index] != 1 ||
        failureEndpointCounts[index] != 1) {
      return failure(body, owner,
                     "failure record must have one invoke, predecessor, "
                     "parameter, and terminal endpoint");
    }
  }
  if (std::any_of(primaryFullExpressionMarkers.begin(),
                  primaryFullExpressionMarkers.end(),
                  [](std::size_t count) { return count != 1; }) ||
      std::any_of(fullExpressionMarkers.begin(), fullExpressionMarkers.end(),
                  [](std::size_t count) { return count == 0; })) {
    return failure(body, owner,
                   "full-expression must have exactly one primary boundary "
                   "marker");
  }
  if (std::any_of(cleanupBoundaryMarkers.begin(), cleanupBoundaryMarkers.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "cleanup sequence must have exactly one boundary marker");
  }
  std::unordered_map<MirInstructionId, std::size_t> bindingDropCoverage;
  const auto temporaryDependencyEnd = [&](const MirInstruction &instruction) {
    if (instruction.kind != MirInstructionKind::EndBorrow || !instruction.loan)
      return false;
    return std::any_of(
        body.dropObligations.begin(), body.dropObligations.end(),
        [&](const MirDropObligation &drop) {
          const auto *place = body.findPlace(drop.place);
          const auto carrier =
              place == nullptr
                  ? MirTemporaryCarrierDependency{}
                  : findMirTemporaryCarrierDependency(body, *place);
          return carrier && mirLoanRetainsDependency(body, carrier.loan->id,
                                                     *instruction.loan);
        });
  };
  const auto precedingDropSequence = [&](const MirBlock &markerBlock,
                                         std::size_t markerIndex,
                                         std::size_t dropCount)
      -> std::optional<std::vector<const MirInstruction *>> {
    std::vector<const MirInstruction *> reversed;
    reversed.reserve(dropCount);
    const MirBlock *cursor = &markerBlock;
    std::size_t cursorIndex = markerIndex;
    std::unordered_set<MirBlockId> visited;
    while (reversed.size() < dropCount) {
      if (!visited.insert(cursor->id).second) {
        return std::nullopt;
      }
      while (cursorIndex != 0 && reversed.size() < dropCount) {
        const MirInstruction &candidate = cursor->instructions[cursorIndex - 1];
        if (candidate.kind == MirInstructionKind::EndBorrow) {
          const MirLoan *loan = body.findLoan(candidate.loan.value_or(0));
          // Lifecycle dataflow independently proves this exact carrier's
          // obligation inactive before its dependency can end. Ancestor loan
          // ends may follow that retirement within the same drain; ordinary
          // loan dataflow independently requires child-before-parent order.
          if (loan != nullptr &&
              (loan->lifecycleBound || temporaryDependencyEnd(candidate) ||
               std::any_of(body.loans.begin(), body.loans.end(),
                           [&](const MirLoan &carrier) {
                             return carrier.lifecycleBound &&
                                    mirLoanRetainsDependency(body, carrier.id,
                                                             loan->id);
                           }))) {
            --cursorIndex;
            continue;
          }
        }
        if (candidate.kind != MirInstructionKind::Drop) {
          break;
        }
        reversed.push_back(&candidate);
        --cursorIndex;
      }
      if (reversed.size() == dropCount) {
        break;
      }
      if (cursorIndex != 0) {
        return std::nullopt;
      }
      const MirBlock *predecessor = nullptr;
      std::size_t predecessorCount = 0;
      for (const MirBlock &candidate : body.blocks) {
        const std::vector<MirBlockId> outgoing =
            successors(candidate.terminator);
        if (std::find(outgoing.begin(), outgoing.end(), cursor->id) ==
            outgoing.end()) {
          continue;
        }
        ++predecessorCount;
        if (candidate.terminator.kind == MirTerminatorKind::Invoke &&
            candidate.terminator.target == cursor->id &&
            candidate.activeFailure == cursor->activeFailure) {
          predecessor = &candidate;
        }
      }
      if (predecessorCount != 1 || predecessor == nullptr) {
        return std::nullopt;
      }
      cursor = predecessor;
      cursorIndex = cursor->instructions.size();
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
  };
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.cleanupBoundaryEnd == 0) {
        continue;
      }
      const MirCleanupBoundary &boundary =
          body.cleanupBoundaries[marker.cleanupBoundaryEnd - 1];
      const auto drops = precedingDropSequence(block, markerIndex,
                                               boundary.obligations.size());
      if (!drops || drops->size() != boundary.obligations.size()) {
        return failure(body, owner,
                       "cleanup sequence is not connected to its exact "
                       "boundary marker",
                       block.id, marker.id);
      }
      for (std::size_t dropIndex = 0; dropIndex < boundary.obligations.size();
           ++dropIndex) {
        const MirInstruction &drop = *(*drops)[dropIndex];
        if (drop.lifecycle.size() != 1 ||
            drop.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop.lifecycle.front().source != boundary.obligations[dropIndex] ||
            drop.lifecycle.front().failureCleanup !=
                (boundary.kind == MirCleanupBoundaryKind::Failure)) {
          return failure(body, owner,
                         "cleanup sequence is not the exact reverse "
                         "construction order",
                         block.id, drop.id);
        }
        ++bindingDropCoverage[drop.id];
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1) {
        continue;
      }
      const MirDropObligation *obligation =
          body.findDropObligation(instruction.lifecycle.front().source);
      const bool failureCleanup = instruction.lifecycle.front().failureCleanup;
      const bool requiresBoundary =
          failureCleanup ||
          (obligation != nullptr &&
           obligation->kind == MirDropObligationKind::Binding);
      if (requiresBoundary && bindingDropCoverage[instruction.id] != 1) {
        return failure(body, owner,
                       failureCleanup
                           ? "failure drop is not covered by exactly one "
                             "failure cleanup boundary"
                           : "binding drop is not covered by exactly one "
                             "lexical cleanup boundary",
                       block.id, instruction.id);
      }
    }
  }

  if (std::any_of(definitionCounts.begin(), definitionCounts.end(),
                  [](std::size_t count) { return count != 1; })) {
    return failure(body, owner,
                   "every value must have exactly one instruction or block "
                   "parameter definition");
  }

  const std::vector<bool> expectedReachability = reachableBlocks(body);
  for (std::size_t index = 0; index < body.blocks.size(); ++index) {
    if (body.blocks[index].reachable != expectedReachability[index]) {
      return failure(body, owner, "block reachability index is stale",
                     body.blocks[index].id);
    }
  }
  for (const MirBlock &block : body.blocks) {
    const MirTerminatorProvenance &provenance = block.terminator.provenance;
    if (provenance.kind == MirTerminatorProvenanceKind::None) {
      if (provenance.foldSourceValue != 0) {
        return failure(body, owner,
                       "terminator without rewrite provenance carries a fold "
                       "source",
                       block.id);
      }
      continue;
    }
    if (provenance.kind != MirTerminatorProvenanceKind::BranchFold ||
        block.terminator.kind != MirTerminatorKind::Goto ||
        block.terminator.target == 0 || provenance.foldSourceValue == 0) {
      return failure(body, owner,
                     "terminator rewrite provenance is outside the "
                     "branch-fold contract",
                     block.id);
    }
    const MirValue *condition = body.findValue(provenance.foldSourceValue);
    bool literalCondition = false;
    if (condition != nullptr) {
      for (const MirBlock &candidate : body.blocks) {
        for (const MirInstruction &instruction : candidate.instructions) {
          if (instruction.result &&
              *instruction.result == provenance.foldSourceValue &&
              instruction.kind == MirInstructionKind::Compute &&
              instruction.operation == MirOperation::Literal &&
              instruction.literal &&
              std::holds_alternative<bool>(*instruction.literal)) {
            literalCondition = true;
          }
        }
      }
    }
    if (!literalCondition) {
      return failure(body, owner,
                     "branch fold does not retain its dominating literal "
                     "condition",
                     block.id);
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
      case MirValueUseKind::EdgeArgument: {
        const MirOperand *argument = nullptr;
        if (block != nullptr && use.edgeIndex == 0 &&
            block->terminator.target == use.edgeTarget &&
            use.operandIndex < block->terminator.targetArguments.size()) {
          argument = &block->terminator.targetArguments[use.operandIndex];
        } else if (block != nullptr && use.edgeIndex == 1 &&
                   block->terminator.elseTarget == use.edgeTarget &&
                   use.operandIndex < block->terminator.elseArguments.size()) {
          argument = &block->terminator.elseArguments[use.operandIndex];
        } else if (block != nullptr && use.edgeIndex >= 2 &&
                   use.edgeIndex - 2 < block->terminator.switchTargets.size()) {
          const MirSwitchTarget &target =
              block->terminator.switchTargets[use.edgeIndex - 2];
          if (target.target == use.edgeTarget &&
              use.operandIndex < target.arguments.size()) {
            argument = &target.arguments[use.operandIndex];
          }
        }
        if (argument == nullptr || argument->kind != MirOperandKind::Value ||
            argument->value != use.value) {
          return failure(body, owner,
                         "indexed edge argument use does not match MIR");
        }
        break;
      }
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

  const std::optional<MirDominanceInfo> dominance = computeMirDominance(body);
  if (!dominance) {
    return failure(body, owner, "MIR dominance analysis rejected the body CFG");
  }
  const auto strictlyPrecedes = [&](const MirInstruction &before,
                                    const MirInstruction &after) {
    const auto beforeBlock = instructionBlocks.find(before.id);
    const auto afterBlock = instructionBlocks.find(after.id);
    const auto beforeOrder = instructionOrders.find(before.id);
    const auto afterOrder = instructionOrders.find(after.id);
    if (beforeBlock == instructionBlocks.end() ||
        afterBlock == instructionBlocks.end() ||
        beforeOrder == instructionOrders.end() ||
        afterOrder == instructionOrders.end()) {
      return false;
    }
    if (beforeBlock->second == afterBlock->second) {
      return beforeOrder->second < afterOrder->second;
    }
    return dominance->dominates(beforeBlock->second, afterBlock->second);
  };
  const auto definingInstruction = [&](MirValueId valueId) {
    const MirValue *value = body.findValue(valueId);
    if (value == nullptr) {
      return static_cast<const MirInstruction *>(nullptr);
    }
    const auto definition = instructionsById.find(value->definition);
    return definition == instructionsById.end() ? nullptr : definition->second;
  };
  for (const MirBlock &block : body.blocks) {
    const MirTerminatorProvenance &provenance = block.terminator.provenance;
    if (provenance.kind != MirTerminatorProvenanceKind::BranchFold) {
      continue;
    }
    const MirInstruction *source =
        definingInstruction(provenance.foldSourceValue);
    const auto sourceBlock = source == nullptr
                                 ? instructionBlocks.end()
                                 : instructionBlocks.find(source->id);
    if (source == nullptr || sourceBlock == instructionBlocks.end() ||
        (sourceBlock->second != block.id &&
         !dominance->dominates(sourceBlock->second, block.id))) {
      return failure(body, owner,
                     "branch fold condition does not dominate the folded "
                     "terminator",
                     block.id);
    }
  }
  const auto exactComputeFoldRewrite = [&](const MirInstruction &instruction) {
    const MirLiteralProvenance &provenance = instruction.literalProvenance;
    if (!instruction.literal || !instruction.result ||
        provenance.sourceValue != 0 || provenance.sourceValues.empty() ||
        provenance.sourceValues.size() > 2) {
      return false;
    }
    std::vector<MirComputeFoldOperand> operands;
    operands.reserve(provenance.sourceValues.size());
    for (const MirValueId sourceValue : provenance.sourceValues) {
      const MirValue *value = body.findValue(sourceValue);
      const MirInstruction *source = definingInstruction(sourceValue);
      if (value == nullptr || source == nullptr ||
          sourceValue == *instruction.result ||
          !strictlyPrecedes(*source, instruction) ||
          source->kind != MirInstructionKind::Compute ||
          source->operation != MirOperation::Literal || !source->literal) {
        return false;
      }
      operands.push_back(
          {.literal = *source->literal, .type = value->info.type});
    }
    const std::optional<Literal> folded = evaluateMirComputeFold(
        provenance.sourceOperation, operands, instruction.info.type);
    return folded && *folded == *instruction.literal;
  };
  const auto exactLiteralRewrite = [&](const MirInstruction &instruction) {
    if (!instruction.literal || !instruction.result ||
        instruction.literalProvenance.kind !=
            MirLiteralProvenanceKind::IdentityFold) {
      return false;
    }
    const SemanticType &type = instruction.info.type;
    MirValueId sourceValue = instruction.literalProvenance.sourceValue;
    const MirInstruction *source = definingInstruction(sourceValue);
    if (source == nullptr || !strictlyPrecedes(*source, instruction)) {
      return false;
    }

    std::unordered_set<MirValueId> visited;
    while (sourceValue != 0 && visited.size() <= body.values.size() &&
           visited.insert(sourceValue).second) {
      const MirValue *value = body.findValue(sourceValue);
      source = definingInstruction(sourceValue);
      if (value == nullptr || source == nullptr || value->info.type != type ||
          source->info.type != type ||
          source->kind != MirInstructionKind::Compute) {
        return false;
      }
      if (source->operation == MirOperation::Literal) {
        return source->literal && *source->literal == *instruction.literal;
      }
      if (source->operation != MirOperation::Identity ||
          source->operands.size() != 1 ||
          source->operands.front().kind != MirOperandKind::Value ||
          source->operands.front().type != type) {
        return false;
      }
      sourceValue = source->operands.front().value;
    }
    return false;
  };
  std::unordered_map<HirValueId, std::size_t>
      programConstantMaterializationCounts;
  std::unordered_map<HirValueId, std::unordered_set<MirValueId>>
      programConstantScheduleValues;
  std::unordered_set<MirInstructionId> programConstantMagnitudeInstructions;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Load &&
          expectedProgramConstants.contains(instruction.hirValue)) {
        return failure(body, owner,
                       "program-constant substitution was lowered as a "
                       "storage load",
                       block.id, instruction.id);
      }
      if (instruction.programConstantSubstitution) {
        const auto expected =
            expectedProgramConstants.find(instruction.hirValue);
        const MirValue *resultValue =
            instruction.result ? body.findValue(*instruction.result) : nullptr;
        if (expected == expectedProgramConstants.end() ||
            ++programConstantMaterializationCounts[instruction.hirValue] != 1 ||
            resultValue == nullptr ||
            resultValue->sourceValue != instruction.hirValue ||
            resultValue->info != instruction.info ||
            instruction.info.category != ValueCategory::Value ||
            instruction.info.access != AccessMode::ReadOnly ||
            !programConstantMatchesType(*expected->second,
                                        instruction.info.type)) {
          return failure(body, owner,
                         "program-constant substitution materialization "
                         "does not match its exact MIR inventory",
                         block.id, instruction.id);
        }
        programConstantScheduleValues[instruction.hirValue].insert(
            *instruction.result);
        const ConstantValue &constant = *expected->second;
        const auto *integer = std::get_if<ConstantInteger>(&constant);
        const auto *enumerator = std::get_if<ConstantEnum>(&constant);
        const std::optional<Literal> literal = programConstantLiteral(constant);
        bool exact = false;
        if (instruction.info.type.kind == SemanticType::Enum) {
          const ConstantInteger *enumValue =
              enumerator == nullptr ? integer : &enumerator->value;
          exact = enumValue != nullptr &&
                  (enumerator == nullptr ||
                   enumerator->enumId == instruction.info.type.enumId) &&
                  instruction.operation == MirOperation::EnumConstant &&
                  instruction.operands.empty() && !instruction.literal &&
                  instruction.enumOwner == instruction.info.type.enumId &&
                  instruction.enumValue ==
                      std::optional<EnumConstant>{
                          EnumConstant{.negative = enumValue->negative,
                                       .magnitude = enumValue->magnitude}};
        } else if (integer != nullptr && integer->negative) {
          const MirValueId magnitudeValue =
              instruction.operands.size() == 1 &&
                      instruction.operands.front().kind == MirOperandKind::Value
                  ? instruction.operands.front().value
                  : 0;
          const MirInstruction *magnitude =
              magnitudeValue == 0 ? nullptr
                                  : definingInstruction(magnitudeValue);
          const std::vector<MirValueUse> &magnitudeUses =
              body.usesOf(magnitudeValue);
          exact = instruction.operation == MirOperation::Negate &&
                  instruction.operands.size() == 1 && magnitude != nullptr &&
                  magnitude->result == magnitudeValue &&
                  strictlyPrecedes(*magnitude, instruction) &&
                  magnitude->kind == MirInstructionKind::Compute &&
                  magnitude->hirValue == instruction.hirValue &&
                  magnitude->operation == MirOperation::Literal &&
                  magnitude->literal == literal &&
                  magnitude->literalProvenance.kind ==
                      MirLiteralProvenanceKind::Source &&
                  !magnitude->programConstantSubstitution &&
                  magnitude->info.type == instruction.info.type &&
                  magnitudeUses.size() == 1 &&
                  magnitudeUses.front().kind ==
                      MirValueUseKind::InstructionOperand &&
                  magnitudeUses.front().instruction == instruction.id &&
                  magnitudeUses.front().operandIndex == 0;
          if (exact) {
            programConstantScheduleValues[instruction.hirValue].insert(
                magnitudeValue);
            programConstantMagnitudeInstructions.insert(magnitude->id);
          }
        } else {
          exact = literal && instruction.operation == MirOperation::Literal &&
                  instruction.operands.empty() &&
                  instruction.literal == literal &&
                  instruction.literalProvenance.kind ==
                      MirLiteralProvenanceKind::Source;
        }
        if (!exact) {
          return failure(body, owner,
                         "program-constant substitution has the wrong exact "
                         "constant schedule",
                         block.id, instruction.id);
        }
      }
      if (instruction.literalProvenance.kind ==
              MirLiteralProvenanceKind::IdentityFold &&
          !exactLiteralRewrite(instruction)) {
        return failure(body, owner,
                       "literal rewrite does not retain an exact dominating "
                       "MIR identity-fold proof",
                       block.id, instruction.id);
      }
      if (instruction.literalProvenance.kind ==
              MirLiteralProvenanceKind::ComputeFold &&
          !exactComputeFoldRewrite(instruction)) {
        return failure(body, owner,
                       "literal rewrite does not retain an exact dominating "
                       "MIR compute-fold proof",
                       block.id, instruction.id);
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind == MirInstructionKind::Compute &&
          expectedProgramConstants.contains(instruction.hirValue) &&
          !instruction.programConstantSubstitution &&
          !programConstantMagnitudeInstructions.contains(instruction.id)) {
        return failure(body, owner,
                       "program-constant substitution retains an extra "
                       "unmarked compute",
                       block.id, instruction.id);
      }
    }
  }
  // An ordered call stages its inputs, so a substituted constant used as a
  // receiver or argument keeps its exact HIR identity on the staged value as
  // well as on its one materialization. Admit only a stage that forwards an
  // already scheduled value of the same constant.
  for (bool changed = true; changed;) {
    changed = false;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind != MirInstructionKind::CallInput ||
            instruction.programConstantSubstitution || !instruction.result ||
            instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value) {
          continue;
        }
        const auto schedule =
            programConstantScheduleValues.find(instruction.hirValue);
        if (schedule == programConstantScheduleValues.end() ||
            !schedule->second.contains(instruction.operands.front().value)) {
          continue;
        }
        const MirValue *staged = body.findValue(*instruction.result);
        if (staged == nullptr || staged->sourceValue != instruction.hirValue) {
          continue;
        }
        changed =
            schedule->second.insert(*instruction.result).second || changed;
      }
    }
  }
  for (const MirValue &value : body.values) {
    const auto expected = expectedProgramConstants.find(value.sourceValue);
    if (expected == expectedProgramConstants.end()) {
      continue;
    }
    const auto schedule = programConstantScheduleValues.find(value.sourceValue);
    if (schedule == programConstantScheduleValues.end() ||
        !schedule->second.contains(value.id)) {
      return failure(body, owner,
                     "program-constant substitution retains an extra MIR "
                     "value schedule");
    }
  }
  for (const auto &[hirValue, _] : expectedProgramConstants) {
    if (programConstantMaterializationCounts[hirValue] != 1) {
      return failure(body, owner,
                     "program-constant substitution has no exact MIR "
                     "materialization");
    }
  }
  const auto isFullExpressionRoot = [&](const MirInstruction &instruction) {
    return std::any_of(
        body.fullExpressions.begin(), body.fullExpressions.end(),
        [&](const MirFullExpression &expression) {
          return std::find(expression.roots.begin(), expression.roots.end(),
                           instruction.hirValue) != expression.roots.end();
        });
  };
  std::unordered_map<HirValueId, std::size_t> orderedInvocationCounts;
  for (const auto &block : body.blocks) {
    for (const auto &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::StorageConstructPreflight &&
          instruction.storagePreflight == 0)
        continue;
      const auto id =
          instruction.kind == MirInstructionKind::StorageConstructPreflight
              ? instruction.id
              : instruction.storagePreflight;
      const auto *append = mirStorageConstructForPreflight(body, id);
      const auto check = instructionsById.find(id);
      if (append == nullptr || check == instructionsById.end() ||
          (instruction.storagePreflight != 0 && append != &instruction))
        return failure(
            body, owner,
            "prefix construction has no exact preflight success pair", block.id,
            instruction.id);
      for (std::size_t index = 0; index < append->operands.size(); ++index) {
        const auto &operand = append->operands[index];
        const auto *value = operand.kind == MirOperandKind::Value
                                ? body.findValue(operand.value)
                                : nullptr;
        const auto stage = value == nullptr
                               ? instructionsById.end()
                               : instructionsById.find(value->definition);
        if (stage == instructionsById.end() ||
            stage->second->kind != MirInstructionKind::CallInput ||
            stage->second->callSite != append->callSite ||
            stage->second->callInputRole != MirCallInputRole::Argument ||
            stage->second->callInputIndex != index ||
            !strictlyPrecedes(*stage->second, *check->second) ||
            (index == 0 &&
             (stage->second->callInputKind != HirCallInputKind::MutableBorrow ||
              stage->second->preparedParameterDrop ||
              !stage->second->lifecycle.empty())))
          return failure(body, owner,
                         "prefix preflight must precede the exact "
                         "prepared-parameter handoff",
                         block.id, instruction.id);
      }
    }
  }
  std::vector<std::size_t> preparedParameterCounts(body.dropObligations.size(),
                                                   0);
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if ((instruction.kind == MirInstructionKind::Call ||
           instruction.kind == MirInstructionKind::Construct) &&
          instruction.callSite != 0 &&
          ++orderedInvocationCounts[instruction.callSite] != 1) {
        return failure(body, owner,
                       "ordered invocation site is executed more than once",
                       block.id, instruction.id);
      }
      if (instruction.kind != MirInstructionKind::CallInput) {
        continue;
      }
      if (instruction.preparedParameterDrop) {
        ++preparedParameterCounts[*instruction.preparedParameterDrop - 1];
      }
      std::vector<MirValueUse> uses = body.usesOf(*instruction.result);
      // Prefix storage has one non-consuming preflight use in addition to
      // the exact constructor invocation.
      // The pair and all staged inputs were independently validated above.
      if (instruction.callInputRole == MirCallInputRole::Argument &&
          instruction.callInputIndex == 0 &&
          instruction.callInputKind == HirCallInputKind::MutableBorrow) {
        std::erase_if(uses, [&](const MirValueUse &use) {
          const auto *append =
              mirStorageConstructForPreflight(body, use.instruction);
          return append != nullptr &&
                 append->callSite == instruction.callSite &&
                 use.kind == MirValueUseKind::InstructionOperand &&
                 use.operandIndex == instruction.callInputIndex &&
                 use.operandIndex == 0 &&
                 append->operands[use.operandIndex].value ==
                     *instruction.result;
        });
      }
      if (uses.empty() && mirCallInputAbandonedByTerminal(body, instruction)) {
        continue;
      }
      if (uses.size() != 1) {
        return failure(body, owner,
                       "call input must have exactly one executable use or "
                       "be abandoned by a terminal argument",
                       block.id, instruction.id);
      }
      const MirValueUse &use = uses.front();
      const auto callEntry = instructionsById.find(use.instruction);
      const MirInstruction *invocation =
          callEntry == instructionsById.end() ? nullptr : callEntry->second;
      const bool receiverUse =
          *instruction.callInputRole == MirCallInputRole::Receiver &&
          use.kind == MirValueUseKind::InstructionReceiver;
      const bool argumentUse =
          *instruction.callInputRole == MirCallInputRole::Argument &&
          use.kind == MirValueUseKind::InstructionOperand &&
          use.operandIndex == instruction.callInputIndex;
      const bool callLike = invocation != nullptr &&
                            (invocation->kind == MirInstructionKind::Call ||
                             invocation->kind == MirInstructionKind::Construct);
      if (!callLike || invocation->callSite != instruction.callSite ||
          (invocation->kind == MirInstructionKind::Construct && receiverUse) ||
          (!receiverUse && !argumentUse)) {
        return failure(body, owner,
                       "call input use does not match its ordered invocation",
                       block.id, instruction.id);
      }
    }
  }
  for (const MirDropObligation &obligation : body.dropObligations) {
    const std::size_t references = preparedParameterCounts[obligation.id - 1];
    if ((obligation.kind == MirDropObligationKind::PreparedParameter) !=
        (references == 1)) {
      return failure(body, owner,
                     "prepared-parameter obligation must belong to exactly one "
                     "call input");
    }
  }
  const auto callInputFor =
      [&](const MirOperand &operand) -> const MirInstruction * {
    if (operand.kind != MirOperandKind::Value) {
      return nullptr;
    }
    const MirValue *value = body.findValue(operand.value);
    if (value == nullptr) {
      return nullptr;
    }
    const auto definition = instructionsById.find(value->definition);
    return definition == instructionsById.end() ||
                   definition->second->kind != MirInstructionKind::CallInput
               ? nullptr
               : definition->second;
  };
  const auto exactReceiverInputKind = [&](const MirInstruction &invocation,
                                          const MirInstruction &receiver) {
    if (!invocation.callableInvocation) {
      return receiver.callInputKind == HirCallInputKind::Value ||
             receiver.callInputKind == HirCallInputKind::ReadBorrow ||
             receiver.callInputKind == HirCallInputKind::MutableBorrow ||
             receiver.callInputKind == HirCallInputKind::ReadTemporaryBorrow ||
             receiver.callInputKind == HirCallInputKind::MutableTemporaryBorrow;
    }
    const bool exactMovedReceiver =
        ((receiver.callInputKind == HirCallInputKind::MutableTemporaryBorrow ||
          receiver.callInputKind == HirCallInputKind::RetainedReceiver ||
          receiver.callInputKind == HirCallInputKind::MoveValue) &&
         invocation.receiver &&
         consumedCallableReceiver(body, *invocation.receiver,
                                  MirValueUseKind::InstructionReceiver,
                                  invocation.id));
    switch (*invocation.callableInvocation) {
    case CallableInvocationCapability::Read:
      return receiver.callInputKind == HirCallInputKind::Value ||
             receiver.callInputKind == HirCallInputKind::ReadBorrow ||
             receiver.callInputKind == HirCallInputKind::ReadTemporaryBorrow ||
             exactMovedReceiver;
    case CallableInvocationCapability::Mutable:
      return receiver.callInputKind == HirCallInputKind::Value ||
             receiver.callInputKind == HirCallInputKind::MutableBorrow ||
             receiver.callInputKind ==
                 HirCallInputKind::MutableTemporaryBorrow ||
             exactMovedReceiver;
    case CallableInvocationCapability::Once:
      return exactMovedReceiver;
    }
    return false;
  };
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &invocation : block.instructions) {
      if ((invocation.kind != MirInstructionKind::Call &&
           invocation.kind != MirInstructionKind::Construct) ||
          invocation.callSite == 0) {
        continue;
      }
      std::vector<const MirInstruction *> inputs;
      if (invocation.receiver) {
        const MirInstruction *receiver = callInputFor(*invocation.receiver);
        if (receiver == nullptr || receiver->callSite != invocation.callSite ||
            *receiver->callInputRole != MirCallInputRole::Receiver ||
            !exactReceiverInputKind(invocation, *receiver) ||
            receiver->info.type != invocation.receiver->type) {
          return failure(body, owner,
                         "ordered call receiver is not prepared by its exact "
                         "receiver input",
                         block.id, invocation.id);
        }
        inputs.push_back(receiver);
      }
      bool defaultArgumentSuffix = false;
      for (std::size_t index = 0; index < invocation.operands.size(); ++index) {
        const MirInstruction *argument =
            callInputFor(invocation.operands[index]);
        const SemanticType &parameter = invocation.parameterTypes[index];
        const bool mutableStorageInput =
            argument != nullptr && index == 0 &&
            argument->callInputKind == HirCallInputKind::MutableBorrow &&
            (parameter.kind == SemanticType::PrefixStorage ||
             parameter.kind == SemanticType::Class) &&
            (invocation.intrinsic == IntrinsicKind::PrefixStorageAppend ||
             invocation.intrinsic == IntrinsicKind::PrefixStorageInsert ||
             invocation.intrinsic == IntrinsicKind::OptionalStorageConstruct);
        const bool temporaryReadArgument =
            argument != nullptr &&
            invocation.kind == MirInstructionKind::Call &&
            parameter.kind == SemanticType::Reference &&
            parameter.referenceAccess == AccessMode::ReadOnly &&
            argument->callInputKind == HirCallInputKind::ReadTemporaryBorrow;
        const bool exactKind =
            argument != nullptr &&
            (mutableStorageInput || temporaryReadArgument ||
             ((parameter.kind == SemanticType::Class ||
               (parameter.kind == SemanticType::Lambda &&
                argument->callInputKind != HirCallInputKind::Value) ||
               (parameter.kind == SemanticType::Enum &&
                argument->info.traits.drop == DropKind::Lexical))
                  ? (argument->callInputKind == HirCallInputKind::CopyValue ||
                     argument->callInputKind == HirCallInputKind::MoveValue ||
                     (parameter.kind == SemanticType::Class &&
                      parameter == argument->info.type &&
                      parameter == argument->operands.front().type &&
                      argument->info.traits.containsBorrowedState &&
                      argument->callInputKind ==
                          HirCallInputKind::MoveBorrowedTemporary))
                  : argument->callInputKind ==
                        (parameter.kind != SemanticType::Reference
                             ? HirCallInputKind::Value
                             : (parameter.referenceAccess == AccessMode::Mutable
                                    ? HirCallInputKind::MutableBorrow
                                    : HirCallInputKind::ReadBorrow))));
        if (argument == nullptr || argument->callSite != invocation.callSite ||
            *argument->callInputRole != MirCallInputRole::Argument ||
            argument->callInputIndex != index || !exactKind ||
            argument->info.type != invocation.parameterTypes[index]) {
          return failure(body, owner,
                         "ordered invocation argument is not prepared by its "
                         "exact "
                         "indexed input",
                         block.id, invocation.id);
        }
        if (argument->defaultArgument) {
          defaultArgumentSuffix = true;
        } else if (defaultArgumentSuffix) {
          return failure(body, owner,
                         "ordered invocation default arguments must form a "
                         "trailing parameter suffix",
                         block.id, invocation.id);
        }
        inputs.push_back(argument);
      }
      const MirInstruction *previous = nullptr;
      std::unordered_set<MirDropObligationId> preparedDrops;
      const bool preparedConstructor =
          invocation.kind == MirInstructionKind::Construct &&
          invocation.constructorTarget &&
          invocation.constructorKind == ConstructorKind::Ordinary &&
          std::any_of(inputs.begin(), inputs.end(),
                      [](const MirInstruction *input) {
                        return input->preparedParameterDrop.has_value();
                      });
      for (const MirInstruction *input : inputs) {
        if (input->callInputKind == HirCallInputKind::ReadTemporaryBorrow ||
            input->callInputKind == HirCallInputKind::MutableTemporaryBorrow) {
          const MirPlace *place = body.findPlace(input->operands.front().place);
          const MirTemporaryMaterialization materialization =
              place == nullptr ? MirTemporaryMaterialization{}
                               : findMirTemporaryMaterialization(
                                     body, *place, input->hirValue);
          const MirInstruction *producer = materialization.producer;
          if (producer == nullptr || !strictlyPrecedes(*producer, *input)) {
            return failure(body, owner,
                           "temporary producer must strictly precede its "
                           "ordered borrow input",
                           block.id, invocation.id);
          }
        }
        if ((previous != nullptr && !strictlyPrecedes(*previous, *input)) ||
            !strictlyPrecedes(*input, invocation)) {
          return failure(body, owner,
                         "ordered invocation inputs must form a strict "
                         "receiver, argument, invocation chain",
                         block.id, invocation.id);
        }
        const bool owningInput =
            input->callInputKind == HirCallInputKind::CopyValue ||
            input->callInputKind == HirCallInputKind::MoveValue ||
            input->callInputKind == HirCallInputKind::MoveBorrowedTemporary;
        const MirDropObligation *receiverLifetime = nullptr;
        if (input->callInputRole == MirCallInputRole::Receiver &&
            input->callInputKind == HirCallInputKind::MoveValue &&
            !input->preparedParameterDrop) {
          for (const auto &drop : body.dropObligations) {
            if (drop.kind == MirDropObligationKind::Value &&
                drop.value == input->hirValue &&
                drop.dropType.type == input->info.type) {
              receiverLifetime = &drop;
            }
          }
        }
        if (receiverLifetime != nullptr) {
          const auto transfersReceiver = [&](const MirLifecycleEvent &event) {
            return event.source == receiverLifetime->id &&
                   event.kind != MirLifecycleEventKind::Drop;
          };
          for (const auto &candidate : body.blocks) {
            if (std::any_of(candidate.terminator.successLifecycle.begin(),
                            candidate.terminator.successLifecycle.end(),
                            transfersReceiver) ||
                std::any_of(candidate.instructions.begin(),
                            candidate.instructions.end(),
                            [&](const auto &item) {
                              return std::any_of(item.lifecycle.begin(),
                                                 item.lifecycle.end(),
                                                 transfersReceiver);
                            })) {
              return failure(body, owner,
                             "consumed receiver must retain its caller-owned "
                             "full-expression lifetime until cleanup",
                             block.id, invocation.id);
            }
          }
        }
        if ((invocation.kind == MirInstructionKind::Call ||
             preparedConstructor) &&
            owningInput && receiverLifetime == nullptr) {
          if (!input->preparedParameterDrop ||
              !preparedDrops.insert(*input->preparedParameterDrop).second) {
            return failure(
                body, owner,
                "staged invocation owning inputs require distinct prepared "
                "parameter obligations",
                block.id, invocation.id);
          }
          const std::size_t transfers = static_cast<std::size_t>(std::count_if(
              invocation.lifecycle.begin(), invocation.lifecycle.end(),
              [&](const MirLifecycleEvent &event) {
                return event.kind == MirLifecycleEventKind::TransferOut &&
                       event.source == *input->preparedParameterDrop;
              }));
          if (transfers != 1) {
            return failure(
                body, owner,
                "prepared owning parameter must transfer exactly once when "
                "the callee begins",
                block.id, invocation.id);
          }
        } else if (input->preparedParameterDrop) {
          return failure(body, owner,
                         "prepared parameter obligation is attached to an "
                         "unsupported invocation input",
                         block.id, invocation.id);
        }
        previous = input;
      }
      std::vector<MirLoanId> temporaryArgumentLoans;
      for (const MirInstruction *input : inputs) {
        if (input->callInputRole == MirCallInputRole::Argument &&
            input->callInputKind == HirCallInputKind::ReadTemporaryBorrow) {
          if (!input->loan) {
            return failure(body, owner,
                           "temporary argument input has no call-scoped loan",
                           block.id, invocation.id);
          }
          temporaryArgumentLoans.push_back(*input->loan);
        }
      }
      const auto exactEndPrefix = [&](const MirBlock *endingBlock,
                                      std::size_t begin) -> bool {
        if (endingBlock == nullptr || begin + temporaryArgumentLoans.size() >
                                          endingBlock->instructions.size()) {
          return false;
        }
        for (std::size_t offset = 0; offset < temporaryArgumentLoans.size();
             ++offset) {
          const MirInstruction &ending =
              endingBlock->instructions[begin + offset];
          const MirLoanId expected =
              temporaryArgumentLoans[temporaryArgumentLoans.size() - offset -
                                     1];
          if (ending.kind != MirInstructionKind::EndBorrow ||
              ending.loan != expected) {
            return false;
          }
        }
        return true;
      };
      if (!temporaryArgumentLoans.empty()) {
        const bool invoke =
            block.terminator.kind == MirTerminatorKind::Invoke &&
            block.terminator.invokeInstruction == invocation.id;
        if (invoke) {
          if (!exactEndPrefix(body.findBlock(block.terminator.target), 0) ||
              !exactEndPrefix(body.findBlock(block.terminator.elseTarget), 0)) {
            return failure(
                body, owner,
                "temporary argument loans must end in reverse order at the "
                "start of both invocation successors",
                block.id, invocation.id);
          }
        } else {
          const auto position =
              std::find_if(block.instructions.begin(), block.instructions.end(),
                           [&](const MirInstruction &candidate) {
                             return candidate.id == invocation.id;
                           });
          const std::size_t begin =
              position == block.instructions.end()
                  ? block.instructions.size()
                  : static_cast<std::size_t>(
                        std::distance(block.instructions.begin(), position)) +
                        1;
          if (!exactEndPrefix(&block, begin)) {
            return failure(body, owner,
                           "temporary argument loans must end in reverse "
                           "order immediately after the invocation",
                           block.id, invocation.id);
          }
        }
      }
      for (const MirLifecycleEvent &event : invocation.lifecycle) {
        if ((invocation.intrinsic == IntrinsicKind::ObjectConstruct ||
             (invocation.intrinsic == IntrinsicKind::PrefixStorageAppend &&
              invocation.storagePreflight != 0)) &&
            (event.kind != MirLifecycleEventKind::TransferOut ||
             !preparedDrops.contains(event.source))) {
          return failure(body, owner,
                         "prefix construction may transfer only its exact "
                         "prepared parameters",
                         block.id, invocation.id);
        }
        const MirDropObligation *source =
            event.kind == MirLifecycleEventKind::TransferOut
                ? body.findDropObligation(event.source)
                : nullptr;
        if (source != nullptr &&
            source->kind == MirDropObligationKind::PreparedParameter &&
            !preparedDrops.contains(source->id)) {
          return failure(body, owner,
                         "call transfers a prepared parameter owned by a "
                         "different invocation",
                         block.id, invocation.id);
        }
      }
    }
  }
  struct PreparedCallArgumentContext {
    std::vector<MirDropObligationId> priorPreparedDrops;
  };
  const auto preparedCallArgumentContext = [&](const MirInstruction &detector)
      -> std::optional<PreparedCallArgumentContext> {
    if (!detector.result || isFullExpressionRoot(detector)) {
      return std::nullopt;
    }
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &invocation : block.instructions) {
        if ((invocation.kind != MirInstructionKind::Call &&
             invocation.kind != MirInstructionKind::Construct) ||
            invocation.callSite == 0) {
          continue;
        }
        for (std::size_t index = 0; index < invocation.operands.size();
             ++index) {
          const MirInstruction *argument =
              callInputFor(invocation.operands[index]);
          if (argument == nullptr || argument->hirValue != detector.hirValue ||
              (argument->callInputKind != HirCallInputKind::Value &&
               !(argument->callInputKind == HirCallInputKind::MoveValue &&
                 detector.kind == MirInstructionKind::Construct &&
                 detector.info.type.kind == SemanticType::Class)) ||
              argument->operands.size() != 1 ||
              argument->operands.front().kind != MirOperandKind::Value ||
              argument->operands.front().value != *detector.result ||
              !strictlyPrecedes(detector, *argument)) {
            continue;
          }
          PreparedCallArgumentContext context;
          const auto addPriorStage = [&](const MirInstruction *input) {
            if (input != nullptr && input->preparedParameterDrop &&
                strictlyPrecedes(*input, detector)) {
              context.priorPreparedDrops.push_back(
                  *input->preparedParameterDrop);
            }
          };
          if (invocation.receiver) {
            addPriorStage(callInputFor(*invocation.receiver));
          }
          for (std::size_t prior = 0; prior < index; ++prior) {
            addPriorStage(callInputFor(invocation.operands[prior]));
          }
          if (!context.priorPreparedDrops.empty() ||
              (detector.kind == MirInstructionKind::Construct &&
               detector.info.type.kind == SemanticType::Class &&
               argument->callInputKind == HirCallInputKind::MoveValue)) {
            return context;
          }
        }
      }
    }
    return std::nullopt;
  };
  for (const auto &[instructionId, instruction] : instructionsById) {
    const std::size_t invokeCount = invokedInstructions[instructionId];
    const bool fullExpressionRoot = isFullExpressionRoot(*instruction);
    const std::optional<PreparedCallArgumentContext> argumentContext =
        preparedCallArgumentContext(*instruction);
    const MirFailureControlFlowPosition position =
        fullExpressionRoot ? MirFailureControlFlowPosition::FullExpressionRoot
        : argumentContext.has_value()
            ? MirFailureControlFlowPosition::PreparedCallArgumentRoot
            : MirFailureControlFlowPosition::None;
    const bool requiresEdge =
        mirBodyRoutesFailureEdges(body) &&
        requiresMirFailureControlFlow(*instruction, position);
    if ((requiresEdge && invokeCount != 1) ||
        (!requiresEdge && invokeCount != 0)) {
      return failure(body, owner,
                     "failure-capable operation does not have exactly one "
                     "eligible invoke edge",
                     instructionBlocks.at(instructionId), instructionId);
    }
    if (!argumentContext || invokeCount != 1) {
      continue;
    }
    const MirBlock &producer =
        body.blocks[instructionBlocks.at(instructionId) - 1];
    // A failure-capable destructor routes its own secondary failure, so one
    // reverse cleanup edge can span a chain of blocks instead of ending in the
    // immediate failure successor. Follow the normal continuation of each
    // cleanup invoke so every staged owner is counted exactly once across the
    // whole chain.
    std::vector<const MirBlock *> cleanupChain;
    std::unordered_set<MirBlockId> visitedCleanupBlocks;
    for (MirBlockId next = producer.terminator.elseTarget;
         next != 0 && next <= body.blocks.size() &&
         visitedCleanupBlocks.insert(next).second;) {
      const MirBlock &cleanupBlock = body.blocks[next - 1];
      cleanupChain.push_back(&cleanupBlock);
      if (cleanupBlock.terminator.kind != MirTerminatorKind::Invoke) {
        break;
      }
      next = cleanupBlock.terminator.target;
    }
    for (const MirDropObligationId prepared :
         argumentContext->priorPreparedDrops) {
      std::size_t cleanupCount = 0;
      for (const MirBlock *failureBlock : cleanupChain) {
        cleanupCount += static_cast<std::size_t>(std::count_if(
            failureBlock->instructions.begin(),
            failureBlock->instructions.end(),
            [&](const MirInstruction &cleanup) {
              return cleanup.kind == MirInstructionKind::Drop &&
                     cleanup.lifecycle.size() == 1 &&
                     cleanup.lifecycle.front().kind ==
                         MirLifecycleEventKind::Drop &&
                     cleanup.lifecycle.front().source == prepared &&
                     cleanup.lifecycle.front().failureCleanup;
            }));
      }
      if (cleanupCount != 1) {
        return failure(
            body, owner,
            "nested call-argument failure cleanup must drop every earlier "
            "prepared owning parameter exactly once",
            producer.id, instructionId);
      }
    }
  }

  const auto markerCanReach = [&](const MirInstruction &from,
                                  const MirInstruction &to,
                                  const MirFullExpression &expression) {
    const MirBlockId fromBlock = instructionBlocks.at(from.id);
    const MirBlockId toBlock = instructionBlocks.at(to.id);
    if (fromBlock == toBlock) {
      return instructionOrders.at(from.id) < instructionOrders.at(to.id);
    }
    const auto beginsNextEvaluation = [&](const MirBlock &block) {
      const auto isRoot = [&](HirValueId value) {
        return value != 0 &&
               std::find(expression.roots.begin(), expression.roots.end(),
                         value) != expression.roots.end();
      };
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.id == to.id) {
          return false;
        }
        // Boundary lifecycle instructions repeat the source root identity;
        // only an ordinary root-producing instruction starts a new dynamic
        // evaluation of this full expression.
        if (instruction.fullExpressionEnd == 0 &&
            isRoot(instruction.hirValue)) {
          return true;
        }
      }
      return block.id != toBlock && isRoot(block.terminator.hirValue);
    };
    std::queue<MirBlockId> pendingBlocks;
    std::unordered_set<MirBlockId> visitedBlocks;
    const auto entersNextIteration = [&](MirBlockId predecessor,
                                         MirBlockId successor) {
      // A source full expression cannot straddle an enclosing loop's
      // backedge. In the next iteration, an argument may propagate an error
      // before the expression's outer call/root instruction executes. Root
      // identity alone therefore cannot distinguish those two evaluations.
      return dominance->dominates(successor, predecessor) &&
             dominance->dominates(successor, fromBlock);
    };
    const MirBlock *origin = body.findBlock(fromBlock);
    if (origin != nullptr) {
      for (const MirBlockId successor : successors(origin->terminator)) {
        if (!entersNextIteration(fromBlock, successor)) {
          pendingBlocks.push(successor);
        }
      }
    }
    while (!pendingBlocks.empty()) {
      const MirBlockId candidate = pendingBlocks.front();
      pendingBlocks.pop();
      if (!visitedBlocks.insert(candidate).second) {
        continue;
      }
      const MirBlock *block = body.findBlock(candidate);
      if (block == nullptr || beginsNextEvaluation(*block)) {
        continue;
      }
      if (candidate == toBlock) {
        return true;
      }
      for (const MirBlockId successor : successors(block->terminator)) {
        if (!entersNextIteration(candidate, successor)) {
          pendingBlocks.push(successor);
        }
      }
    }
    return false;
  };
  for (std::size_t expressionIndex = 0;
       expressionIndex < fullExpressionMarkerRows.size(); ++expressionIndex) {
    const std::vector<const MirInstruction *> &markers =
        fullExpressionMarkerRows[expressionIndex];
    const MirFullExpression &expression = body.fullExpressions[expressionIndex];
    for (std::size_t left = 0; left < markers.size(); ++left) {
      for (std::size_t right = left + 1; right < markers.size(); ++right) {
        if (markerCanReach(*markers[left], *markers[right], expression) ||
            markerCanReach(*markers[right], *markers[left], expression)) {
          return failure(body, owner,
                         "full-expression boundary markers are not on mutually "
                         "exclusive control-flow paths",
                         instructionBlocks.at(markers[right]->id),
                         markers[right]->id);
        }
      }
    }
  }

  const auto precedingFullExpressionDrops = [&](const MirBlock &markerBlock,
                                                std::size_t markerIndex) {
    std::vector<const MirInstruction *> reversed;
    const MirBlock *cursor = &markerBlock;
    std::size_t cursorIndex = markerIndex;
    std::unordered_set<MirBlockId> visited;
    while (visited.insert(cursor->id).second) {
      while (cursorIndex != 0) {
        const MirInstruction &candidate = cursor->instructions[cursorIndex - 1];
        if (temporaryDependencyEnd(candidate)) {
          --cursorIndex;
          continue;
        }
        if (candidate.kind != MirInstructionKind::Drop) {
          std::reverse(reversed.begin(), reversed.end());
          return reversed;
        }
        reversed.push_back(&candidate);
        --cursorIndex;
      }
      const MirBlock *predecessor = nullptr;
      std::size_t predecessorCount = 0;
      for (const MirBlock &candidate : body.blocks) {
        const std::vector<MirBlockId> outgoing =
            successors(candidate.terminator);
        if (std::find(outgoing.begin(), outgoing.end(), cursor->id) ==
            outgoing.end()) {
          continue;
        }
        ++predecessorCount;
        if (candidate.terminator.kind == MirTerminatorKind::Invoke &&
            candidate.terminator.target == cursor->id &&
            candidate.activeFailure == cursor->activeFailure) {
          predecessor = &candidate;
        }
      }
      if (predecessorCount != 1 || predecessor == nullptr) {
        break;
      }
      cursor = predecessor;
      cursorIndex = cursor->instructions.size();
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
  };

  std::unordered_map<MirInstructionId, std::size_t> fullExpressionDropCoverage;
  for (const MirBlock &block : body.blocks) {
    for (std::size_t markerIndex = 0; markerIndex < block.instructions.size();
         ++markerIndex) {
      const MirInstruction &marker = block.instructions[markerIndex];
      if (marker.fullExpressionEnd == 0) {
        continue;
      }
      const MirFullExpression &expression =
          body.fullExpressions[marker.fullExpressionEnd - 1];
      const std::vector<const MirInstruction *> drops =
          precedingFullExpressionDrops(block, markerIndex);
      std::size_t previousDrop = std::numeric_limits<std::size_t>::max();
      std::unordered_set<MirDropObligationId> pathObligations;
      for (const MirInstruction *drop : drops) {
        const MirDropObligationId obligation =
            drop->lifecycle.size() == 1 ? drop->lifecycle.front().source : 0;
        const MirDropObligation *dropDescriptor =
            body.findDropObligation(obligation);
        if (drop->kind != MirInstructionKind::Drop ||
            drop->lifecycle.size() != 1 ||
            drop->lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop->lifecycle.front().failureCleanup ||
            dropDescriptor == nullptr ||
            dropDescriptor->fullExpression != expression.id ||
            !pathObligations.insert(obligation).second ||
            dropDescriptor->constructionOrder >= previousDrop) {
          return failure(
              body, owner,
              "full-expression drops must be contiguous and in reverse "
              "obligation order",
              block.id, drop->id);
        }
        previousDrop = dropDescriptor->constructionOrder;
        ++fullExpressionDropCoverage[drop->id];
      }
      const MirInstruction &firstBoundary =
          drops.empty() ? marker : *drops.front();
      if (marker.earlyExitFullExpressionEnd) {
        continue;
      }
      for (const HirValueId root : expression.roots) {
        bool rootBeforeBoundary = false;
        bool hasConcreteValue = false;
        for (const MirValue &value : body.values) {
          if (value.sourceValue != root) {
            continue;
          }
          hasConcreteValue = true;
          const auto definition = instructionsById.find(value.definition);
          if (definition != instructionsById.end()) {
            rootBeforeBoundary =
                rootBeforeBoundary ||
                strictlyPrecedes(*definition->second, firstBoundary);
          }
        }
        for (const MirBlock &definitionBlock : body.blocks) {
          for (std::size_t definitionIndex = 0;
               definitionIndex < definitionBlock.instructions.size();
               ++definitionIndex) {
            const MirInstruction &definition =
                definitionBlock.instructions[definitionIndex];
            if (definition.fullExpressionEnd != 0 ||
                definition.hirValue != root) {
              continue;
            }
            if (strictlyPrecedes(definition, firstBoundary)) {
              rootBeforeBoundary = true;
            }
          }
        }
        const bool explicitVoidCompletion =
            !hasConcreteValue && marker.hirValue == root;
        if (!rootBeforeBoundary && !explicitVoidCompletion) {
          return failure(body, owner,
                         "full-expression drop or marker precedes one of its "
                         "roots",
                         block.id, marker.id);
        }
      }
    }
  }
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.kind != MirInstructionKind::Drop ||
          instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
          instruction.lifecycle.front().failureCleanup) {
        continue;
      }
      const MirDropObligation *obligation =
          body.findDropObligation(instruction.lifecycle.front().source);
      if (obligation != nullptr && obligation->fullExpression != 0 &&
          fullExpressionDropCoverage[instruction.id] != 1) {
        return failure(body, owner,
                       "full-expression cleanup is not connected to exactly "
                       "one boundary marker",
                       block.id, instruction.id);
      }
    }
  }

  const auto appendValue = [](std::vector<MirValueId> &values,
                              MirValueId value) {
    if (value != 0 &&
        std::find(values.begin(), values.end(), value) == values.end()) {
      values.push_back(value);
    }
  };
  const auto appendPlaceValues = [&](std::vector<MirValueId> &values,
                                     MirPlaceId placeId) {
    const MirPlace *place = body.findPlace(placeId);
    if (place == nullptr) {
      return;
    }
    if (place->root == MirPlaceRootKind::Value) {
      appendValue(values, place->value);
    }
    for (const MirPlaceProjection &projection : place->projections) {
      if (projection.kind == MirProjectionKind::Index ||
          projection.kind == MirProjectionKind::RawIndex) {
        appendValue(values, projection.index);
      }
    }
  };
  const auto appendOperandValues = [&](std::vector<MirValueId> &values,
                                       const MirOperand &operand) {
    switch (operand.kind) {
    case MirOperandKind::Value:
      appendValue(values, operand.value);
      break;
    case MirOperandKind::Address:
    case MirOperandKind::Copy:
    case MirOperandKind::Move:
    case MirOperandKind::BorrowRead:
    case MirOperandKind::BorrowWrite:
      appendPlaceValues(values, operand.place);
      break;
    case MirOperandKind::Constant:
      break;
    case MirOperandKind::Loan:
      if (operand.place != 0) {
        appendPlaceValues(values, operand.place);
      }
      break;
    }
  };
  const auto verifyAvailable = [&](MirValueId valueId, MirBlockId useBlock,
                                   std::size_t useOrder,
                                   MirInstructionId instruction)
      -> std::optional<MirVerificationResult> {
    const MirValue *value = body.findValue(valueId);
    if (value == nullptr) {
      return failure(body, owner, "value use has an invalid identity", useBlock,
                     instruction);
    }
    if (value->definitionBlock == useBlock) {
      if (value->definition == 0) {
        return std::nullopt;
      }
      const auto definition = instructionOrders.find(value->definition);
      if (definition == instructionOrders.end() ||
          definition->second >= useOrder) {
        return failure(body, owner,
                       "value " + std::to_string(valueId) +
                           " is used before its definition in the same block",
                       useBlock, instruction);
      }
      return std::nullopt;
    }
    // Unreachable blocks have no forward-dominance relation to the entry and
    // cannot execute. Same-block ordering above is still checked so their
    // local representation remains coherent.
    if (!dominance->isReachable(useBlock)) {
      return std::nullopt;
    }
    if (!dominance->dominates(value->definitionBlock, useBlock)) {
      return failure(body, owner,
                     "value " + std::to_string(valueId) +
                         " is used in a block not dominated by its definition",
                     useBlock, instruction);
    }
    return std::nullopt;
  };

  for (const MirBlock &block : body.blocks) {
    for (std::size_t instructionIndex = 0;
         instructionIndex < block.instructions.size(); ++instructionIndex) {
      const MirInstruction &instruction = block.instructions[instructionIndex];
      std::vector<MirValueId> usedValues;
      if (instruction.destination) {
        appendPlaceValues(usedValues, *instruction.destination);
      }
      if (instruction.receiver) {
        appendOperandValues(usedValues, *instruction.receiver);
      }
      for (const MirOperand &operand : instruction.operands) {
        appendOperandValues(usedValues, operand);
      }
      for (const MirValueId used : usedValues) {
        if (std::optional<MirVerificationResult> invalid = verifyAvailable(
                used, block.id, instructionIndex, instruction.id)) {
          return std::move(*invalid);
        }
      }
    }

    if (block.terminator.value) {
      std::vector<MirValueId> usedValues;
      appendOperandValues(usedValues, *block.terminator.value);
      for (const MirValueId used : usedValues) {
        if (std::optional<MirVerificationResult> invalid =
                verifyAvailable(used, block.id, block.instructions.size(), 0)) {
          return std::move(*invalid);
        }
      }
    }
    const auto verifyEdgeValues = [&](const std::vector<MirOperand> &arguments)
        -> std::optional<MirVerificationResult> {
      std::vector<MirValueId> usedValues;
      for (const MirOperand &argument : arguments) {
        appendOperandValues(usedValues, argument);
      }
      for (const MirValueId used : usedValues) {
        if (std::optional<MirVerificationResult> invalid =
                verifyAvailable(used, block.id, block.instructions.size(), 0)) {
          return invalid;
        }
      }
      return std::nullopt;
    };
    if (std::optional<MirVerificationResult> invalid =
            verifyEdgeValues(block.terminator.targetArguments)) {
      return std::move(*invalid);
    }
    if (std::optional<MirVerificationResult> invalid =
            verifyEdgeValues(block.terminator.elseArguments)) {
      return std::move(*invalid);
    }
    for (const MirSwitchTarget &target : block.terminator.switchTargets) {
      if (std::optional<MirVerificationResult> invalid =
              verifyEdgeValues(target.arguments)) {
        return std::move(*invalid);
      }
    }
  }
  if (MirVerificationResult bases = verifyBaseInitializations(body, owner);
      !bases.valid()) {
    return bases;
  }
  if (MirVerificationResult allocations = verifyObjectAllocations(body, owner);
      !allocations.valid()) {
    return allocations;
  }
  if (MirVerificationResult lifecycle = verifyMirLifecycleFlow(body, owner);
      !lifecycle.valid()) {
    return lifecycle;
  }
  if (MirVerificationResult initialization =
          verifyMirScalarInitializationFlow(body, owner);
      !initialization.valid()) {
    return initialization;
  }
  if (MirVerificationResult ownership = verifyMirOwnershipFlow(body, owner);
      !ownership.valid()) {
    return ownership;
  }
  if (MirVerificationResult ranges = verifyMirRangeIterations(body, owner);
      !ranges.valid()) {
    return ranges;
  }
  return verifyMirLoanFlow(body, owner);
}

bool mirDropFollowsDefiniteMove(const MirBlock &block,
                                MirDropObligationId obligation,
                                MirInstructionId beforeInstruction) {
  if (obligation == 0) {
    return false;
  }
  bool moved = false;
  for (const MirInstruction &instruction : block.instructions) {
    if (instruction.tombstone) {
      if (instruction.id == beforeInstruction) {
        return false;
      }
      continue;
    }
    if (instruction.id == beforeInstruction) {
      return moved;
    }
    for (const MirLifecycleEvent &event : instruction.lifecycle) {
      if (event.source == obligation) {
        moved = !event.conditional &&
                (event.kind == MirLifecycleEventKind::Move ||
                 event.kind == MirLifecycleEventKind::Replace);
      }
      if (event.target == obligation) {
        moved = false;
      }
    }
  }
  return beforeInstruction == 0 && moved;
}

} // namespace lang
