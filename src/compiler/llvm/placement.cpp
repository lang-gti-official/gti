#include "placement.h"
#include "../mir_verification_queries.h"
#include "atomic_types.h"
#include "closure_environment.h"
#include "gti/backend.h"
#include "gti/mir_effects.h"
#include "memory_types.h"
#include "mutexes.h"
#include "threads.h"

#include <algorithm>
#include <exception>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace lang::native_llvm {
bool isFreshObjectProducer(const MirProgram &program,
                           const MirInstruction &producer) {
  // A closure returned by value is published by its callee in the result
  // destination like a class result.
  if (producer.tombstone || !producer.result || producer.destination ||
      (producer.info.type.kind != SemanticType::Class &&
       !(producer.info.type.kind == SemanticType::Lambda &&
         producer.kind == MirInstructionKind::Call)) ||
      producer.info.category != ValueCategory::Value)
    return false;
  // A construction's loan is the borrow its new object stores; the object
  // itself is still fresh.
  if (producer.kind == MirInstructionKind::Construct)
    return producer.constructorKind == ConstructorKind::Ordinary;
  // A taken thread result is relocated out of its runtime box, and the
  // previous object of a restoring exchange out of the place the exchange
  // vacates, into the destination the operation is given, like a by-value
  // call result.
  if (producer.kind == MirInstructionKind::Call &&
      (producer.intrinsic == IntrinsicKind::ThreadResultTake ||
       producer.intrinsic == IntrinsicKind::RestoringExchange))
    return !producer.loan && !producer.receiver && !producer.functionTarget &&
           !producer.constructorTarget && !producer.lambdaTarget &&
           !producer.bodyTarget && !producer.threadTaskAdapter;
  // A consuming member call names its source function like any other call;
  // only a callable without a function target has another result contract.
  if (producer.kind != MirInstructionKind::Call || producer.loan ||
      producer.intrinsic != IntrinsicKind::None || producer.constructorTarget ||
      producer.lambdaTarget || producer.bodyTarget ||
      producer.nativeCalleeType ||
      (producer.callableInvocation && !producer.functionTarget) ||
      producer.threadTaskAdapter || producer.nativeCallbackAdapter ||
      !producer.functionTarget ||
      (producer.dispatch != CallDispatch::Static &&
       producer.dispatch != CallDispatch::Virtual))
    return false;
  const auto *target = program.findFunctionInstance(*producer.functionTarget);
  // The callee's own body publishes into the result destination; a virtual
  // call names its sealed root, whose implementations share that contract.
  return target && target->linkage == LanguageLinkage::Gti &&
         target->returnType == producer.info.type &&
         (producer.dispatch == CallDispatch::Virtual
              ? target->virtualMethod
              : target->definitionKind == MirDefinitionKind::Source);
}

bool isFreshHierarchyAggregate(const MirProgram &program,
                               const MirInstruction &producer) {
  if (producer.tombstone || !producer.result || producer.destination ||
      producer.kind != MirInstructionKind::Compute || producer.receiver ||
      producer.loan || producer.info.category != ValueCategory::Value)
    return false;
  const auto &type = producer.info.type;
  if (producer.operation == MirOperation::Aggregate) {
    if (type.kind != SemanticType::Array || type.arguments.size() != 1 ||
        type.arrayLengthParameterId ||
        type.arrayLength != producer.operands.size())
      return false;
  } else if (producer.operation == MirOperation::PayloadConstruct) {
    // One payload alternative aggregated from its exact fields.
    const auto *payload = type.kind == SemanticType::Enum
                              ? program.findPayloadEnum(type)
                              : nullptr;
    if (!payload || !producer.enumVariant ||
        *producer.enumVariant >= payload->variants.size() ||
        payload->variants[*producer.enumVariant].payloadTypes.size() !=
            producer.operands.size())
      return false;
  } else if (producer.operation == MirOperation::Closure) {
    // A closure environment aggregates its captures in declaration order.
    if (type.kind != SemanticType::Lambda || !producer.lambdaTarget ||
        producer.closureCaptureTypes.size() != producer.operands.size() ||
        producer.closureCaptureModes.size() != producer.operands.size())
      return false;
  } else {
    return false;
  }
  try {
    return containsHierarchyStorage(program, producer.info.type);
  } catch (const BackendUnsupportedError &) {
    return false;
  }
}

std::optional<MirPlaceId>
freshConstructionDestination(const MirProgram &program, MirBodyAddress address,
                             const MirBody &body, const Placement &placement,
                             const MirInstruction &producer) {
  // An ordinary construction may name the hidden temporary MIR materializes
  // for its result as its own destination.
  const bool materialized =
      !producer.tombstone && producer.result && producer.destination &&
      producer.kind == MirInstructionKind::Construct &&
      producer.constructorKind == ConstructorKind::Ordinary && !producer.loan &&
      producer.info.type.kind == SemanticType::Class &&
      producer.info.category == ValueCategory::Value;
  if (findMirBody(program, address) != &body ||
      (address.kind != MirBodyKind::Function &&
       address.kind != MirBodyKind::Constructor &&
       address.kind != MirBodyKind::Destructor &&
       address.kind != MirBodyKind::FieldInitializers &&
       address.kind != MirBodyKind::Module) ||
      (!isFreshObjectProducer(program, producer) && !materialized &&
       !isFreshHierarchyAggregate(program, producer)))
    return {};
  const auto *target = placement.target(*producer.result);
  const auto *place =
      target && target->place ? body.findPlace(target->place) : nullptr;
  if (!target || target->aggregate || target->returned || !place ||
      place->capture || place->foreignStorage ||
      place->type != producer.info.type)
    return {};
  // The hidden temporary MIR materializes for exactly this producer's result
  // is that object's final storage. Only drops of that place end it.
  const auto materialization =
      place->root == MirPlaceRootKind::Temporary
          ? findMirTemporaryMaterialization(body, *place, place->sourceValue)
          : MirTemporaryMaterialization{};
  if (materialized ||
      (materialization && materialization.producer == &producer)) {
    if (place->root != MirPlaceRootKind::Temporary ||
        !place->projections.empty() || !materialization ||
        materialization.producer != &producer ||
        materialization.value->id != *producer.result ||
        (producer.destination && *producer.destination != place->id))
      return {};
    for (const auto &event : producer.lifecycle) {
      const auto *drop = body.findDropObligation(event.target);
      if (event.kind != MirLifecycleEventKind::Initialize || event.source ||
          event.conditional || event.failureCleanup || !drop ||
          drop->place != place->id || drop->initiallyActive)
        return {};
    }
    if (producer.successResultDrop) {
      const auto *drop = body.findDropObligation(*producer.successResultDrop);
      if (!drop || drop->place != place->id || drop->initiallyActive)
        return {};
    }
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && &stage != &producer && stage.destination &&
            *stage.destination == place->id &&
            stage.kind != MirInstructionKind::Drop)
          return {};
    return target->place;
  }
  const MirInstruction *publication = nullptr;
  const MirBlock *definition = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions) {
      if (&stage == &producer)
        definition = &block;
      if (!stage.tombstone && stage.kind == MirInstructionKind::Initialize &&
          stage.destination == target->place && stage.operands.size() == 1 &&
          stage.operands.front().kind == MirOperandKind::Value &&
          stage.operands.front().value == *producer.result) {
        if (publication)
          return {};
        publication = &stage;
      }
    }
  const auto *value = body.findValue(*producer.result);
  if (!definition || !publication || !value ||
      value->definition != producer.id ||
      value->definitionBlock != definition->id ||
      value->info.type != producer.info.type)
    return {};
  const std::vector<HirBindingId> *parameters = nullptr;
  if (address.kind == MirBodyKind::Function)
    parameters =
        &program.findFunctionInstance(address.owner)->parameterBindings;
  if (address.kind == MirBodyKind::Constructor)
    parameters =
        &program.findConstructorInstance(address.owner)->parameterBindings;
  if (parameters && std::find(parameters->begin(), parameters->end(),
                              place->binding) != parameters->end())
    return {};
  if (address.kind == MirBodyKind::Constructor &&
      place->root == MirPlaceRootKind::This) {
    const auto &constructor = *program.findConstructorInstance(address.owner);
    if (!publication->constructorInitializer ||
        publication->constructorInitializer > constructor.initializers.size() ||
        place->projections.size() != 1 ||
        place->projections.front().kind != MirProjectionKind::Field)
      return {};
    const auto &selected =
        constructor.initializers[publication->constructorInitializer - 1];
    if (selected.kind != ConstructorInitializerTargetKind::Field ||
        selected.field != place->projections.front().field ||
        selected.targetType != place->type || selected.storesReference ||
        selected.ownedParameter)
      return {};
    return target->place;
  }
  // A hidden temporary initialized from this producer, such as one arm of a
  // conditional receiver, is published like a whole local.
  if (place->root == MirPlaceRootKind::Temporary)
    return place->projections.empty() ? std::optional(target->place)
                                      : std::nullopt;
  if (place->root != MirPlaceRootKind::Binding || !place->binding ||
      !place->projections.empty())
    return {};
  if (address.kind == MirBodyKind::FieldInitializers) {
    const auto &owner = *program.findClassInstance(address.owner);
    if (std::none_of(owner.declaredFields.begin(), owner.declaredFields.end(),
                     [&](const auto &field) {
                       return field.field == place->binding &&
                              field.symbol == place->symbol &&
                              field.type == place->type &&
                              field.access == place->access;
                     }))
      return {};
  }
  return target->place;
}

// The assignment that relocates a replacing value into its destination after
// the paired cleanup ends the destination's previous object.
static const MirInstruction *replacementAssignment(const MirBody &body,
                                            const MirValueUse &use) {
  if (use.kind != MirValueUseKind::InstructionOperand || use.operandIndex != 0)
    return nullptr;
  const auto *block = body.findBlock(use.block);
  if (!block)
    return nullptr;
  for (const auto &stage : block->instructions)
    if (!stage.tombstone && stage.id == use.instruction)
      return stage.kind == MirInstructionKind::Assign && stage.destination &&
                     stage.operation == MirOperation::Assign &&
                     stage.operands.size() == 1
                 ? &stage
                 : nullptr;
  return nullptr;
}

std::optional<FreshObjectStorage>
freshObjectStorage(const MirProgram &program, MirBodyAddress address,
                   const MirBody &body, const Placement &placement,
                   const MirInstruction &producer) {
  if (freshConstructionDestination(program, address, body, placement, producer))
    return FreshObjectStorage::Local;
  const bool aggregate = isFreshHierarchyAggregate(program, producer);
  if (findMirBody(program, address) != &body ||
      (!isFreshObjectProducer(program, producer) && !aggregate))
    return {};
  const auto value = *producer.result;
  const auto *definition = body.findValue(value);
  if (!definition || definition->id != value ||
      definition->definition != producer.id ||
      definition->info.type != producer.info.type)
    return {};
  // Only value-rooted places of this exact object, and drop obligations on
  // its unprojected place, may name the producer's own storage.
  const MirPlace *whole = nullptr;
  for (const auto &place : body.places)
    if (place.root == MirPlaceRootKind::Value && place.value == value) {
      if (place.capture || place.foreignStorage)
        return {};
      if (place.projections.empty()) {
        if (whole || place.type != producer.info.type)
          return {};
        whole = &place;
      }
    }
  for (const auto &obligation : body.dropObligations)
    if (obligation.generatedValue == value ||
        (obligation.place && body.findPlace(obligation.place) &&
         body.findPlace(obligation.place)->root == MirPlaceRootKind::Value &&
         body.findPlace(obligation.place)->value == value))
      if (!whole || obligation.place != whole->id ||
          (obligation.kind != MirDropObligationKind::Value &&
           obligation.kind != MirDropObligationKind::ReturnResult))
        return {};
  const auto *target = placement.target(value);
  if (target && target->aggregate) {
    // An element constructed directly in its slot of a fresh hierarchy
    // aggregate: an array element or one field of a payload alternative. Its
    // one use is that slot's operand; the aggregate takes its value
    // obligation and is itself published in its own planned storage.
    const MirInstruction *outer = nullptr;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.result == target->aggregate)
          outer = &stage;
    const bool alternative =
        outer && outer->operation == MirOperation::PayloadConstruct;
    const bool closure = outer && outer->operation == MirOperation::Closure;
    const SemanticType *slot = nullptr;
    if (outer && isFreshHierarchyAggregate(program, *outer)) {
      if (alternative && target->variant == outer->enumVariant)
        slot = &program.findPayloadEnum(outer->info.type)
                    ->variants[*outer->enumVariant]
                    .payloadTypes[target->element];
      else if (closure && !target->variant &&
               target->element < outer->closureCaptureTypes.size() &&
               outer->closureCaptureModes[target->element] ==
                   LambdaCaptureMode::Move)
        slot = &outer->closureCaptureTypes[target->element];
      else if (!alternative && !closure && !target->variant)
        slot = &outer->info.type.arguments.front();
    }
    if (!slot || target->element >= outer->operands.size() ||
        outer->operands[target->element].kind != MirOperandKind::Value ||
        outer->operands[target->element].value != value ||
        *slot != producer.info.type ||
        !freshObjectStorage(program, address, body, placement, *outer))
      return {};
    for (const auto &use : body.usesOf(value))
      if (use.kind != MirValueUseKind::PlaceRoot &&
          (use.kind != MirValueUseKind::InstructionOperand ||
           use.instruction != outer->id ||
           use.operandIndex != target->element))
        return {};
    for (const auto &obligation : body.dropObligations)
      if (whole && obligation.place == whole->id &&
          std::none_of(outer->lifecycle.begin(), outer->lifecycle.end(),
                       [&](const auto &event) {
                         return event.source == obligation.id &&
                                event.kind ==
                                    MirLifecycleEventKind::TransferOut &&
                                !event.target && !event.conditional &&
                                !event.failureCleanup;
                       }))
        return {};
    return FreshObjectStorage::Element;
  }
  // A closure may also be constructed directly in a by-value argument's
  // prepared storage; other aggregates publish only into final storage.
  if (aggregate && target && producer.operation != MirOperation::Closure)
    return {};
  if (target && target->place && !target->aggregate && !target->returned) {
    // A by-value argument constructed directly in its prepared storage.
    const auto *place = body.findPlace(target->place);
    const MirInstruction *input = nullptr;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone && stage.kind == MirInstructionKind::CallInput &&
            stage.destination == target->place) {
          if (input)
            return {};
          input = &stage;
        }
    const auto *prepared =
        input && input->preparedParameterDrop
            ? body.findDropObligation(*input->preparedParameterDrop)
            : nullptr;
    if (!place || place->root != MirPlaceRootKind::Temporary ||
        !place->projections.empty() || place->capture ||
        place->foreignStorage || place->type != producer.info.type ||
        !input || input->callInputKind != HirCallInputKind::MoveValue ||
        input->callInputRole != MirCallInputRole::Argument ||
        input->operands.size() != 1 ||
        input->operands.front().kind != MirOperandKind::Value ||
        input->operands.front().value != value || !prepared ||
        prepared->kind != MirDropObligationKind::PreparedParameter ||
        prepared->place != place->id)
      return {};
    // The prepared obligation is armed fresh, or reparented from the exact
    // success-result obligation of the producer's own value place.
    const MirDropObligation *produced = nullptr;
    if (whole)
      for (const auto &obligation : body.dropObligations)
        if (obligation.place == whole->id)
          produced = &obligation;
    const auto &events = input->lifecycle;
    if (events.size() != 1 || events.front().conditional ||
        events.front().failureCleanup ||
        events.front().target != prepared->id ||
        events.front().kind != (produced ? MirLifecycleEventKind::Reparent
                                         : MirLifecycleEventKind::Initialize) ||
        events.front().source != (produced ? produced->id : 0) ||
        (produced && produced->kind != MirDropObligationKind::Value))
      return {};
    for (const auto &use : body.usesOf(value))
      if (use.kind != MirValueUseKind::PlaceRoot &&
          (use.kind != MirValueUseKind::InstructionOperand ||
           use.instruction != input->id))
        return {};
    return FreshObjectStorage::Argument;
  }
  bool returned = false;
  bool replacing = false;
  for (const auto &use : body.usesOf(value)) {
    if (use.kind == MirValueUseKind::PlaceRoot)
      continue;
    // A replacing value lives in its own temporary until its assignment
    // relocates it into the destination.
    if (!replacing && replacementAssignment(body, use)) {
      replacing = true;
      continue;
    }
    const auto *block = body.findBlock(use.block);
    if (use.kind != MirValueUseKind::Terminator || !block ||
        block->terminator.kind != MirTerminatorKind::Return ||
        !block->terminator.value ||
        block->terminator.value->kind != MirOperandKind::Value ||
        block->terminator.value->value != value)
      return {};
    returned = true;
  }
  if ((returned && replacing) || (aggregate && returned))
    return {};
  if (returned) {
    // A thread-local initializer constructs its object directly in the
    // thread's storage slot, which its caller passes as the result.
    if (!target || !target->returned || body.returnType != producer.info.type ||
        (address.kind != MirBodyKind::Function &&
         address.kind != MirBodyKind::Lambda &&
         address.kind != MirBodyKind::ThreadLocalInitializer))
      return {};
    return FreshObjectStorage::Returned;
  }
  // A replacing value of an inert class may have no value place at all.
  if (target || (!whole && !replacing))
    return {};
  return FreshObjectStorage::Temporary;
}

bool completeObjectPlace(const MirProgram &program, const MirPlace &place) {
  if (place.capture || place.foreignStorage)
    return false;
  // A payload enum has no derived type: every place of it is complete.
  if (place.type.kind == SemanticType::Enum)
    return program.findPayloadEnum(place.type) != nullptr;
  if (place.type.kind != SemanticType::Class)
    return false;
  if (place.projections.empty() &&
      (place.root == MirPlaceRootKind::Binding ||
       place.root == MirPlaceRootKind::Temporary ||
       place.root == MirPlaceRootKind::Value))
    return true;
  if (!place.projections.empty()) {
    const auto kind = place.projections.back().kind;
    if (kind == MirProjectionKind::Field || kind == MirProjectionKind::Index ||
        kind == MirProjectionKind::PackElement)
      return true;
  }
  const MirClassInstance *owner = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == place.type) {
      if (owner)
        return false;
      owner = &candidate;
    }
  if (!owner)
    return false;
  for (const auto &candidate : program.classInstances())
    for (const auto *rows : {&candidate.bases, &candidate.structuralBases})
      for (const auto &row : *rows)
        if (row.instance == owner->id)
          return false;
  return true;
}

bool uniqueOwnerPointee(const MirProgram &program,
                        const SemanticType &pointee) {
  if (mir_detail::completeObjectType(program, pointee))
    return true;
  if (pointee.kind != SemanticType::Class)
    return false;
  const MirClassInstance *selected = nullptr;
  for (const auto &candidate : program.classInstances())
    if (candidate.type == pointee) {
      if (selected)
        return false;
      selected = &candidate;
    }
  return selected && selected->polymorphic &&
         selected->compilerCapability == CompilerCapabilityTypeKind::None &&
         !selected->cOpaqueHandle;
}

namespace {

[[nodiscard]] bool computeHierarchyStorage(const MirProgram &program,
                                           const SemanticType &type) {
  // These keys and results live only for this immutable program query. A
  // completed sibling is not a cycle; only the current inline path is active.
  std::set<const void *> visiting;
  std::map<const void *, bool> completed;
  const auto reject = [](const char *message) -> void {
    throw BackendUnsupportedError(message);
  };
  const auto visit = [&](const auto &self,
                         const SemanticType &selected) -> bool {
    if (selected.kind == SemanticType::Reference ||
        selected.kind == SemanticType::RawPointer) {
      if (selected.arguments.size() != 1 ||
          selected.arguments.front().kind == SemanticType::Unknown ||
          selected.arguments.front().kind == SemanticType::TypeParameter)
        reject("LLVM hierarchy exclusion requires an exact indirection type");
      return false;
    }
    if (selected.kind == SemanticType::UniqueOwner) {
      if (selected.arguments.size() != 1 ||
          !uniqueOwnerPointee(program, selected.arguments.front()))
        reject("LLVM hierarchy exclusion requires an exact owner handle");
      // The handle is indirect. Construction and cleanup still need their
      // independent owner/pointee proofs, including hierarchy exclusions.
      return false;
    }
    if (selected.kind == SemanticType::Array) {
      if (selected.arguments.size() != 1 || selected.arrayLengthParameterId)
        reject("LLVM hierarchy exclusion requires a concrete array element");
      return self(self, selected.arguments.front());
    }
    if (selected.kind == SemanticType::TypePack) {
      if (!selected.concretePack)
        reject("LLVM hierarchy exclusion requires a concrete type pack");
      bool result = false;
      for (const auto &element : selected.arguments)
        result = self(self, element) || result;
      return result;
    }

    const auto inspect = [&](const void *identity,
                             const auto &contents) -> bool {
      if (visiting.contains(identity))
        reject("LLVM hierarchy exclusion has recursive inline object storage");
      if (const auto found = completed.find(identity); found != completed.end())
        return found->second;
      visiting.insert(identity);
      const bool result = contents();
      visiting.erase(identity);
      completed.emplace(identity, result);
      return result;
    };

    if (selected.kind == SemanticType::Lambda) {
      const auto environment = closureEnvironmentLayout(program, selected);
      if (!selected.hasLambdaShape() || !selected.lambdaId || !environment ||
          environment->captureTypes.size() != selected.lambdaCaptureCount ||
          environment->captureTypes.size() !=
              environment->captureModes.size() ||
          environment->captureTypes.size() !=
              environment->captureRequiresActiveCleanup.size() ||
          !std::equal(environment->captureTypes.begin(),
                      environment->captureTypes.end(),
                      selected.lambdaCaptureTypes().begin()))
        reject(
            "LLVM hierarchy exclusion requires an exact closure environment");
      const MirLambdaInstance *representation = nullptr;
      for (const auto &candidate : program.lambdaInstances()) {
        if (candidate.type != selected)
          continue;
        if (!candidate.id || candidate.declaration != selected.lambdaId ||
            program.findLambda(candidate.id) != &candidate ||
            std::count_if(
                program.lambdaInstances().begin(),
                program.lambdaInstances().end(),
                [&](const auto &row) { return row.id == candidate.id; }) != 1 ||
            candidate.captureTypes.size() != candidate.captureSymbols.size() ||
            candidate.captureTypes.size() != candidate.captureModes.size() ||
            candidate.captureTypes.size() !=
                candidate.captureRequiresActiveCleanup.size())
          reject("LLVM hierarchy exclusion has an ambiguous closure identity");
        std::set<SymbolId> symbols;
        for (std::size_t index = 0; index < candidate.captureTypes.size();
             ++index) {
          const auto mode = candidate.captureModes[index];
          const auto &capture = candidate.captureTypes[index];
          if ((mode != LambdaCaptureMode::Copy &&
               mode != LambdaCaptureMode::Move && !isBorrowingCapture(mode)) ||
              !candidate.captureSymbols[index] ||
              !symbols.insert(candidate.captureSymbols[index]).second ||
              (isBorrowingCapture(mode) &&
               (capture.kind != SemanticType::Reference ||
                capture.arguments.size() != 1 ||
                capture.referenceAccess != (mode == LambdaCaptureMode::BorrowMut
                                                ? AccessMode::Mutable
                                                : AccessMode::ReadOnly))))
            reject("LLVM hierarchy exclusion requires verified capture storage "
                   "modes");
        }
        if (!representation)
          representation = &candidate;
      }
      if (!representation)
        reject("LLVM hierarchy exclusion lost its closure inventory");
      // Agreement came from closureEnvironmentLayout. This key selects only
      // the shared inline graph, never an invocation or cleanup body.
      return inspect(representation, [&] {
        bool result = false;
        for (const auto &capture : environment->captureTypes)
          result = self(self, capture) || result;
        return result;
      });
    }
    if (selected.kind == SemanticType::Enum) {
      const MirIntegralEnum *integral = nullptr;
      const MirPayloadEnum *payload = nullptr;
      for (const auto &row : program.integralEnums())
        if (row.type == selected) {
          if (integral)
            reject("LLVM hierarchy exclusion has an ambiguous integral enum");
          integral = &row;
        }
      for (const auto &row : program.payloadEnums())
        if (row.type == selected) {
          if (payload)
            reject("LLVM hierarchy exclusion has an ambiguous payload enum");
          payload = &row;
        }
      if (!selected.enumId || (integral != nullptr) == (payload != nullptr) ||
          (integral && integral->declaration != selected.enumId) ||
          (payload && payload->declaration != selected.enumId))
        reject("LLVM hierarchy exclusion requires one exact enum inventory");
      if (integral) {
        const auto &underlying = integral->underlyingType;
        if (!underlying.arguments.empty() ||
            !((underlying.kind >= SemanticType::Int8 &&
               underlying.kind <= SemanticType::UInt64) ||
              underlying.kind == SemanticType::Bool ||
              underlying.kind == SemanticType::Char))
          reject(
              "LLVM hierarchy exclusion requires a sealed integral enum type");
        return false;
      }
      if (payload->variants.empty())
        reject("LLVM hierarchy exclusion lost its payload variants");
      return inspect(payload, [&] {
        bool result = false;
        for (std::size_t index = 0; index < payload->variants.size(); ++index) {
          const auto &variant = payload->variants[index];
          if (variant.variantIndex != index ||
              variant.payloadTypes.size() != variant.payloadSymbols.size() ||
              variant.payloadTypes.size() !=
                  variant.payloadRequiresActiveCleanup.size())
            reject("LLVM hierarchy exclusion lost exact payload field "
                   "inventories");
          for (const auto &field : variant.payloadTypes)
            result = self(self, field) || result;
        }
        return result;
      });
    }
    if (selected.kind != SemanticType::Class) {
      switch (selected.kind) {
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
      case SemanticType::StringView:
      case SemanticType::CString:
      case SemanticType::NullPtr:
      case SemanticType::Function:
      case SemanticType::FunctionItem:
      case SemanticType::NativeFunction:
      case SemanticType::FunctionSignature:
        return false;
      default:
        reject("LLVM hierarchy exclusion has no concrete inline-storage proof");
      }
    }
    const MirClassInstance *owner = nullptr;
    for (const auto &row : program.classInstances())
      if (row.type == selected) {
        if (owner)
          reject("LLVM hierarchy exclusion has an ambiguous class type");
        owner = &row;
      }
    if (!owner || !owner->id || program.findClassInstance(owner->id) != owner ||
        std::count_if(
            program.classInstances().begin(), program.classInstances().end(),
            [&](const auto &row) { return row.id == owner->id; }) != 1 ||
        owner->cOpaqueHandle)
      reject(
          "LLVM hierarchy exclusion requires an exact inline class identity");
    return inspect(owner, [&] {
      if (sharedOwnerRepresentation(program, *owner))
        return false;
      if (uninitializedStorageRepresentation(program, *owner))
        return self(self, owner->type.arguments.front());
      if (owner->compilerCapability ==
          CompilerCapabilityTypeKind::FunctionPointer) {
        if (!mir_detail::functionPointerSignature(program, selected) ||
            !owner->declaredFields.empty() || !owner->bases.empty() ||
            !owner->structuralBases.empty() || owner->polymorphic ||
            owner->abstract || owner->unionLayout)
          reject("LLVM hierarchy exclusion requires an exact function-pointer "
                 "handle");
        return false;
      }
      if (owner->compilerCapability ==
          CompilerCapabilityTypeKind::AtomicStorage) {
        if (!nativeAtomicElement(program, selected))
          reject("LLVM hierarchy exclusion requires exact atomic storage");
        return false;
      }
      if (owner->compilerCapability ==
              CompilerCapabilityTypeKind::MutexStorage ||
          owner->compilerCapability ==
              CompilerCapabilityTypeKind::MutexLockToken) {
        const auto payload =
            nativeMutexElement(program, selected, owner->compilerCapability);
        if (!payload)
          reject("LLVM hierarchy exclusion requires exact mutex storage/token");
        return owner->compilerCapability ==
                   CompilerCapabilityTypeKind::MutexStorage &&
               self(self, *payload);
      }
      // An inline polymorphic slot stores a self-relative payload view and
      // dispatch-bearing object bytes: it is hierarchy storage itself.
      if (owner->compilerCapability ==
          CompilerCapabilityTypeKind::PolymorphicStorage) {
        if (!polymorphicStorageRepresentation(program, *owner))
          reject("LLVM hierarchy exclusion requires an exact inline slot");
        return true;
      }
      if (owner->compilerCapability ==
          CompilerCapabilityTypeKind::ThreadHandle) {
        if (!nativeThreadHandle(program, selected))
          reject("LLVM hierarchy exclusion requires an exact thread handle");
        return false;
      }
      if (owner->compilerCapability != CompilerCapabilityTypeKind::None)
        reject(
            "LLVM hierarchy exclusion has no inline capability representation");
      bool result = owner->polymorphic || owner->abstract ||
                    owner->kind == ClassKind::Interface ||
                    !owner->bases.empty() || !owner->structuralBases.empty();
      // All union fields are potential inline storage. This neither selects
      // an active member nor changes its existing unsafe/lifetime contract.
      for (const auto &field : owner->declaredFields)
        result = self(self, field.type) || result;
      const auto base = [&](const auto &row) {
        const auto *target = program.findClassInstance(row.instance);
        if (!target || target->type != row.type ||
            row.interface != (target->kind == ClassKind::Interface))
          reject("LLVM hierarchy exclusion lost an exact base identity");
        result = self(self, row.type) || result;
      };
      for (const auto &row : owner->bases)
        base(row);
      for (const auto &row : owner->structuralBases)
        base(row);
      return result;
    });
  };
  return visit(visit, type);
}

[[nodiscard]] std::size_t outcomeHash(const SemanticType &type) {
  // Only a bucket selector: equal types collide, and lookup confirms the
  // complete type with operator==.
  std::size_t hash = static_cast<std::size_t>(type.kind);
  const auto mix = [&](std::size_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  };
  mix(type.classId);
  mix(type.enumId);
  mix(type.lambdaId);
  mix(type.arrayLength);
  mix(type.arguments.size());
  for (const auto &argument : type.arguments)
    mix(outcomeHash(argument));
  return hash;
}

} // namespace

struct HierarchyStorageScope::Outcomes {
  struct Outcome {
    SemanticType type;
    bool result = false;
    std::exception_ptr rejection;
  };

  explicit Outcomes(const MirProgram &program) : program(&program) {}

  const MirProgram *program;
  std::unordered_map<std::size_t, std::vector<Outcome>> buckets;
  std::size_t size = 0;
};

namespace {

thread_local HierarchyStorageScope::Outcomes *activeHierarchyOutcomes = nullptr;

} // namespace

HierarchyStorageScope::HierarchyStorageScope(const MirProgram &program)
    : previous(activeHierarchyOutcomes) {
  if (previous == nullptr || previous->program != &program) {
    outcomes = std::make_unique<Outcomes>(program);
    activeHierarchyOutcomes = outcomes.get();
  }
}

HierarchyStorageScope::~HierarchyStorageScope() {
  activeHierarchyOutcomes = previous;
}

std::size_t HierarchyStorageScope::cachedTypes() const {
  return outcomes ? outcomes->size : previous->size;
}

bool containsHierarchyStorage(const MirProgram &program,
                              const SemanticType &type) {
  auto *outcomes = activeHierarchyOutcomes;
  if (outcomes == nullptr || outcomes->program != &program)
    return computeHierarchyStorage(program, type);
  auto &bucket = outcomes->buckets[outcomeHash(type)];
  for (const auto &outcome : bucket)
    if (outcome.type == type) {
      if (outcome.rejection)
        std::rethrow_exception(outcome.rejection);
      return outcome.result;
    }
  // Only a completed top-level query is recorded. Its outcome depends on the
  // type's whole reachable storage graph, never on another query's path.
  HierarchyStorageScope::Outcomes::Outcome outcome{.type = type};
  try {
    outcome.result = computeHierarchyStorage(program, type);
  } catch (const BackendUnsupportedError &) {
    outcome.rejection = std::current_exception();
  }
  const std::exception_ptr rejection = outcome.rejection;
  const bool result = outcome.result;
  bucket.push_back(std::move(outcome));
  ++outcomes->size;
  if (rejection)
    std::rethrow_exception(rejection);
  return result;
}

bool isPayloadFieldMove(const MirInstruction &stage) {
  if (stage.tombstone || !stage.result ||
      stage.kind != MirInstructionKind::Compute ||
      stage.operation != MirOperation::PayloadExtract ||
      stage.payloadExtractionKind != PayloadExtractionKind::MoveValue ||
      !stage.enumVariant || !stage.payloadIndex ||
      stage.operands.size() != 1 ||
      (stage.info.type.kind != SemanticType::Class &&
       stage.info.type.kind != SemanticType::Enum) ||
      stage.intrinsic != IntrinsicKind::None || stage.destination ||
      stage.receiver || stage.loan || stage.functionTarget || stage.bodyTarget)
    return false;
  const auto &source = stage.operands.front();
  return source.kind == MirOperandKind::Move && source.place && !source.loan &&
         !source.value && !source.literal;
}

bool isFreshHierarchyTransferProducer(const MirInstruction &stage) {
  if (isPayloadFieldMove(stage))
    return true;
  // A payload enum or a closure is transferred only by moving it.
  const bool movedEnum = stage.kind == MirInstructionKind::Move &&
                         (stage.info.type.kind == SemanticType::Enum ||
                          stage.info.type.kind == SemanticType::Lambda);
  if (stage.tombstone || !stage.result ||
      (stage.info.type.kind != SemanticType::Class && !movedEnum) ||
      stage.operands.size() != 1 || stage.operation != MirOperation::None ||
      (stage.intrinsic != IntrinsicKind::None &&
       !(stage.kind == MirInstructionKind::Move &&
         stage.intrinsic == IntrinsicKind::Move)) ||
      stage.destination || stage.receiver || stage.loan ||
      stage.functionTarget || stage.bodyTarget)
    return false;
  const auto &source = stage.operands.front();
  if (source.value || source.literal)
    return false;
  return (stage.kind == MirInstructionKind::Construct &&
          !stage.constructorTarget &&
          stage.constructorKind == ConstructorKind::Copy &&
          (source.kind == MirOperandKind::BorrowRead ||
           source.kind == MirOperandKind::Loan)) ||
         (stage.kind == MirInstructionKind::Load &&
          source.kind == MirOperandKind::Copy && source.place && !source.loan &&
          source.type == stage.info.type) ||
         (stage.kind == MirInstructionKind::Move &&
          source.kind == MirOperandKind::Move && source.place && !source.loan &&
          source.type == stage.info.type);
}

namespace {

MirBodyAddress transferBodyIdentity(const MirProgram &program,
                                    const MirBody &body) {
  std::optional<MirBodyAddress> identity;
  for (const auto address : enumerateMirBodyAddresses(program))
    if (findMirBody(program, address) == &body) {
      if (identity)
        throw std::logic_error("LLVM transfer body has repeated identity");
      identity = address;
    }
  if (!identity)
    throw std::invalid_argument("LLVM transfer uses a foreign MIR body");
  if (body.kind != identity->kind)
    throw std::logic_error(
        "LLVM transfer body kind disagrees with its program identity");
  return *identity;
}

const MirBody &ownedTransferBody(const MirProgram &program,
                                 const MirBody &body) {
  static_cast<void>(transferBodyIdentity(program, body));
  return body;
}

// A value whose generated copy is exactly its bytes: passive scalars, raw
// and C-string addresses, stateless function items, and arrays or closures
// built from them by value without cleanup. Any other field keeps its own
// class-level generated-member contract.
bool bitwiseGeneratedCopy(const MirProgram &program, const SemanticType &type) {
  if (isPassiveScalarValueType(type) || type == SemanticType::CString)
    return true;
  if (type.kind == SemanticType::FunctionItem)
    return type.hasFunctionItemShape();
  if (type.kind == SemanticType::RawPointer)
    return type.arguments.size() == 1;
  if (type.kind == SemanticType::Array)
    return type.arguments.size() == 1 && !type.arrayLengthParameterId &&
           bitwiseGeneratedCopy(program, type.arguments.front());
  if (type.kind != SemanticType::Lambda)
    return false;
  const auto environment = closureEnvironmentLayout(program, type);
  if (!environment ||
      environment->captureTypes.size() != environment->captureModes.size() ||
      environment->captureTypes.size() !=
          environment->captureRequiresActiveCleanup.size())
    return false;
  for (std::size_t index = 0; index < environment->captureTypes.size();
       ++index)
    if (isBorrowingCapture(environment->captureModes[index]) ||
        environment->captureRequiresActiveCleanup[index] ||
        !bitwiseGeneratedCopy(program, environment->captureTypes[index]))
      return false;
  return true;
}

class FreshTransferQuery final {
public:
  FreshTransferQuery(const MirProgram &program, const MirBody &body)
      : program(program), body(body),
        identity(transferBodyIdentity(program, body)) {}

  FreshHierarchyTransfer plan(const MirInstruction &producer) {
    const MirBlock *definition = nullptr;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (&stage == &producer)
          definition = &block;
    if (!definition)
      throw std::invalid_argument("LLVM transfer uses a foreign MIR producer");
    if (!isFreshHierarchyTransferProducer(producer))
      unsupported("LLVM fresh hierarchy transfer requires an exact producer");
    const auto *value = body.findValue(*producer.result);
    if (!value || value->id != *producer.result ||
        value->definition != producer.id ||
        value->definitionBlock != definition->id ||
        value->info.type != producer.info.type)
      malformed("LLVM transfer lost its exact result definition");
    // A payload enum's complete, empty dependency set may carry the evidence
    // of its active alternative; it still names no loan. A moved carrier
    // takes the loans it stores along: they name its external referents,
    // which a relocation does not move.
    const bool relocating = isPayloadFieldMove(producer) ||
                            producer.info.type.kind == SemanticType::Enum ||
                            (producer.info.type.kind == SemanticType::Lambda &&
                             producer.kind == MirInstructionKind::Move);
    const bool moving = relocating || producer.kind == MirInstructionKind::Move;
    if (value->borrowDependencies &&
        ((!moving && !value->borrowDependencies->dependencies.empty()) ||
         (!relocating &&
          !value->borrowDependencies->possibleAlternatives.empty())))
      unsupported("LLVM fresh hierarchy transfer excludes borrowed values");
    // A thread-local initializer returns into its thread's storage slot.
    if (identity.kind != MirBodyKind::Function &&
        identity.kind != MirBodyKind::Constructor &&
        identity.kind != MirBodyKind::Destructor &&
        identity.kind != MirBodyKind::ThreadLocalInitializer)
      unsupported("LLVM fresh hierarchy transfer requires a local body");
    if (producer.destination || producer.receiver || producer.loan ||
        producer.functionTarget || producer.bodyTarget ||
        producer.functionPointerFunctionTarget ||
        producer.functionPointerLambdaTarget || producer.nativeCalleeType ||
        producer.nativeCallbackAdapter || producer.threadTaskAdapter ||
        !producer.scopedThreadTasks.empty() || producer.lambdaTarget ||
        producer.callableBoundary || producer.callableInvocation ||
        !producer.callableArguments.empty() || producer.objectAllocation ||
        producer.preparedParameterDrop || producer.retainedOwnerPlace ||
        producer.successResultDrop || producer.successResultDestination ||
        producer.elementCleanup || producer.replacementCleanup ||
        producer.replacementPair || producer.literal ||
        (producer.operation != MirOperation::None &&
         !isPayloadFieldMove(producer)) ||
        producer.unsafeOperation != UnsafeOperationKind::None ||
        producer.rawMemoryAccess || !producer.definedFailure.empty() ||
        producer.borrowOrigin != BorrowOriginKind::None ||
        producer.borrowPlace || !producer.borrowArguments.empty() ||
        !producer.borrowFields.empty() || !producer.closureCaptureTypes.empty())
      unsupported("LLVM fresh hierarchy transfer excludes additional effects");
    if (relocating)
      return relocation(producer);

    FreshHierarchyTransfer result{.source = producer.operands.front(),
                                  .kind =
                                      producer.kind == MirInstructionKind::Move
                                          ? ConstructorKind::Move
                                          : ConstructorKind::Copy};
    const auto &owner = instance(producer.info.type);
    if (owner.kind == ClassKind::Interface || owner.abstract)
      unsupported(
          "LLVM transfer destination must be a concrete ordinary class");
    result.owner = owner.id;
    result.type = owner.type;
    std::set<HirClassInstanceId> path;
    // A generated move is the type's verified relocation: every inline
    // subobject's move is generated and cannot fail. Its typed emitter, not
    // this planner, decides each field's representation transfer.
    if (result.kind == ConstructorKind::Move)
      relocationShape(owner);
    else
      shape(program, owner, result.kind, true, path);
    if (!containsHierarchyStorage(program, owner.type))
      unsupported("LLVM hierarchy transfer requires hierarchy storage");
    if (producer.kind == MirInstructionKind::Construct) {
      if (producer.parameterTypes !=
              std::vector<SemanticType>{
                  SemanticType::referenceTo(owner.type)} ||
          result.source.type != producer.parameterTypes.front() ||
          (result.source.kind == MirOperandKind::BorrowRead &&
           (!result.source.place || result.source.loan)) ||
          (result.source.kind == MirOperandKind::Loan && !result.source.loan))
        malformed("LLVM generated copy lost its exact reference formal");
    } else if (!producer.parameterTypes.empty() || producer.constructorTarget ||
               producer.constructorKind != ConstructorKind::Ordinary) {
      malformed("LLVM class-value transfer has constructor metadata");
    }

    const auto &source = result.source;
    if (source.value || source.literal || (!source.place && !source.loan) ||
        unreference(source.type) != owner.type)
      unsupported("LLVM transfer requires an exact static source view");
    const auto *loan = source.loan ? body.findLoan(source.loan) : nullptr;
    if (source.loan && (!loan || loan->id != source.loan || !simpleLoan(*loan)))
      unsupported("LLVM transfer requires a simple checked source loan");
    const auto *place =
        body.findPlace(source.place ? source.place : loan->source);
    if (!place)
      malformed("LLVM transfer lost its source place");
    if ((source.place && place->id != source.place) ||
        (!source.place && place->id != loan->source))
      malformed("LLVM transfer source place has repeated identity");
    // Program storage (a namespace or thread-local object or a static
    // field) is a complete object its symbol names; a copy reads it in place
    // and never moves out of it.
    const bool programSource = place->root == MirPlaceRootKind::Symbol &&
                               result.kind == ConstructorKind::Copy;
    if (unreference(place->type) != owner.type || place->foreignStorage ||
        place->capture ||
        (place->root != MirPlaceRootKind::Binding &&
         place->root != MirPlaceRootKind::This &&
         place->root != MirPlaceRootKind::Loan && !programSource) ||
        (!packElement(*place) &&
         std::any_of(place->projections.begin(), place->projections.end(),
                     [&](const auto &projection) {
                       // A copy also reads a class field in place: the field
                       // is a complete object of its declared class.
                       return projection.kind != MirProjectionKind::Base &&
                              projection.kind !=
                                  MirProjectionKind::Dereference &&
                              !(projection.kind == MirProjectionKind::Field &&
                                result.kind == ConstructorKind::Copy);
                     })))
      unsupported("LLVM transfer source is not a selected ordinary class view");
    if (place->root == MirPlaceRootKind::Loan) {
      const auto *root = body.findLoan(place->loan);
      if (!root || !simpleLoan(*root))
        unsupported("LLVM transfer source has unsupported loan provenance");
    }
    if (loan) {
      const auto *origin = body.findPlace(loan->source);
      if (!origin)
        malformed("LLVM transfer loan lost its exact origin");
      path.clear();
      const auto &originOwner = instance(unreference(origin->type));
      shape(program, originOwner, result.kind, false, path);
      path.clear();
      if (baseOccurrences(originOwner, owner.id, path) != 1)
        unsupported("LLVM copy loan does not protect its selected class view");
    }

    bool consumed = false;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        if (stage.result == producer.result && &stage != &producer)
          malformed("LLVM transfer result has repeated producers");
        for (std::size_t index = 0; index < stage.operands.size(); ++index) {
          const auto &operand = stage.operands[index];
          if (operand.value != *producer.result)
            continue;
          if (consumed || operand.kind != MirOperandKind::Value ||
              operand.place || operand.loan || operand.literal ||
              operand.type != owner.type)
            unsupported("LLVM transfer requires one direct consumer");
          consumed = true;
          if (stage.kind == MirInstructionKind::Initialize &&
              stage.destination && stage.operands.size() == 1 &&
              stage.info.type == owner.type) {
            result.publication = &stage;
            result.destination = *stage.destination;
          } else if (result.kind == ConstructorKind::Move &&
                     stage.kind == MirInstructionKind::CallInput &&
                     stage.callInputKind == HirCallInputKind::MoveValue &&
                     (stage.callInputRole == MirCallInputRole::Argument ||
                      (stage.callInputRole == MirCallInputRole::Receiver &&
                       stage.result && consumingReceiver(*stage.result))) &&
                     stage.destination && stage.preparedParameterDrop &&
                     stage.operands.size() == 1 &&
                     stage.info.type == owner.type) {
            // A by-value argument, or the receiver of a consuming call,
            // relocated into its exact prepared slot.
            result.publication = &stage;
            result.destination = *stage.destination;
            result.publishes = FreshHierarchyPublication::Argument;
          } else if (result.kind == ConstructorKind::Move &&
                     stage.kind == MirInstructionKind::Call &&
                     stage.intrinsic ==
                         IntrinsicKind::PolymorphicStorageConstruct &&
                     index == 1 && stage.operands.size() == 2 &&
                     stage.parameterTypes.size() == 2 &&
                     stage.parameterTypes[1] == owner.type) {
            result.publication = &stage;
            result.publishes = FreshHierarchyPublication::Slot;
          } else if (result.kind == ConstructorKind::Move &&
                     stage.kind == MirInstructionKind::CallInput &&
                     stage.callInputKind == HirCallInputKind::MoveValue &&
                     stage.callInputRole == MirCallInputRole::Receiver &&
                     !stage.destination && !stage.preparedParameterDrop &&
                     stage.result && stage.lifecycle.empty() &&
                     stage.operands.size() == 1 &&
                     stage.info.type == owner.type &&
                     consumingReceiver(*stage.result)) {
            // The moved object is relocated into its own value storage and
            // stays there as one consuming call's receiver; the caller keeps
            // its value obligation through the full expression.
            result.publication = &stage;
            result.publishes = FreshHierarchyPublication::Receiver;
          } else if (result.kind == ConstructorKind::Move &&
                     stage.kind == MirInstructionKind::Call &&
                     stage.intrinsic == IntrinsicKind::ThreadSpawn &&
                     stage.threadTaskAdapter && index == 1 &&
                     stage.operands.size() == 2 && !stage.destination &&
                     !stage.receiver && stage.scopedThreadTasks.empty()) {
            // The moved object waits in its own value storage until the
            // spawn relocates it into the runtime task box with this type's
            // generated relocation; the spawn takes over its obligation.
            result.publication = &stage;
            result.publishes = FreshHierarchyPublication::Task;
          } else if (result.kind == ConstructorKind::Move &&
                     stage.kind == MirInstructionKind::Call &&
                     stage.intrinsic == IntrinsicKind::RestoringExchange &&
                     index == 1 && stage.operands.size() == 2 &&
                     stage.parameterTypes.size() == 2 &&
                     stage.parameterTypes[1] == owner.type && stage.result &&
                     stage.info.type == owner.type && !stage.destination &&
                     !stage.receiver) {
            // The moved object waits in its own value storage until the
            // restoring exchange relocates it, with this type's generated
            // relocation, into the place the exchange vacates; the exchange
            // takes over its obligation.
            result.publication = &stage;
            result.publishes = FreshHierarchyPublication::Exchange;
          } else if (result.kind == ConstructorKind::Move && index == 0 &&
                     stage.kind == MirInstructionKind::Assign &&
                     stage.destination &&
                     stage.operation == MirOperation::Assign &&
                     stage.operands.size() == 1 &&
                     stage.info.type == owner.type) {
            // The moved object waits in its own value storage while the
            // paired cleanup ends the destination's previous object; the
            // assignment then relocates it into the destination.
            result.publication = &stage;
            result.publishes = FreshHierarchyPublication::Replacement;
          } else if (result.kind == ConstructorKind::Move &&
                     isFreshHierarchyAggregate(program, stage) &&
                     elementSlot(stage, index) == owner.type) {
            // The moved object is relocated into its slot of a fresh array,
            // payload alternative or closure environment, which publishes
            // into its own storage.
            result.publication = &stage;
            result.publishes = FreshHierarchyPublication::Element;
            result.element = index;
          } else {
            unsupported("LLVM transfer requires one direct consumer");
          }
        }
        if ((stage.receiver && stage.receiver->value == *producer.result) ||
            stage.literalProvenance.sourceValue == *producer.result ||
            std::find(stage.literalProvenance.sourceValues.begin(),
                      stage.literalProvenance.sourceValues.end(),
                      *producer.result) !=
                stage.literalProvenance.sourceValues.end())
          unsupported("LLVM transfer result has an alternate value consumer");
      }
    for (const auto &block : body.blocks)
      if (block.terminator.kind == MirTerminatorKind::Return &&
          block.terminator.value &&
          block.terminator.value->value == *producer.result) {
        // Returning a moved whole binding relocates it into the caller's
        // destination, and returning an inert copy writes its fields there;
        // the binding itself is never aliased to that storage.
        if (consumed ||
            block.terminator.value->kind != MirOperandKind::Value ||
            block.terminator.value->type != owner.type ||
            body.returnType != owner.type ||
            (identity.kind != MirBodyKind::Function &&
             identity.kind != MirBodyKind::ThreadLocalInitializer))
          unsupported("LLVM transfer requires one direct consumer");
        consumed = true;
        result.publishes = FreshHierarchyPublication::Returned;
      }
    if (!consumed)
      unsupported("LLVM transfer has no fresh local publication");
    const bool local = result.publishes == FreshHierarchyPublication::Local;
    const bool argument =
        result.publishes == FreshHierarchyPublication::Argument;
    const MirPlace *destination = nullptr;
    if (local || argument) {
      destination = body.findPlace(result.destination);
      if (!destination)
        malformed("LLVM transfer publication lost its destination");
    }
    if (argument) {
      const auto *prepared =
          body.findDropObligation(*result.publication->preparedParameterDrop);
      if (destination->root != MirPlaceRootKind::Temporary ||
          !destination->projections.empty() || destination->capture ||
          destination->foreignStorage || destination->type != owner.type ||
          destination->initiallyAvailable || !prepared ||
          prepared->kind != MirDropObligationKind::PreparedParameter ||
          prepared->place != destination->id)
        unsupported("LLVM transfer argument is not its exact prepared slot");
    }
    // A constructor publishes into its own field through the exact field
    // initializer that names it.
    const bool field = local && constructorField(*result.publication,
                                                 *destination, owner);
    if (local && !field &&
        (!wholeLocal(*destination) || destination->type != owner.type ||
         destination->initiallyAvailable ||
         destination->binding == place->binding))
      unsupported("LLVM transfer destination is not a fresh whole local");
    if (local || argument) {
      const auto &stage = *result.publication;
      if ((local && stage.result) || (argument && !stage.result) ||
          (local && stage.preparedParameterDrop) || stage.receiver ||
          stage.loan || (stage.constructorInitializer && !field) ||
          stage.constructorTarget || stage.functionTarget || stage.bodyTarget ||
          stage.intrinsic != IntrinsicKind::None ||
          stage.operation != MirOperation::None || stage.retainedOwnerPlace ||
          stage.successResultDrop || stage.successResultDestination ||
          stage.elementCleanup || stage.replacementCleanup ||
          stage.replacementPair || !stage.definedFailure.empty())
        unsupported("LLVM transfer publication has additional effects");
    }

    // A prepared argument may take the moved binding's own obligation
    // directly: the move then has no intermediate value drop.
    const bool bindingMove =
        argument && producer.lifecycle.empty() &&
        result.publication->lifecycle.size() == 1 &&
        result.publication->lifecycle.front().kind ==
            MirLifecycleEventKind::Move &&
        result.publication->lifecycle.front().source && [&] {
          const auto *from = body.findDropObligation(
              result.publication->lifecycle.front().source);
          return from && from->kind == MirDropObligationKind::Binding &&
                 from->place == place->id && from->binding == place->binding;
        }();
    if (result.kind == ConstructorKind::Move) {
      // A by-value parameter is an owned binding of this body too.
      if (!wholeBinding(*place) || place->type != owner.type || source.loan ||
          !place->key || !producer.ownership ||
          producer.ownership->kind != OwnershipEventKind::Move ||
          producer.ownership->place != *place->key ||
          producer.ownership->before != OwnershipStateSet::Available ||
          producer.ownership->after != OwnershipStateSet::Moved ||
          !producer.ownership->reachable)
        unsupported(
            "LLVM move requires its exact whole-binding ownership event");
      if (!producer.lifecycle.empty()) {
        if (producer.lifecycle.size() != 1)
          malformed("LLVM move has multiple lifecycle transitions");
        const auto &event = producer.lifecycle.front();
        if (event.kind != MirLifecycleEventKind::Move || !event.source ||
            !event.target || event.conditional || event.failureCleanup)
          malformed("LLVM move lost its source-to-value lifecycle transition");
        result.sourceDrop = event.source;
        result.valueDrop = event.target;
        const auto &from = drop(result.sourceDrop, owner, parameter(*place));
        const auto &to = drop(result.valueDrop, owner);
        const auto *temporary = body.findPlace(to.place);
        if (from.kind != MirDropObligationKind::Binding ||
            from.place != place->id || from.binding != place->binding ||
            to.kind != MirDropObligationKind::Value || !temporary ||
            temporary->root != MirPlaceRootKind::Value ||
            temporary->value != *producer.result ||
            !temporary->projections.empty() || to.dropType != from.dropType)
          malformed("LLVM move drop identities do not match its exact storage");
      } else if (owner.requiresActiveCleanup && !bindingMove) {
        malformed("LLVM active move has no source-to-value lifecycle transfer");
      }
      // A returned object leaves this invocation's ownership through the
      // return edge: the moved value's obligation, if any, transfers out
      // before that return. The slot relocates an inert payload itself.
      if (result.publishes == FreshHierarchyPublication::Returned) {
        if (result.valueDrop && !returnedObligation(result.valueDrop))
          unsupported("LLVM returned transfer must release its value "
                      "obligation at the return");
      } else if (result.publishes == FreshHierarchyPublication::Receiver ||
                 result.publishes == FreshHierarchyPublication::Replacement) {
        if (result.valueDrop &&
            !droppedObligation(result.valueDrop, result.publication))
          unsupported("LLVM consumed receiver must keep its value obligation "
                      "until its drop");
      } else if (result.publishes == FreshHierarchyPublication::Element) {
        // The aggregate takes the moved value's obligation as its own.
        if (result.valueDrop &&
            std::none_of(result.publication->lifecycle.begin(),
                         result.publication->lifecycle.end(),
                         [&](const auto &event) {
                           return event.kind ==
                                      MirLifecycleEventKind::TransferOut &&
                                  event.source == result.valueDrop &&
                                  !event.target && !event.conditional &&
                                  !event.failureCleanup;
                         }))
          unsupported("LLVM element transfer must hand its value obligation "
                      "to its aggregate");
      } else if (result.publishes == FreshHierarchyPublication::Task) {
        if (result.valueDrop &&
            !handedObligation(result.valueDrop, result.publication))
          unsupported("LLVM thread task transfer must hand its value "
                      "obligation to the spawn");
      } else if (result.publishes == FreshHierarchyPublication::Exchange) {
        if (result.valueDrop &&
            !handedObligation(result.valueDrop, result.publication))
          unsupported("LLVM exchanged transfer must hand its value "
                      "obligation to the exchange");
      } else if (!local && !argument &&
                 (owner.requiresActiveCleanup || result.valueDrop ||
                  (result.publishes == FreshHierarchyPublication::Slot &&
                   !result.publication->lifecycle.empty()))) {
        unsupported("LLVM transfer requires an inert slot object");
      }
    } else {
      // This scalar-only generated-copy profile has no active cleanup: source
      // destruction (including an active base) makes copying unavailable.
      // A future active-copy profile must accept a NEW value Initialize and
      // its Reparent publication, not mistake it for source mutation.
      if (!producer.lifecycle.empty() ||
          (producer.ownership &&
           (producer.ownership->kind != OwnershipEventKind::Read ||
            producer.ownership->before != OwnershipStateSet::Available ||
            producer.ownership->after != OwnershipStateSet::Available)))
        unsupported(
            "LLVM copy must preserve its source ownership and lifetime");
    }
    // An inert field a constructor initializes has no rollback obligation
    // to publish into: MIR records no drop for its slot and no transition.
    const bool unobligatedField =
        field && result.publication->lifecycle.empty() && !result.valueDrop &&
        !owner.requiresActiveCleanup &&
        std::none_of(body.dropObligations.begin(), body.dropObligations.end(),
                     [&](const auto &obligation) {
                       return obligation.place == destination->id;
                     });
    if ((local || argument) && !unobligatedField) {
      const auto &events = result.publication->lifecycle;
      if (events.size() != 1 || events.front().conditional ||
          events.front().failureCleanup || !events.front().target ||
          (!bindingMove &&
           (events.front().source != result.valueDrop ||
            events.front().kind != (result.valueDrop
                                        ? MirLifecycleEventKind::Reparent
                                        : MirLifecycleEventKind::Initialize))))
        malformed(
            "LLVM transfer lost its exact destination publication lifecycle");
      if (bindingMove)
        result.sourceDrop = events.front().source;
      const auto &published = drop(events.front().target, owner);
      if (argument ? published.kind != MirDropObligationKind::PreparedParameter ||
                         published.id !=
                             *result.publication->preparedParameterDrop ||
                         published.place != destination->id
          : field  ? published.kind !=
                             MirDropObligationKind::ConstructionRollback ||
                         published.place != destination->id
                   : published.kind != MirDropObligationKind::Binding ||
                         published.place != destination->id ||
                         published.binding != destination->binding)
        malformed("LLVM transfer publication drop names different storage");
    }

    const auto used = [&](const MirOperand &operand) {
      return operand.value == *producer.result;
    };
    const auto resultPlace = [&](MirPlaceId id) {
      const auto *selected = body.findPlace(id);
      return selected && selected->root == MirPlaceRootKind::Value &&
             selected->value == *producer.result;
    };
    // A returned value's obligation may move into the return-result
    // obligation that a failing cleanup before the return drops instead.
    const MirDropObligationId returnResult =
        result.publishes == FreshHierarchyPublication::Returned
            ? reparentedReturnResult(result.valueDrop)
            : 0;
    for (const auto &block : body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        for (const auto &operand : stage.operands)
          if (resultPlace(operand.place))
            unsupported(
                "LLVM transfer result has an intermediate address consumer");
        if (stage.receiver && resultPlace(stage.receiver->place))
          unsupported("LLVM transfer result supplies a method receiver");
        if (stage.destination && resultPlace(*stage.destination) &&
            (stage.kind != MirInstructionKind::Drop || !result.valueDrop ||
             std::none_of(stage.lifecycle.begin(), stage.lifecycle.end(),
                          [&](const auto &event) {
                            return event.kind == MirLifecycleEventKind::Drop &&
                                   (event.source == result.valueDrop ||
                                    (returnResult &&
                                     event.source == returnResult));
                          })))
          unsupported("LLVM transfer result has an intermediate destination");
      }
      const auto &term = block.terminator;
      const bool returnedHere =
          result.publishes == FreshHierarchyPublication::Returned &&
          term.kind == MirTerminatorKind::Return && term.value &&
          used(*term.value);
      if ((term.value && used(*term.value) && !returnedHere) ||
          std::any_of(term.targetArguments.begin(), term.targetArguments.end(),
                      used) ||
          std::any_of(term.elseArguments.begin(), term.elseArguments.end(),
                      used))
        unsupported("LLVM transfer result escapes through a terminator");
      for (const auto &target : term.switchTargets)
        if (std::any_of(target.arguments.begin(), target.arguments.end(), used))
          unsupported("LLVM transfer result escapes through an edge");
    }
    for (const auto &candidate : body.places) {
      if (candidate.root == MirPlaceRootKind::Value &&
          candidate.value == *producer.result &&
          (!result.valueDrop ||
           body.findDropObligation(result.valueDrop)->place != candidate.id ||
           !candidate.projections.empty() || candidate.type != owner.type))
        unsupported("LLVM transfer result has an intermediate place use");
      for (const auto &projection : candidate.projections)
        if (projection.index == *producer.result)
          unsupported("LLVM transfer result is used as a place index");
    }
    for (const auto &loan : body.loans)
      if (resultPlace(loan.source) ||
          (loan.referenceSelection &&
           loan.referenceSelection->value == *producer.result))
        unsupported("LLVM transfer result supplies a selected reference");
    return result;
  }

private:
  // A payload enum moved out of a whole binding or out of a field of this
  // body's consumed receiver, or the one field of a payload alternative
  // moved out of a whole enum binding. The type's generated relocation moves
  // the object into the one fresh whole local that publishes it; MIR's
  // ownership and lifecycle events prove the source is not used or dropped
  // again.
  FreshHierarchyTransfer relocation(const MirInstruction &producer) const {
    const bool extraction = isPayloadFieldMove(producer);
    const auto &type = producer.info.type;
    if (!producer.localFailureSites.empty() ||
        !producer.parameterTypes.empty() || producer.constructorTarget ||
        producer.constructorKind != ConstructorKind::Ordinary ||
        (!extraction && producer.kind != MirInstructionKind::Move))
      unsupported("LLVM payload relocation excludes additional effects");
    if (!containsHierarchyStorage(program, type) ||
        mirTypeMoveRelocationKind(program, type) ==
            RelocationKind::Unavailable ||
        (type.kind == SemanticType::Enum && !program.findPayloadEnum(type)))
      unsupported(
          "LLVM payload relocation requires relocatable hierarchy storage");
    FreshHierarchyTransfer result{.source = producer.operands.front(),
                                  .kind = ConstructorKind::Move};
    result.type = type;
    if (type.kind == SemanticType::Class) {
      const auto &owner = instance(type);
      if (owner.kind == ClassKind::Interface || owner.abstract)
        unsupported(
            "LLVM transfer destination must be a concrete ordinary class");
      relocationShape(owner);
      result.owner = owner.id;
    }
    const auto canonical = mirUniqueCleanupType(program, type);
    const auto recipe = [&](const MirDropObligation &drop) {
      return canonical ? drop.dropType == *canonical
                       : drop.dropType.type == type &&
                             !drop.dropType.requiresActiveCleanup;
    };
    // A by-value parameter owns its object from entry; any other source or
    // value obligation begins inside this body.
    const auto obligation = [&](MirDropObligationId id,
                                bool entry =
                                    false) -> const MirDropObligation & {
      const auto *found = body.findDropObligation(id);
      if (!found || found->id != id || (found->initiallyActive && !entry))
        malformed("LLVM payload relocation lost its exact drop obligation");
      return *found;
    };
    const std::vector<HirBindingId> *parameters =
        identity.kind == MirBodyKind::Function
            ? &program.findFunctionInstance(identity.owner)->parameterBindings
        : identity.kind == MirBodyKind::Constructor
            ? &program.findConstructorInstance(identity.owner)
                   ->parameterBindings
            : nullptr;
    const auto parameter = [&](const MirPlace &place) {
      return parameters && place.root == MirPlaceRootKind::Binding &&
             std::find(parameters->begin(), parameters->end(), place.binding) !=
                 parameters->end();
    };
    const auto bindingObligation =
        [&](const MirPlace &place) -> const MirDropObligation * {
      const MirDropObligation *found = nullptr;
      for (const auto &drop : body.dropObligations)
        if (drop.place == place.id) {
          if (found || drop.kind != MirDropObligationKind::Binding ||
              drop.binding != place.binding)
            unsupported("LLVM payload relocation names shared storage");
          found = &drop;
        }
      return found;
    };
    const auto wholeEnum = [&](const MirPlace &place) {
      return place.root == MirPlaceRootKind::Binding && place.binding &&
             !place.capture && !place.foreignStorage &&
             place.projections.empty() &&
             place.type.kind == SemanticType::Enum &&
             program.findPayloadEnum(place.type);
    };
    // A whole closure binding, such as a by-value callable parameter.
    const auto wholeClosure = [&](const MirPlace &place) {
      return place.root == MirPlaceRootKind::Binding && place.binding &&
             !place.capture && !place.foreignStorage &&
             place.projections.empty() &&
             place.type.kind == SemanticType::Lambda;
    };
    const auto *place = body.findPlace(result.source.place);
    if (!place || place->id != result.source.place)
      malformed("LLVM payload relocation lost its source place");
    if (extraction) {
      // One field of a one-field alternative leaves its enum binding, whose
      // obligation, if any, ends here.
      const auto *payload =
          wholeEnum(*place) ? program.findPayloadEnum(place->type) : nullptr;
      if (!payload || *producer.enumVariant >= payload->variants.size() ||
          payload->variants[*producer.enumVariant].payloadTypes.size() != 1 ||
          *producer.payloadIndex != 0 ||
          payload->variants[*producer.enumVariant].payloadTypes.front() !=
              type ||
          result.source.type != place->type || producer.ownership)
        unsupported("LLVM payload field move requires the field of a "
                    "one-field alternative in a whole enum binding");
      result.sourceVariant = *producer.enumVariant;
      result.sourceField = 0;
      const auto *enumDrop = bindingObligation(*place);
      if (producer.lifecycle.empty() ? enumDrop != nullptr
                                     : producer.lifecycle.size() != 1 ||
                                           !enumDrop ||
                                           producer.lifecycle.front().kind !=
                                               MirLifecycleEventKind::
                                                   TransferOut ||
                                           producer.lifecycle.front().source !=
                                               enumDrop->id ||
                                           producer.lifecycle.front().target ||
                                           producer.lifecycle.front().conditional ||
                                           producer.lifecycle.front()
                                               .failureCleanup)
        unsupported("LLVM payload field move must end its enum obligation");
      if (enumDrop)
        result.sourceDrop = enumDrop->id;
      // Nothing in the extracting block reads the enum after the move.
      for (const auto &block : body.blocks) {
        bool after = false;
        for (const auto &stage : block.instructions) {
          if (&stage == &producer) {
            after = true;
            continue;
          }
          if (!after || stage.tombstone)
            continue;
          const auto names = [&](const MirOperand &operand) {
            return operand.place == place->id;
          };
          if ((stage.destination && *stage.destination == place->id) ||
              std::any_of(stage.operands.begin(), stage.operands.end(),
                          names) ||
              (stage.receiver && names(*stage.receiver)))
            unsupported("LLVM payload field move source is used again");
        }
        if (after && block.terminator.value &&
            block.terminator.value->place == place->id)
          unsupported("LLVM payload field move source is used again");
      }
    } else {
      const auto *function = identity.kind == MirBodyKind::Function
                                 ? program.findFunctionInstance(identity.owner)
                                 : nullptr;
      // A consumed receiver is this body's own object: moving its field
      // begins the moved value's obligation.
      const bool receiverField =
          place->root == MirPlaceRootKind::This && !place->capture &&
          !place->foreignStorage && place->projections.size() == 1 &&
          place->projections.front().kind == MirProjectionKind::Field &&
          function &&
          function->receiverMutability == ReceiverMutability::Consuming;
      if ((!wholeEnum(*place) && !wholeClosure(*place) && !receiverField) ||
          place->type != type || result.source.type != type || !place->key ||
          !producer.ownership ||
          producer.ownership->kind != OwnershipEventKind::Move ||
          producer.ownership->place != *place->key ||
          producer.ownership->before != OwnershipStateSet::Available ||
          producer.ownership->after != OwnershipStateSet::Moved ||
          !producer.ownership->reachable)
        unsupported("LLVM payload move requires its exact ownership event");
      if (!producer.lifecycle.empty()) {
        const auto &event = producer.lifecycle.front();
        if (producer.lifecycle.size() != 1 || !event.target ||
            event.conditional || event.failureCleanup ||
            (receiverField
                 ? event.kind != MirLifecycleEventKind::Initialize ||
                       event.source
                 : event.kind != MirLifecycleEventKind::Move || !event.source))
          malformed("LLVM payload move lost its exact lifecycle transition");
        if (!receiverField) {
          const auto &from =
              obligation(event.source, type.kind == SemanticType::Lambda &&
                                           parameter(*place));
          if (from.kind != MirDropObligationKind::Binding ||
              from.place != place->id || from.binding != place->binding ||
              !recipe(from))
            malformed("LLVM payload move source obligation names other "
                      "storage");
          result.sourceDrop = from.id;
        }
        result.valueDrop = event.target;
        const auto &to = obligation(result.valueDrop);
        const auto *temporary = body.findPlace(to.place);
        if (to.kind != MirDropObligationKind::Value || !temporary ||
            temporary->root != MirPlaceRootKind::Value ||
            temporary->value != *producer.result ||
            !temporary->projections.empty() || !recipe(to))
          malformed("LLVM payload move value obligation names other storage");
      } else if (canonical && canonical->requiresActiveCleanup) {
        malformed("LLVM active payload move has no lifecycle transition");
      } else if (!receiverField && bindingObligation(*place) &&
                 bindingObligation(*place)->dropType.requiresActiveCleanup) {
        malformed("LLVM payload move left its source obligation behind");
      }
    }

    // The one consumer publishes into a fresh whole local, or a moved
    // closure is the task a managed-thread spawn relocates into its box.
    const MirInstruction *publication = nullptr;
    bool task = false;
    bool argument = false;
    bool element = false;
    std::size_t elementIndex = 0;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        if (stage.result == producer.result && &stage != &producer)
          malformed("LLVM transfer result has repeated producers");
        for (std::size_t index = 0; index < stage.operands.size(); ++index) {
          const auto &operand = stage.operands[index];
          if (operand.value != *producer.result)
            continue;
          if (publication || operand.kind != MirOperandKind::Value ||
              operand.place || operand.loan || operand.literal ||
              operand.type != type)
            unsupported("LLVM payload relocation requires one fresh local "
                        "publication");
          if (type.kind == SemanticType::Lambda &&
              stage.kind == MirInstructionKind::Call &&
              stage.intrinsic == IntrinsicKind::ThreadSpawn &&
              stage.threadTaskAdapter && index == 1 &&
              stage.operands.size() == 2 && !stage.destination &&
              !stage.receiver && stage.scopedThreadTasks.empty())
            task = true;
          // A moved closure passed on by value relocates into the exact
          // prepared slot of its by-value parameter.
          else if (type.kind == SemanticType::Lambda &&
                   stage.kind == MirInstructionKind::CallInput &&
                   stage.callInputKind == HirCallInputKind::MoveValue &&
                   stage.callInputRole == MirCallInputRole::Argument &&
                   stage.destination && stage.preparedParameterDrop &&
                   stage.result && stage.operands.size() == 1 &&
                   stage.info.type == type)
            argument = true;
          // A moved closure captured again by value relocates into its slot
          // of the fresh outer closure environment.
          else if (type.kind == SemanticType::Lambda &&
                   isFreshHierarchyAggregate(program, stage) &&
                   stage.operation == MirOperation::Closure &&
                   elementSlot(stage, index) == type) {
            element = true;
            elementIndex = index;
          } else if (stage.kind != MirInstructionKind::Initialize ||
                   !stage.destination || stage.operands.size() != 1 ||
                   stage.info.type != type)
            unsupported("LLVM payload relocation requires one fresh local "
                        "publication");
          publication = &stage;
        }
        if (stage.receiver && stage.receiver->value == *producer.result)
          unsupported("LLVM payload relocation result supplies a receiver");
      }
    // A moved closure returned by value relocates into the caller's
    // destination through the return edge.
    bool returned = false;
    if (type.kind == SemanticType::Lambda)
      for (const auto &block : body.blocks)
        if (block.terminator.kind == MirTerminatorKind::Return &&
            block.terminator.value &&
            block.terminator.value->value == *producer.result) {
          if (publication || returned ||
              block.terminator.value->kind != MirOperandKind::Value ||
              block.terminator.value->type != type ||
              body.returnType != type ||
              identity.kind != MirBodyKind::Function)
            unsupported("LLVM transfer requires one direct consumer");
          returned = true;
        }
    if (returned) {
      if (result.valueDrop && !returnedObligation(result.valueDrop))
        unsupported("LLVM returned transfer must release its value "
                    "obligation at the return");
      result.publishes = FreshHierarchyPublication::Returned;
    }
    if (!publication && !returned)
      unsupported("LLVM payload relocation has no fresh local publication");
    if (task) {
      // The moved closure waits in its own value storage until the spawn
      // relocates it into the runtime task box with its generated relocation;
      // the spawn takes over its value obligation.
      if (!result.valueDrop ||
          !handedObligation(result.valueDrop, publication))
        unsupported("LLVM thread task transfer must hand its value "
                    "obligation to the spawn");
      result.publication = publication;
      result.publishes = FreshHierarchyPublication::Task;
    }
    if (element) {
      // The outer closure takes the moved value's obligation as its own.
      if (result.valueDrop &&
          std::none_of(publication->lifecycle.begin(),
                       publication->lifecycle.end(), [&](const auto &event) {
                         return event.kind ==
                                    MirLifecycleEventKind::TransferOut &&
                                event.source == result.valueDrop &&
                                !event.target && !event.conditional &&
                                !event.failureCleanup;
                       }))
        unsupported("LLVM element transfer must hand its value obligation "
                    "to its aggregate");
      result.publication = publication;
      result.publishes = FreshHierarchyPublication::Element;
      result.element = elementIndex;
    }
    if (argument) {
      const auto *slot = body.findPlace(*publication->destination);
      const auto *prepared =
          body.findDropObligation(*publication->preparedParameterDrop);
      const auto &events = publication->lifecycle;
      if (!slot || slot->root != MirPlaceRootKind::Temporary ||
          !slot->projections.empty() || slot->capture ||
          slot->foreignStorage || slot->type != type ||
          slot->initiallyAvailable || !prepared ||
          prepared->kind != MirDropObligationKind::PreparedParameter ||
          prepared->place != slot->id)
        unsupported("LLVM transfer argument is not its exact prepared slot");
      if (events.size() != 1 || events.front().conditional ||
          events.front().failureCleanup ||
          events.front().target != prepared->id ||
          events.front().source != result.valueDrop ||
          events.front().kind != (result.valueDrop
                                      ? MirLifecycleEventKind::Reparent
                                      : MirLifecycleEventKind::Initialize) ||
          publication->receiver || publication->loan ||
          publication->constructorTarget || publication->functionTarget ||
          publication->intrinsic != IntrinsicKind::None ||
          publication->operation != MirOperation::None ||
          !publication->definedFailure.empty())
        malformed("LLVM transfer lost its exact destination publication "
                  "lifecycle");
      result.publication = publication;
      result.destination = slot->id;
      result.publishes = FreshHierarchyPublication::Argument;
    }
    const auto *destination = task || argument || element || returned
                                  ? nullptr
                                  : body.findPlace(*publication->destination);
    const auto *formal = parameters;
    if (!task && !argument && !element && !returned &&
        (!destination || destination->root != MirPlaceRootKind::Binding ||
         !destination->binding || destination->capture ||
         destination->foreignStorage || !destination->projections.empty() ||
         destination->type != type || destination->initiallyAvailable ||
         (place->root == MirPlaceRootKind::Binding &&
          destination->binding == place->binding) ||
         (formal && std::find(formal->begin(), formal->end(),
                              destination->binding) != formal->end())))
      unsupported("LLVM payload relocation destination is not a fresh whole "
                  "local");
    if (!task && !argument && !element && !returned &&
        (publication->result || publication->receiver || publication->loan ||
         publication->constructorInitializer ||
         publication->constructorTarget || publication->functionTarget ||
         publication->bodyTarget ||
         publication->intrinsic != IntrinsicKind::None ||
         publication->operation != MirOperation::None ||
         publication->preparedParameterDrop ||
         publication->retainedOwnerPlace || publication->successResultDrop ||
         publication->successResultDestination || publication->elementCleanup ||
         publication->replacementCleanup || publication->replacementPair ||
         !publication->definedFailure.empty()))
      unsupported("LLVM transfer publication has additional effects");
    if (!task && !argument && !element && !returned) {
      const auto *published = bindingObligation(*destination);
      const auto &events = publication->lifecycle;
      if (events.empty() ? published || result.valueDrop
                         : events.size() != 1 || !published ||
                               events.front().conditional ||
                               events.front().failureCleanup ||
                               events.front().target != published->id ||
                               events.front().source != result.valueDrop ||
                               events.front().kind !=
                                   (result.valueDrop
                                        ? MirLifecycleEventKind::Reparent
                                        : MirLifecycleEventKind::Initialize) ||
                               !recipe(*published))
        malformed("LLVM transfer lost its exact destination publication "
                  "lifecycle");
      result.publication = publication;
      result.destination = destination->id;
      result.publishes = FreshHierarchyPublication::Local;
    }

    // The result is used only by its publication, and its value place only
    // by the drops of its value obligation.
    const auto resultPlace = [&](MirPlaceId id) {
      const auto *selected = body.findPlace(id);
      return selected && selected->root == MirPlaceRootKind::Value &&
             selected->value == *producer.result;
    };
    for (const auto &block : body.blocks) {
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        for (const auto &operand : stage.operands)
          if (resultPlace(operand.place))
            unsupported(
                "LLVM transfer result has an intermediate address consumer");
        if (stage.receiver && resultPlace(stage.receiver->place))
          unsupported("LLVM transfer result supplies a method receiver");
        if (stage.destination && resultPlace(*stage.destination) &&
            (stage.kind != MirInstructionKind::Drop || !result.valueDrop ||
             std::none_of(stage.lifecycle.begin(), stage.lifecycle.end(),
                          [&](const auto &event) {
                            return event.kind == MirLifecycleEventKind::Drop &&
                                   event.source == result.valueDrop;
                          })))
          unsupported("LLVM transfer result has an intermediate destination");
      }
      const auto used = [&](const MirOperand &operand) {
        return operand.value == *producer.result;
      };
      const auto &term = block.terminator;
      if ((term.value && used(*term.value) &&
           !(returned && term.kind == MirTerminatorKind::Return)) ||
          std::any_of(term.targetArguments.begin(), term.targetArguments.end(),
                      used) ||
          std::any_of(term.elseArguments.begin(), term.elseArguments.end(),
                      used))
        unsupported("LLVM transfer result escapes through a terminator");
      for (const auto &target : term.switchTargets)
        if (std::any_of(target.arguments.begin(), target.arguments.end(), used))
          unsupported("LLVM transfer result escapes through an edge");
    }
    for (const auto &candidate : body.places) {
      if (candidate.root == MirPlaceRootKind::Value &&
          candidate.value == *producer.result &&
          (!result.valueDrop ||
           body.findDropObligation(result.valueDrop)->place != candidate.id ||
           !candidate.projections.empty() || candidate.type != type))
        unsupported("LLVM transfer result has an intermediate place use");
      for (const auto &projection : candidate.projections)
        if (projection.index == *producer.result)
          unsupported("LLVM transfer result is used as a place index");
    }
    for (const auto &loan : body.loans)
      if (resultPlace(loan.source) ||
          (loan.referenceSelection &&
           loan.referenceSelection->value == *producer.result))
        unsupported("LLVM transfer result supplies a selected reference");
    return result;
  }

  const MirProgram &program;
  const MirBody &body;
  MirBodyAddress identity;

  [[noreturn]] static void unsupported(const char *message) {
    throw BackendUnsupportedError(message);
  }
  [[noreturn]] static void malformed(const char *message) {
    throw std::logic_error(message);
  }
  static const SemanticType &unreference(const SemanticType &type) {
    if (type.kind != SemanticType::Reference)
      return type;
    if (type.arguments.size() != 1)
      malformed("LLVM transfer has malformed reference metadata");
    return type.arguments.front();
  }
public:
  static const MirClassInstance &instanceOf(const MirProgram &program,
                                            const SemanticType &type) {
    const MirClassInstance *result = nullptr;
    for (const auto &owner : program.classInstances())
      if (owner.type == type) {
        if (result)
          malformed("LLVM transfer has repeated class type identity");
        result = &owner;
      }
    if (!result || program.findClassInstance(result->id) != result)
      malformed("LLVM transfer lost its exact class instance");
    return *result;
  }

private:
  const MirClassInstance &instance(const SemanticType &type) const {
    const MirClassInstance *result = nullptr;
    for (const auto &owner : program.classInstances())
      if (owner.type == type) {
        if (result)
          malformed("LLVM transfer has repeated class type identity");
        result = &owner;
      }
    if (!result || program.findClassInstance(result->id) != result)
      malformed("LLVM transfer lost its exact class instance");
    return *result;
  }
  // The field a constructor's own field initializer stage completes.
  bool constructorField(const MirInstruction &publication,
                        const MirPlace &destination,
                        const MirClassInstance &owner) const {
    if (identity.kind != MirBodyKind::Constructor ||
        publication.kind != MirInstructionKind::Initialize ||
        !publication.constructorInitializer ||
        destination.root != MirPlaceRootKind::This || destination.capture ||
        destination.foreignStorage || destination.projections.size() != 1 ||
        destination.projections.front().kind != MirProjectionKind::Field ||
        destination.type != owner.type)
      return false;
    const auto *constructor = program.findConstructorInstance(identity.owner);
    if (!constructor ||
        publication.constructorInitializer > constructor->initializers.size())
      return false;
    const auto &selected =
        constructor->initializers[publication.constructorInitializer - 1];
    return selected.kind == ConstructorInitializerTargetKind::Field &&
           selected.field == destination.projections.front().field &&
           selected.targetType == destination.type && !selected.storesReference;
  }
  // The moved value's obligation is created by the producer's move and ends
  // only through one unconditional TransferOut in the block whose terminator
  // returns it, or through a failure-path drop before that release. When a
  // cleanup that can fail runs between the move and the return, such as a
  // sibling local's destructor, MIR first reparents it, unconditionally and
  // once, into the body's return-result obligation on the same value place;
  // that obligation then ends the same way.
  bool returnedObligation(MirDropObligationId obligation) const {
    const auto *moved = body.findDropObligation(obligation);
    MirDropObligationId result = 0;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        for (const auto &event : stage.lifecycle) {
          if (event.source != obligation ||
              event.kind != MirLifecycleEventKind::Reparent)
            continue;
          const auto *target = body.findDropObligation(event.target);
          if (result || !moved || !target ||
              target->kind != MirDropObligationKind::ReturnResult ||
              target->place != moved->place ||
              target->dropType != moved->dropType || target->initiallyActive)
            return false;
          result = target->id;
        }
      }
    return result ? releasedAtReturn(obligation, 0, result) &&
                        releasedAtReturn(result, obligation, 0)
                  : releasedAtReturn(obligation, 0, 0);
  }
  // The return-result obligation a moved value's obligation is reparented
  // into before a cleanup that can fail, if any.
  MirDropObligationId
  reparentedReturnResult(MirDropObligationId obligation) const {
    if (!obligation)
      return 0;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions)
        if (!stage.tombstone)
          for (const auto &event : stage.lifecycle)
            if (event.source == obligation &&
                event.kind == MirLifecycleEventKind::Reparent)
              if (const auto *target = body.findDropObligation(event.target);
                  target && target->kind == MirDropObligationKind::ReturnResult)
                return target->id;
    return 0;
  }
  // Whether `obligation` ends only through failure-path drops plus either one
  // unconditional TransferOut in the block whose terminator returns, or, with
  // `next`, one unconditional Reparent into `next`. It is created by the
  // producer's move or, with `incoming`, by that obligation's Reparent.
  bool releasedAtReturn(MirDropObligationId obligation,
                        MirDropObligationId incoming,
                        MirDropObligationId next) const {
    const MirBlock *release = nullptr;
    bool handed = false;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        for (const auto &event : stage.lifecycle) {
          if (event.target == obligation &&
              event.kind != MirLifecycleEventKind::Move &&
              !(incoming && event.kind == MirLifecycleEventKind::Reparent &&
                event.source == incoming && !event.conditional &&
                !event.failureCleanup))
            return false;
          if (event.source != obligation)
            continue;
          if (event.kind == MirLifecycleEventKind::Drop &&
              stage.kind == MirInstructionKind::Drop)
            continue;
          if (next) {
            if (handed || event.kind != MirLifecycleEventKind::Reparent ||
                event.target != next || event.conditional ||
                event.failureCleanup)
              return false;
            handed = true;
            continue;
          }
          if (release || event.kind != MirLifecycleEventKind::TransferOut ||
              event.target || event.conditional || event.failureCleanup)
            return false;
          release = &block;
        }
      }
    if (next)
      return handed;
    return release && release->terminator.kind == MirTerminatorKind::Return &&
           release->terminator.value;
  }
  // The moved value's obligation is created by the producer's move and ends
  // only through drops of its own value place, or through the replacement of
  // the assignment that consumes it.
  bool droppedObligation(MirDropObligationId obligation,
                         const MirInstruction *replacement) const {
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        for (const auto &event : stage.lifecycle) {
          if (event.target == obligation &&
              event.kind != MirLifecycleEventKind::Move)
            return false;
          if (event.source == obligation &&
              !(event.kind == MirLifecycleEventKind::Drop &&
                stage.kind == MirInstructionKind::Drop) &&
              !(event.kind == MirLifecycleEventKind::Replace &&
                &stage == replacement &&
                stage.kind == MirInstructionKind::Assign &&
                !event.conditional && !event.failureCleanup))
            return false;
        }
      }
    return true;
  }
  // The moved value's obligation ends only through one unconditional
  // TransferOut at the consumer that relocates it out of the value's own
  // storage -- a spawn into its task box, or a restoring exchange into the
  // place it vacates -- or through a failure-path drop before that consumer.
  bool handedObligation(MirDropObligationId obligation,
                        const MirInstruction *consumer) const {
    bool released = false;
    for (const auto &block : body.blocks)
      for (const auto &stage : block.instructions) {
        if (stage.tombstone)
          continue;
        for (const auto &event : stage.lifecycle) {
          if (event.target == obligation &&
              event.kind != MirLifecycleEventKind::Move)
            return false;
          if (event.source != obligation)
            continue;
          if (event.kind == MirLifecycleEventKind::Drop &&
              stage.kind == MirInstructionKind::Drop)
            continue;
          if (released || &stage != consumer ||
              event.kind != MirLifecycleEventKind::TransferOut ||
              event.target || event.conditional || event.failureCleanup)
            return false;
          released = true;
        }
      }
    return released;
  }
  // A receiver input used exactly once, as the receiver of a direct or
  // virtual call to a consuming member of a class.
  // The element type of one operand slot of a fresh hierarchy aggregate.
  SemanticType elementSlot(const MirInstruction &aggregate,
                           std::size_t index) const {
    // A closure captures a moved object by value in its environment slot.
    if (aggregate.operation == MirOperation::Closure)
      return index < aggregate.closureCaptureTypes.size() &&
                     index < aggregate.closureCaptureModes.size() &&
                     aggregate.closureCaptureModes[index] ==
                         LambdaCaptureMode::Move
                 ? aggregate.closureCaptureTypes[index]
                 : SemanticType::Unknown;
    if (aggregate.operation == MirOperation::PayloadConstruct) {
      const auto *payload = program.findPayloadEnum(aggregate.info.type);
      return payload && aggregate.enumVariant &&
                     *aggregate.enumVariant < payload->variants.size() &&
                     index < payload->variants[*aggregate.enumVariant]
                                 .payloadTypes.size()
                 ? payload->variants[*aggregate.enumVariant]
                       .payloadTypes[index]
                 : SemanticType::Unknown;
    }
    return aggregate.info.type.arguments.size() == 1
               ? aggregate.info.type.arguments.front()
               : SemanticType::Unknown;
  }
  bool consumingReceiver(MirValueId value) const {
    const MirInstruction *call = nullptr;
    for (const auto &use : body.usesOf(value)) {
      const auto *block = body.findBlock(use.block);
      if (use.kind != MirValueUseKind::InstructionReceiver || !block || call)
        return false;
      for (const auto &stage : block->instructions)
        if (!stage.tombstone && stage.id == use.instruction)
          call = &stage;
    }
    if (!call || call->kind != MirInstructionKind::Call || !call->receiver ||
        call->receiver->kind != MirOperandKind::Value ||
        call->receiver->value != value || !call->functionTarget)
      return false;
    const auto *target = program.findFunctionInstance(*call->functionTarget);
    return target && target->owner && !target->staticMember &&
           target->receiverMutability == ReceiverMutability::Consuming;
  }
  bool parameter(const MirPlace &place) const {
    const std::vector<HirBindingId> *formal = nullptr;
    if (identity.kind == MirBodyKind::Function)
      formal = &program.findFunctionInstance(identity.owner)->parameterBindings;
    else if (identity.kind == MirBodyKind::Constructor)
      formal =
          &program.findConstructorInstance(identity.owner)->parameterBindings;
    return formal && place.root == MirPlaceRootKind::Binding &&
           std::find(formal->begin(), formal->end(), place.binding) !=
               formal->end();
  }
  // One concrete element of a by-value parameter pack is itself an owned
  // whole object of this body.
  bool packElement(const MirPlace &place) const {
    return place.root == MirPlaceRootKind::Binding && place.binding &&
           parameter(place) && place.projections.size() == 1 &&
           place.projections.front().kind == MirProjectionKind::PackElement;
  }
  bool wholeBinding(const MirPlace &place) const {
    return place.root == MirPlaceRootKind::Binding && place.binding &&
           !place.capture && !place.foreignStorage &&
           (place.projections.empty() || packElement(place)) &&
           place.type.kind == SemanticType::Class;
  }
  void relocationShape(const MirClassInstance &owner) const {
    if ((owner.kind != ClassKind::Class && owner.kind != ClassKind::Struct) ||
        owner.abstract || owner.cAbiRecord || owner.cAbiLayout ||
        owner.cOpaqueHandle || owner.unionLayout ||
        owner.compilerCapability != CompilerCapabilityTypeKind::None)
      unsupported("LLVM transfer requires an ordinary hierarchy");
    // A borrow carrier relocates as the addresses it holds; MIR's generated
    // value move decides that, and its loans follow the moved object.
    if (owner.moveConstructor != SpecialMemberStatus::Generated ||
        !owner.traits.movable ||
        mirTypeMoveRelocationKind(program, owner.type) ==
            RelocationKind::Unavailable ||
        !mirTypeRelocationIsDefinedFailureFree(program, owner.type))
      unsupported("LLVM move requires its verified generated relocation");
    if (owner.requiresActiveDropState != owner.destructor.has_value())
      malformed("LLVM transfer own active flag has no exact destructor");
    for (const auto &selection : owner.virtualDispatch) {
      const auto *target = program.findFunctionInstance(selection.target);
      if (!target)
        malformed("LLVM transfer state lost its selected function");
      if (target->pureVirtual)
        unsupported("LLVM transfer excludes a pure executable state");
    }
  }
  bool wholeLocal(const MirPlace &place) const {
    if (place.root != MirPlaceRootKind::Binding || !place.binding ||
        place.capture || place.foreignStorage || !place.projections.empty() ||
        place.type.kind != SemanticType::Class)
      return false;
    if (identity.kind == MirBodyKind::Function) {
      const auto &formal =
          program.findFunctionInstance(identity.owner)->parameterBindings;
      return std::find(formal.begin(), formal.end(), place.binding) ==
             formal.end();
    }
    if (identity.kind == MirBodyKind::Constructor) {
      const auto &formal =
          program.findConstructorInstance(identity.owner)->parameterBindings;
      return std::find(formal.begin(), formal.end(), place.binding) ==
             formal.end();
    }
    return identity.kind == MirBodyKind::Destructor;
  }
  static bool simpleLoan(const MirLoan &loan) {
    return !loan.referenceSelection && !loan.storedDependency &&
           loan.additionalDependencies.empty() && loan.conditions.empty() &&
           !loan.twoPhase && !loan.lifecycleBound && !loan.escapes &&
           (loan.kind == MirLoanKind::Local ||
            (loan.kind == MirLoanKind::Parameter && loan.entry));
  }
public:
  static void shape(const MirProgram &program, const MirClassInstance &owner,
                    ConstructorKind kind, bool special,
                    std::set<HirClassInstanceId> &path) {
    const bool interface = owner.kind == ClassKind::Interface;
    if ((!interface && owner.kind != ClassKind::Class &&
         owner.kind != ClassKind::Struct) ||
        (!interface && owner.abstract) || owner.cAbiRecord ||
        owner.cAbiLayout || owner.cOpaqueHandle || owner.unionLayout ||
        owner.compilerCapability != CompilerCapabilityTypeKind::None ||
        owner.traits.containsBorrowedState)
      unsupported("LLVM transfer requires a borrow-free ordinary hierarchy");
    if (!path.insert(owner.id).second)
      malformed("LLVM transfer hierarchy has an inline base cycle");
    if (special && kind == ConstructorKind::Copy && owner.requiresActiveCleanup)
      unsupported("LLVM scalar hierarchy copy excludes active-copy lifetimes");
    if (special && !interface &&
        (kind == ConstructorKind::Copy
             ? owner.copyConstructor != SpecialMemberStatus::Generated ||
                   !owner.traits.copyable
             : owner.moveConstructor != SpecialMemberStatus::Generated ||
                   !owner.traits.movable))
      unsupported("LLVM transfer requires its generated special-member policy");
    if (interface &&
        (!owner.declaredFields.empty() || owner.destructor ||
         owner.requiresActiveDropState || owner.requiresActiveCleanup))
      unsupported("LLVM transfer interface metadata carries object state");
    if (owner.requiresActiveDropState != owner.destructor.has_value())
      malformed("LLVM transfer own active flag has no exact destructor");
    if (owner.destructor) {
      const auto *destructor =
          program.findDestructorInstance(*owner.destructor);
      if (!destructor || destructor->owner != owner.id ||
          destructor->definitionKind != MirDefinitionKind::Source)
        malformed("LLVM transfer lost its exact source destructor");
    }
    if (!interface)
      for (const auto &selection : owner.virtualDispatch) {
        const auto *target = program.findFunctionInstance(selection.target);
        if (!target)
          malformed("LLVM transfer state lost its selected function");
        if (target->pureVirtual)
          unsupported("LLVM transfer excludes a pure executable state");
      }
    const auto base = [&](const HirBaseInstance &row) {
      const auto *selected = program.findClassInstance(row.instance);
      if (!selected || selected->type != row.type ||
          row.interface != (selected->kind == ClassKind::Interface))
        malformed("LLVM transfer lost an exact base identity");
      shape(program, *selected, kind, special, path);
    };
    for (const auto &row : owner.bases)
      base(row);
    for (const auto &row : owner.structuralBases)
      base(row);
    for (const auto &field : owner.declaredFields) {
      if (bitwiseGeneratedCopy(program, field.type))
        continue;
      if (field.type.kind != SemanticType::Class)
        unsupported("LLVM hierarchy transfer requires scalar or record fields");
      // A field is an independent complete object. Its own generated-member
      // contract must hold; a generated outer move cannot bypass a source
      // constructor selected for a field.
      shape(program, instanceOf(program, field.type), kind, special, path);
    }
    path.erase(owner.id);
  }

private:
  std::size_t baseOccurrences(const MirClassInstance &owner,
                              HirClassInstanceId target,
                              std::set<HirClassInstanceId> &path) const {
    if (!path.insert(owner.id).second)
      malformed("LLVM transfer loan has a cyclic base path");
    std::size_t result = owner.id == target;
    for (const auto &base : owner.bases)
      result += baseOccurrences(instance(base.type), target, path);
    path.erase(owner.id);
    return result;
  }
  // A by-value parameter's binding obligation is active on entry; every
  // other transfer obligation starts inactive.
  const MirDropObligation &drop(MirDropObligationId id,
                                const MirClassInstance &owner,
                                bool entry = false) const {
    const auto *result = body.findDropObligation(id);
    if (!result || result->id != id || (result->initiallyActive && !entry))
      malformed("LLVM transfer lost its exact inactive drop obligation");
    const auto canonical = mirUniqueCleanupType(program, owner.type);
    const MirDropType inert{.type = owner.type, .classInstance = owner.id};
    if ((owner.requiresActiveCleanup && !canonical) ||
        result->dropType != (canonical ? *canonical : inert))
      malformed("LLVM transfer drop recipe differs from its sealed schedule");
    return *result;
  }
};

} // namespace

void preflightGeneratedHierarchyCopy(const MirProgram &program,
                                     const SemanticType &type) {
  std::set<HirClassInstanceId> path;
  const auto &owner = FreshTransferQuery::instanceOf(program, type);
  if (owner.kind == ClassKind::Interface || owner.abstract)
    throw BackendUnsupportedError(
        "LLVM hierarchy copy requires a concrete ordinary class");
  FreshTransferQuery::shape(program, owner, ConstructorKind::Copy, true, path);
}

FreshHierarchyTransfer
planFreshHierarchyTransfer(const MirProgram &program, const MirBody &body,
                           const MirInstruction &producer) {
  return FreshTransferQuery(program, body).plan(producer);
}

Placement::Placement(const MirBody &body) { collect(nullptr, body); }

Placement::Placement(const MirProgram &program, const MirBody &body) {
  static_cast<void>(ownedTransferBody(program, body));
  collect(&program, body);
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions) {
      if (!isFreshHierarchyTransferProducer(stage) ||
          !containsHierarchyStorage(program, stage.info.type))
        continue;
      std::optional<FreshHierarchyTransfer> transfer;
      try {
        transfer = planFreshHierarchyTransfer(program, body, stage);
      } catch (const BackendUnsupportedError &) {
        continue; // An unsupported candidate gains no new placement authority.
      }
      if (!producers.emplace(*stage.result, &stage).second)
        throw std::logic_error(
            "LLVM hierarchy transfer already has generic placement authority");
      hierarchyTransfers.insert(*stage.result);
      // Bind only the exact proven publication. Do not replay the generic
      // CallInput/aggregate/return scan after enabling hierarchy freshness.
      // A slot construction relocates from the moved binding itself, a
      // consumed receiver, replacing value, spawned task or exchanged
      // replacement lives in its own value storage, and an element lives in
      // its aggregate's slot.
      if (transfer->publishes == FreshHierarchyPublication::Returned)
        bind(*stage.result, {.returned = true});
      else if (transfer->publishes == FreshHierarchyPublication::Element)
        bind(*stage.result,
             {.aggregate = *transfer->publication->result,
              .element = transfer->element,
              .variant = transfer->publication->operation ==
                                 MirOperation::PayloadConstruct
                             ? transfer->publication->enumVariant
                             : std::nullopt});
      else if (transfer->publishes != FreshHierarchyPublication::Slot &&
               transfer->publishes != FreshHierarchyPublication::Receiver &&
               transfer->publishes != FreshHierarchyPublication::Replacement &&
               transfer->publishes != FreshHierarchyPublication::Task &&
               transfer->publishes != FreshHierarchyPublication::Exchange)
        bind(*stage.result, {.place = transfer->destination});
    }
}

void Placement::collect(const MirProgram *program, const MirBody &body) {
  const auto storesAddress = [&](const SemanticType &type) {
    return program ? addressValue(*program, type) : addressValue(type);
  };
  // A payload construction always produces a payload enum, and a closure its
  // environment, which live in memory even where no program classifies the
  // type.
  // A call returning a closure by value produces an environment in memory too.
  const auto payloadAggregate = [](const MirInstruction &stage) {
    return (stage.kind == MirInstructionKind::Compute &&
            (stage.operation == MirOperation::PayloadConstruct ||
             stage.operation == MirOperation::Closure)) ||
           (stage.kind == MirInstructionKind::Call &&
            stage.info.type.kind == SemanticType::Lambda &&
            stage.info.category == ValueCategory::Value);
  };
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions)
      if (!stage.tombstone && stage.result &&
          (storesAddress(stage.info.type) || payloadAggregate(stage))) {
        const bool hierarchyTransport =
            program &&
            (stage.kind == MirInstructionKind::Load ||
             stage.kind == MirInstructionKind::Move ||
             (stage.kind == MirInstructionKind::Construct &&
              stage.constructorKind != ConstructorKind::Ordinary) ||
             isPayloadFieldMove(stage)) &&
            containsHierarchyStorage(*program, stage.info.type);
        if (!hierarchyTransport)
          producers.emplace(*stage.result, &stage);
      }
  for (const auto &block : body.blocks) {
    for (const auto &stage : block.instructions) {
      if (stage.tombstone)
        continue;
      if ((stage.kind == MirInstructionKind::Initialize ||
           stage.kind == MirInstructionKind::CallInput) &&
          stage.destination && stage.operands.size() == 1 &&
          stage.operands[0].kind == MirOperandKind::Value) {
        const auto *place = body.findPlace(*stage.destination);
        const auto producer = producers.find(stage.operands[0].value);
        if (place &&
            (storesAddress(place->type) ||
             (producer != producers.end() &&
              payloadAggregate(*producer->second))) &&
            place->type == stage.operands[0].type)
          bind(stage.operands[0].value, {.place = *stage.destination});
      }
      if (stage.operation == MirOperation::Aggregate && stage.result &&
          storesAddress(stage.info.type))
        for (std::size_t index = 0; index < stage.operands.size(); ++index)
          if (stage.operands[index].kind == MirOperandKind::Value)
            bind(stage.operands[index].value,
                 {.aggregate = *stage.result, .element = index});
      if (stage.operation == MirOperation::PayloadConstruct && stage.result &&
          stage.enumVariant)
        for (std::size_t index = 0; index < stage.operands.size(); ++index)
          if (stage.operands[index].kind == MirOperandKind::Value)
            bind(stage.operands[index].value, {.aggregate = *stage.result,
                                               .element = index,
                                               .variant = stage.enumVariant});
    }
    const auto &term = block.terminator;
    if (term.kind == MirTerminatorKind::Return && term.value &&
        term.value->kind == MirOperandKind::Value &&
        body.returnType == term.value->type)
      bind(term.value->value, {.returned = true});
  }
  // MIR gives no destination to a producer whose result it materializes in a
  // hidden temporary, such as a call result that receives a further call or
  // carries a value drop inside a conditional operand. That temporary is the
  // result object, so the producer constructs directly into its storage
  // rather than into a fresh slot the temporary never sees.
  for (const auto &place : body.places) {
    if (place.root != MirPlaceRootKind::Temporary ||
        !place.projections.empty() || !storesAddress(place.type))
      continue;
    const auto materialization =
        findMirTemporaryMaterialization(body, place, place.sourceValue);
    if (materialization &&
        materialization.value->info.type == place.type &&
        !targets.contains(materialization.value->id))
      bind(materialization.value->id, {.place = place.id});
  }
}

bool Placement::fresh(MirValueId value) const {
  const auto found = producers.find(value);
  if (found == producers.end())
    return false;
  if (hierarchyTransfers.contains(value))
    return true;
  const auto &stage = *found->second;
  if (stage.kind == MirInstructionKind::CallInput &&
      stage.operands.size() == 1 &&
      stage.operands[0].kind == MirOperandKind::Value)
    return fresh(stage.operands[0].value);
  // A by-value call result is constructed fresh even when it carries borrowed
  // state, and so a loan; only a call returning a reference selects existing
  // storage.
  return stage.kind == MirInstructionKind::Construct ||
         (stage.kind == MirInstructionKind::Call &&
          (!stage.loan || stage.info.category == ValueCategory::Value) &&
          (stage.intrinsic == IntrinsicKind::None ||
           stage.intrinsic == IntrinsicKind::ThreadResultTake ||
           stage.intrinsic == IntrinsicKind::RestoringExchange)) ||
         (stage.kind == MirInstructionKind::Compute &&
          (stage.operation == MirOperation::Aggregate ||
           stage.operation == MirOperation::PayloadConstruct ||
           stage.operation == MirOperation::EnumConstant ||
           stage.operation == MirOperation::Closure));
}

void Placement::bind(MirValueId value, Target target) {
  if (!fresh(value))
    return;
  const auto &stage = *producers.at(value);
  if (stage.kind == MirInstructionKind::CallInput) {
    bind(stage.operands[0].value, target);
    return;
  }
  const auto [found, inserted] = targets.emplace(value, target);
  if (!inserted && found->second != target)
    throw BackendUnsupportedError(
        "LLVM fresh object has conflicting physical destinations");
}

const Placement::Target *Placement::target(MirValueId value) const {
  if (const auto found = producers.find(value);
      found != producers.end() &&
      found->second->kind == MirInstructionKind::CallInput && fresh(value))
    return target(found->second->operands[0].value);
  const auto found = targets.find(value);
  return found == targets.end() ? nullptr : &found->second;
}

bool carrierLoanDependency(
    const MirBody &body, const MirLoan &loan,
    const std::function<bool(const MirLoan &)> &supported,
    const std::function<bool(const MirTerminator &)> &referenceReturn) {
  const auto *stored = body.findLoan(loan.storedDependency);
  if (!stored || stored->id == loan.id || loan.referenceSelection ||
      loan.twoPhase || loan.lifecycleBound || loan.entry ||
      !loan.additionalDependencies.empty() || !loan.conditions.empty())
    return false;
  if (loan.kind == MirLoanKind::Local) {
    const auto *source = body.findPlace(loan.source);
    return !loan.escapes && source && source->binding &&
           source->root == MirPlaceRootKind::Binding &&
           source->projections.empty() && stored->kind == MirLoanKind::Stored &&
           std::find(stored->carriers.begin(), stored->carriers.end(),
                     source->binding) != stored->carriers.end();
  }
  if (loan.kind != MirLoanKind::Return || !loan.escapes)
    return false;
  const MirLoan *relayed = nullptr;
  for (const auto &block : body.blocks)
    for (const auto &stage : block.instructions)
      if (!stage.tombstone && stage.kind == MirInstructionKind::Borrow &&
          stage.loan == loan.id) {
        if (relayed || stage.operands.size() != 1 ||
            stage.operands.front().kind != MirOperandKind::Loan)
          return false;
        relayed = body.findLoan(stage.operands.front().loan);
        if (!relayed)
          return false;
      }
  return relayed && relayed->id != loan.id &&
         relayed->storedDependency == loan.storedDependency &&
         relayed->source == loan.source && supported(*relayed) &&
         std::any_of(body.blocks.begin(), body.blocks.end(),
                     [&](const auto &block) {
                       return block.terminator.returnLoan == loan.id &&
                              referenceReturn(block.terminator);
                     });
}

} // namespace lang::native_llvm
