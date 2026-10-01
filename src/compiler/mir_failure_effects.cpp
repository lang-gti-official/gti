#include "gti/mir.h"
#include "gti/mir_dominance.h"
#include "gti/mir_editor.h"
#include "gti/mir_effects.h"
#include "gti/mir_verifier.h"
#include "gti/optimization/safety.h"
#include "gti/optimization/sccp.h"
#include "mir_verification_queries.h"

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
static std::vector<bool>
deriveMirFunctionDefinedFailureEffects(const MirProgram &program,
                                       std::vector<bool> *destructorEffects) {
  const auto scalarType = [](const SemanticType &type) {
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
    case SemanticType::NullPtr:
      return true;
    default:
      return false;
    }
  };
  const auto scalarInfo = [&](const ExpressionInfo &info) {
    return scalarType(info.type) && info.traits.drop == DropKind::Trivial &&
           !info.traits.containsBorrowedState;
  };
  const auto rawPointerType = [](const SemanticType &type) {
    return type.kind == SemanticType::RawPointer &&
           type.arguments.size() == 1 &&
           type.arguments.front() != SemanticType::Void;
  };
  const auto rawPointerOffsetType = [](const SemanticType &type) {
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
  const auto passiveCAbiRecordType = [&](const SemanticType &type) {
    if (type.kind != SemanticType::Class) {
      return false;
    }
    const MirClassInstance *selected = nullptr;
    for (const MirClassInstance &candidate : program.classInstances()) {
      if (candidate.type != type) {
        continue;
      }
      if (selected != nullptr) {
        return false;
      }
      selected = &candidate;
    }
    return selected != nullptr && selected->cAbiRecord &&
           selected->cAbiLayout.has_value() &&
           !selected->unionLayout.has_value() &&
           !selected->requiresActiveDropState &&
           !selected->requiresActiveCleanup;
  };
  // Passive values cannot raise merely by being loaded, copied, or passed.
  // `[[c_abi]]` records are part of that set by their verified
  // standard-layout/trivially-copyable contract. This says nothing about
  // dereferencing a raw pointer; those operations are admitted separately
  // through exact verified MIR shapes below.
  const auto passiveType = [&](const SemanticType &type) {
    return scalarType(type) || type.kind == SemanticType::StringView ||
           type.kind == SemanticType::CString ||
           (type.kind == SemanticType::RawPointer &&
            type.arguments.size() == 1) ||
           (type.kind == SemanticType::NativeFunction &&
            type.hasNativeFunctionShape()) ||
           passiveCAbiRecordType(type);
  };
  const auto passiveInfo = [&](const ExpressionInfo &info) {
    return passiveType(info.type) && info.traits.drop == DropKind::Trivial &&
           !info.traits.containsBorrowedState;
  };
  // A C-linkage or runtime-binding target carries
  // FailurePropagationKind::None by language contract: the native surface
  // has no defined-failure channel, so the call proves failure-free without
  // recursing into a body that does not exist.
  const auto nativeNoFailureTarget = [](const MirFunctionInstance &target) {
    return target.linkage == LanguageLinkage::C ||
           target.definitionKind ==
               MirFunctionInstance::DefinitionKind::RuntimeBinding;
  };
  const auto wrappingIntrinsic = [](IntrinsicKind intrinsic) {
    switch (intrinsic) {
    case IntrinsicKind::IntegerWrappingAdd:
    case IntrinsicKind::IntegerWrappingSubtract:
    case IntrinsicKind::IntegerWrappingMultiply:
    case IntrinsicKind::IntegerSaturatingAdd:
    case IntrinsicKind::IntegerSaturatingSubtract:
    case IntrinsicKind::IntegerSaturatingMultiply:
      return true;
    default:
      return false;
    }
  };
  const auto validStaticDispatchOwner = [&](const MirInstruction &instruction) {
    if (instruction.dispatchOwner == SemanticType::Unknown) {
      return true;
    }
    const MirFunctionInstance *target =
        instruction.kind == MirInstructionKind::Call &&
                instruction.functionTarget
            ? program.findFunctionInstance(*instruction.functionTarget)
            : nullptr;
    const MirClassInstance *owner =
        target != nullptr && target->staticMember && target->owner
            ? program.findClassInstance(*target->owner)
            : nullptr;
    return instruction.dispatch == CallDispatch::Static &&
           !instruction.receiver && owner != nullptr &&
           instruction.dispatchOwner == owner->type;
  };
  const auto scalarOwnership = [](const std::optional<OwnershipEvent> &event) {
    return !event ||
           ((event->kind == OwnershipEventKind::Read ||
             event->kind == OwnershipEventKind::Reinitialize) &&
            event->before == OwnershipStateSet::Available &&
            event->after == OwnershipStateSet::Available && event->reachable);
  };
  const auto scalarOperation = [](MirOperation operation) {
    switch (operation) {
    case MirOperation::Literal:
    case MirOperation::Identity:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Positive:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
      return true;
    default:
      return false;
    }
  };
  const auto stableScalarProvenance = [&](const MirInstruction &instruction) {
    switch (instruction.literalProvenance.kind) {
    case MirLiteralProvenanceKind::None:
    case MirLiteralProvenanceKind::Source:
    case MirLiteralProvenanceKind::IdentityFold:
      return true;
    case MirLiteralProvenanceKind::ComputeFold:
    case MirLiteralProvenanceKind::SparseConstant:
      // Optimizer literals retain their source operation. Do not strengthen
      // an executable body's defined-failure summary merely because an
      // operation outside this bounded proof folded to a literal.
      return scalarOperation(instruction.literalProvenance.sourceOperation);
    case MirLiteralProvenanceKind::Count:
      return false;
    }
    return false;
  };
  const auto noDefinedFailureDestructor = [&](HirDestructorInstanceId id) {
    const MirDestructorInstance *destructor =
        program.findDestructorInstance(id);
    if (destructor == nullptr || destructor->id != id ||
        destructor->owner == 0 ||
        destructor->definitionKind != MirDefinitionKind::Source ||
        destructor->body.kind != MirBodyKind::Destructor ||
        destructor->body.returnType != SemanticType::Void ||
        destructor->body.blocks.empty() || !destructor->body.loans.empty() ||
        !destructor->body.dropObligations.empty() ||
        !destructor->body.cleanupBoundaries.empty() ||
        !destructor->body.failureRecords.empty() ||
        !verifyMirBody(destructor->body, destructor->id).valid()) {
      return false;
    }
    const MirBody &body = destructor->body;
    if (!std::all_of(body.places.begin(), body.places.end(),
                     [&](const MirPlace &place) {
                       const bool storage =
                           place.root == MirPlaceRootKind::Symbol &&
                           place.binding == 0 && place.symbol != 0 &&
                           place.capture == 0 && place.temporary == 0 &&
                           place.value == 0 && place.loan == 0 &&
                           place.projections.empty() && !place.key &&
                           !place.initiallyAvailable &&
                           scalarType(place.type) &&
                           place.traits.drop == DropKind::Trivial &&
                           !place.traits.containsBorrowedState;
                       // A destructor reading one of its receiver's scalar
                       // fields cannot raise; writes stay confined to storage
                       // places below.
                       const bool receiverField =
                           place.root == MirPlaceRootKind::This &&
                           place.binding == 0 && place.symbol == 0 &&
                           place.capture == 0 && place.temporary == 0 &&
                           place.value == 0 && place.loan == 0 &&
                           !place.initiallyAvailable &&
                           ((place.projections.empty() &&
                             place.type.kind == SemanticType::Class) ||
                            (place.projections.size() == 1 &&
                             place.projections.front().kind ==
                                 MirProjectionKind::Field &&
                             scalarType(place.type) &&
                             place.traits.drop == DropKind::Trivial &&
                             !place.traits.containsBorrowedState));
                       return storage || receiverField;
                     }) ||
        !std::all_of(
            body.values.begin(), body.values.end(),
            [&](const MirValue &value) { return scalarInfo(value.info); })) {
      return false;
    }
    for (const MirBlock &block : body.blocks) {
      const MirTerminator &terminator = block.terminator;
      const bool exitShape =
          block.failureParameter == 0 && terminator.invokeInstruction == 0 &&
          terminator.failureRecord == 0 && terminator.switchTargets.empty() &&
          terminator.successLifecycle.empty() &&
          ((terminator.kind == MirTerminatorKind::Goto &&
            terminator.target != 0 && !terminator.value) ||
           (terminator.kind == MirTerminatorKind::Branch && terminator.value &&
            terminator.value->kind == MirOperandKind::Value) ||
           (terminator.kind == MirTerminatorKind::Return && !terminator.value));
      if (!exitShape) {
        return false;
      }
    }
    for (const MirBlock &enclosing : body.blocks)
      for (const MirInstruction &instruction : enclosing.instructions) {
        const bool common =
            instruction.unsafeOperation == UnsafeOperationKind::None &&
            !instruction.rawMemoryAccess && instruction.callSite == 0 &&
            instruction.constructorInitializer == 0 &&
            !instruction.callInputRole && instruction.callInputIndex == 0 &&
            instruction.callInputKind == HirCallInputKind::Value &&
            !instruction.preparedParameterDrop &&
            !instruction.successResultDrop &&
            !instruction.successResultDestination && !instruction.receiver &&
            instruction.parameterTypes.empty() &&
            instruction.closureCaptureTypes.empty() &&
            instruction.closureCaptureModes.empty() && !instruction.loan &&
            instruction.borrowOrigin == BorrowOriginKind::None &&
            instruction.borrowArgument == 0 &&
            instruction.borrowAccess == AccessMode::ReadOnly &&
            !instruction.borrowPlace && !instruction.enumOwner &&
            !instruction.enumValue && !instruction.enumVariant &&
            !instruction.payloadIndex &&
            instruction.intrinsic == IntrinsicKind::None &&
            instruction.synchronization.kind ==
                SynchronizationOperationKind::None &&
            instruction.definedFailure.empty() &&
            instruction.localFailureSites.empty() &&
            instruction.dispatch == CallDispatch::Static &&
            instruction.dispatchOwner == SemanticType::Unknown &&
            !instruction.functionTarget && !instruction.constructorTarget &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            !instruction.lambdaTarget &&
            instruction.callableArguments.empty() &&
            !instruction.callableBoundary && !instruction.callableInvocation &&
            !instruction.ownership;
        if (!common) {
          return false;
        }
        if (instruction.kind == MirInstructionKind::Compute) {
          const bool literalForm =
              instruction.operation == MirOperation::Literal &&
              instruction.literal.has_value() && instruction.operands.empty();
          const bool operandForm =
              instruction.operation != MirOperation::Literal &&
              scalarOperation(instruction.operation) && !instruction.literal &&
              std::all_of(instruction.operands.begin(),
                          instruction.operands.end(),
                          [](const MirOperand &operand) {
                            return operand.kind == MirOperandKind::Value;
                          });
          if (!instruction.result || instruction.destination ||
              !scalarInfo(instruction.info) || (!literalForm && !operandForm) ||
              !stableScalarProvenance(instruction) ||
              !instruction.lifecycle.empty() ||
              instruction.fullExpressionEnd != 0 ||
              instruction.cleanupBoundaryEnd != 0) {
            return false;
          }
          continue;
        }
        if (instruction.kind == MirInstructionKind::Load) {
          if (!instruction.result || instruction.destination ||
              !scalarInfo(instruction.info) ||
              instruction.operation != MirOperation::None ||
              instruction.literal || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != MirOperandKind::Copy ||
              !instruction.lifecycle.empty() ||
              instruction.fullExpressionEnd != 0 ||
              instruction.cleanupBoundaryEnd != 0) {
            return false;
          }
          continue;
        }
        if (instruction.kind == MirInstructionKind::Assign) {
          const MirPlace *place = instruction.destination
                                      ? body.findPlace(*instruction.destination)
                                      : nullptr;
          if (!instruction.result || place == nullptr ||
              place->root != MirPlaceRootKind::Symbol ||
              !scalarInfo(instruction.info) ||
              instruction.operation != MirOperation::Assign ||
              instruction.literal || instruction.operands.size() != 1 ||
              instruction.operands.front().kind != MirOperandKind::Value ||
              !instruction.lifecycle.empty() ||
              instruction.fullExpressionEnd != 0 ||
              instruction.cleanupBoundaryEnd != 0) {
            return false;
          }
          continue;
        }
        if (instruction.kind != MirInstructionKind::Lifecycle ||
            instruction.result || instruction.destination ||
            !instruction.operands.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !instruction.lifecycle.empty() ||
            instruction.fullExpressionEnd == 0 ||
            instruction.cleanupBoundaryEnd != 0) {
          return false;
        }
      }
    return true;
  };
  if (destructorEffects != nullptr) {
    destructorEffects->assign(program.destructorInstances().size(), true);
    for (const MirDestructorInstance &destructor :
         program.destructorInstances()) {
      if (destructor.id != 0 && destructor.id <= destructorEffects->size() &&
          noDefinedFailureDestructor(destructor.id)) {
        (*destructorEffects)[destructor.id - 1] = false;
      }
    }
  }
  const auto cleanupClass =
      [&](const SemanticType &type) -> const MirClassInstance * {
    if (type.kind != SemanticType::Class) {
      return nullptr;
    }
    const MirClassInstance *selected = nullptr;
    for (const MirClassInstance &candidate : program.classInstances()) {
      if (candidate.type != type) {
        continue;
      }
      if (selected != nullptr) {
        return nullptr;
      }
      selected = &candidate;
    }
    return selected != nullptr && selected->id != 0 &&
                   selected->declaration != 0 && selected->bases.empty() &&
                   selected->structuralBases.empty() && !selected->abstract &&
                   !selected->polymorphic && !selected->cAbiRecord &&
                   !selected->cAbiLayout && !selected->unionLayout &&
                   selected->declaredFields.empty() &&
                   selected->fields.empty() &&
                   selected->fieldDropOrder.empty() && selected->destructor &&
                   selected->requiresActiveDropState &&
                   selected->requiresActiveCleanup &&
                   noDefinedFailureDestructor(*selected->destructor)
               ? selected
               : nullptr;
  };
  const auto classDefaultCleanupNoFailure = [&](const MirFunctionInstance
                                                    &function) {
    const MirBody &body = function.body;
    if (function.id == 0 || function.declaration == 0 ||
        function.definitionKind !=
            MirFunctionInstance::DefinitionKind::Source ||
        function.linkage != LanguageLinkage::Gti ||
        !function.externalSymbol.empty() || !function.parameterTypes.empty() ||
        !function.parameterBindings.empty() ||
        !scalarType(function.returnType) ||
        body.kind != MirBodyKind::Function ||
        body.returnType != function.returnType || body.blocks.size() != 1 ||
        !body.loans.empty() || !body.failureRecords.empty() ||
        body.cleanupBoundaries.size() != 1 || body.dropObligations.empty() ||
        body.dropObligations.size() % 2 != 0 ||
        !verifyMirBody(body, function.id).valid()) {
      return false;
    }
    const std::size_t localCount = body.dropObligations.size() / 2;
    const MirBlock &block = body.blocks.front();
    if (!block.reachable || block.failureParameter != 0 ||
        body.places.size() != localCount * 2 + 1 ||
        body.values.size() != localCount + 1 ||
        block.instructions.size() != localCount * 4 + 3 ||
        block.terminator.kind != MirTerminatorKind::Return ||
        !block.terminator.value ||
        block.terminator.value->kind != MirOperandKind::Value ||
        block.terminator.invokeInstruction != 0 ||
        block.terminator.failureRecord != 0 || block.terminator.target != 0 ||
        block.terminator.elseTarget != 0 ||
        !block.terminator.switchTargets.empty() ||
        !block.terminator.successLifecycle.empty()) {
      return false;
    }

    std::unordered_set<MirPlaceId> usedPlaces;
    std::unordered_set<MirValueId> usedValues;
    std::vector<MirDropObligationId> lexicalDrops;
    for (std::size_t index = 0; index < localCount; ++index) {
      const MirInstruction &construct = block.instructions[index * 3];
      const MirInstruction &initialize = block.instructions[index * 3 + 1];
      const MirInstruction &boundary = block.instructions[index * 3 + 2];
      const MirValue *value =
          construct.result ? body.findValue(*construct.result) : nullptr;
      const MirPlace *destination =
          initialize.destination ? body.findPlace(*initialize.destination)
                                 : nullptr;
      const MirDropObligation *valueDrop =
          construct.lifecycle.size() == 1
              ? body.findDropObligation(construct.lifecycle.front().target)
              : nullptr;
      const MirDropObligation *bindingDrop =
          initialize.lifecycle.size() == 1
              ? body.findDropObligation(initialize.lifecycle.front().target)
              : nullptr;
      const MirPlace *valuePlace =
          valueDrop == nullptr ? nullptr : body.findPlace(valueDrop->place);
      const MirClassInstance *classInstance =
          value == nullptr ? nullptr : cleanupClass(value->info.type);
      if (construct.kind != MirInstructionKind::Construct ||
          !construct.result || construct.destination ||
          !construct.operands.empty() ||
          construct.operation != MirOperation::None || construct.literal ||
          construct.constructorTarget || !construct.definedFailure.empty() ||
          !construct.localFailureSites.empty() || construct.ownership ||
          construct.fullExpressionEnd != 0 ||
          construct.cleanupBoundaryEnd != 0 ||
          construct.lifecycle.size() != 1 || value == nullptr ||
          classInstance == nullptr || valueDrop == nullptr ||
          valuePlace == nullptr ||
          construct.lifecycle.front().kind !=
              MirLifecycleEventKind::Initialize ||
          construct.lifecycle.front().source != 0 ||
          construct.lifecycle.front().conditional ||
          construct.lifecycle.front().failureCleanup ||
          valueDrop->kind != MirDropObligationKind::Value ||
          valueDrop->dropType.classInstance != classInstance->id ||
          valueDrop->dropType.destructor != classInstance->destructor ||
          !valueDrop->dropType.requiresActiveCleanup ||
          valuePlace->root != MirPlaceRootKind::Value ||
          valuePlace->value != value->id || valuePlace->key ||
          initialize.kind != MirInstructionKind::Initialize ||
          initialize.result || destination == nullptr ||
          initialize.operands.size() != 1 ||
          initialize.operands.front().kind != MirOperandKind::Value ||
          initialize.operands.front().value != value->id ||
          initialize.operation != MirOperation::None ||
          !initialize.definedFailure.empty() || initialize.ownership ||
          initialize.lifecycle.size() != 1 || bindingDrop == nullptr ||
          initialize.lifecycle.front().kind !=
              MirLifecycleEventKind::Reparent ||
          initialize.lifecycle.front().source != valueDrop->id ||
          initialize.lifecycle.front().target != bindingDrop->id ||
          initialize.lifecycle.front().conditional ||
          initialize.lifecycle.front().failureCleanup ||
          bindingDrop->kind != MirDropObligationKind::Binding ||
          bindingDrop->place != destination->id ||
          bindingDrop->dropType.classInstance != classInstance->id ||
          bindingDrop->dropType.destructor != classInstance->destructor ||
          !bindingDrop->dropType.requiresActiveCleanup ||
          destination->root != MirPlaceRootKind::Binding ||
          destination->type != value->info.type ||
          boundary.kind != MirInstructionKind::Lifecycle || boundary.result ||
          boundary.destination || !boundary.operands.empty() ||
          !boundary.definedFailure.empty() || boundary.ownership ||
          !boundary.lifecycle.empty() || boundary.fullExpressionEnd == 0 ||
          boundary.cleanupBoundaryEnd != 0 ||
          !usedPlaces.insert(valuePlace->id).second ||
          !usedPlaces.insert(destination->id).second ||
          !usedValues.insert(value->id).second) {
        return false;
      }
      lexicalDrops.push_back(bindingDrop->id);
    }

    const std::size_t returnBase = localCount * 3;
    const MirInstruction &load = block.instructions[returnBase];
    const MirInstruction &returnBoundary = block.instructions[returnBase + 1];
    const MirPlace *global = load.operands.size() == 1
                                 ? body.findPlace(load.operands.front().place)
                                 : nullptr;
    const MirValue *returned =
        load.result ? body.findValue(*load.result) : nullptr;
    if (load.kind != MirInstructionKind::Load || !load.result ||
        load.destination || load.operands.size() != 1 ||
        load.operands.front().kind != MirOperandKind::Copy ||
        load.operation != MirOperation::None || load.literal ||
        !load.definedFailure.empty() || !scalarInfo(load.info) ||
        returned == nullptr || global == nullptr ||
        global->root != MirPlaceRootKind::Symbol ||
        !global->projections.empty() || !global->key ||
        global->type != returned->info.type ||
        returnBoundary.kind != MirInstructionKind::Lifecycle ||
        returnBoundary.result || returnBoundary.destination ||
        !returnBoundary.operands.empty() ||
        !returnBoundary.definedFailure.empty() ||
        !returnBoundary.lifecycle.empty() ||
        returnBoundary.fullExpressionEnd == 0 ||
        returnBoundary.cleanupBoundaryEnd != 0 ||
        !usedPlaces.insert(global->id).second ||
        !usedValues.insert(returned->id).second ||
        block.terminator.value->value != returned->id) {
      return false;
    }

    std::size_t instructionIndex = returnBase + 2;
    for (std::size_t reverse = localCount; reverse > 0; --reverse) {
      const MirDropObligation *drop =
          body.findDropObligation(lexicalDrops[reverse - 1]);
      const MirInstruction &instruction =
          block.instructions[instructionIndex++];
      if (drop == nullptr || instruction.kind != MirInstructionKind::Drop ||
          instruction.destination != drop->place || instruction.result ||
          !instruction.operands.empty() ||
          instruction.operation != MirOperation::None ||
          !instruction.definedFailure.empty() || instruction.ownership ||
          instruction.lifecycle.size() != 1 ||
          instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
          instruction.lifecycle.front().source != drop->id ||
          instruction.lifecycle.front().target != 0 ||
          instruction.lifecycle.front().conditional ||
          instruction.lifecycle.front().failureCleanup) {
        return false;
      }
    }
    const MirInstruction &cleanup = block.instructions[instructionIndex];
    std::reverse(lexicalDrops.begin(), lexicalDrops.end());
    return cleanup.kind == MirInstructionKind::Lifecycle &&
           cleanup.result == std::nullopt && !cleanup.destination &&
           cleanup.operands.empty() && cleanup.definedFailure.empty() &&
           cleanup.lifecycle.empty() && cleanup.fullExpressionEnd == 0 &&
           cleanup.cleanupBoundaryEnd == body.cleanupBoundaries.front().id &&
           body.cleanupBoundaries.front().kind ==
               MirCleanupBoundaryKind::Normal &&
           body.cleanupBoundaries.front().obligations == lexicalDrops &&
           usedPlaces.size() == body.places.size() &&
           usedValues.size() == body.values.size();
  };

  enum class State : unsigned char { Unvisited, Visiting, Complete };
  std::vector<State> states(program.functionInstances().size(),
                            State::Unvisited);
  std::vector<bool> mayRaise(program.functionInstances().size(), true);

  const auto canProveNoFailure = [&](const auto &self,
                                     HirFunctionInstanceId id) -> bool {
    if (id == 0 || id > program.functionInstances().size()) {
      return false;
    }
    State &state = states[id - 1];
    if (state == State::Complete) {
      return !mayRaise[id - 1];
    }
    if (state == State::Visiting) {
      return false;
    }
    state = State::Visiting;

    const MirFunctionInstance &function = program.functionInstances()[id - 1];
    if (classDefaultCleanupNoFailure(function)) {
      state = State::Complete;
      mayRaise[id - 1] = false;
      return true;
    }
    const MirBody &body = function.body;
    // A raw place is a single passive lvalue projection from an exact raw
    // pointer SSA value. The operation remains unsafe, but it has no GTI
    // defined-failure channel: violating its proof obligations is outside the
    // defined-failure model rather than a recoverable language failure.
    const auto rawPlaceOperation =
        [&](const MirPlace *place) -> std::optional<UnsafeOperationKind> {
      if (place == nullptr || place->root != MirPlaceRootKind::Value ||
          place->binding != 0 || place->symbol != 0 || place->capture != 0 ||
          place->temporary != 0 || place->value == 0 || place->loan != 0 ||
          place->projections.size() != 1 || !passiveType(place->type) ||
          place->traits.drop != DropKind::Trivial ||
          place->traits.containsBorrowedState || place->initiallyAvailable) {
        return std::nullopt;
      }
      const MirValue *pointer = body.findValue(place->value);
      if (pointer == nullptr || !passiveInfo(pointer->info) ||
          !rawPointerType(pointer->info.type) ||
          pointer->info.type.arguments.front() != place->type) {
        return std::nullopt;
      }
      const MirPlaceProjection &projection = place->projections.front();
      if (projection.field != 0 || projection.constantIndex ||
          projection.selection != 0) {
        return std::nullopt;
      }
      if (projection.kind == MirProjectionKind::RawDereference &&
          projection.index == 0) {
        return UnsafeOperationKind::RawDereference;
      }
      const MirValue *index =
          projection.index == 0 ? nullptr : body.findValue(projection.index);
      if (projection.kind != MirProjectionKind::RawIndex || index == nullptr ||
          !scalarInfo(index->info) || !rawPointerOffsetType(index->info.type)) {
        return std::nullopt;
      }
      return UnsafeOperationKind::RawIndex;
    };
    const auto exactValueOperand = [&](const MirOperand &operand) {
      const MirValue *value = operand.kind == MirOperandKind::Value
                                  ? body.findValue(operand.value)
                                  : nullptr;
      return value != nullptr && operand.place == 0 && operand.loan == 0 &&
             !operand.literal && operand.type == value->info.type;
    };
    const auto exactRawCompute = [&](const MirInstruction &instruction) {
      if (instruction.kind != MirInstructionKind::Compute ||
          !instruction.result || instruction.destination ||
          instruction.rawMemoryAccess || !passiveInfo(instruction.info)) {
        return false;
      }
      if (instruction.operation == MirOperation::AddressOf) {
        const MirOperand *source = instruction.operands.size() == 1
                                       ? &instruction.operands.front()
                                       : nullptr;
        const MirPlace *place =
            source != nullptr && source->kind == MirOperandKind::Address
                ? body.findPlace(source->place)
                : nullptr;
        return instruction.unsafeOperation == UnsafeOperationKind::AddressOf &&
               source != nullptr && place != nullptr &&
               source->type == place->type &&
               rawPointerType(instruction.info.type) &&
               instruction.info.type.arguments.front() == source->type &&
               instruction.info.type.pointerAccess == place->access;
      }
      if (instruction.unsafeOperation !=
              UnsafeOperationKind::PointerArithmetic ||
          instruction.operands.size() != 2 ||
          !exactValueOperand(instruction.operands[0]) ||
          !exactValueOperand(instruction.operands[1])) {
        return false;
      }
      const auto pointerOperand = [&](const MirOperand &operand,
                                      const SemanticType &type) {
        return operand.type == type && rawPointerType(operand.type);
      };
      const auto offsetOperand = [&](const MirOperand &operand) {
        return rawPointerOffsetType(operand.type);
      };
      switch (instruction.operation) {
      case MirOperation::PointerAdd:
        return rawPointerType(instruction.info.type) &&
               ((pointerOperand(instruction.operands[0],
                                instruction.info.type) &&
                 offsetOperand(instruction.operands[1])) ||
                (offsetOperand(instruction.operands[0]) &&
                 pointerOperand(instruction.operands[1],
                                instruction.info.type)));
      case MirOperation::PointerSubtract:
        return rawPointerType(instruction.info.type) &&
               pointerOperand(instruction.operands[0], instruction.info.type) &&
               offsetOperand(instruction.operands[1]);
      case MirOperation::PointerDifference:
        return instruction.info.type == SemanticType::Int64 &&
               rawPointerType(instruction.operands[0].type) &&
               instruction.operands[0].type == instruction.operands[1].type;
      default:
        return false;
      }
    };
    const auto exactNativeCallbackCompute =
        [&](const MirInstruction &instruction) {
          const MirNativeCallbackAdapter *adapter =
              instruction.nativeCallbackAdapter
                  ? program.findNativeCallbackAdapter(
                        *instruction.nativeCallbackAdapter)
                  : nullptr;
          const MirFunctionInstance *target =
              adapter == nullptr
                  ? nullptr
                  : program.findFunctionInstance(adapter->target);
          return instruction.kind == MirInstructionKind::Compute &&
                 instruction.operation == MirOperation::NativeCallback &&
                 instruction.result && !instruction.destination &&
                 !instruction.receiver && instruction.callSite == 0 &&
                 !instruction.callInputRole &&
                 instruction.parameterTypes.empty() &&
                 !instruction.functionTarget &&
                 !instruction.constructorTarget &&
                 instruction.operands.empty() && !instruction.literal &&
                 passiveInfo(instruction.info) && adapter != nullptr &&
                 target != nullptr && adapter->type == instruction.info.type;
        };
    const auto exactRawMemoryInstruction =
        [&](const MirInstruction &instruction) {
          const MirPlace *place = nullptr;
          if (instruction.kind == MirInstructionKind::Load &&
              instruction.operands.size() == 1 &&
              instruction.operands.front().kind == MirOperandKind::Copy) {
            place = body.findPlace(instruction.operands.front().place);
          } else if (instruction.kind == MirInstructionKind::Assign &&
                     instruction.destination) {
            place = body.findPlace(*instruction.destination);
          }
          const std::optional<UnsafeOperationKind> operation =
              rawPlaceOperation(place);
          return operation && instruction.rawMemoryAccess &&
                 instruction.unsafeOperation == *operation;
        };
    const auto exactRawLifecycle = [](const MirInstruction &instruction) {
      return instruction.kind == MirInstructionKind::Lifecycle &&
             !instruction.rawMemoryAccess &&
             (instruction.unsafeOperation ==
                  UnsafeOperationKind::RawDereference ||
              instruction.unsafeOperation == UnsafeOperationKind::RawIndex);
    };
    const auto exactForeignPointerOperation =
        [&](const MirInstruction &instruction) {
          if (instruction.unsafeOperation !=
                  UnsafeOperationKind::ForeignPointerCall ||
              instruction.rawMemoryAccess || instruction.hirValue == 0) {
            return false;
          }
          const auto nativeCall = [&](const MirInstruction &candidate) {
            const MirFunctionInstance *target =
                candidate.kind == MirInstructionKind::Call &&
                        candidate.functionTarget
                    ? program.findFunctionInstance(*candidate.functionTarget)
                    : nullptr;
            return candidate.hirValue == instruction.hirValue &&
                   candidate.unsafeOperation ==
                       UnsafeOperationKind::ForeignPointerCall &&
                   !candidate.rawMemoryAccess && target != nullptr &&
                   target->linkage == LanguageLinkage::C;
          };
          if (instruction.kind == MirInstructionKind::Call) {
            return nativeCall(instruction);
          }
          return instruction.kind == MirInstructionKind::Lifecycle &&
                 std::any_of(body.blocks.begin(), body.blocks.end(),
                             [&](const MirBlock &candidateBlock) {
                               return std::any_of(
                                   candidateBlock.instructions.begin(),
                                   candidateBlock.instructions.end(),
                                   nativeCall);
                             });
        };
    const auto exactDeclaredUnsafeOperation =
        [&](const MirInstruction &instruction) {
          if (instruction.unsafeOperation !=
                  UnsafeOperationKind::DeclaredUnsafeCall ||
              instruction.rawMemoryAccess || instruction.hirValue == 0) {
            return false;
          }
          const auto unsafeCall = [&](const MirInstruction &candidate) {
            const auto *target =
                candidate.kind == MirInstructionKind::Call &&
                        candidate.functionTarget
                    ? program.findFunctionInstance(*candidate.functionTarget)
                    : nullptr;
            return candidate.hirValue == instruction.hirValue &&
                   candidate.unsafeOperation ==
                       UnsafeOperationKind::DeclaredUnsafeCall &&
                   !candidate.rawMemoryAccess && target != nullptr &&
                   target->requiresUnsafe;
          };
          if (instruction.kind == MirInstructionKind::Call) {
            return unsafeCall(instruction);
          }
          return instruction.kind == MirInstructionKind::Lifecycle &&
                 std::any_of(body.blocks.begin(), body.blocks.end(),
                             [&](const MirBlock &candidateBlock) {
                               return std::any_of(
                                   candidateBlock.instructions.begin(),
                                   candidateBlock.instructions.end(),
                                   unsafeCall);
                             });
        };
    // A reference parameter cannot raise by itself: the reference is a
    // compile-proven borrow, and what the body reads through it is
    // guarded by the place rules below.
    const auto passiveParameterType = [&](const SemanticType &type) {
      return passiveType(type) || (type.kind == SemanticType::Reference &&
                                   type.arguments.size() == 1);
    };
    bool valid =
        function.id == id && function.declaration != 0 &&
        function.definitionKind ==
            MirFunctionInstance::DefinitionKind::Source &&
        function.linkage == LanguageLinkage::Gti &&
        function.externalSymbol.empty() &&
        function.parameterTypes.size() == function.parameterBindings.size() &&
        std::all_of(function.parameterTypes.begin(),
                    function.parameterTypes.end(), passiveParameterType) &&
        (function.returnType == SemanticType::Void ||
         passiveType(function.returnType)) &&
        body.kind == MirBodyKind::Function &&
        body.returnType == function.returnType && body.loans.empty() &&
        body.dropObligations.empty() && body.cleanupBoundaries.empty() &&
        verifyMirBody(body, function.id).valid();
    for (const MirPlace &place : body.places) {
      const bool ordinaryScalar =
          (place.root == MirPlaceRootKind::Binding ||
           place.root == MirPlaceRootKind::Temporary) &&
          place.projections.empty() && passiveType(place.type) &&
          place.traits.drop == DropKind::Trivial &&
          !place.traits.containsBorrowedState && place.loan == 0;
      // Namespace globals and static fields are represented as Symbol roots.
      // Reading passive storage cannot itself raise a defined failure; any
      // failing initializer is accounted for by program initialization, not
      // by a later load from the initialized object.
      const bool passiveStaticStorage =
          place.root == MirPlaceRootKind::Symbol && place.binding == 0 &&
          place.symbol != 0 && place.capture == 0 && place.temporary == 0 &&
          place.value == 0 && place.loan == 0 && place.projections.empty() &&
          passiveType(place.type) && place.traits.drop == DropKind::Trivial &&
          !place.traits.containsBorrowedState;
      // A read-only receiver's passive field load cannot raise a defined
      // failure: the bare receiver place is only the projection carrier and
      // the projected place reads one trivially droppable passive field.
      const bool receiverField =
          place.root == MirPlaceRootKind::This && place.binding == 0 &&
          place.symbol == 0 && place.capture == 0 && place.temporary == 0 &&
          place.value == 0 && place.loan == 0 && !place.initiallyAvailable &&
          function.owner.has_value() &&
          ((place.projections.empty() &&
            place.type.kind == SemanticType::Class) ||
           (place.projections.size() == 1 &&
            place.projections.front().kind == MirProjectionKind::Field &&
            passiveType(place.type) && place.traits.drop == DropKind::Trivial &&
            !place.traits.containsBorrowedState));
      // A reference parameter's passive field load cannot raise either: the
      // bare reference binding and its dereference are only projection
      // carriers, and the projected place reads one trivially droppable
      // passive field through the compile-proven borrow.
      const bool referenceParameterField =
          place.root == MirPlaceRootKind::Binding && place.loan == 0 &&
          std::find(function.parameterBindings.begin(),
                    function.parameterBindings.end(),
                    place.binding) != function.parameterBindings.end() &&
          ((place.projections.empty() &&
            place.type.kind == SemanticType::Reference &&
            place.type.arguments.size() == 1) ||
           (!place.projections.empty() &&
            place.projections.front().kind == MirProjectionKind::Dereference &&
            ((place.projections.size() == 1 &&
              place.type.kind == SemanticType::Class) ||
             (place.projections.size() == 2 &&
              place.projections[1].kind == MirProjectionKind::Field &&
              passiveType(place.type) &&
              place.traits.drop == DropKind::Trivial &&
              !place.traits.containsBorrowedState))));
      const bool acceptedPlace = ordinaryScalar || passiveStaticStorage ||
                                 receiverField || referenceParameterField ||
                                 rawPlaceOperation(&place).has_value();
      valid = valid && acceptedPlace;
    }
    for (const MirValue &value : body.values) {
      valid = valid && passiveInfo(value.info);
    }

    std::unordered_set<MirInstructionId> exactCalls;
    for (const MirBlock &block : body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.tombstone) {
          continue;
        }
        if (!valid) {
          break;
        }
        const bool exactRawComputeInstruction = exactRawCompute(instruction);
        const bool exactNativeCallbackInstruction =
            exactNativeCallbackCompute(instruction);
        const bool exactRawMemory = exactRawMemoryInstruction(instruction);
        const bool exactRawBoundary = exactRawLifecycle(instruction);
        const bool exactForeignPointer =
            exactForeignPointerOperation(instruction);
        const bool exactDeclaredUnsafe =
            exactDeclaredUnsafeOperation(instruction);
        const bool admittedOperationMetadata =
            (instruction.unsafeOperation == UnsafeOperationKind::None &&
             !instruction.rawMemoryAccess) ||
            exactRawComputeInstruction || exactRawMemory || exactRawBoundary ||
            exactForeignPointer || exactDeclaredUnsafe;
        const bool common =
            admittedOperationMetadata &&
            instruction.closureCaptureTypes.empty() &&
            instruction.closureCaptureModes.empty() && !instruction.loan &&
            instruction.borrowOrigin == BorrowOriginKind::None &&
            instruction.borrowArgument == 0 &&
            instruction.borrowAccess == AccessMode::ReadOnly &&
            !instruction.borrowPlace && !instruction.enumOwner &&
            !instruction.enumValue && !instruction.enumVariant &&
            !instruction.payloadIndex &&
            (instruction.intrinsic == IntrinsicKind::None ||
             (instruction.kind == MirInstructionKind::Call &&
              wrappingIntrinsic(instruction.intrinsic))) &&
            instruction.synchronization.kind ==
                SynchronizationOperationKind::None &&
            instruction.localFailureSites.empty() &&
            instruction.dispatch == CallDispatch::Static &&
            validStaticDispatchOwner(instruction) &&
            !instruction.constructorTarget &&
            instruction.constructorKind == ConstructorKind::Ordinary &&
            !instruction.lambdaTarget &&
            instruction.callableArguments.empty() &&
            !instruction.callableBoundary && !instruction.callableInvocation &&
            instruction.lifecycle.empty() &&
            !instruction.preparedParameterDrop &&
            !instruction.successResultDrop &&
            !instruction.successResultDestination &&
            instruction.cleanupBoundaryEnd == 0 &&
            scalarOwnership(instruction.ownership);
        if (!common || !instruction.definedFailure.localOrigins.empty()) {
          valid = false;
          break;
        }

        switch (instruction.kind) {
        case MirInstructionKind::Compute:
          // Literals and identity copies may carry any passive value. Other
          // ordinary operations stay scalar. Raw operations and native
          // callback address formation are admitted only by their exact,
          // verifier-backed shapes above.
          valid =
              instruction.callSite == 0 && !instruction.callInputRole &&
              !instruction.destination && !instruction.receiver &&
              instruction.parameterTypes.empty() &&
              !instruction.functionTarget && instruction.result &&
              (exactRawComputeInstruction || exactNativeCallbackInstruction ||
               ((instruction.operation == MirOperation::Literal ||
                 instruction.operation == MirOperation::Identity)
                    ? passiveInfo(instruction.info)
                    : scalarInfo(instruction.info))) &&
              ((scalarOperation(instruction.operation) &&
                stableScalarProvenance(instruction)) ||
               exactRawComputeInstruction || exactNativeCallbackInstruction) &&
              instruction.definedFailure.empty() &&
              instruction.fullExpressionEnd == 0 &&
              (instruction.operation == MirOperation::Literal
                   ? instruction.operands.empty() &&
                         instruction.literal.has_value()
                   : !instruction.literal &&
                         (exactRawComputeInstruction ||
                          exactNativeCallbackInstruction ||
                          std::all_of(instruction.operands.begin(),
                                      instruction.operands.end(),
                                      [](const MirOperand &operand) {
                                        return operand.kind ==
                                               MirOperandKind::Value;
                                      })));
          break;
        case MirInstructionKind::Load:
          valid = instruction.callSite == 0 && !instruction.callInputRole &&
                  !instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && instruction.result &&
                  passiveInfo(instruction.info) &&
                  instruction.operation == MirOperation::None &&
                  !instruction.literal && instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd == 0 &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().kind == MirOperandKind::Copy &&
                  (instruction.unsafeOperation == UnsafeOperationKind::None ||
                   exactRawMemory);
          break;
        case MirInstructionKind::Initialize:
          valid =
              instruction.callSite == 0 && !instruction.callInputRole &&
              instruction.destination && !instruction.receiver &&
              instruction.parameterTypes.empty() &&
              !instruction.functionTarget && !instruction.result &&
              passiveInfo(instruction.info) &&
              instruction.operation == MirOperation::None &&
              !instruction.literal && instruction.definedFailure.empty() &&
              instruction.fullExpressionEnd == 0 &&
              instruction.operands.size() == 1 &&
              (instruction.operands.front().kind == MirOperandKind::Value ||
               instruction.operands.front().kind == MirOperandKind::Constant);
          break;
        case MirInstructionKind::Assign:
          valid = instruction.callSite == 0 && !instruction.callInputRole &&
                  instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && instruction.result &&
                  passiveInfo(instruction.info) &&
                  instruction.operation == MirOperation::Assign &&
                  !instruction.literal && instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd == 0 &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().kind == MirOperandKind::Value &&
                  (instruction.unsafeOperation == UnsafeOperationKind::None ||
                   exactRawMemory);
          break;
        case MirInstructionKind::Lifecycle:
          valid =
              instruction.callSite == 0 && !instruction.callInputRole &&
              !instruction.destination && !instruction.receiver &&
              instruction.parameterTypes.empty() &&
              !instruction.functionTarget && !instruction.result &&
              instruction.operation == MirOperation::None &&
              !instruction.literal && instruction.definedFailure.empty() &&
              instruction.fullExpressionEnd != 0 &&
              instruction.operands.empty() &&
              (instruction.unsafeOperation == UnsafeOperationKind::None ||
               exactRawBoundary || exactForeignPointer || exactDeclaredUnsafe);
          break;
        case MirInstructionKind::CallInput:
          valid = instruction.callSite != 0 &&
                  instruction.callInputRole == MirCallInputRole::Argument &&
                  instruction.callInputKind == HirCallInputKind::Value &&
                  !instruction.destination && !instruction.receiver &&
                  instruction.parameterTypes.empty() &&
                  !instruction.functionTarget && instruction.result &&
                  passiveInfo(instruction.info) &&
                  instruction.operation == MirOperation::None &&
                  !instruction.literal && instruction.definedFailure.empty() &&
                  instruction.fullExpressionEnd == 0 &&
                  instruction.operands.size() == 1 &&
                  instruction.operands.front().kind == MirOperandKind::Value;
          break;
        case MirInstructionKind::Call: {
          if (instruction.intrinsic != IntrinsicKind::None) {
            // A wrapping/saturating arithmetic intrinsic carries no failure
            // channel: None propagation, no sites, scalar operands.
            valid =
                wrappingIntrinsic(instruction.intrinsic) &&
                instruction.callSite != 0 && !instruction.callInputRole &&
                !instruction.destination && !instruction.receiver &&
                !instruction.functionTarget && instruction.result &&
                scalarInfo(instruction.info) &&
                instruction.definedFailure.empty() &&
                instruction.definedFailure.propagation ==
                    FailurePropagationKind::None &&
                !instruction.operands.empty() &&
                instruction.operands.size() <= 2 &&
                std::all_of(instruction.operands.begin(),
                            instruction.operands.end(),
                            [&](const MirOperand &operand) {
                              return operand.kind == MirOperandKind::Value &&
                                     scalarType(operand.type);
                            });
            if (valid) {
              exactCalls.insert(instruction.id);
            }
            break;
          }
          const MirFunctionInstance *target =
              instruction.functionTarget
                  ? program.findFunctionInstance(*instruction.functionTarget)
                  : nullptr;
          valid =
              instruction.callSite != 0 && !instruction.callInputRole &&
              !instruction.destination && !instruction.receiver &&
              instruction.functionTarget && target != nullptr &&
              instruction.parameterTypes == target->parameterTypes &&
              instruction.operands.size() ==
                  instruction.parameterTypes.size() &&
              std::all_of(instruction.parameterTypes.begin(),
                          instruction.parameterTypes.end(), passiveType) &&
              instruction.operation == MirOperation::None &&
              !instruction.literal && !instruction.ownership &&
              (instruction.definedFailure.propagation ==
                   FailurePropagationKind::DirectCall ||
               instruction.definedFailure.propagation ==
                   FailurePropagationKind::None) &&
              instruction.fullExpressionEnd == 0 &&
              instruction.info.type == target->returnType &&
              (instruction.info.type == SemanticType::Void ||
               passiveInfo(instruction.info)) &&
              (instruction.info.type == SemanticType::Void
                   ? !instruction.result
                   : instruction.result.has_value()) &&
              std::all_of(
                  instruction.operands.begin(), instruction.operands.end(),
                  [&](const MirOperand &operand) {
                    if (operand.kind != MirOperandKind::Value ||
                        !passiveType(operand.type)) {
                      return false;
                    }
                    const MirValue *value = body.findValue(operand.value);
                    const MirInstruction *input =
                        value == nullptr
                            ? nullptr
                            : mir_detail::definitionFor(body, operand.value);
                    return input != nullptr &&
                           input->kind == MirInstructionKind::CallInput &&
                           input->callSite == instruction.callSite;
                  }) &&
              (nativeNoFailureTarget(*target)
                   ? instruction.definedFailure.propagation ==
                         FailurePropagationKind::None
                   : self(self, *instruction.functionTarget));
          if (valid) {
            exactCalls.insert(instruction.id);
          }
          break;
        }
        case MirInstructionKind::Move:
        case MirInstructionKind::Borrow:
        case MirInstructionKind::ActivateBorrow:
        case MirInstructionKind::Construct:
        case MirInstructionKind::Drop:
        case MirInstructionKind::EndBorrow:
        case MirInstructionKind::Modify:
        case MirInstructionKind::CallBody:
        case MirInstructionKind::AllocateObject:
        case MirInstructionKind::InitializeBase:
        case MirInstructionKind::CleanupBeforeAssign:
        case MirInstructionKind::Count:
          valid = false;
          break;
        }
      }
    }

    for (const MirFailureRecord &record : body.failureRecords) {
      const MirBlock *producer = body.findBlock(record.producerBlock);
      const MirBlock *parameter = body.findBlock(record.parameterBlock);
      const auto found =
          producer == nullptr
              ? std::vector<MirInstruction>::const_iterator{}
              : std::find_if(producer->instructions.begin(),
                             producer->instructions.end(),
                             [&](const MirInstruction &instruction) {
                               return instruction.id ==
                                      record.producerInstruction;
                             });
      const MirInstruction *instruction =
          producer == nullptr || found == producer->instructions.end()
              ? nullptr
              : &*found;
      valid =
          valid && instruction != nullptr && parameter != nullptr &&
          exactCalls.contains(record.producerInstruction) &&
          producer->terminator.kind == MirTerminatorKind::Invoke &&
          producer->terminator.invokeInstruction ==
              record.producerInstruction &&
          producer->terminator.failureRecord == record.id &&
          parameter->failureParameter == record.id &&
          parameter->instructions.empty() &&
          parameter->terminator.kind == MirTerminatorKind::PropagateFailure &&
          parameter->terminator.failureRecord == record.id;
    }
    for (const MirBlock &block : body.blocks) {
      if (block.failureParameter != 0) {
        const MirFailureRecord *record =
            body.findFailureRecord(block.failureParameter);
        valid = valid && record != nullptr &&
                record->parameterBlock == block.id &&
                block.instructions.empty() &&
                block.terminator.kind == MirTerminatorKind::PropagateFailure;
        continue;
      }
      switch (block.terminator.kind) {
      case MirTerminatorKind::Goto:
      case MirTerminatorKind::Branch:
      case MirTerminatorKind::Switch:
      case MirTerminatorKind::Return:
      case MirTerminatorKind::Unreachable:
        break;
      case MirTerminatorKind::Invoke:
        valid = valid &&
                exactCalls.contains(block.terminator.invokeInstruction) &&
                block.terminator.failureRecord != 0;
        break;
      case MirTerminatorKind::PropagateFailure:
      case MirTerminatorKind::None:
      case MirTerminatorKind::Exit:
      case MirTerminatorKind::ContainFailure:
      case MirTerminatorKind::TerminateCleanupFailure:
        valid = false;
        break;
      }
    }

    state = State::Complete;
    mayRaise[id - 1] = !valid;
    return valid;
  };

  for (const MirFunctionInstance &function : program.functionInstances()) {
    (void)canProveNoFailure(canProveNoFailure, function.id);
  }
  return mayRaise;
}

namespace {

class MirOwnedFailureClosure {
public:
  MirOwnedFailureClosure(const MirProgram &program,
                         MirDefinedFailureEffects &effects)
      : program(program), effects(effects),
        functionStates(program.functionInstances().size()),
        constructorStates(program.constructorInstances().size()),
        destructorStates(program.destructorInstances().size()) {}

  void derive() {
    for (const MirDestructorInstance &destructor :
         program.destructorInstances()) {
      (void)proveDestructor(destructor.id);
    }
    for (const MirConstructorInstance &constructor :
         program.constructorInstances()) {
      (void)proveConstructor(constructor.id);
    }
    for (const MirFunctionInstance &function : program.functionInstances()) {
      (void)proveFunction(function.id);
    }
  }

private:
  enum class State : unsigned char { Unvisited, Visiting, Complete };

  [[nodiscard]] static bool scalarType(const SemanticType &type) {
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
  }

  [[nodiscard]] static bool scalarTraits(const SemanticTypeTraits &traits,
                                         const SemanticType &type) {
    return scalarType(type) && traits.drop == DropKind::Trivial &&
           !traits.containsBorrowedState;
  }

  [[nodiscard]] static bool callbackType(const SemanticType &type) {
    return type.kind == SemanticType::NativeFunction &&
           type.hasNativeFunctionShape();
  }

  [[nodiscard]] static bool passiveValueTraits(const SemanticTypeTraits &traits,
                                               const SemanticType &type) {
    return (scalarType(type) || callbackType(type)) &&
           traits.drop == DropKind::Trivial && !traits.containsBorrowedState;
  }

  [[nodiscard]] static bool
  passiveConstructorValueType(const SemanticType &type) {
    if (scalarType(type)) {
      return true;
    }
    return type.kind == SemanticType::Array && type.arguments.size() == 1 &&
           type.arrayLength != 0 && type.arrayLengthParameterId == 0 &&
           passiveConstructorValueType(type.arguments.front());
  }

  [[nodiscard]] static bool
  passiveConstructorValueTraits(const SemanticTypeTraits &traits,
                                const SemanticType &type) {
    return passiveConstructorValueType(type) &&
           traits.drop == DropKind::Trivial && !traits.containsBorrowedState;
  }

  [[nodiscard]] static bool scalarOperation(MirOperation operation) {
    switch (operation) {
    case MirOperation::Literal:
    case MirOperation::Identity:
    case MirOperation::BitwiseAnd:
    case MirOperation::BitwiseOr:
    case MirOperation::BitwiseXor:
    case MirOperation::Equal:
    case MirOperation::NotEqual:
    case MirOperation::Less:
    case MirOperation::LessEqual:
    case MirOperation::Greater:
    case MirOperation::GreaterEqual:
    case MirOperation::Positive:
    case MirOperation::LogicalNot:
    case MirOperation::BitwiseNot:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] const MirClassInstance *
  classForType(const SemanticType &type) const {
    if (type.kind != SemanticType::Class) {
      return nullptr;
    }
    const MirClassInstance *selected = nullptr;
    for (const MirClassInstance &candidate : program.classInstances()) {
      if (candidate.type != type) {
        continue;
      }
      if (selected != nullptr) {
        return nullptr;
      }
      selected = &candidate;
    }
    if (selected == nullptr || selected->id == 0 ||
        selected->declaration == 0 || !selected->bases.empty() ||
        !selected->structuralBases.empty() || selected->abstract ||
        selected->polymorphic || selected->cAbiRecord || selected->cAbiLayout ||
        selected->unionLayout || !selected->fields.empty() ||
        !selected->fieldDropOrder.empty() ||
        !selected->fieldInitializers.values.empty() ||
        !selected->fieldInitializers.loans.empty() ||
        !selected->fieldInitializers.dropObligations.empty() ||
        !selected->fieldInitializers.failureRecords.empty() ||
        !selected->staticFieldInitializers.places.empty() ||
        !selected->staticFieldInitializers.values.empty() ||
        !selected->staticFieldInitializers.loans.empty() ||
        !selected->staticFieldInitializers.dropObligations.empty() ||
        !selected->staticFieldInitializers.failureRecords.empty() ||
        !std::all_of(selected->declaredFields.begin(),
                     selected->declaredFields.end(),
                     [](const MirClassFieldInfo &field) {
                       return field.field != 0 && field.symbol != 0 &&
                              scalarType(field.type) &&
                              field.dropKind == DropKind::Trivial &&
                              !field.requiresActiveCleanup;
                     })) {
      return nullptr;
    }
    if (!selected->destructor || !selected->requiresActiveDropState ||
        !selected->requiresActiveCleanup) {
      return nullptr;
    }
    const MirDestructorInstance *destructor =
        program.findDestructorInstance(*selected->destructor);
    return destructor != nullptr && destructor->owner == selected->id ? selected
                                                                      : nullptr;
  }

  [[nodiscard]] bool eligibleType(const SemanticType &type) const {
    if (type == SemanticType::Void || scalarType(type) || callbackType(type) ||
        classForType(type) != nullptr) {
      return true;
    }
    return type.kind == SemanticType::Reference && type.arguments.size() == 1 &&
           classForType(type.arguments.front()) != nullptr;
  }

  // A source constructor that initializes every passive scalar/fixed-array
  // field from a matching passive value has no hidden class lifecycle work.
  // In-class defaults are not evaluated for fields explicitly named by that
  // constructor, so they do not make this exact constructor failure-capable.
  // Keep this proof separate from classForType(): that helper admits the
  // destructor-owning closure used by function/drop analysis, while this
  // shape deliberately has no destructor or active cleanup.
  [[nodiscard]] bool
  passiveConstructorOwner(const MirClassInstance &owner) const {
    return owner.id != 0 && owner.declaration != 0 && owner.bases.empty() &&
           owner.structuralBases.empty() && !owner.abstract &&
           !owner.polymorphic && !owner.cAbiRecord && !owner.cAbiLayout &&
           !owner.unionLayout && !owner.destructor &&
           !owner.requiresActiveDropState && !owner.requiresActiveCleanup &&
           owner.fields.empty() && owner.fieldDropOrder.empty() &&
           std::all_of(owner.declaredFields.begin(), owner.declaredFields.end(),
                       [](const MirClassFieldInfo &field) {
                         return field.field != 0 && field.symbol != 0 &&
                                passiveConstructorValueType(field.type) &&
                                field.dropKind == DropKind::Trivial &&
                                !field.requiresActiveCleanup;
                       }) &&
           std::count_if(program.classInstances().begin(),
                         program.classInstances().end(),
                         [&](const MirClassInstance &candidate) {
                           return candidate.type == owner.type;
                         }) == 1;
  }

  [[nodiscard]] static bool
  commonInstruction(const MirInstruction &instruction) {
    return instruction.unsafeOperation == UnsafeOperationKind::None &&
           !instruction.rawMemoryAccess &&
           instruction.closureCaptureTypes.empty() &&
           instruction.closureCaptureModes.empty() && !instruction.loan &&
           instruction.borrowOrigin == BorrowOriginKind::None &&
           instruction.borrowArgument == 0 &&
           instruction.borrowAccess == AccessMode::ReadOnly &&
           !instruction.borrowPlace && !instruction.enumOwner &&
           !instruction.enumValue && !instruction.enumVariant &&
           !instruction.payloadIndex &&
           (instruction.intrinsic == IntrinsicKind::None ||
            (instruction.kind == MirInstructionKind::Move &&
             instruction.intrinsic == IntrinsicKind::Move)) &&
           instruction.synchronization.kind ==
               SynchronizationOperationKind::None &&
           instruction.definedFailure.localOrigins.empty() &&
           instruction.localFailureSites.empty() &&
           instruction.dispatch == CallDispatch::Static &&
           instruction.dispatchOwner == SemanticType::Unknown &&
           !instruction.lambdaTarget && instruction.callableArguments.empty() &&
           !instruction.callableBoundary && !instruction.callableInvocation;
  }

  [[nodiscard]] bool proveFunction(HirFunctionInstanceId id);
  [[nodiscard]] bool proveConstructor(HirConstructorInstanceId id);
  [[nodiscard]] bool proveDestructor(HirDestructorInstanceId id);
  [[nodiscard]] bool
  proveSingleFieldTransferConstructor(const MirConstructorInstance &constructor,
                                      const MirClassInstance &owner) const;
  [[nodiscard]] bool proveBody(const MirBody &body, MirBodyKind kind,
                               std::size_t owner,
                               const MirClassInstance *ownerClass);

  const MirProgram &program;
  MirDefinedFailureEffects &effects;
  std::vector<State> functionStates;
  std::vector<State> constructorStates;
  std::vector<State> destructorStates;
};

bool MirOwnedFailureClosure::proveBody(const MirBody &body, MirBodyKind kind,
                                       std::size_t owner,
                                       const MirClassInstance *ownerClass) {
  if (body.kind != kind || body.blocks.empty() || !body.loans.empty() ||
      !verifyMirBody(body, owner).valid()) {
    return false;
  }
  const auto eligibleBodyType = [&](const SemanticType &type) {
    return eligibleType(type) ||
           (kind == MirBodyKind::Constructor && ownerClass != nullptr &&
            passiveConstructorOwner(*ownerClass) &&
            (type == ownerClass->type || passiveConstructorValueType(type)));
  };
  for (const MirPlace &place : body.places) {
    if (!eligibleBodyType(place.type) || place.root == MirPlaceRootKind::Loan ||
        place.loan != 0 || place.capture != 0) {
      return false;
    }
    if (passiveConstructorValueType(place.type) &&
        !passiveConstructorValueTraits(place.traits, place.type)) {
      return false;
    }
    if (place.root == MirPlaceRootKind::This) {
      if (ownerClass == nullptr || place.projections.size() > 1 ||
          (!place.projections.empty() &&
           (place.projections.front().kind != MirProjectionKind::Field ||
            std::none_of(ownerClass->declaredFields.begin(),
                         ownerClass->declaredFields.end(),
                         [&](const MirClassFieldInfo &field) {
                           return field.symbol ==
                                      place.projections.front().field &&
                                  field.type == place.type;
                         })))) {
        return false;
      }
    } else if (!place.projections.empty()) {
      return false;
    }
  }
  for (const MirValue &value : body.values) {
    if (!eligibleBodyType(value.info.type) ||
        (callbackType(value.info.type) &&
         !passiveValueTraits(value.info.traits, value.info.type)) ||
        (passiveConstructorValueType(value.info.type) &&
         !passiveConstructorValueTraits(value.info.traits, value.info.type))) {
      return false;
    }
  }
  for (const MirDropObligation &drop : body.dropObligations) {
    const MirClassInstance *dropClass = classForType(drop.dropType.type);
    if (dropClass == nullptr || drop.dropType.classInstance != dropClass->id ||
        drop.dropType.destructor != dropClass->destructor ||
        !drop.dropType.requiresActiveCleanup || !dropClass->destructor ||
        !proveDestructor(*dropClass->destructor)) {
      return false;
    }
  }

  std::unordered_set<MirInstructionId> provedInvokeEdges;
  for (const MirBlock &block : body.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (!commonInstruction(instruction)) {
        return false;
      }
      switch (instruction.kind) {
      case MirInstructionKind::Compute: {
        const bool passiveAggregate =
            kind == MirBodyKind::Constructor && ownerClass != nullptr &&
            passiveConstructorOwner(*ownerClass) &&
            instruction.operation == MirOperation::Aggregate &&
            instruction.info.type.kind == SemanticType::Array &&
            instruction.info.type.arguments.size() == 1 &&
            instruction.info.type.arrayLengthParameterId == 0 &&
            instruction.operands.size() == instruction.info.type.arrayLength &&
            passiveConstructorValueTraits(instruction.info.traits,
                                          instruction.info.type) &&
            std::all_of(instruction.operands.begin(),
                        instruction.operands.end(),
                        [&](const MirOperand &operand) {
                          return operand.kind == MirOperandKind::Value &&
                                 operand.type ==
                                     instruction.info.type.arguments.front();
                        });
        const bool scalar =
            scalarTraits(instruction.info.traits, instruction.info.type) &&
            scalarOperation(instruction.operation);
        const MirNativeCallbackAdapter *callbackAdapter =
            instruction.nativeCallbackAdapter
                ? program.findNativeCallbackAdapter(
                      *instruction.nativeCallbackAdapter)
                : nullptr;
        const bool nativeCallback =
            instruction.operation == MirOperation::NativeCallback &&
            passiveValueTraits(instruction.info.traits,
                               instruction.info.type) &&
            callbackAdapter != nullptr &&
            callbackAdapter->type == instruction.info.type &&
            program.findFunctionInstance(callbackAdapter->target) != nullptr &&
            instruction.operands.empty() && !instruction.literal;
        if (!instruction.result || instruction.destination ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.definedFailure.propagation !=
                FailurePropagationKind::None ||
            (!scalar && !passiveAggregate && !nativeCallback) ||
            (instruction.operation == MirOperation::Literal
                 ? !instruction.literal || !instruction.operands.empty()
                 : instruction.literal.has_value())) {
          return false;
        }
        break;
      }
      case MirInstructionKind::Load:
        if (!instruction.result || instruction.destination ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            (instruction.operands.front().kind != MirOperandKind::Copy &&
             instruction.operands.front().kind != MirOperandKind::Address &&
             instruction.operands.front().kind != MirOperandKind::Move)) {
          return false;
        }
        break;
      case MirInstructionKind::Initialize:
        if (!instruction.destination || instruction.result ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            (instruction.operands.front().kind != MirOperandKind::Value &&
             instruction.operands.front().kind != MirOperandKind::Constant)) {
          return false;
        }
        break;
      case MirInstructionKind::Assign:
        if (!instruction.destination || !instruction.result ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            !instruction.lifecycle.empty() ||
            instruction.operation != MirOperation::Assign ||
            instruction.literal || instruction.operands.size() != 1 ||
            instruction.operands.front().kind != MirOperandKind::Value ||
            !passiveValueTraits(instruction.info.traits,
                                instruction.info.type)) {
          return false;
        }
        break;
      case MirInstructionKind::Move:
        if (!instruction.result || instruction.destination ||
            instruction.receiver || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.functionTarget ||
            instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            classForType(instruction.info.type) == nullptr ||
            instruction.lifecycle.empty()) {
          return false;
        }
        break;
      case MirInstructionKind::CallInput: {
        const bool scalar = scalarType(instruction.info.type) ||
                            callbackType(instruction.info.type);
        const bool owned = classForType(instruction.info.type) != nullptr;
        const MirDropObligation *prepared =
            instruction.preparedParameterDrop
                ? body.findDropObligation(*instruction.preparedParameterDrop)
                : nullptr;
        if (instruction.callSite == 0 ||
            instruction.callInputRole != MirCallInputRole::Argument ||
            !instruction.result || instruction.receiver ||
            instruction.functionTarget || instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || instruction.operands.size() != 1 ||
            (scalar &&
             (instruction.callInputKind != HirCallInputKind::Value ||
              instruction.destination || instruction.preparedParameterDrop ||
              !instruction.lifecycle.empty())) ||
            (owned &&
             (instruction.callInputKind != HirCallInputKind::MoveValue ||
              !instruction.destination || prepared == nullptr ||
              prepared->place != *instruction.destination ||
              instruction.lifecycle.empty())) ||
            (!scalar && !owned)) {
          return false;
        }
        break;
      }
      case MirInstructionKind::Call: {
        const MirFunctionInstance *target =
            instruction.functionTarget
                ? program.findFunctionInstance(*instruction.functionTarget)
                : nullptr;
        if (instruction.callSite == 0 || instruction.callInputRole ||
            instruction.destination || instruction.receiver ||
            !instruction.functionTarget || target == nullptr ||
            instruction.constructorTarget ||
            instruction.parameterTypes != target->parameterTypes ||
            instruction.operands.size() != target->parameterTypes.size() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !proveFunction(target->id) ||
            (instruction.definedFailure.propagation !=
                 FailurePropagationKind::DirectCall &&
             instruction.definedFailure.propagation !=
                 FailurePropagationKind::None)) {
          return false;
        }
        provedInvokeEdges.insert(instruction.id);
        break;
      }
      case MirInstructionKind::Construct: {
        const MirConstructorInstance *target =
            instruction.constructorTarget ? program.findConstructorInstance(
                                                *instruction.constructorTarget)
                                          : nullptr;
        const MirClassInstance *constructed =
            classForType(instruction.info.type);
        const bool generatedDefault =
            !instruction.constructorTarget && constructed != nullptr &&
            constructed->declaredFields.empty() &&
            instruction.operands.empty() && instruction.parameterTypes.empty();
        if (instruction.callInputRole || instruction.destination ||
            instruction.receiver || instruction.functionTarget ||
            instruction.constructorKind != ConstructorKind::Ordinary ||
            constructed == nullptr ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !instruction.result ||
            instruction.lifecycle.empty() ||
            (!generatedDefault &&
             (target == nullptr || target->owner != constructed->id ||
              instruction.parameterTypes != target->parameterTypes ||
              instruction.operands.size() != target->parameterTypes.size() ||
              !proveConstructor(target->id))) ||
            (instruction.definedFailure.propagation !=
                 FailurePropagationKind::Constructor &&
             instruction.definedFailure.propagation !=
                 FailurePropagationKind::None)) {
          return false;
        }
        provedInvokeEdges.insert(instruction.id);
        break;
      }
      case MirInstructionKind::Drop: {
        const MirDropObligation *drop =
            instruction.lifecycle.size() == 1
                ? body.findDropObligation(instruction.lifecycle.front().source)
                : nullptr;
        if (!instruction.destination || instruction.result ||
            !instruction.operands.empty() || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.receiver ||
            instruction.functionTarget || instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || drop == nullptr ||
            instruction.lifecycle.front().kind != MirLifecycleEventKind::Drop ||
            drop->place != *instruction.destination) {
          return false;
        }
        break;
      }
      case MirInstructionKind::Lifecycle:
        if (instruction.result || instruction.destination ||
            !instruction.operands.empty() || instruction.callSite != 0 ||
            instruction.callInputRole || instruction.receiver ||
            instruction.functionTarget || instruction.constructorTarget ||
            !instruction.parameterTypes.empty() ||
            instruction.operation != MirOperation::None ||
            instruction.literal || !instruction.lifecycle.empty() ||
            (instruction.fullExpressionEnd == 0 &&
             instruction.cleanupBoundaryEnd == 0)) {
          return false;
        }
        break;
      case MirInstructionKind::Modify:
      case MirInstructionKind::Borrow:
      case MirInstructionKind::ActivateBorrow:
      case MirInstructionKind::EndBorrow:
      case MirInstructionKind::CallBody:
      case MirInstructionKind::AllocateObject:
      case MirInstructionKind::InitializeBase:
      case MirInstructionKind::CleanupBeforeAssign:
      case MirInstructionKind::Count:
        return false;
      }
    }
  }

  for (const MirBlock &block : body.blocks) {
    if (block.failureParameter != 0) {
      const MirFailureRecord *record =
          body.findFailureRecord(block.failureParameter);
      if (record == nullptr || record->parameterBlock != block.id ||
          !provedInvokeEdges.contains(record->producerInstruction)) {
        return false;
      }
      continue;
    }
    switch (block.terminator.kind) {
    case MirTerminatorKind::Goto:
    case MirTerminatorKind::Branch:
    case MirTerminatorKind::Switch:
    case MirTerminatorKind::Return:
    case MirTerminatorKind::Unreachable:
      break;
    case MirTerminatorKind::Invoke:
      if (!provedInvokeEdges.contains(block.terminator.invokeInstruction) ||
          block.terminator.failureRecord == 0) {
        return false;
      }
      break;
    case MirTerminatorKind::PropagateFailure: {
      const MirFailureRecord *record =
          body.findFailureRecord(block.terminator.failureRecord);
      if (record == nullptr ||
          !provedInvokeEdges.contains(record->producerInstruction)) {
        return false;
      }
      break;
    }
    case MirTerminatorKind::None:
    case MirTerminatorKind::Exit:
    case MirTerminatorKind::ContainFailure:
    case MirTerminatorKind::TerminateCleanupFailure:
      return false;
    }
  }
  return true;
}

bool MirOwnedFailureClosure::proveSingleFieldTransferConstructor(
    const MirConstructorInstance &constructor,
    const MirClassInstance &owner) const {
  if (constructor.id == 0 || constructor.owner != owner.id ||
      constructor.definitionKind != MirDefinitionKind::Source ||
      constructor.borrowOrigin != BorrowOriginKind::None || owner.id == 0 ||
      owner.declaration == 0 || !owner.bases.empty() ||
      !owner.structuralBases.empty() || owner.abstract || owner.polymorphic ||
      owner.cAbiRecord || owner.cAbiLayout || owner.unionLayout ||
      owner.destructor || owner.requiresActiveDropState ||
      owner.destructorStatus != SpecialMemberStatus::Generated ||
      owner.moveConstructor != SpecialMemberStatus::Generated ||
      owner.declaredFields.size() != 1 || owner.fields.size() != 1 ||
      owner.fieldDropOrder.size() != 1 ||
      constructor.parameterTypes.size() != 1 ||
      constructor.parameterBindings.size() != 1 ||
      constructor.initializers.size() != 1) {
    return false;
  }

  const MirClassFieldInfo &field = owner.declaredFields.front();
  const bool intrinsicTransferField =
      field.type.kind == SemanticType::UniqueOwner ||
      field.type.kind == SemanticType::SharedPointer ||
      field.type.kind == SemanticType::Storage ||
      field.type.kind == SemanticType::PrefixStorage;
  const MirConstructorInitializer &initializer =
      constructor.initializers.front();
  if (!intrinsicTransferField || field.dropKind != DropKind::Lexical ||
      !field.requiresActiveCleanup ||
      constructor.parameterTypes.front() != field.type ||
      initializer.kind != ConstructorInitializerTargetKind::Field ||
      initializer.targetType != field.type ||
      initializer.field != field.symbol || initializer.base ||
      initializer.constructorTarget || initializer.arguments.size() != 1 ||
      initializer.storesReference || initializer.generatedDefault ||
      initializer.ownedParameter != 0 ||
      !mirTypeMoveIsDefinedFailureFree(program, field.type)) {
    return false;
  }

  const MirBody &body = constructor.body;
  if (body.kind != MirBodyKind::Constructor ||
      body.returnType != SemanticType::Void || body.blocks.size() != 1 ||
      body.places.size() != 3 || !body.loans.empty() ||
      body.fullExpressions.size() != 1 || body.cleanupBoundaries.size() != 1 ||
      body.dropObligations.size() != 3 || !body.failureRecords.empty() ||
      body.values.size() != 1 || !verifyMirBody(body, constructor.id).valid()) {
    return false;
  }

  const MirPlace *parameterPlace = nullptr;
  const MirPlace *temporaryPlace = nullptr;
  const MirPlace *fieldPlace = nullptr;
  for (const MirPlace &place : body.places) {
    if (place.root == MirPlaceRootKind::Binding &&
        place.binding == constructor.parameterBindings.front() &&
        place.projections.empty() && place.type == field.type) {
      parameterPlace = &place;
      continue;
    }
    if (place.root == MirPlaceRootKind::Value && place.projections.empty() &&
        place.type == field.type) {
      temporaryPlace = &place;
      continue;
    }
    if (place.root == MirPlaceRootKind::This && place.projections.size() == 1 &&
        place.projections.front().kind == MirProjectionKind::Field &&
        place.projections.front().field == field.symbol &&
        place.type == field.type) {
      fieldPlace = &place;
    }
  }
  if (parameterPlace == nullptr || temporaryPlace == nullptr ||
      fieldPlace == nullptr || !parameterPlace->initiallyAvailable ||
      temporaryPlace->initiallyAvailable || fieldPlace->initiallyAvailable) {
    return false;
  }

  const MirValue &value = body.values.front();
  if (value.sourceValue != initializer.arguments.front() ||
      value.info.type != field.type || temporaryPlace->value != value.id ||
      temporaryPlace->sourceValue != initializer.arguments.front()) {
    return false;
  }

  const MirDropObligation *parameterDrop = nullptr;
  const MirDropObligation *temporaryDrop = nullptr;
  const MirDropObligation *fieldDrop = nullptr;
  for (const MirDropObligation &drop : body.dropObligations) {
    if (drop.dropType.type != field.type ||
        !drop.dropType.requiresActiveCleanup || drop.dropType.classInstance ||
        drop.dropType.lambdaInstance || drop.dropType.destructor) {
      return false;
    }
    if (drop.kind == MirDropObligationKind::Binding &&
        drop.place == parameterPlace->id &&
        drop.binding == constructor.parameterBindings.front()) {
      parameterDrop = &drop;
    } else if (drop.kind == MirDropObligationKind::Value &&
               drop.place == temporaryPlace->id &&
               drop.value == initializer.arguments.front()) {
      temporaryDrop = &drop;
    } else if (drop.kind == MirDropObligationKind::ConstructionRollback &&
               drop.place == fieldPlace->id) {
      fieldDrop = &drop;
    } else {
      return false;
    }
  }
  if (parameterDrop == nullptr || temporaryDrop == nullptr ||
      fieldDrop == nullptr || !parameterDrop->initiallyActive ||
      temporaryDrop->initiallyActive || fieldDrop->initiallyActive) {
    return false;
  }

  const MirFullExpression &fullExpression = body.fullExpressions.front();
  const MirCleanupBoundary &cleanup = body.cleanupBoundaries.front();
  const MirBlock &block = body.blocks.front();
  if (fullExpression.id == 0 || fullExpression.constructorInitializer != 1 ||
      fullExpression.roots != initializer.arguments || cleanup.id == 0 ||
      cleanup.kind != MirCleanupBoundaryKind::Normal ||
      cleanup.obligations !=
          std::vector<MirDropObligationId>{parameterDrop->id} ||
      !block.reachable || block.failureParameter != 0 ||
      block.activeFailure != 0 || block.instructions.size() != 6 ||
      block.terminator.kind != MirTerminatorKind::Return ||
      block.terminator.value || block.terminator.returnLoan ||
      block.terminator.invokeInstruction != 0 ||
      block.terminator.failureRecord != 0) {
    return false;
  }

  for (const MirInstruction &instruction : block.instructions) {
    if (!commonInstruction(instruction) || instruction.callSite != 0 ||
        instruction.callInputRole || instruction.callInputIndex != 0 ||
        instruction.preparedParameterDrop || instruction.successResultDrop ||
        instruction.successResultDestination || instruction.receiver ||
        !instruction.parameterTypes.empty() || instruction.functionTarget ||
        instruction.constructorTarget || instruction.bodyTarget ||
        instruction.operation != MirOperation::None || instruction.literal ||
        instruction.definedFailure.propagation !=
            FailurePropagationKind::None) {
      return false;
    }
  }

  const MirInstruction &move = block.instructions[0];
  const MirInstruction &initialize = block.instructions[1];
  const MirInstruction &fullEnd = block.instructions[2];
  const MirInstruction &transfer = block.instructions[3];
  const MirInstruction &drop = block.instructions[4];
  const MirInstruction &cleanupEnd = block.instructions[5];
  const auto exactLifecycle =
      [](const MirInstruction &instruction, MirLifecycleEventKind kind,
         MirDropObligationId source, MirDropObligationId target) {
        return instruction.lifecycle.size() == 1 &&
               instruction.lifecycle.front().kind == kind &&
               instruction.lifecycle.front().source == source &&
               instruction.lifecycle.front().target == target &&
               !instruction.lifecycle.front().conditional &&
               !instruction.lifecycle.front().failureCleanup;
      };
  return move.kind == MirInstructionKind::Move && move.result == value.id &&
         move.hirValue == initializer.arguments.front() &&
         move.operands.size() == 1 &&
         move.operands.front().kind == MirOperandKind::Move &&
         move.operands.front().place == parameterPlace->id &&
         move.info.type == field.type &&
         exactLifecycle(move, MirLifecycleEventKind::Move, parameterDrop->id,
                        temporaryDrop->id) &&
         initialize.kind == MirInstructionKind::Initialize &&
         !initialize.result && initialize.destination == fieldPlace->id &&
         initialize.constructorInitializer == 1 &&
         initialize.hirValue == initializer.arguments.front() &&
         initialize.operands.size() == 1 &&
         initialize.operands.front().kind == MirOperandKind::Value &&
         initialize.operands.front().value == value.id &&
         initialize.info.type == field.type &&
         exactLifecycle(initialize, MirLifecycleEventKind::Reparent,
                        temporaryDrop->id, fieldDrop->id) &&
         fullEnd.kind == MirInstructionKind::Lifecycle && !fullEnd.result &&
         !fullEnd.destination && fullEnd.operands.empty() &&
         fullEnd.lifecycle.empty() &&
         fullEnd.fullExpressionEnd == fullExpression.id &&
         fullEnd.cleanupBoundaryEnd == 0 &&
         transfer.kind == MirInstructionKind::Lifecycle && !transfer.result &&
         !transfer.destination && transfer.operands.empty() &&
         exactLifecycle(transfer, MirLifecycleEventKind::TransferOut,
                        fieldDrop->id, 0) &&
         drop.kind == MirInstructionKind::Drop && !drop.result &&
         drop.destination == parameterPlace->id && drop.operands.empty() &&
         drop.info.type == field.type &&
         exactLifecycle(drop, MirLifecycleEventKind::Drop, parameterDrop->id,
                        0) &&
         cleanupEnd.kind == MirInstructionKind::Lifecycle &&
         !cleanupEnd.result && !cleanupEnd.destination &&
         cleanupEnd.operands.empty() && cleanupEnd.lifecycle.empty() &&
         cleanupEnd.fullExpressionEnd == 0 &&
         cleanupEnd.cleanupBoundaryEnd == cleanup.id;
}

bool MirOwnedFailureClosure::proveDestructor(HirDestructorInstanceId id) {
  if (id == 0 || id > program.destructorInstances().size()) {
    return false;
  }
  if (!effects.destructors[id - 1]) {
    return true;
  }
  State &state = destructorStates[id - 1];
  if (state == State::Complete) {
    return !effects.destructors[id - 1];
  }
  if (state == State::Visiting) {
    return false;
  }
  state = State::Visiting;
  const MirDestructorInstance &destructor =
      program.destructorInstances()[id - 1];
  const MirClassInstance *owner = program.findClassInstance(destructor.owner);
  const bool valid =
      destructor.id == id && owner != nullptr &&
      owner->destructor == destructor.id &&
      classForType(owner->type) == owner &&
      destructor.definitionKind == MirDefinitionKind::Source &&
      destructor.body.returnType == SemanticType::Void &&
      proveBody(destructor.body, MirBodyKind::Destructor, destructor.id, owner);
  state = State::Complete;
  effects.destructors[id - 1] = !valid;
  return valid;
}

bool MirOwnedFailureClosure::proveConstructor(HirConstructorInstanceId id) {
  if (id == 0 || id > program.constructorInstances().size()) {
    return false;
  }
  State &state = constructorStates[id - 1];
  if (state == State::Complete) {
    return !effects.constructors[id - 1];
  }
  if (state == State::Visiting) {
    return false;
  }
  state = State::Visiting;
  const MirConstructorInstance &constructor =
      program.constructorInstances()[id - 1];
  const MirClassInstance *owner = program.findClassInstance(constructor.owner);
  bool valid = owner != nullptr &&
               proveSingleFieldTransferConstructor(constructor, *owner);
  if (!valid) {
    valid = constructor.id == id && owner != nullptr &&
            (classForType(owner->type) == owner ||
             passiveConstructorOwner(*owner)) &&
            constructor.definitionKind == MirDefinitionKind::Source &&
            constructor.borrowOrigin == BorrowOriginKind::None &&
            constructor.parameterTypes.size() ==
                constructor.parameterBindings.size() &&
            std::all_of(constructor.parameterTypes.begin(),
                        constructor.parameterTypes.end(), scalarType) &&
            constructor.initializers.size() == owner->declaredFields.size() &&
            constructor.body.fullExpressions.size() ==
                constructor.initializers.size() &&
            proveBody(constructor.body, MirBodyKind::Constructor,
                      constructor.id, owner);
  }

  if (valid && owner != nullptr &&
      proveSingleFieldTransferConstructor(constructor, *owner)) {
    state = State::Complete;
    effects.constructors[id - 1] = false;
    return true;
  }

  std::unordered_set<SymbolId> fields;
  std::unordered_set<std::size_t> stages;
  std::unordered_set<std::size_t> boundaries;
  for (std::size_t index = 0; valid && index < constructor.initializers.size();
       ++index) {
    const MirConstructorInitializer &initializer =
        constructor.initializers[index];
    const MirClassFieldInfo &field = owner->declaredFields[index];
    valid = initializer.kind == ConstructorInitializerTargetKind::Field &&
            initializer.field == field.symbol &&
            fields.insert(initializer.field).second &&
            initializer.targetType == field.type && !initializer.base &&
            !initializer.constructorTarget &&
            initializer.arguments.size() == 1 && !initializer.storesReference &&
            !initializer.generatedDefault && !initializer.ownedParameter;
    if (!valid) {
      break;
    }
    const MirInstruction *stage = nullptr;
    const MirInstruction *boundary = nullptr;
    for (const MirBlock &block : constructor.body.blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.kind == MirInstructionKind::Initialize &&
            instruction.constructorInitializer == index + 1) {
          if (stage != nullptr) {
            valid = false;
          }
          stage = &instruction;
        }
        if (instruction.kind == MirInstructionKind::Lifecycle &&
            instruction.fullExpressionEnd == index + 1) {
          if (boundary != nullptr) {
            valid = false;
          }
          boundary = &instruction;
        }
      }
    }
    const MirPlace *destination =
        stage != nullptr && stage->destination
            ? constructor.body.findPlace(*stage->destination)
            : nullptr;
    const auto full = std::find_if(
        constructor.body.fullExpressions.begin(),
        constructor.body.fullExpressions.end(),
        [&](const auto &candidate) { return candidate.id == index + 1; });
    // Passive arrays now publish through the same explicit MIR field write as
    // scalars. An Aggregate value alone proves computation, not initialization
    // of this field; require its exact stage instead of reconstructing a write
    // from the declaration's initializer inventory.
    const bool passiveStage =
        passiveConstructorValueType(initializer.targetType) &&
        stage != nullptr && !stage->result && stage->destination &&
        stage->hirValue == initializer.arguments.front() &&
        stage->operation == MirOperation::None && !stage->literal &&
        stage->operands.size() == 1 &&
        (stage->operands.front().kind == MirOperandKind::Value ||
         stage->operands.front().kind == MirOperandKind::Constant) &&
        stage->operands.front().type == initializer.targetType &&
        stage->info.type == initializer.targetType && destination != nullptr &&
        destination->root == MirPlaceRootKind::This &&
        destination->projections.size() == 1 &&
        destination->projections.front().kind == MirProjectionKind::Field &&
        destination->projections.front().field == initializer.field &&
        destination->sourceValue == initializer.arguments.front() &&
        destination->type == initializer.targetType &&
        stages.insert(index + 1).second;
    valid = valid && passiveStage && boundary != nullptr &&
            boundaries.insert(index + 1).second &&
            full != constructor.body.fullExpressions.end() &&
            full->constructorInitializer == index + 1 &&
            full->roots == initializer.arguments;
  }
  valid = valid && fields.size() == owner->declaredFields.size() &&
          stages.size() == constructor.initializers.size() &&
          boundaries.size() == constructor.initializers.size();
  state = State::Complete;
  effects.constructors[id - 1] = !valid;
  return valid;
}

bool MirOwnedFailureClosure::proveFunction(HirFunctionInstanceId id) {
  if (id == 0 || id > program.functionInstances().size()) {
    return false;
  }
  if (!effects.functions[id - 1]) {
    return true;
  }
  State &state = functionStates[id - 1];
  if (state == State::Complete) {
    return !effects.functions[id - 1];
  }
  if (state == State::Visiting) {
    return false;
  }
  state = State::Visiting;
  const MirFunctionInstance &function = program.functionInstances()[id - 1];
  const bool valid =
      function.id == id && function.declaration != 0 && !function.owner &&
      !function.staticMember && function.entryKind == ProgramEntryKind::None &&
      !function.entryArgumentAppendTarget &&
      function.definitionKind == MirDefinitionKind::Source &&
      function.linkage == LanguageLinkage::Gti &&
      function.externalSymbol.empty() && !function.virtualMethod &&
      !function.pureVirtual && !function.overrideMethod &&
      function.virtualRoots.empty() && function.callableParameters.empty() &&
      function.returnBorrowOrigin == BorrowOriginKind::None &&
      function.parameterTypes.size() == function.parameterBindings.size() &&
      std::all_of(function.parameterTypes.begin(),
                  function.parameterTypes.end(),
                  [&](const SemanticType &type) {
                    return scalarType(type) || callbackType(type) ||
                           classForType(type) != nullptr;
                  }) &&
      (function.returnType == SemanticType::Void ||
       scalarType(function.returnType) || callbackType(function.returnType)) &&
      proveBody(function.body, MirBodyKind::Function, function.id, nullptr);
  state = State::Complete;
  effects.functions[id - 1] = !valid;
  return valid;
}
} // namespace
MirDefinedFailureEffects
deriveMirDefinedFailureEffects(const MirProgram &program) {
  MirDefinedFailureEffects effects;
  effects.functions =
      deriveMirFunctionDefinedFailureEffects(program, &effects.destructors);
  effects.constructors.assign(program.constructorInstances().size(), true);
  MirOwnedFailureClosure(program, effects).derive();
  return effects;
}

bool mirTypeMoveIsDefinedFailureFree(const MirProgram &program,
                                     const SemanticType &type) {
  const auto closedType = [](const auto &self,
                             const SemanticType &candidate) -> bool {
    if (candidate.kind == SemanticType::Unknown ||
        candidate.kind == SemanticType::TypeParameter ||
        candidate.kind == SemanticType::TypePack ||
        candidate.kind == SemanticType::TypeName ||
        std::any_of(candidate.valueArguments.begin(),
                    candidate.valueArguments.end(),
                    [](const CompileTimeValue &value) {
                      return value.kind == CompileTimeValue::Parameter;
                    })) {
      return false;
    }
    return std::all_of(
        candidate.arguments.begin(), candidate.arguments.end(),
        [&](const SemanticType &argument) { return self(self, argument); });
  };
  if (!closedType(closedType, type)) {
    return false;
  }

  std::unordered_set<HirClassInstanceId> visiting;
  const auto prove = [&](const auto &self,
                         const SemanticType &candidate) -> bool {
    switch (candidate.kind) {
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
    case SemanticType::StringView:
    case SemanticType::CString:
    case SemanticType::NullPtr:
    case SemanticType::RawPointer:
    case SemanticType::FunctionItem:
    case SemanticType::NativeFunction:
    case SemanticType::Enum:
    case SemanticType::Reference:
      return true;
    case SemanticType::UniqueOwner:
    case SemanticType::SharedPointer:
    case SemanticType::Storage:
    case SemanticType::PrefixStorage:
      // These compiler capabilities move only their control/storage handle;
      // they do not move or destroy the represented element.
      return candidate.arguments.size() == 1;
    case SemanticType::Array:
      return candidate.arguments.size() == 1 &&
             candidate.arrayLengthParameterId == 0 &&
             candidate.arrayLength != 0 &&
             self(self, candidate.arguments.front());
    case SemanticType::Expected:
      return candidate.arguments.size() == 2 &&
             (candidate.arguments.front() == SemanticType::Void ||
              self(self, candidate.arguments.front())) &&
             self(self, candidate.arguments.back());
    case SemanticType::Unexpected:
      return candidate.arguments.size() == 1 &&
             self(self, candidate.arguments.front());
    case SemanticType::Class: {
      const MirClassInstance *instance = nullptr;
      for (const MirClassInstance &item : program.classInstances()) {
        if (item.type != candidate) {
          continue;
        }
        if (instance != nullptr) {
          return false;
        }
        instance = &item;
      }
      if (instance == nullptr || instance->id == 0 ||
          instance->moveConstructor != SpecialMemberStatus::Generated ||
          instance->destructorStatus != SpecialMemberStatus::Generated ||
          instance->destructor || instance->requiresActiveDropState ||
          instance->abstract || instance->polymorphic ||
          instance->unionLayout || !instance->bases.empty() ||
          !instance->structuralBases.empty() ||
          !visiting.insert(instance->id).second) {
        return false;
      }
      if (instance->compilerCapability ==
          CompilerCapabilityTypeKind::OptionalStorage) {
        visiting.erase(instance->id);
        return candidate.arguments.size() == 1 &&
               self(self, candidate.arguments.front());
      }
      const bool fields = std::all_of(instance->declaredFields.begin(),
                                      instance->declaredFields.end(),
                                      [&](const MirClassFieldInfo &field) {
                                        return field.field != 0 &&
                                               field.symbol != 0 &&
                                               self(self, field.type);
                                      });
      visiting.erase(instance->id);
      return fields;
    }
    case SemanticType::Unknown:
    case SemanticType::Void:
    case SemanticType::TypeParameter:
    case SemanticType::TypePack:
    case SemanticType::TypeName:
    case SemanticType::Function:
    case SemanticType::FunctionSignature:
    case SemanticType::Lambda:
      return false;
    }
    return false;
  };
  return prove(prove, type);
}

std::vector<bool>
deriveMirScalarDefinedFailureEffects(const MirProgram &program) {
  return deriveMirDefinedFailureEffects(program).functions;
}

} // namespace lang
